/*******************************************************************************
* Copyright 2016-2020 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <assert.h>
#include <math.h>

#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/type_helpers.hpp"
#include "common/ve/memory_desc_wrapper_opt.hpp" // we use CoordsFor vectorization helper.
#include "common/dnnl_optimize.h"

#include "cpu/ref_lrn.hpp"

#include <iostream> // debug
#include <sstream> // debug
#include <iomanip>

namespace dnnl {
namespace impl {
namespace cpu {

#ifndef MVL
#if defined(__ve)
#define MVL 256
#else
#define MVL 32
#endif
#endif
#define LISTVEC PragmaQuote(_NEC list_vector)
#define NOVEC   PragmaQuote(_NEC novector);
#define SQUARE(...) ((__VA_ARGS__)*(__VA_ARGS__))
#define FOR_CHAN for(dim_t i=0; i<C; ++i)
//#define GATHER(PTR,BYTE_OFFSET) *(decltype(PTR))((uint8_t const*)(void*)(PTR)+(BYTE_OFFSET))
/** DID NOT WORK -- nc++-3.0.27 disallows all vectorization [sometimes] for
 * runtime-size arrays. Looking at .LL diagnostics, it may always be claiming
 * "unvectorizable dependency", no matter what you do.
 *
 * No pragma-based workarounds were found (ivdep, assume, ... no effect)
 *
 * These are used e.g. to rewrite tmp stack buffers
 * to replace multiple vector gathers with contiguous vector loads.
 *
 * But we can LIE about array size, using C++ undefined behavior [or worse]
 * to simulate what is allowed in C.
 *
 * \note this is a very bad \b hack
 */            
#define ARRAY_LYING_ABOUT_SIZE(TYPE, var, runtime_size, pretend_compile_time_size) \
            TYPE var##_valid_mem[runtime_size]; \
            auto var = *(TYPE (*)[pretend_compile_time_size]) &var##_valid_mem[0]

namespace {

typedef float acc_data_t;

static inline acc_data_t fast_negative_powf(acc_data_t omega, acc_data_t beta) {
    acc_data_t Y;
    /*
         * Y = omega^(-3/4) =
         * = 1.0f / sqrtf(omega) * sqrtf(1.0f / sqrtf(omega))
         * = sqrtf(1.0f / sqrtf(omega)) * 1.0f / sqrtf(omega)
         * = sqrtf(1.0f / sqrtf(omega)) / sqrtf(omega)
         * = sqrtf(1.0f / sqrtf(omega) / omega)
         * = sqrtf(1.0f / (sqrtf(omega) * omega))
         */
    if (beta == 0.75f) {
        Y = sqrtf(1.0f / (sqrtf(omega) * omega));
    } else {
        Y = 1.0f / powf(omega, beta);
    }
    return Y;
};

// nc++ with 32768 sometimes stoppped, but 16384 worked
dim_t constexpr stack_channels = 16384; // stack usage threshold for channel offsets

/** Divide \c hi into restricted-size blocks.
 *
 * \return a large, MVL-friendly block size < \c stack_channels for
 *         partitioning [0,hi).
 *
 * \c stack_channels maximum block size is some large multiple of MVL,
 * (unlike JIT vl suggestions bounded by MVL)
 */
inline dim_t stack_friendly_blksz(dim_t const hi){
#if 0
    dim_t constexpr stack_channels = 16384;
    dim_t ret = hi;
    if (hi > stack_channels) {
        dim_t const nFull = hi/stack_channels;
        dim_t const rem   = hi%stack_channels;
        dim_t const nLoops = nFull + (rem!=0);
        if (rem > stack_channels/2) { // remainder "big enough"
            printf("+");
            ret = (hi+nLoops-1) / nLoops;
        }else{                  // o.w. ~equal "2 2 1"
            dim_t const nLoops = nFull + (rem!=0);
            //dimt_t const nLoops = (hi+stack_channels-1) / stack_channels;
            ret = (hi+nLoops-1) / nLoops;
        }
    }
#elif 0 // some better behavior (some fast heuristic)
    dim_t const nFull = hi/stack_channels;
    dim_t const rem   = hi%stack_channels;
    dim_t const nLoops = nFull + (rem!=0);
    if (hi > stack_channels && rem > stack_channels/2) {
        ret = (hi+nLoops-1) / nLoops;
        //printf("+%d",(int)ret); // rough equipartition
    }
    auto rem256 = ret % 256;
    if (hi > 256 && rem256 && rem256<32) { // low remainder?
        auto ret2 = (hi+nLoops) / (nLoops+1); // accept one more loop
        //printf("-%d",(int)ret2);
        if( ret2%256 > rem256 ){
            //printf("!");
            ret = ret2;
        }
    }
#else // simpler (see dev/blk_friendly.cpp test prog)
    dim_t ret = hi;
    if (hi > stack_channels) {
        ret = stack_channels;
        dim_t const nFull = hi/stack_channels;
        dim_t const rem   = hi%stack_channels;
        if (rem < stack_channels/4) {
            dim_t const nLoops = nFull + (rem!=0);
            ret = (hi+nLoops-1) / nLoops;
            //printf("+%d",(int)ret); // rough equipartition
            if (ret < stack_channels - MVL) {
                ret = (ret+MVL-1)/MVL*MVL;
                //printf("^%d",(int)ret); // round up
            }
        }
    }
#endif
    //assert( ret < stack_channels );
    return ret;
}
/** but also want blksz low enough that max_threads can actually be used...
 * XXX return to this with measurements XXX. */
inline dim_t stack_friendly_blksz(dim_t const hi, dim_t const other_work){
    dim_t stack_lim = (other_work*hi/dnnl_get_max_threads());
    // adjust following, which assumes one MVL is "enough work"
    // here MVL ~ min desirable blksz (but we can go lower, a bit)
    if (stack_lim < MVL) stack_lim = MVL;
    stack_lim = (stack_lim+(MVL-1)) / MVL * MVL;
    if (stack_lim > stack_channels) stack_lim = stack_channels;

    // now heuristic with possibly smaller version of stack_channels
    dim_t ret = hi;
    if (hi > stack_lim) {
        ret = (stack_lim+31)/32*32;
        dim_t const nFull = hi/stack_lim;
        dim_t const rem   = hi%stack_lim;
        if (rem < MVL/4) {
            dim_t const nLoops = nFull + (rem!=0);
            ret = (hi+nLoops-1) / nLoops;
            //printf("+%d",(int)ret); // rough equipartition
            if (ret < stack_lim - MVL) {
                ret = (ret+MVL-1)/MVL*MVL;
                //printf("^%d",(int)ret); // round up
            }
        }
        ret = (ret+31)/32*32;
    }
    //assert( ret < stack_channels );
    return ret;
}
#if 0 // hmm maybe don't need this way
inline dim_t stack_friendly_nparts(dim_t const hi){
    dim_t constexpr stack_channels = 16384;
    dim_t ret = 1;
    if (hi > stack_channels) {
        dim_t const nFull = hi/stack_channels;
        dim_t const rem   = hi%stack_channels;
        //dimt_t const nLoops = (hi+stack_channels-1) / stack_channels;
        dim_t const nLoops = nFull + (rem!=0);
        if (rem > stack_channels/2) { // remainder "big enough"
            ret = nLoops;
        }else{                  // o.w. ~equal "2 2 1"
            dim_t blkeq = (hi+nLoops-1) / nLoops;
            ret = (hi+blkeq-1) / blkeq;
        }
    }
    typedef long int ld;
    printf(" partn(%ld)->blksz=%ld\n",(ld)hi,(ld)ret);
    return ret;
}
#endif

// ? constexpr unsigned ndims<tag> ?
#define OFFSET_ARGS memory_desc_wrapper const& md, \
    dim_t const mb, dim_t const stride_mb, \
    dim_t const c, dim_t const C, \
    dim_t const d, dim_t const D, \
    dim_t const h, dim_t const H, \
    dim_t const w, dim_t const W

typedef size_t offset_ret_t;
/** For nc++, templated specialized templates make dead-code elimination easier
 * than the original lambda function. */
template <impl::format_tag_t tag> static inline offset_ret_t offset(OFFSET_ARGS) {
    if (md.ndims() >= 5) return md.off(mb, c, d, h, w);
    if (md.ndims() >= 4) return md.off(mb, c, h, w);
    if (md.ndims() >= 3) return md.off(mb, c, w);
    return md.off(mb, c);
};
template<> inline offset_ret_t offset<format_tag::nChw8c>(OFFSET_ARGS) {
    constexpr int blksize = 8;
    return mb * stride_mb + (c / blksize) * H * W * blksize
        + h * W * blksize + w * blksize + c % blksize;
};
template<> inline offset_ret_t offset<format_tag::nChw16c>(OFFSET_ARGS) {
    constexpr int blksize = 16;
    return mb * stride_mb + (c / blksize) * H * W * blksize
        + h * W * blksize + w * blksize + c % blksize;
};
template<> inline offset_ret_t offset<format_tag::nchw>(OFFSET_ARGS) {
    return mb * stride_mb + c * H * W + h * W + w;
};
template<> inline offset_ret_t offset<format_tag::nhwc>(OFFSET_ARGS) {
    return mb * stride_mb + h * W * C + w * C + c;
};

} // namespace

/** Calculate phy offset for channels [clo,chi) sequentially into memory
 * \c data_offf.  (i) Prepare coords for vectorized physical offset calc.
 * (ii) calc phys offset via func calls to memory_desc_wrapper_opt.
 *
 * \note Do not use if you have a formula version like nchw, nChw8c because
 * these: (i) do not need a function call; (ii) optimize well already.
 *
 * \note If use in other places, provide such a vectorized helper directly
 * from mwd_opt.
 */
template<typename SIZE_T>
static inline void channel_offsets( memory_desc_wrapper_opt const& data_opt,
        SIZE_T * const data_off, dim_t const mb, dim_t const clo, dim_t const chi,
        dim_t const d, dim_t const h, dim_t const w) {
    //assert(!data_opt.is_zero());
    static_assert(sizeof(SIZE_T) == sizeof(dim_t), "bad-sized channel offsets");
    typedef CoordsForNd<6,uint64_t,uint64_t> Coords;
    auto const dm = data_opt.ndims();
    auto cf = (dm >= 5)? Coords::mk(mb,mb+1, clo,chi, d,d+1, h,h+1, w,w+1)
        : (dm >= 4)? Coords::mk(mb,mb+1, clo,chi, h,h+1, w,w+1)
        : (dm >= 3)? Coords::mk(mb,mb+1, clo,chi, w,w+1)
        : Coords::mk(mb,mb+1, clo,chi); // dm>=2 ?
    for ( ; cf; ++cf) { // channel coords in simd-length chunks
        data_opt.vec_off_v(
                cf.base(), // VecPos& vector register data
                (dim_t*)&data_off[cf.get_pos()], // outputs (seq)
                cf.get_vl(), // register len, num ouptuts
                false/*is_pos_padded*/);
    }
};

// Forward LRN formula:
// y_i = x_i * (k + a / n * Sum:j [x_j^2])^-b, where
// k, a(alpha), b(beta), n(local_size) - lrn hyperparameters;
// j - kernel points, j in [i - n/2, i + n/2] for ACROSS, 2d-shape for WITHIN;

template <impl::data_type_t d_type>
template <impl::format_tag_t tag>
void ref_lrn_fwd_t<d_type>::execute_forward(const exec_ctx_t &ctx) const {
    using namespace alg_kind;
    using namespace format_tag;
    typedef CoordsForNd<6,uint64_t,uint64_t> Coords;

    auto src = CTX_IN_MEM(const data_t *, DNNL_ARG_SRC);
    auto dst = CTX_OUT_MEM(data_t *, DNNL_ARG_DST);

    // offset calcs with builting formula vectorize well:
    //bool const formula = true;
    //bool const formula = false;
    bool const formula = (tag == nchw || tag == nhwc || tag == nChw8c || tag == nChw16c);
    //bool const formula = (tag != any); // via pd()->dat_tag_, see ref_lrn.hpp
    //const memory_desc_wrapper data_d(pd()->src_md());
    //
    // If we have a formula, it vectorizes well.  If not, we pull in some vectorization
    // machinery to calculate physical offset in simd-length batches
    // I'd LIKE to have an empty data_opt wrapper if I do not need it:
    //
    //memory_desc_wrapper_opt data_opt(formula? memory_desc_t(): *pd()->src_md());
    //
    // UNFORTUNATELY, wrapper asserts that it is_block_desc() at several points..
    //
    // so just always construct the optimized one (slightly more work)
    //
    const memory_desc_wrapper_opt data_opt(pd()->src_md());
    const memory_desc_wrapper& data_d = data_opt;

    const dim_t MB = pd()->MB();
    const dim_t C = pd()->C();
    const dim_t D = pd()->D();
    const dim_t H = pd()->H();
    const dim_t W = pd()->W();
    const auto stride_mb = data_d.blocking_desc().strides[0];
    const bool across_channels = pd()->desc()->alg_kind == lrn_across_channels;
    static constexpr dim_t blksize = tag == nChw16c ? 16 : 8;
    const auto ndims = data_d.ndims();

    auto compute_n_summands = [&](dim_t size) {
        if (across_channels) {
            return size;
        } else { // within_channel
            dim_t n_summands = 1;
            for (auto d = ndims - 2; d > 0; --d)
                n_summands *= size;
            return n_summands;
        }
    };

    const acc_data_t alpha = static_cast<acc_data_t>(pd()->desc()->lrn_alpha);
    const acc_data_t beta = static_cast<acc_data_t>(pd()->desc()->lrn_beta);
    const acc_data_t k = static_cast<acc_data_t>(pd()->desc()->lrn_k);
    const dim_t size = pd()->desc()->local_size;
    const dim_t half_size = (size - 1) / 2;
    const dim_t summands = compute_n_summands(size); // lrn window elements

    // VE does not like vectorizing nstl::max or nstl::min inside loops
#define DEFINE_HALFSIZE_RANGE(var_st, var_en, i, lo, hi) \
    const dim_t var_st = ((i) - half_size + 0 <  dim_t{lo}? dim_t{lo}: (i) - half_size + 0); \
    const dim_t var_en = ((i) + half_size + 1 >= dim_t{hi}? dim_t{hi}: (i) + half_size + 1);

    // perhaps also accelerate
    if(ndims==3) assert( data_d.matches_one_of_tag(format_tag::abc) );
    //const auto wrk_per_C = MB * D * H * W;
    if(0){
    }
    else if(ndims == 2) { // degenerate case
        // degenerate case: lrn window of size 1 is all we can use.
        // come in with tag="any"
        // NOTE: lrn pd always has MB() as axis 0 and C() as axis 1 (fmt ab)
        assert( data_d.matches_one_of_tag(format_tag::ab/*,format_tag::ba*/) );
#define COFF(c) (mb * stride_mb + (c))
        //dim_t blksz = stack_friendly_blksz(C);
        dim_t blksz = stack_friendly_blksz(C,MB/*other iter dims*/);
        //printf("mb=%ld C=%ld blksz=%ld div_up(C,blksz)=%ld\n",
        //        MB,C,blksz,utils::div_up(C, blksz));

        parallel_nd(MB, utils::div_up(C, blksz), [&](dim_t mb, dim_t c_blk)
            {
                dim_t const clo = c_blk * blksz;
                dim_t const chi = (clo + blksz >= C? C: clo + blksz);
                if (across_channels) {
                    dim_t const cspan = chi - clo;
                    acc_data_t sum[cspan];
                    for(dim_t c=0; c<cspan; ++c) sum[c]= 0;
                    NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                        // ... valid c have lrn window still in range [0,C)
                        dim_t oc_lo = (0-l < 0? 0: 0-l);
                        dim_t oc_hi = (C-l > C? C: C-l);
                        // ... and ALSO we focus on range [clo,chi)
                        if (oc_lo < clo) oc_lo = clo;
                        if (oc_hi > chi) oc_hi = chi;
                        for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                            sum[c-clo] += SQUARE(acc_data_t{src[COFF(c+l)]});
                        }
                    }
                    for(dim_t oc=clo; oc<chi; ++oc) {
                        float const sum_oc = k + alpha * sum[oc-clo] / summands;
                        dst[COFF(oc)] = static_cast<data_t>( src[COFF(oc)]
                                * fast_negative_powf(sum_oc, beta));
                    }
                }else{ // "lrn sum" = src value at one point
                    // too slow: used "mb * stride_mb + c" directly
                    //channel_offsets( data_opt, cent, mb, clo,chi, 1, 1, 1 );
                    for(dim_t c=clo; c<chi; ++c) {
                        acc_data_t central = src[COFF(c)];
                        auto const sum = k + alpha * SQUARE(central) / summands;
                        dst[COFF(c)] = static_cast<data_t>( central
                                * fast_negative_powf(sum, beta));
                    }
                }
#undef COFF
            });
    }
    // --lrn --tag=nchw ic2002ih10 --tag=ncdhw ic2002id10
    // orig : 1.74 15.8 ms
    //else if (across_channels && C <= stack_channels)
    //  for dev:
    else if (!formula && across_channels && C <= stack_channels)
    {
        //auto ker_across_vec = [&src,&dst,&stride_mb,&C,&D,&H,&W,&half_size,&k,&alpha,&beta,&summands](
        auto ker_across_vec = [&](
                size_t * const dst_off,     // oc from 
                dim_t const mb,             // 0 .. C-1
                dim_t const od, dim_t const oh, dim_t const ow) {
            acc_data_t sum[C];
            FOR_CHAN sum[i]= 0;
            FOR_CHAN {
                DEFINE_HALFSIZE_RANGE(c_st, c_en, i, 0, C);
                for(dim_t c = c_st; c < c_en; ++c) {
                    const acc_data_t s = src[dst_off[c]];
                    sum[i] += s * s;
                }
            }
            FOR_CHAN sum[i] = k + alpha * sum[i] / summands;
            FOR_CHAN dst[dst_off[i]] = static_cast<data_t>( src[dst_off[i]]
                        * fast_negative_powf(sum[i], beta));
        };

        // size^{2 or 3}? XXX
        // and MB*D*H*W work amount sufficiently large for omp? XXX
        // vectorize across channels, using fast offsets calculated on stack.
        parallel_nd(MB, D, H, W, [&](dim_t const mb, dim_t const d, dim_t const h, dim_t const w) {
                //size_t data_off[C];
                size_t data_off[stack_channels]; // too big, but compilet-time
                if (formula || C <= size ) { // maybe size^2 or size^3 ?
#if 0 // only slightly better vectorization (recheck)
                    if (tag == nchw ) for(size_t c=0U; c<C; ++c)
                        data_off[c] = mb * stride_mb + c * H * W + h * W + w;
                    else if (tag == nhwc ) for(size_t c=0U; c<C; ++c)
                        data_off[c] = mb * stride_mb + h * W * C + w * C + c;
                    else
#endif
                    // always unvectorizable. Fix: use `size_t c` (not "unsigned")
                    for(size_t c=0U; c<C; ++c) data_off[c] = offset<tag>(
                            data_d, mb,stride_mb, c,C, d,D, h,H, w,W );
                } else { // function call phys offset needs a little help to vectorize
                    // unvec: function calls init_nd and vec_off_v
                    channel_offsets(data_opt, data_off, mb, 0, C, d, h, w);
                }
                ker_across_vec(&data_off[0], mb, /*c,*/ d, h, w);
            });
    }
    // dense strided chan + lapped (no restriction on C)
    else if ((tag==nchw || tag==nhwc) && across_channels) {
        // C <= stack_channels orig: --tag=nchw,nhwc ic2002ih10 0.184 0.162 ms
        // Large channels: split over threads, and overlap offset calcs,
        // because lrn window can extend half_size past the central range.
        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), H, W,
                [&](dim_t const mb, dim_t const c_blk, dim_t const h, dim_t const w) {
            dim_t const clo = c_blk * blksz;
            dim_t const chi = nstl::min(clo + blksz, C);
            dim_t const cspan = chi - clo;
            acc_data_t sum[cspan];
            for(dim_t c=0; c<cspan; ++c) sum[c]= 0;

            dim_t c_off0 = offset<tag>(data_d, mb,stride_mb, 0,C, 0,D, h,H, w,W );
            dim_t const c_stride = (tag==nhwc? dim_t{1}: dim_t{H*W});
#define COFF(c) (c_off0 + (c) * c_stride)
#if 0 // correct, but slow loop order
            // nchw ic2002ih10 ic32777ih10  1.35, 22.41 ms
            for(dim_t c=clo; c<chi; ++c) { // c ~ central channel
                DEFINE_HALFSIZE_RANGE(c_st, c_en, c, 0, C);
                for(dim_t k = c_st; k < c_en; ++k) { // k ~ lrn window
                    const acc_data_t s = src[COFF(k)]; // lrn window datum
                    sum[c-clo] += s * s; // accum central sum
                }
            }
#else // Two-stage loop limits for reordered loops
            // nchw ic2002ih10 ic32777ih10 0.179, 2.97 ms (7.5x speedup)
            NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                // oc_lo+l >= clo ==> oc_lo >= clo-l
                // (oc_hi-1)+l < chi  ==> oc_hi < chi-l+1
                // ... valid c in range [0,C) "for sure"
                dim_t oc_lo = (0-l < 0? 0: 0-l);
                dim_t oc_hi = (C-l > C? C: C-l);
                // ... (tricky) THEN adjusted to [clo,chi]
                if (oc_lo < clo) oc_lo = clo;
                if (oc_hi > chi) oc_hi = chi;
                for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                    data_t const s = src[COFF(c+l)]; // lrn window datum
                    sum[c-clo] += SQUARE( acc_data_t{s} );
                }
            }
#endif
            for(dim_t oc=clo; oc<chi; ++oc) {
                float const sum_oc = k + alpha * sum[oc-clo] / summands;
                dst[COFF(oc)] = static_cast<data_t>( src[COFF(oc)]
                        * fast_negative_powf(sum_oc, beta));
            }
#undef COFF
        });
    }
    // XXX Compare timing w/ next specialization. Can we drop specialized code?
    //     (orig: restrict C<=stack_channels, simpler code
    else if ((tag==nChw8c || tag==nChw16c) && across_channels && C <= stack_channels)
    {
        // 16c ic2002ih10 ic32777ih10 0.196 37.2 ms (37.2 NOT HERE)
        //
        //#if 0 // orig ms : dense: 0.184 0.162 blocked: **2.34 2.34 ms**
        // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=FWD_D ic2002ih10iw10
        //#elif 0
        //#elif 0 // cleanup : 1.07, 1.06 ms
        //#elif 1 // run-time array --> compile-time size ==> 0.199, 0.203 ms
        assert( D==1 );
        parallel_nd(MB, H, W, [&](dim_t mb, dim_t h, dim_t w) {
                acc_data_t sum[C];
                size_t data_off[C];
                // srcdata: vec gather once --> linear vec loads later
                // (and potentially single conversion to acc_data_t)
                //acc_data_t srcdata[C];
                // Undesirable workaround:
                //   If runtime-array, nc++ gets confused about vector dependencies.
                //   But making it compile-time size (max size) now vectorizes.
                // NO amount of pragmas could convince nc++ otherwise
                acc_data_t srcdata[stack_channels];

                // (loop combin with next kills vectorization)
                for(dim_t c=0U; c<C; ++c) sum[c] = acc_data_t{0};
                for(dim_t c=0U; c<C; ++c){
#if 0
#define OFF(blksize) (mb * stride_mb + (c / (blksize)) * H * W * (blksize) \
        + h * W * (blksize) + w * (blksize) + c % (blksize))
                    data_off[c] = (tag==nChw8c? OFF(8): OFF(16));
#undef OFF
#else // same speed
                    data_off[c] = offset<tag>(data_d, mb,stride_mb,
                            c,C, 0,1, h,H, w,W );
#endif
                    srcdata[c] = src[data_off[c]];
                }
                for (dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                    // oc+l must lie in data bounds:
                    dim_t const oc_lo = ( -l < 0? 0:  -l);
                    dim_t const oc_hi = (C-l > C? C: C-l);
                    for(dim_t oc=oc_lo; oc<oc_hi; ++oc)
                        sum[oc] += SQUARE( srcdata[oc+l] );
                }
                for(dim_t oc=0; oc<C; ++oc){
                    float const sum_cs = k + alpha * sum[oc] / summands;
                    dst[data_off[oc]] = static_cast<data_t>(
                            srcdata[oc]
                            * fast_negative_powf(
                                sum_cs, //k + alpha * sum[oc] / summands,
                                beta));
                }
            });
    }
    // any number of channels : not hugely slower than C<=stack_channels
    else if ((tag==nChw8c || tag==nChw16c) && across_channels && half_size<100)
    {
        // 16c ic2002ih10 ic32777ih10 0.196 37.2 ms
        // --> here 1.07 19.42
        // --> 0.417 8.13 (adjust [] sizes)
        // now 2 over-sized arrays (uggh) ---> 0.204 2.00 ms
        //
        // XXX 2.00 ms is even faster than nchw (2.97 ms)
        //     So perhaps copying into stride-4 srcdata[] vector load area
        //     is universally "a good thing" ? ? ?
        //
        //#if 0 // orig ms : dense: 0.184 0.162 blocked: **2.34 2.34 ms**
        // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=FWD_D ic2002ih10iw10
        //#elif 0
        //#elif 0 // cleanup : 1.07, 1.06 ms
        //#elif 1 // run-time array --> compile-time size ==> 0.199, 0.203 ms
        assert( D==1 );
        dim_t const blksz = stack_friendly_blksz(C);
        assert( blksz <= stack_channels );
        using std::cout; using std::endl;
        parallel_nd(MB, utils::div_up(C, blksz), H, W,
                [&](dim_t const mb, dim_t const c_blk, dim_t const h, dim_t const w) {
            dim_t const clo = c_blk * blksz;
            dim_t const chi = nstl::min(clo + blksz, C);
            dim_t const cspan = chi - clo;

            //acc_data_t sum[cspan];            // desired
            //acc_data_t sum[stack_channels];   // $$$ workaround
            //
            // "lie" about array extent so nc++ vectorizes
            // allowed in C, but you should not do this in C++:
#if 1
            acc_data_t sum[cspan]; // nc++ runtime size MIGHT kill vectorizn
#else
            acc_data_t sum[stack_channels]; // big compile time size $$$
#endif

            for(dim_t c=0; c<cspan; ++c) sum[c]= acc_data_t{0};

            // for arb C, clo, chi, we need data_off and srcdata possibly
            // half_size past ends
            dim_t const clolo = nstl::max(clo - half_size + 0, (dim_t)0);
            dim_t const chihi = nstl::min(chi + half_size + 1, C);
            dim_t offset_span = chihi - clolo;
            size_t data_off[offset_span];
            for (dim_t c=clolo; c<chihi; ++c) {
                data_off[c-clolo] = offset<tag>(data_d, mb,stride_mb,
                        c,C, 0,D, h,H, w,W );
            }

            // srcdata: vec gather once --> linear vec loads later
            // (and potentially single conversion to acc_data_t)
            //acc_data_t srcdata[C];
            // Undesirable workaround:
            //   If runtime-array, nc++ gets confused about vector dependencies.
            //   But making it compile-time size (max size) now vectorizes.
            // NO amount of pragmas could convince nc++ otherwise
            //
#if 0
            acc_data_t srcdata[offset_span];  // kills vectorizn !!!
#elif 0 // FAILED WORKAROUND
            // turn runtime array size into "array of known size"
            //   C allows, C++ disallows (at least undefined behavior)
            acc_data_t srcdata_valid_mem[offset_span];
            auto srcdata = *(acc_data_t (*)[stack_channels+2*100])(&srcdata_valid_mem[0]);
#else
            acc_data_t srcdata[stack_channels+2*100]; // $$$ workaround
#endif

            // Finding : ONE of srcdata or sum must have compile-time size

            // (loop combine with next kills vectorization)
            for (dim_t c=0; c<offset_span; ++c) {
                srcdata[c] = src[data_off[c]]; // c=0 ~ channel clolo
            }

            NOVEC for (dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                dim_t oc_lo = ( -l < 0? 0:  -l);
                dim_t oc_hi = (C-l > C? C: C-l);
                // lapped range: extra adjust
                if (oc_lo < clo) oc_lo = clo;
                if (oc_hi > chi) oc_hi = chi;
                // unvec !!! so made sum[] compile-time max dim :(
                for(dim_t c=oc_lo; c<oc_hi; ++c)
                    sum[c-clo] += SQUARE( srcdata[c+l-clolo] );
            }
            for(dim_t oc=clo; oc<chi; ++oc){
                float const sum_oc = k + alpha * sum[oc-clo] / summands;
                dst[data_off[oc-clolo]] = static_cast<data_t>( srcdata[oc-clolo]
                        * fast_negative_powf(sum_oc, beta));
            }
        });
    }
    else if (across_channels) {
        // actually this will handle any number of channels,
        // as well as trivial ndims=2 case
        /** if channels large, break apart and overlap the offset calcs */
        auto ker_across_vec_lapped = [&](
                dim_t const clo, dim_t const chi, // 'central' channels range
                size_t * const dst_off, // now from max(0,clo-half_size) to min(chi+half_size+1,C)
                // but &dst_off[0] corresponds to 'clo'  (small -ve offsets possible)
                dim_t const mb, // internally c=0..C-1
                dim_t const od, dim_t const oh, dim_t const ow) {
            dim_t const cspan = chi - clo;
            dim_t const clolo = nstl::max(clo - half_size + 0, (dim_t)0); // corresp. dst_off[0]
            acc_data_t sum[cspan];
            for(dim_t c=0; c<cspan; ++c) sum[c]= 0;
            for(dim_t c=clo; c<chi; ++c) { // c ~ central channel
                DEFINE_HALFSIZE_RANGE(c_st, c_en, c, 0, C);
                for(dim_t k = c_st; k < c_en; ++k) { // k ~ lrn window
                    const acc_data_t s = src[dst_off[k-clolo]]; // lrn window datum
                    sum[c-clo] += s * s; // accum central sum
                }
            }
            for(int c=0; c<cspan; ++c) sum[c] = k + alpha * sum[c] / summands;
            // offset data for 'central' region, fwd by up to half_size
            size_t * central_off = dst_off + (clo - clolo);
            for(int c=0; c<cspan; ++c)
                dst[central_off[c]] = static_cast<data_t>(
                        src[central_off[c]] * fast_negative_powf(sum[c], beta));
        };

        // Large channels: split over threads, and overlap offset calcs,
        // because lrn window can extend half_size past the central range. */
        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                [&](dim_t mb, dim_t c_blk, dim_t d, dim_t h, dim_t w) {
            dim_t clo = c_blk * blksz;
            dim_t chi = nstl::min(clo + blksz, C);
            // lrn kernel extends up to half_size past [clo,chi)
            dim_t clolo = nstl::max(clo - half_size, dim_t{0});
            dim_t chihi = nstl::min(chi + half_size + 1, C);
            dim_t offset_span = chihi - clolo;
            size_t data_off[offset_span];
            if(formula) {
                for(size_t c=clolo; c<chihi; ++c){
                    data_off[c-clolo] = offset<tag>(data_d, mb,stride_mb,
                            c,C, d,D, h,H, w,W );
                }
            }else{ // vec_off_v phys offset calc in simd-length chunks
                channel_offsets(data_opt, data_off, mb, clolo, chihi, d, h, w);
            }
            ker_across_vec_lapped( clo, chi, &data_off[0], mb, /*c,*/ d, h, w);
            });
    }
#if 0 // very old...
    else if (across_channels) {
        // ker single channel (original way, ok if D*H*W is big enough?)
        // (one part is calculating the channel-wise "central" offsets)
        parallel_nd(MB, C, D, H, W,
                [&](dim_t mb, dim_t c, dim_t d, dim_t h, dim_t w) {
                const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, d,D, h,H, w,W);
                ker_within(off, mb, c, d, h, w);
                });
    }
#endif
    else { // lrn fwd within : multiple versions
        assert( !across_channels );
        assert( ndims >= 3 );
        auto ker_within = [&](dim_t const central_off,
                dim_t const mb, dim_t const oc,
                dim_t const od, dim_t const oh, dim_t const ow) {
            // 6 VRspill!
            acc_data_t sum = 0;
            // --mode=C --lrn --tag=ncdhw --dir=FWD_D --alg=ACROSS,WITHIN ic202id10 --tag=nchw ic202ih10
            //      2.366 166 0.231 3.41 ms
            //                      3.36
            // perf: 2.39 174 0.266 3.46
            // perf: 2.39 172 0.266 3.66 expand formula cases here
            //      2.43 167 0.266 3.38 ms
            //           133 with ker_within_vec_generic
            // generic fwd lrn_within_channel
            assert(ndims >= 3);
            assert(ndims <= 5);
            DEFINE_HALFSIZE_RANGE(d_st, d_en, od, 0, D);
            DEFINE_HALFSIZE_RANGE(h_st, h_en, oh, 0, H);
            DEFINE_HALFSIZE_RANGE(w_st, w_en, ow, 0, W);
            if (formula) {
                // formula ==> DENSE layouts and formula-driven vectorize well
                if (tag==nchw || tag==nhwc || tag==nChw8c || tag==nChw16c) {
                    // 4D formulas
                    for_(dim_t h = h_st; h < h_en; ++h)
                    for (dim_t w = w_st; w < w_en; ++w)
                    {
                        acc_data_t s=0;
                        if (tag == nchw )
                            s = src[mb * stride_mb + oc * H * W + h * W + w];
                        else if (tag == nhwc )
                            s = src[mb * stride_mb + h * W * C + w * C + oc];
                        else if(tag == nChw8c ) {
                            constexpr int blksize = 8;
                            s = src[mb * stride_mb + (oc / blksize) * H * W * blksize
                                + h * W * blksize + w * blksize + oc % blksize];
                        } else /*if(tag == nChw16c)*/ {
                            constexpr int blksize = 16;
                            s = src[mb * stride_mb + (oc / blksize) * H * W * blksize
                                + h * W * blksize + w * blksize + oc % blksize];
                        }
                        sum += s * s;
                    }
                } else { // only for 'formula=true' testing
                    for_(dim_t d = d_st; d < d_en; ++d)
                    for_(dim_t h = h_st; h < h_en; ++h)
                    for (dim_t w = w_st; w < w_en; ++w)
                    {
                        acc_data_t const s = src[offset<tag>(
                                data_d, mb,stride_mb, oc,C, d,D, h,H, w,W)];
                        sum += s * s;
                    }
                }
            } else { // ncdhw: 1866 --> 182 ms (~ 9x faster, using vec_off_v)
                //using dnnl::impl::CoordRegs<uint64_t,6>; // match memory_desc_wrapper_opt::VecPos
                typedef CoordsForNd<6,uint64_t,uint64_t> Coords;
                auto cf= (ndims >= 5)? Coords::mk(mb,mb+1, oc,oc+1, d_st,d_en, h_st,h_en, w_st,w_en)
                : (ndims >= 4)? Coords::mk(mb,mb+1, oc,oc+1, h_st,h_en, w_st,w_en)
                : Coords::mk(mb,mb+1, oc,oc+1, w_st,w_en); // ndims>=3
                NOVEC for ( ; cf; ++cf) { // VR 1spill 2restore
                    dim_t lrnp[MVL];
                    const unsigned vl=cf.get_vl();
                    data_opt.vec_off_v( cf.base(), &lrnp[0], vl, false/*is_pos_padded*/ );
                    for(size_t dhw=0U; dhw<vl; ++dhw) {
                        sum += SQUARE(acc_data_t{ src[lrnp[dhw]] });
                    }
                }
            }
            data_t central = src[central_off];
            sum = k + alpha * sum / summands;
            dst[central_off] = static_cast<data_t>(central * fast_negative_powf(sum, beta));
        };

        assert( !across_channels );
        assert( ndims >= 3 );

        // "old-style" (vectorize across lrn window, ~5)
        // "new style" block C loop so kernels exec longer.
        // Too bad offsets can't easily be re-used unless d,h,w
        // get tiled, but that is hard.  I guess some fairly large d,h,w
        // tiling could be used (just recalc around tile edges).
        if (tag == nChw16c || tag == nChw8c) {
            // This blocking MIGHT buy a little bit of locality, I guess
            parallel_nd(MB, utils::div_up(C, blksize), H, W,
                    [&](dim_t mb, dim_t c_blk, dim_t h, dim_t w) {
                    dim_t const c = c_blk * blksize;
                    const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, 0,1, h,H, w,W);
                    PRAGMA_OMP_SIMD()
                    dim_t const cc_en = (blksize <= C - c? blksize: C -c);
                    for (dim_t cc = 0; cc < cc_en; ++cc)
                        // novec (func call)
                        ker_within(off+cc, mb, c + cc, 0, h, w);
                    });
        }
#if 0 //orig
        // lrn fwd_d across --tag=nchw ic2002ih10 36.9 ms
        else if (tag == nchw || tag == nhwc) {
            parallel_nd(MB, C, H, W, [&](dim_t mb, dim_t c, dim_t h, dim_t w)
            {
                const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, 0,1, h,H, w,W);
                ker_within(off, mb, c, 0, h, w);
            });
        }
#elif 0 // expanded original
        // lrn fwd_d across --tag=nchw ic2002ih10  18.6 ms
#elif 0 // block along C: expanded original
        // lrn fwd_d across --tag=nchw ic2002ih10  17.0 ms
elif 0 // clean up // lrn fwd_d across --tag=nchw ic2002ih10  16.83 ms
#elif 1 // split loops, adress .LL report <Unvectorized loop.>
        // Use mem areas, allowing loop reorder so 'oc' loop is inner
        // Add c_stride for these dense formats
        // lrn fwd_d across --tag=nchw ic2002ih10  6.18 ms, 5.87 ms, 0.274 ms
        //
        // begin: 18.6 ms    end: 0.274 ms    67x speedup
        else if (tag == nchw || tag == nhwc) {
            dim_t const blksz = stack_friendly_blksz(C);
            //printf(".");
            parallel_nd(MB, utils::div_up(C, blksz), H, W,
                    [&](dim_t const mb, dim_t const c_blk, dim_t const oh, dim_t const ow)
            {
                dim_t const clo = c_blk * blksz;
                DEFINE_HALFSIZE_RANGE(h_st, h_en, oh, 0, H);
                dim_t const chi = (clo + blksz >= C? C: clo+blksz);
                DEFINE_HALFSIZE_RANGE(w_st, w_en, ow, 0, W);
                dim_t const cspan = chi - clo;

                acc_data_t sum[cspan];
                for(dim_t cs=0; cs<cspan; ++cs) sum[cs] = acc_data_t{0};
                data_t const __restrict__ *rsrc = &src[0];

                for_(dim_t h = h_st; h < h_en; ++h)
                for_(dim_t w = w_st; w < w_en; ++w)
                for(dim_t oc=clo; oc<chi; ++oc) // potential large VL as inner loop
                {
                    dim_t const off = offset<tag>(data_d, mb,stride_mb,
                            oc,C, 0,1, h,H, w,W );
                    acc_data_t const s = rsrc[off];
                    sum[oc-clo] += s * s;
                }

#if 0 // orig 0.276 ms
                dim_t const c_off = offset<tag>( data_d, mb,stride_mb,
                        clo,C, 0,1, oh,H, ow,W );
                dim_t const c_stride = (tag==nhwc? dim_t{1}: dim_t{H*W});

                LISTVEC for(dim_t cs=0; cs<cspan; ++cs){
                    //sum[cs] = k + alpha * sum[cs] / summands;
                    //acc_data_t const sum_cs = k + alpha * sum[cs] / summands;
                    dst[c_off + cs*c_stride] = static_cast<data_t>(
                            src[c_off + cs*c_stride]
                            * fast_negative_powf(
                                //sum_cs,
                                k + alpha * sum[cs] / summands,
                                beta));
                }
#else // 0.274 ms
                LISTVEC for(dim_t cs=0; cs<cspan; ++cs){
                    //sum[cs] = k + alpha * sum[cs] / summands;
                    acc_data_t const sum_cs = k + alpha * sum[cs] / summands;
                    dim_t const oc_off = offset<tag>( data_d, mb,stride_mb,
                            clo+cs,C, 0,1, oh,H, ow,W );
                    dst[oc_off] = static_cast<data_t>(
                            src[oc_off]
                            * fast_negative_powf(
                                sum_cs, //k + alpha * sum[cs] / summands,
                                beta));
                }
#endif
            });
        }
#endif
        else if( !formula && C > size) {
#if 0
            // Here we address func call offsets, any # channels
            // lrn fwd_d ACROSS,WITHIN --tag=ncdhw ic32777id10 --tag=nchw ic32777ih10
            // 300 **3799** 32 529 ms
            // C=802 : still good
            auto ker_within_vec_generic2= [&](dim_t const mb,
                    dim_t const clo, dim_t const chi,
                    dim_t const od, dim_t const oh, dim_t const ow
                    )
            {
                static_assert( sizeof(dim_t) == sizeof(size_t), "type issue" );
#if 0 // 134 ms for ncdhw ic202id10
                // the obvious way to use the "Coords" iterator
                DEFINE_HALFSIZE_RANGE(d_st, d_en, od, 0, D);
                DEFINE_HALFSIZE_RANGE(h_st, h_en, oh, 0, H);
                DEFINE_HALFSIZE_RANGE(w_st, w_en, ow, 0, W);
                size_t center[chi-clo];
                channel_offsets( data_opt, &center[0], mb, clo, chi, od, oh, ow );
                for(dim_t oc=clo; oc<chi; ++oc) {
                    acc_data_t sum = 0;
                    typedef CoordsForNd<6,uint64_t,uint64_t> Coords;
                    auto cf= (ndims >= 5)? Coords::mk(mb,mb+1, oc,oc+1, d_st,d_en, h_st,h_en, w_st,w_en)
                        : (ndims >= 4)? Coords::mk(mb,mb+1, oc,oc+1, h_st,h_en, w_st,w_en)
                        : Coords::mk(mb,mb+1, oc,oc+1, w_st,w_en); // ndims>=3
                    // constructor and iter *many* times :(
                    NOVEC for ( ; cf; ++cf) {
                        const unsigned vl=cf.get_vl();
                        dim_t lrnp[MVL]; // get lrn window phys coords
                        data_opt.vec_off_v(cf.base(), (dim_t *)&lrnp[0], vl);
                        ShortLoop() for(unsigned dhw=0U; dhw<vl; ++dhw) {
                            sum += SQUARE(acc_data_t{ src[lrnp[dhw]] });
                        }
                    }
                    sum = k + alpha * sum / summands;
                    data_t const central = src[center[oc-clo]];
                    dst[center[oc-clo]] = static_cast<data_t>(central * fast_negative_powf(sum, beta));
                }
#else // 24.5 ms  26.4 ms (x5 faster, reorder loops)
                acc_data_t sum[chi-clo];
                for(dim_t oc=clo; oc<chi; ++oc) sum[oc-clo] = 0;
                DEFINE_HALFSIZE_RANGE(d_st, d_en, od, 0, D);
                DEFINE_HALFSIZE_RANGE(h_st, h_en, oh, 0, H);
                DEFINE_HALFSIZE_RANGE(w_st, w_en, ow, 0, W);
                typedef CoordsForNd<6,uint64_t,uint64_t> Coords;
                auto cf= (ndims >= 5)? Coords::mk(mb,mb+1, 0,1, d_st,d_en, h_st,h_en, w_st,w_en)
                    : (ndims >= 4)? Coords::mk(mb,mb+1, 0,1, h_st,h_en, w_st,w_en)
                    : Coords::mk(mb,mb+1, 0,1, w_st,w_en); // ndims>=3
                // outer loop: Coords constructor and iter is slow
                NOVEC for ( ; cf; ++cf) {
                    const unsigned vl=cf.get_vl(); // vl ~ d,h,w coords
                    for(dim_t oc=clo; oc<chi; ++oc)
                    {
                        ShortLoop() for(unsigned i=0U; i<vl; ++i)
                            cf.vp[1][i] = oc; // manual adjustment
                        dim_t lrnp[MVL];
                        data_opt.vec_off_v(cf.base(), (dim_t *)&lrnp[0], vl);
                        for(unsigned i=0U; i<vl; ++i) {
                            sum[oc-clo] += SQUARE(acc_data_t{ src[lrnp[i]] });
                        }
                    }
                    ShortLoop() for(unsigned i=0U; i<vl; ++i)
                        cf.vp[1][i] = 0; // restore orig cf channel (0)
                }
                size_t center[chi-clo];
                channel_offsets( data_opt, center, mb, clo, chi, od, oh, ow );
                for(dim_t oc=clo; oc<chi; ++oc) {
                    auto const sum_oc = k + alpha * sum[oc-clo] / summands;
                    data_t const central = src[center[oc-clo]];
                    dst[center[oc-clo]] = static_cast<data_t>(central
                            * fast_negative_powf(sum_oc, beta));
                }
#endif
            };
#endif

            dim_t const blksz = stack_friendly_blksz(C);
            parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                    [&](dim_t mb, dim_t c_blk, dim_t od, dim_t oh, dim_t ow) {
                    dim_t clo = c_blk * blksz;
                    dim_t chi = (clo + blksz >= C? C: clo + blksz);
                    //printf(" clo,chi=%ld,%ld\n", clo, chi);
                    //ker_within_vec_generic2(mb, clo,chi, d, h, w);
                    acc_data_t sum[chi-clo];
                    for(dim_t oc=clo; oc<chi; ++oc) sum[oc-clo] = 0; // VRrestore3!
                    DEFINE_HALFSIZE_RANGE(d_st, d_en, od, 0, D);
                    DEFINE_HALFSIZE_RANGE(h_st, h_en, oh, 0, H);
                    DEFINE_HALFSIZE_RANGE(w_st, w_en, ow, 0, W);
                    typedef CoordsForNd<6,uint64_t,uint64_t> Coords;
                    auto cf= (ndims >= 5)? Coords::mk(mb,mb+1, 0,1, d_st,d_en, h_st,h_en, w_st,w_en)
                        : (ndims >= 4)? Coords::mk(mb,mb+1, 0,1, h_st,h_en, w_st,w_en)
                        : Coords::mk(mb,mb+1, 0,1, w_st,w_en); // ndims>=3
                    // Coords constructor and iter is pain point.
                    NOVEC for ( ; cf; ++cf) { // VR 2spill 2restore!
                        const unsigned vl=cf.get_vl(); // vl ~ d,h,w coords
                        for(dim_t oc=clo; oc<chi; ++oc) // VR 4spill 4restore!
                        {
                            ShortLoop() for(unsigned i=0U; i<vl; ++i)
                                cf.vp[1][i] = oc; // manual adjustment
                            dim_t lrnp[MVL];
                            data_opt.vec_off_v(cf.base(), (dim_t *)&lrnp[0], vl);
                            for(unsigned i=0U; i<vl; ++i) {
                                sum[oc-clo] += SQUARE(acc_data_t{ src[lrnp[i]] });
                            }
                        }
                        ShortLoop() for(unsigned i=0U; i<vl; ++i)
                            cf.vp[1][i] = 0; // restore orig cf channel (0)
                    }
                    size_t center[chi-clo];
                    channel_offsets( data_opt, center, mb, clo, chi, od, oh, ow );
                    for(dim_t oc=clo; oc<chi; ++oc) { // 6 VRspill,restore!
                        auto const sum_oc = k + alpha * sum[oc-clo] / summands;
                        data_t const central = src[center[oc-clo]];
                        dst[center[oc-clo]] = static_cast<data_t>(central
                                * fast_negative_powf(sum_oc, beta));
                    }
                    });
        }
        else{ // original way
            // **655** ms, C=802
            // ker single channel (original way, ok if D*H*W is big enough?)
            // (one part is calculating the channel-wise "central" offsets)
            parallel_nd(MB, C, D, H, W,
                    [&](dim_t mb, dim_t c, dim_t d, dim_t h, dim_t w) {
                    const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, d,D, h,H, w,W);
                    ker_within(off, mb, c, d, h, w);
                    });
        }
    }
}

// Backward LRN formula (refer to Forward LRN formula):
// Partial derivatives:
// dy_i/dx_j =         - 2*a*b/n * x_i * O(i)^-b / O(i) * x_j, i != j
//             O(i)^-b - 2*a*b/n * x_i * O(i)^-b / O(i) * x_j, i == j, where
// O(i) = (k + a / n * Sum:j [x_j^2]), j in [i - n/2, i + n/2]. Note: j depends
//     on i, which means that O(i) may use more points than local_size.
// Now, z_i = Sum:k [dE/dy_k * dy_k/dx_j], where k in [i - n/2, i + n/2]
//     for ACROSS. 2d-shape for WITHIN.
// Then, dE/dy_k = diffDst_k. Finally,
// z_i = Sum:k [dd_k * dy_k/dx_j] = A - B (code variables) =
//     = dd_i * O(i)^-b - 2*a*b/n * x_i * Sum:k {O(k)^-b / O(k) * x_k * dd_k};

template <impl::data_type_t d_type>
template <dnnl_format_tag_t tag>
void ref_lrn_bwd_t<d_type>::execute_backward(const exec_ctx_t &ctx) const {
    using namespace alg_kind;
    using namespace format_tag;

    auto src = CTX_IN_MEM(const data_t *, DNNL_ARG_SRC);
    auto diff_dst = CTX_IN_MEM(const data_t *, DNNL_ARG_DIFF_DST);
    auto diff_src = CTX_OUT_MEM(data_t *, DNNL_ARG_DIFF_SRC);

    //bool const formula = true;
    bool const formula = false;
    //bool const formula = (tag == nchw || tag == nhwc || tag == nChw8c || tag == nChw16c);
    //bool const formula = (tag != any); // via pd()->dat_tag_, see ref_lrn.hpp
    // XXX just always construct the optimized one (slightly more work)
    const memory_desc_wrapper_opt data_opt(pd()->src_md());
    const memory_desc_wrapper& data_d = data_opt;

    const dim_t C = pd()->C();
    const dim_t D = pd()->D();
    const dim_t H = pd()->H();
    const dim_t W = pd()->W();
    const auto stride_mb = data_d.blocking_desc().strides[0];
    const bool across_channels = pd()->desc()->alg_kind == lrn_across_channels;
    static constexpr dim_t blksize = tag == nChw16c ? 16 : 8;
    const auto ndims = data_d.ndims();

    auto compute_n_summands = [&](dim_t size) {
        if (across_channels) {
            return size;
        } else { // within_channel
            dim_t n_summands = 1;
            for (auto d = ndims - 2; d > 0; --d)
                n_summands *= size;
            return n_summands;
        }
    };

    const acc_data_t alpha = static_cast<acc_data_t>(pd()->desc()->lrn_alpha);
    const acc_data_t beta = static_cast<acc_data_t>(pd()->desc()->lrn_beta);
    const acc_data_t k = static_cast<acc_data_t>(pd()->desc()->lrn_k);
    const dim_t size = pd()->desc()->local_size;
    const dim_t half_size = (size - 1) / 2;
    const dim_t summands = compute_n_summands(size);

    // REMOVED for VE: pass by value due to icc170 and icc180 problem on KNL
    auto get_omega = [&](dim_t const mb, dim_t const oc, dim_t const od,
            dim_t const oh, dim_t const ow) {
        acc_data_t sum = 0;
        if (across_channels) {
            const dim_t c_st = nstl::max(oc - half_size + 0, (dim_t)0);
            const dim_t c_en = nstl::min(oc + half_size + 1, C);

            for (dim_t c = c_st; c < c_en; ++c) {
                const acc_data_t s = src[offset<tag>(data_d, mb,stride_mb,
                        c,C, od,D, oh,H, ow,W)];
                sum += s * s;
            }
        } else {
            dim_t d_st = nstl::max(od - half_size + 0, (dim_t)0);
            dim_t d_en = nstl::min(od + half_size + 1, D);
            dim_t h_st = nstl::max(oh - half_size + 0, (dim_t)0);
            dim_t h_en = nstl::min(oh + half_size + 1, H);
            dim_t w_st = nstl::max(ow - half_size + 0, (dim_t)0);
            dim_t w_en = nstl::min(ow + half_size + 1, W);
            for_(dim_t d = d_st; d < d_en; ++d)
            for_(dim_t h = h_st; h < h_en; ++h)
            for (dim_t w = w_st; w < w_en; ++w) {
                const acc_data_t s = src[offset<tag>(
                        data_d, mb,stride_mb, oc,C, d,D, h,H, w,W)];
                sum += s * s;
            }
        }
        return (acc_data_t)(k + alpha * sum / summands);
    };
#if 0
    // get_omega could be scratchpad data from FWD_D pass? XXX
    auto get_omega_across_vec = [&](
            size_t * const dst_off /*[0..C-1]*/,
            // internal : dim_t const oc,
            dim_t const od, dim_t const oh, dim_t const ow) {
        acc_data_t sum[C] = 0;
        FOR_CHAN sum[i] = acc_data_t{0};
        FOR_CHAN {
            const dim_t c_st = nstl::max(oc - half_size + 0, (dim_t)0);
            const dim_t c_en = nstl::min(oc + half_size + 1, C);

            for (dim_t c = c_st; c < c_en; ++c) {
                const acc_data_t s = src[dst_off[c]];
                sum += s * s;
            }
        }
        return (acc_data_t)(k + alpha * sum / summands);
    };
    auto ker_across_vec = [&](
            size_t * const dst_off,     // oc from 
            dim_t const mb,             // 0 .. C-1
            dim_t const od, dim_t const oh, dim_t const ow) {
        for(dim_t oc=0; oc<C; ++oc){
            acc_data_t A = 0, B = 0;
            data_t central = src[dst_off[oc]];
            if(1) {
                const dim_t c_st = nstl::max(oc - half_size + 0, (dim_t)0);
                const dim_t c_en = nstl::min(oc + half_size + 1, C);

                for (dim_t c = c_st; c < c_en; c++) {
                    const auto off = dst_offoffset<tag>(data_d, mb,stride_mb,
                            c,C, od,D, oh,H, ow,W);
                    const acc_data_t omega = get_omega(mb, c, od, oh, ow);
                    const acc_data_t omega_in_beta
                        = fast_negative_powf(omega, beta);
                    const acc_data_t tmp
                        = omega_in_beta * (acc_data_t)diff_dst[off];
                    if (c == oc) A = tmp;
                    B += (src[off] * tmp / omega);
                }
            }
            B *= (2.0f * alpha * beta * central / summands);
            *d = static_cast<data_t>(A - B);
        }
    };
#endif


    auto ker = [&](data_t * const d, dim_t const mb, dim_t const oc,
            dim_t const od, dim_t const oh, dim_t const ow) {
        acc_data_t A = 0, B = 0;
        data_t central = src[offset<tag>(data_d, mb,stride_mb, oc,C, od,D, oh,H, ow,W)];
        if (across_channels) {
            const dim_t c_st = nstl::max(oc - half_size + 0, (dim_t)0);
            const dim_t c_en = nstl::min(oc + half_size + 1, C);

            for (dim_t c = c_st; c < c_en; c++) {
                const auto off = offset<tag>(data_d, mb,stride_mb,
                        c,C, od,D, oh,H, ow,W);
                const acc_data_t omega = get_omega(mb, c, od, oh, ow);
                const acc_data_t omega_in_beta
                        = fast_negative_powf(omega, beta);
                const acc_data_t tmp
                        = omega_in_beta * (acc_data_t)diff_dst[off];
                if (c == oc) A = tmp;
                B += (src[off] * tmp / omega);
            }
        } else {
            dim_t d_st = nstl::max(od - half_size + 0, (dim_t)0);
            dim_t d_en = nstl::min(od + half_size + 1, D);
            dim_t h_st = nstl::max(oh - half_size + 0, (dim_t)0);
            dim_t h_en = nstl::min(oh + half_size + 1, H);
            dim_t w_st = nstl::max(ow - half_size + 0, (dim_t)0);
            dim_t w_en = nstl::min(ow + half_size + 1, W);
            for_(dim_t d = d_st; d < d_en; ++d)
            for_(dim_t h = h_st; h < h_en; ++h)
            for (dim_t w = w_st; w < w_en; ++w) {
                const auto off = offset<tag>(data_d, mb,stride_mb,
                        oc,C, d,D, h,H, w,W);
                const acc_data_t omega = get_omega(mb, oc, d, h, w);
                const acc_data_t omega_in_beta
                        = fast_negative_powf(omega, beta);
                const acc_data_t tmp
                        = omega_in_beta * (acc_data_t)diff_dst[off];
                if (d == od && h == oh && w == ow) A = tmp;
                B += (src[off] * tmp / omega);
            }
        }
        B *= (2.0f * alpha * beta * central / summands);
        *d = static_cast<data_t>(A - B);
    };

    const dim_t MB = pd()->MB();
#if 1
    if(0) {
    }
#else
    dim_t const stack_channels = 32768;
    // new methods =========================================================
    if (across_channels && C >= size && C <= stack_channels ) { // size^{2 or 3}? XXX
        // vectorize across channels, using fast offsets calculated on stack.
        parallel_nd(MB, D, H, W, [&](dim_t mb, dim_t d, dim_t h, dim_t w) {
                size_t data_off[C];
                if (formula) {
                    for(unsigned c=0U; c<C; ++c) data_off[c] = offset<tag>(
                            data_d, mb,stride_mb, c,C, d,D, h,H, w,W );
                } else { // function call phys offset needs a little help to vectorize
                    channel_offsets(data_opt, data_off, mb, 0, C, d, h, w);
                }
                ker_across_vec(&data_off[0], mb, /*c,*/ d, h, w);
            });
    }
#endif
    // old methods =========================================================
    else if (tag == nChw16c || tag == nChw8c) {
        parallel_nd(MB, utils::div_up(C, blksize), H, W,
                [&](dim_t mb, dim_t c_blk, dim_t h, dim_t w) {
                    dim_t c = c_blk * blksize;
                    //const dim_t off = mb * stride_mb + c * H * W
                    //        + (h * W + w) * blksize;
                    const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, 0,1, h,H, w,W);
                    PRAGMA_OMP_SIMD()
                    for (dim_t cc = 0; cc < nstl::min(blksize, C - c); ++cc)
                        ker(&diff_src[off + cc], mb, c + cc, 0, h, w);
                });
    } else if (tag == nhwc || tag == nchw) {
        parallel_nd(MB, H, W, C, [&](dim_t mb, dim_t h, dim_t w, dim_t c) {
            const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, 0,1, h,H, w,W);
            ker(&diff_src[off], mb, c, 0, h, w);
        });
    } else {
        parallel_nd(MB, C, D, H, W,
                [&](dim_t mb, dim_t c, dim_t d, dim_t h, dim_t w) {
                    const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, d,D, h,H, w,W);
                    ker(&diff_src[off], mb, c, d, h, w);
                });
    }
}

template void
ref_lrn_fwd_t<data_type::f32>::execute_forward<format_tag::nChw16c>(
        const exec_ctx_t &ctx) const;
template void
ref_lrn_fwd_t<data_type::f32>::execute_forward<format_tag::nChw8c>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_fwd_t<data_type::f32>::execute_forward<format_tag::nchw>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_fwd_t<data_type::f32>::execute_forward<format_tag::nhwc>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_fwd_t<data_type::f32>::execute_forward<format_tag::any>(
        const exec_ctx_t &ctx) const;
template void
ref_lrn_bwd_t<data_type::f32>::execute_backward<format_tag::nChw16c>(
        const exec_ctx_t &ctx) const;
template void
ref_lrn_bwd_t<data_type::f32>::execute_backward<format_tag::nChw8c>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_bwd_t<data_type::f32>::execute_backward<format_tag::nchw>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_bwd_t<data_type::f32>::execute_backward<format_tag::nhwc>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_bwd_t<data_type::f32>::execute_backward<format_tag::any>(
        const exec_ctx_t &ctx) const;

template void
ref_lrn_fwd_t<data_type::bf16>::execute_forward<format_tag::nChw16c>(
        const exec_ctx_t &ctx) const;
template void
ref_lrn_fwd_t<data_type::bf16>::execute_forward<format_tag::nChw8c>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_fwd_t<data_type::bf16>::execute_forward<format_tag::nchw>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_fwd_t<data_type::bf16>::execute_forward<format_tag::nhwc>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_fwd_t<data_type::bf16>::execute_forward<format_tag::any>(
        const exec_ctx_t &ctx) const;
template void
ref_lrn_bwd_t<data_type::bf16>::execute_backward<format_tag::nChw16c>(
        const exec_ctx_t &ctx) const;
template void
ref_lrn_bwd_t<data_type::bf16>::execute_backward<format_tag::nChw8c>(
        const exec_ctx_t &ctx) const;
template void
ref_lrn_bwd_t<data_type::bf16>::execute_backward<format_tag::nchw>(
        const exec_ctx_t &ctx) const;
template void
ref_lrn_bwd_t<data_type::bf16>::execute_backward<format_tag::nhwc>(
        const exec_ctx_t &ctx) const;
template void ref_lrn_bwd_t<data_type::bf16>::execute_backward<format_tag::any>(
        const exec_ctx_t &ctx) const;

} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino=+s,l0,\:4,N-s

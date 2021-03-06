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
        //PragmaQuote(_NEC noverror_check) //==> func call ==> no vectorizn
        Y = sqrtf(1.0f / (sqrtf(omega) * omega));
    } else {
        Y = 1.0f / powf(omega, beta);
    }
    return Y;
};

// nc++ with 32768 sometimes stoppped, but 16384 worked
/** stack usage threshold (max array size) for channel offsets.
 *
 * \todo VE blocksz chooser for tmp arrays should have use a
 * template compiler-time const max size. Best value depends
 * on across/within/fwd/bwd. (Significant speed differences).
 * Compile-time bound on tmp stack arrays is frequently req'd
 * to allow nc++ to vectorize.
 *
 */
//dim_t constexpr stack_channels = 16384;
//dim_t constexpr stack_channels = 8192;
static dim_t constexpr stack_channels = 4096; // a generally decent value
//dim_t constexpr stack_channels = 2048;
//dim_t constexpr stack_channels = 1000;
//
// lrnb.in
// 1000 is just as good as 4096 for bwd WITHIN
// 4096 is a wee bit bettero for bwd ACROSS (not worth extra mem at this point)
// 1000 vs 1024: allows twice lrn-size (typ 2*5) overhang for bwd
//
// lrnf.in (fwd samples) 
// 4096 sometimes 25% faster than 1000
// 16k slower, except one case
// 2k worse than 4k, generally
// 8k better for nhwc, 4k better for nchw ACROSS
// similar for WITHIN, but blocked fmts better at 4k

/** Divide \c hi into restricted-size blocks.
 *
 * \return a large, MVL-friendly block size < \c stack_channels for
 *         partitioning [0,hi); min return value is 1
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
            //dim_t const nLoops = (hi+stack_channels-1) / stack_channels;
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
    dim_t ret = (hi>0? hi: 1);
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
    dim_t ret = (hi>0? hi: 1);
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
        //dim_t const nLoops = (hi+stack_channels-1) / stack_channels;
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
 * \c data_off.  (i) Prepare coords for vectorized physical offset calc.
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
    bool constexpr formula = (tag == nchw || tag == nhwc || tag == nChw8c || tag == nChw16c);
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
    //static constexpr dim_t blksize = tag == nChw16c ? 16 : 8;
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
    //  for dev:
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
#if 0
    // --tag=nChw16c --alg=ACROSS --dir=FWD_D ic2002ih10 ic9999ih10 ic32777ih10
    // this block:   0.200 0.630 1.97 ms
    // handle below:  (identical)
    // note both use "oversized runtime array" workaround
    else if ((tag==nChw8c || tag==nChw16c) && across_channels && C <= stack_channels)
    {
        // 16c ic2002ih10 ic32777ih10 0.196 37.2 ms (37.2 NOT HERE)
        //
        //#if 0 // orig ms : dense: 0.184 0.162 blocked: **2.34 2.34 ms**
        // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=FWD_D ic2002ih10iw10
        //#elif 0
        //#elif 0 // cleanup : 1.07, 1.06 ms
        //#elif 1 // run-time array --> compile-time size ==> 0.199, 0.203 ms
    }
#endif
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
#if 0
    else if (across_channels && C <= stack_channels)
    {
        // --tag=ncdhw --alg=ACROSS --dir=FWD_D ic2002id10 ic32777id10
        // This block: 16.0 274 ms
        // without (block below) : 15 271 ms
#endif
    else if (across_channels /*&& half_size <= 127*/) // lucky w/ optimizations
    {
        // adapted from bwd_t, when fwd was slower than bwd!
        //   remove lapped-kernel call:
        //      15.67 ms ---> 4.06 ms (whew, faster than bwd now)
        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                [&](dim_t const mb, dim_t const c_blk, dim_t const d, dim_t const h, dim_t const w) {
            dim_t const clo = c_blk * blksz;
            dim_t const chi = nstl::min(clo + blksz, C);

            // for arb C, clo, chi, we need chanoff and srcdata possibly
            // half_size past ends
            dim_t const clolo = nstl::max(clo - half_size + 0, (dim_t)0);
            dim_t const chihi = nstl::min(chi + half_size + 1, C);
            // slowdown for nchw, (maybe not, for nhwc (CHECKME))
            size_t chanoff[chihi - clolo];
            //size_t chanoff[stack_channels + 2*127];

            //acc_data_t srcgt[chihi - clolo]; // src gathered; need [clolo,chihi)
            if(formula) {
                for (dim_t c=clolo; c<chihi; ++c) {
                    chanoff[c-clolo] = offset<tag>(data_d, mb,stride_mb,
                            c,C, 0,D, h,H, w,W );
                    //srcgt[c-clolo] = src[chanoff[c-clolo]];
                }
            }else{ // vec_off_v phys offset calc in simd-length chunks
                channel_offsets(data_opt, chanoff, mb, clolo, chihi, d, h, w);
                //for(size_t c=clolo; c<chihihi; ++c){
                //    srcgt[c-clolo] = src[chanoff[c-clolo]];
                //}
            }
#define COFF(c) (chanoff[ (c) - clolo ])
            //acc_data_t sum[chi-clo];      // rt dim
            acc_data_t sum[stack_channels]; // vec workaround
            for(dim_t c=0; c<chi-clo; ++c) sum[c]= acc_data_t{0};
            float const inv_summands = 1.0f / static_cast<float>(summands);
            NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                // XXX make other fwd look this simple too XXX
                dim_t const oc_lo = (0-l < clo? clo: 0-l);
                dim_t const oc_hi = (C-l > chi? chi: C-l);
                PragmaQuote(_NEC shortloop_reduction)//;
                for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                    sum[c-clo] += SQUARE( acc_data_t{src[COFF(c+l)]} );
                    //sum[c-clo] += SQUARE( srcgt[c+l-clolo] );
                }
            }
            //PragmaQuote(_NEC shortloop_reduction)//;
            //for (dim_t c=clolo; c<chihi; ++c) { // c ~ central channel
            //    sum[c-clolo] = k + (alpha * inv_summands) * sum[c-clolo];
            //}
            for(int c=0; c<chi-clo; ++c) sum[c] = k + (alpha * inv_summands) * sum[c];
            // offset data for 'central' region, fwd by up to half_size
            size_t * central_off = chanoff + (clo - clolo);
            for(int c=0; c<chi-clo; ++c) {
                dst[central_off[c]] = static_cast<data_t>(
                        src[central_off[c]] * fast_negative_powf(
                            sum[c],
                            beta));
            }
        });
#undef COFF
    }
#if 0
    else if (across_channels) { // older lapped-kernel version
        // actually this will handle any number of channels, formula or not,
        // as well as trivial ndims=2 case
        /** if channels large, break apart and overlap the offset calcs */
        auto ker_across_vec_lapped = [&](
                dim_t const clo, dim_t const chi, // 'central' channels range
                size_t * const dst_off, // now from max(0,clo-half_size) to min(chi+half_size+1,C)
                // but &dst_off[0] corresponds to 'clo'  (small -ve offsets possible)
                dim_t const mb, // internally c=0..C-1
                dim_t const od, dim_t const oh, dim_t const ow)
        {
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
                [&](dim_t mb, dim_t c_blk, dim_t d, dim_t h, dim_t w)
        {
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
#endif
    //
    // below : all alg=WITHIN cases
    //
    else if (formula) {
        assert( !across_channels );
        assert( ndims >= 3 );
        // split loops, adress .LL report <Unvectorized loop.>
        // Use mem areas, allowing loop reorder so 'oc' loop is inner
        // Add c_stride for these dense formats
        // lrn fwd_d across --tag=nchw ic2002ih10  6.18 ms, 5.87 ms, 0.274 ms
        //
        // begin: 18.6 ms    end: 0.274 ms    67x speedup
        //else if (tag == nchw || tag == nhwc)
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
    else if (!formula) {
        assert( !across_channels );
        assert( ndims >= 3 );
        // --tag=ncdhw --alg=WITHIN --dir=FWD_D ic2002id10 ic32777id10
        // ... 219 3583 ms
        // cf "ker_within" (following)
        // ... 1727 27878 ms ... leave ker_within "just for show" (?)
        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                [&](dim_t mb, dim_t c_blk, dim_t od, dim_t oh, dim_t ow)
        {
            dim_t clo = c_blk * blksz;
            dim_t chi = (clo + blksz >= C? C: clo + blksz);
            //printf(" clo,chi=%ld,%ld\n", clo, chi);
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
    //bool const formula = false;
    bool const formula = (tag == nchw || tag == nhwc || tag == nChw8c || tag == nChw16c);
    //bool const formula = (tag != any); // via pd()->dat_tag_, see ref_lrn.hpp
    // XXX just always construct the optimized one (slightly more work)
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
    const dim_t summands = compute_n_summands(size);

    if(0) {
    }
    // Note: bound half_size<=127 is **ONLY** to allow compile-time stack array
    //       size, which is required to allow nc++ to vectorize inner loops.
#if 1
    //else if (1 && (tag==nchw || tag==nhwc) && across_channels)
    else if (1 && (tag==nchw || tag==nhwc) && across_channels && half_size <= 127)
    {
        assert(formula);
        // Old version (after this block)
        // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=BWD_D ic2002ih10
        // bwd perf: 0.558 0.641 0.571 0.585
        // remeasure: 0.557 0.638 0.572 0.584   14.23 117.0
        // --tag=nchw --alg=ACROSS --dir=BWD_D ic32777ih10 (14.23 ms)
        // --tag=nchw --alg=ACROSS --dir=BWD_D ic234567ih10 (117.0 ms)
        //
        // Now just enable the JUST_DENSE flag:
        // nchw .558-->0.505 nhwc .638-->0.458 (up to 28% speedup)
        // perf: 98.8% vec, <VL>=251
        // dev version cleanup (again)
        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                [&](dim_t const mb, dim_t const c_blk, dim_t const d, dim_t const h, dim_t const w) {
            dim_t const clo = c_blk * blksz;
            dim_t const chi = nstl::min(clo + blksz, C);
            dim_t const cspan = chi - clo;
            // comp-time array bound [vs cspan] as ++ vectorization workaround
            acc_data_t A[stack_channels + 2*127];
            acc_data_t B[stack_channels + 2*127];
            for(dim_t c=0; c<cspan; ++c) {
                A[c] = 0;
                B[c] = 0;
            }
            dim_t clolo = nstl::max(clo - half_size, dim_t{0});
            dim_t chihi = nstl::min(chi + half_size + 1, C);

#define JUST_DENSE 1
#define USE_SRCGT 0
            // nchw,nhwc --alg=ACROSS --dir=BWD_D ic32777ih10 ic3000ih10
            // USE_SRCGT 0 : 12.4 5.94   0.704 0.631 ms
            // USE_SRCGT 1 : 12.5 5.97   0.703 0.643 ms same speed
#if JUST_DENSE
            dim_t c_off0 = offset<tag>(data_d, mb,stride_mb, 0,C, 0,D, h,H, w,W );
            dim_t const c_stride = (tag==nhwc? dim_t{1}: dim_t{H*W});
#define COFF(c) (c_off0 + (c) * c_stride)
#if USE_SRCGT
            // slowdown for nhwc? (check)
            dim_t const clololo = nstl::max(clolo - half_size, dim_t{0});
            dim_t const chihihi = nstl::min(chihi + half_size + 1, C);
            acc_data_t srcgt[chihihi - clololo]; // src gathered
            {
                dim_t const chihihi = nstl::min(chihi + half_size + 1, C);
                for(size_t c=clololo; c<chihihi; ++c)
                    srcgt[c-clololo] = src[COFF(c)];
            }
#endif
#else
            dim_t const clololo = nstl::max(clolo - half_size, dim_t{0});
            dim_t const chihihi = nstl::min(chihi + half_size + 1, C);
            // slowdown for nchw, (maybe not, for nhwc (CHECKME))
            //size_t chanoff[chihihi - clololo];
            size_t chanoff[stack_channels + 4*127];
#if USE_SRCGT
            acc_data_t srcgt[chihihi - clololo]; // src gathered; need [clolo,chihi)
#endif
            {
                //if(formula) {
                    for(size_t c=clololo; c<chihihi; ++c){
                        chanoff[c-clololo] = offset<tag>(data_d, mb,stride_mb,
                                c,C, d,D, h,H, w,W );
#if USE_SRCGT
                        srcgt[c-clololo] = src[chanoff[c-clololo]];
#endif
                    }
                //}
                //else{ // vec_off_v phys offset calc in simd-length chunks
                //    channel_offsets(data_opt, chanoff, mb, clololo, chihihi, d, h, w);
                //}
                //  opt attempt: src is VGT twice --> do that once...
            }
#define COFF(c) (chanoff[ (c) - clololo ])
#endif // JUST_DENSE

#if USE_SRCGT
#define SRCGT(c) srcgt[(c)-clololo]
#else
#define SRCGT(c) src[COFF(c)]
#endif
            float const inv_summands = 1.0f / static_cast<float>(summands);
            {
                //acc_data_t sum[chihi-clolo]; // rt dim
                acc_data_t sum[stack_channels + 2*127]; // vec workaround
                for (dim_t c=clolo; c<chihi; ++c) sum[c-clolo] = 0;
                NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                    dim_t const oc_lo = (0-l < clolo? clolo: 0-l);
                    dim_t const oc_hi = (C-l > chihi? chihi: C-l);
                    PragmaQuote(_NEC shortloop_reduction)//;
                    for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                        sum[c-clolo] += SQUARE( acc_data_t{src[COFF(c+l)]} );
                    }
                }
                PragmaQuote(_NEC shortloop_reduction)//;
                for (dim_t c=clolo; c<chihi; ++c) { // c ~ central channel
                    sum[c-clolo] = k + (alpha * inv_summands) * sum[c-clolo];
                }
                // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=BWD_D ic2002ih10
                // --tag=nchw --alg=ACROSS --dir=BWD_D ic32777ih10 (14.23 ms)
                // perf: 98.8% vec, <VL>=251
                NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                    dim_t const oc_lo = (0-l < clo? clo: 0-l);
                    dim_t const oc_hi = (C-l > chi? chi: C-l);
                    PragmaQuote(_NEC shortloop_reduction)//;
                    for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                        const auto off = COFF(c+l);
                        const acc_data_t omega = sum[c+l-clolo];
#if 0
                        const acc_data_t omega_in_beta
                            = fast_negative_powf(omega, beta);
                        const acc_data_t tmp
                            = omega_in_beta * (acc_data_t)diff_dst[off];
#else
                        const acc_data_t tmp
                            = fast_negative_powf(omega, beta)
                            * (acc_data_t)diff_dst[off];
#endif
                        if (l == 0) A[c-clo] = tmp;
                        B[c-clo] += (src[off] * tmp / omega);
                    }
                }
            }
            for(dim_t c=clo; c<chi; ++c) { // c ~ central channel
                // vsc and vgt ... (14.6 ms)
                //B[c-clo] *= (2.0f * alpha * beta * inv_summands) * src[COFF(c)];
                //diff_src[COFF(c)] = static_cast<data_t>(A[c-clo] - B[c-clo]);
                // better (3 vld, 1 vsc) (14.3 ms)
                diff_src[COFF(c)] = static_cast<data_t>( A[c-clo]
                        - (2.0f * alpha * beta * inv_summands)
                        * B[c-clo] * src[COFF(c)] );
            }
        });
#undef COFF
#undef JUST_DENSE
#undef SRCGT
#undef USE_SRCGT
    }
#endif
#if 1
    //else if (1 && (tag==nchw || tag==nhwc) && across_channels)
    else if (1 && (tag==nChw8c || tag==nChw16c) && across_channels && half_size <= 127)
    {
        assert(formula);
        // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=BWD_D ic2002ih10
        // bwd perf: 0.558 0.641 0.571 0.585
        // --tag=nchw --alg=ACROSS --dir=BWD_D ic32777ih10 (14.23 ms)
        // --tag=nchw --alg=ACROSS --dir=BWD_D ic234567ih10 (117.0 ms)
        // perf: 98.8% vec, <VL>=251
        // THIS BLOCK: 0.555 0.542 ms  ** maybe ** ~ 7% speedup
        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                [&](dim_t const mb, dim_t const c_blk, dim_t const d, dim_t const h, dim_t const w) {
            dim_t const clo = c_blk * blksz;
            dim_t const chi = nstl::min(clo + blksz, C);
            dim_t const cspan = chi - clo;
            // comp-time array bound [vs cspan] as ++ vectorization workaround
            acc_data_t A[stack_channels + 2*127];
            acc_data_t B[stack_channels + 2*127];
            for(dim_t c=0; c<cspan; ++c) {
                A[c] = 0;
                B[c] = 0;
            }
            dim_t clolo = nstl::max(clo - half_size, dim_t{0});
            dim_t chihi = nstl::min(chi + half_size + 1, C);
            dim_t clololo = nstl::max(clolo - half_size, dim_t{0});

#define JUST_DENSE 0
#define USE_SRCGT 1
            // ic32777ih10 ic3000ih10
            // USE_SRCGT 0 : 7.78 0.817 ms  OHOH nchw times were **12.42** 0.702 ms?
            // USE_SRCGT 1 : 7.23 0.775 ms
#if JUST_DENSE
            dim_t c_off0 = offset<tag>(data_d, mb,stride_mb, 0,C, 0,D, h,H, w,W );
            dim_t const c_stride = (tag==nhwc? dim_t{1}: dim_t{H*W});
#define COFF(c) (c_off0 + (c) * c_stride)
#if USE_SRCGT
            // slowdown for nhwc? (check)
            dim_t const chihihi = nstl::min(chihi + half_size + 1, C);
            acc_data_t srcgt[chihihi - clololo]; // src gathered
            {
                for(size_t c=clololo; c<chihihi; ++c)
                    srcgt[c-clololo] = src[chanoff[c-clololo]];
            }
#endif
#else
            dim_t const chihihi = nstl::min(chihi + half_size + 1, C);
            // slowdown for nchw, (maybe not, for nhwc (CHECKME))
            //size_t chanoff[chihihi - clololo];
            size_t chanoff[stack_channels + 4*127];
#if USE_SRCGT
            acc_data_t srcgt[chihihi - clololo]; // src gathered; need [clolo,chihi)
#endif
            {
                //if(formula) {
                    for(size_t c=clololo; c<chihihi; ++c){
                        chanoff[c-clololo] = offset<tag>(data_d, mb,stride_mb,
                                c,C, d,D, h,H, w,W );
#if USE_SRCGT
                        srcgt[c-clololo] = src[chanoff[c-clololo]];
#endif
                    }
                //}
                //else{ // vec_off_v phys offset calc in simd-length chunks
                //    channel_offsets(data_opt, chanoff, mb, clololo, chihihi, d, h, w);
                //}
                //  opt attempt: src is VGT twice --> do that once...
            }
#define COFF(c) (chanoff[ (c) - clololo ])
#endif // JUST_DENSE

#if USE_SRCGT
#define SRCGT(c) srcgt[(c)-clololo]
#else
#define SRCGT(c) src[COFF(c)]
#endif
            float const inv_summands = 1.0f / static_cast<float>(summands);
            {
                //acc_data_t sum[chihi-clolo]; // rt dim
                acc_data_t sum[stack_channels + 2*127]; // vec workaround
                for (dim_t c=clolo; c<chihi; ++c) sum[c-clolo] = 0;
                NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                    dim_t const oc_lo = (0-l < clolo? clolo: 0-l);
                    dim_t const oc_hi = (C-l > chihi? chihi: C-l);
                    PragmaQuote(_NEC shortloop_reduction)//;
                    for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                        sum[c-clolo] += SQUARE( acc_data_t{SRCGT(c+l)} );
                    }
                }
                PragmaQuote(_NEC shortloop_reduction)//;
                for (dim_t c=clolo; c<chihi; ++c) { // c ~ central channel
                    sum[c-clolo] = k + (alpha * inv_summands) * sum[c-clolo];
                }
                // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=BWD_D ic2002ih10
                // --tag=nchw --alg=ACROSS --dir=BWD_D ic32777ih10 (14.23 ms)
                // perf: 98.8% vec, <VL>=251
                NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                    dim_t const oc_lo = (0-l < clo? clo: 0-l);
                    dim_t const oc_hi = (C-l > chi? chi: C-l);
                    PragmaQuote(_NEC shortloop_reduction)//;
                    for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                        //const auto off = COFF(c+l);
                        const acc_data_t omega = sum[c+l-clolo];
#if 0
                        const acc_data_t omega_in_beta
                            = fast_negative_powf(omega, beta);
                        const acc_data_t tmp
                            = omega_in_beta * (acc_data_t)diff_dst[COFF(c+l)];
#else
                        const acc_data_t tmp
                            = fast_negative_powf(omega, beta)
                            * (acc_data_t)diff_dst[COFF(c+l)];
#endif
                        if (l == 0) A[c-clo] = tmp;
                        B[c-clo] += (SRCGT(c+l) * tmp / omega);
                    }
                }
            }
            for(dim_t c=clo; c<chi; ++c) { // c ~ central channel
                // vsc and vgt ... (14.6 ms)
                //B[c-clo] *= (2.0f * alpha * beta * inv_summands) * src[COFF(c)];
                //diff_src[COFF(c)] = static_cast<data_t>(A[c-clo] - B[c-clo]);
                // better (3 vld, 1 vsc) (14.3 ms)
                diff_src[COFF(c)] = static_cast<data_t>( A[c-clo]
                        - (2.0f * alpha * beta * inv_summands)
                        * B[c-clo] * SRCGT(c)
                        );
            }
        });
#undef COFF
#undef JUST_DENSE
#undef USE_SRCGT
#undef SRCGT
    }
#endif
#if 1
    else if (1 && across_channels && half_size <= 127)
    {
        // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=BWD_D ic2002ih10
        // bwd perf: 0.558 0.641 0.571 0.585
        // --tag=nchw --alg=ACROSS --dir=BWD_D ic32777ih10 (14.23 ms)
        // --tag=nchw --alg=ACROSS --dir=BWD_D ic234567ih10 (117.0 ms)
        // --tag=ncdhw --alg=ACROSS --dir=BWD_D ic2002id10 (fwd~15.7 bwd:7.28 ms)
        // NOTE fwd is a "subset" of bwd -- why is it 2x slower?
        //
        //orig remeasure 7.41 ms
        // mod: gather-once: ncdhw 7.10 ms.  extra stack space for 4% speedup
        //    NOT WORTH gather-once, it seems (removed)
        //
        // perf: 98.8% vec, <VL>=251
        // dev version cleanup (again)
        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                [&](dim_t const mb, dim_t const c_blk, dim_t const d, dim_t const h, dim_t const w) {
            dim_t const clo = c_blk * blksz;
            dim_t const chi = nstl::min(clo + blksz, C);
            dim_t const cspan = chi - clo;
            // comp-time array bound [vs cspan] as ++ vectorization workaround
            acc_data_t A[stack_channels + 2*127];
            acc_data_t B[stack_channels + 2*127];
            for(dim_t c=0; c<cspan; ++c) {
                A[c] = 0;
                B[c] = 0;
            }
            dim_t clolo = nstl::max(clo - half_size, dim_t{0});
            dim_t chihi = nstl::min(chi + half_size + 1, C);
            dim_t clololo = nstl::max(clolo - half_size, dim_t{0});

            //size_t chanoff[chihi - clolo]; // slowdown for nchw (check!)
            size_t chanoff[stack_channels + 4*127];
            dim_t const chihihi = nstl::min(chihi + half_size + 1, C);
            //acc_data_t srcgt[chihihi - clololo]; // src gathered; need [clolo,chihi)
            {
                if(formula) {
                    for(size_t c=clololo; c<chihihi; ++c){
                        chanoff[c-clololo] = offset<tag>(data_d, mb,stride_mb,
                                c,C, d,D, h,H, w,W );
                        //srcgt[c-clololo] = src[chanoff[c-clololo]];
                    }
                }else{ // vec_off_v phys offset calc in simd-length chunks
                    channel_offsets(data_opt, chanoff, mb, clololo, chihihi, d, h, w);
                    //for(size_t c=clololo; c<chihihi; ++c){
                    //    srcgt[c-clololo] = src[chanoff[c-clololo]];
                    //}
                }
            }
#define COFF(c) (chanoff[ (c) - clololo ])
            float const inv_summands = 1.0f / static_cast<float>(summands);
            {
                //acc_data_t sum[chihi-clolo]; // rt dim
                acc_data_t sum[stack_channels + 2*127]; // vec workaround
                for (dim_t c=clolo; c<chihi; ++c) sum[c-clolo] = 0;
                NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                    dim_t const oc_lo = (0-l < clolo? clolo: 0-l);
                    dim_t const oc_hi = (C-l > chihi? chihi: C-l);
                    PragmaQuote(_NEC shortloop_reduction)//;
                    for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                        sum[c-clolo] += SQUARE( acc_data_t{src[COFF(c+l)]} );
                        //sum[c-clolo] += SQUARE( srcgt[c+l-clololo] );
                    }
                }
                PragmaQuote(_NEC shortloop_reduction)//;
                for (dim_t c=clolo; c<chihi; ++c) { // c ~ central channel
                    sum[c-clolo] = k + (alpha * inv_summands) * sum[c-clolo];
                }
                // --tag=nchw,nhwc,nChw8c,nChw16c --alg=ACROSS --dir=BWD_D ic2002ih10
                // --tag=nchw --alg=ACROSS --dir=BWD_D ic32777ih10 (14.23 ms)
                // perf: 98.8% vec, <VL>=251
                NOVEC for_(dim_t l = 0-half_size; l <= 0+half_size; ++l) {
                    dim_t const oc_lo = (0-l < clo? clo: 0-l);
                    dim_t const oc_hi = (C-l > chi? chi: C-l);
                    PragmaQuote(_NEC shortloop_reduction)//;
                    for(dim_t c=oc_lo; c<oc_hi; ++c) {   // central chan
                        const auto off = COFF(c+l);
                        const acc_data_t omega = sum[c+l-clolo];
                        const acc_data_t tmp = fast_negative_powf(omega, beta)
                            * (acc_data_t)diff_dst[off];
                        if (l == 0) A[c-clo] = tmp;
                        B[c-clo] += (src[off] * tmp / omega);
                        //B[c-clo] += (srcgt[c+l-clololo] * tmp / omega);
                    }
                }
            }
            for(dim_t c=clo; c<chi; ++c) { // c ~ central channel
                // vsc and vgt ... (14.6 ms)
                //B[c-clo] *= (2.0f * alpha * beta * inv_summands) * src[COFF(c)];
                //diff_src[COFF(c)] = static_cast<data_t>(A[c-clo] - B[c-clo]);
                // slightly better (3 vld, 1 vsc) (14.3 ms)
                diff_src[COFF(c)] = static_cast<data_t>( A[c-clo]
                        - (2.0f * alpha * beta * inv_summands)
                        * B[c-clo] * src[COFF(c)]
                        //* B[c-clo] * srcgt[c-clololo]
                        );
            }
        });
#undef COFF
    }
#endif
#if 1 // cleaned up
    else if (tag==nchw && !across_channels) { // i.e. within channels, lrn window on d,h,w
        typedef CoordsForNd<6,uint64_t,uint64_t> Coords;
        float const alpha_inv_summands = static_cast<float>(alpha) / static_cast<float>(summands);
        // --lrn --dir=BWD_D --tag=nchw --alg=WITHIN ic202ih10 ic2002ih10 ic32777ih10
        // below version: 84  819 13403 ms
        // 0 : 85 819 13403 ms
        // 1 : see below
        int32_t sz = 2*half_size+1;
        // lrn off at given n,c,h,w is central offset
        // lrn window offsets from this are in lrn_off[]
        // use mask since lrn window can go out of [0,H) [0,W)
        // lrn addresses for ++c all increment by H*W, with same mask.
        int32_t lrn_off0[/* sz * */ sz * sz]; // hw dims of nchw
        {
            int32_t *lrn_off = &lrn_off0[0];
            //for_(dim_t ld = d-half_size; ld < d+half_size+1; ++ld)
            for_(int32_t lh = 0-half_size; lh < 0+half_size+1; ++lh)
            for (int32_t lw = 0-half_size; lw < 0+half_size+1; ++lw) {
                *lrn_off++ = lh * W + lw;
            }
        }
        int32_t const __restrict__* lrn_off = &lrn_off0[0];

        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                [&](dim_t const mb, dim_t const c_blk, dim_t const od, dim_t const oh, dim_t const ow)
        {
            dim_t clo = c_blk * blksz;
            dim_t chi = nstl::min(clo + blksz, C);
            dim_t const cspan = chi - clo;
            // rough guess about threshold
            bool const big_cspan = cspan >= sz*sz/4 && cspan >= 32;
            // comp-time array bound [vs cspan] as ++ vectorization workaround
            //acc_data_t A[cspan], B[cspan];
            acc_data_t A[stack_channels], B[stack_channels];
            for(dim_t c=0; c<cspan; ++c) {
                A[c] = acc_data_t{0};
                B[c] = acc_data_t{0};
            }

            { // calc A[], B[]
                //DEFINE_HALFSIZE_RANGE(d_st, d_en, od, 0, D);
                DEFINE_HALFSIZE_RANGE(h_st, h_en, oh, 0, H);
                DEFINE_HALFSIZE_RANGE(w_st, w_en, ow, 0, W);
                //for_(dim_t d = d_st; d < d_en; ++d)
                for_(dim_t h = h_st; h < h_en; ++h)
                for (dim_t w = w_st; w < w_en; ++w) {
                    acc_data_t om[stack_channels]; // unvec if just cspan
                    for (dim_t c=0; c<cspan; ++c) om[c] = acc_data_t{0};
                    dim_t const chanoff0 = offset<tag>( data_d, mb,stride_mb,
                            0,C, 0,D, h,H, w,W ); // D==1
#define COFF(c) (chanoff0 + (c) * H*W)
                    if (big_cspan) { // [clo,chi) loop on inside
                        // ic3ih100, ic32,64,128,202ih10 (2002 32777)
                        //  1185, 9.1 9.8 10.5 11.1  (17.0 278)
                        //  so if (cspan >= sz*sz / 4) ? && cspan >= 32 ?
                        NOVEC for_(dim_t lh = h-half_size; lh < h+half_size+1; ++lh) {
                        if(lh >= 0 && lh < H) {
                        NOVEC for (dim_t lw = w-half_size; lw < w+half_size+1; ++lw) {
                        if(lw >= 0 && lw < W){
                            auto const dhw = (lh-(h-half_size))*sz + (lw-(w-half_size));
                            auto const lrn_off_dhw = lrn_off[dhw];
                            for_(dim_t oc=clo; oc<chi; ++oc) {
                                acc_data_t s = src[ COFF(oc) + lrn_off[dhw] ];
                                om[oc-clo] += SQUARE( acc_data_t{
                                        src[ COFF(oc) + lrn_off[dhw] ]});
                            }
                        }}}}
                    }else{
                        // ic3ih100, ic32,64,128,202ih10
                        // 382, 6.1 9.1 15.1 22.0   (191 3118)
                        //  so if (cspan < sz*sz / 4) || cspan < 32 ?
                        int lrn_off_ok[sz * sz];
                        {
                            auto *pok = &lrn_off_ok[0];
                            for_(dim_t lh = h-half_size; lh < h+half_size+1; ++lh)
                            for (dim_t lw = w-half_size; lw < w+half_size+1; ++lw)
                                *pok++ = (lh >= 0 && lh < H) && (lw >= 0 && lw < W);
                        }
                        // for oc here 383 22 191 3118
                        for (dim_t oc=clo; oc<chi; ++oc) {
                            for(dim_t dhw=0; dhw<sz*sz; ++dhw) {
                                if (lrn_off_ok[dhw]) {
                                    om[oc-clo] += SQUARE( acc_data_t{
                                             src[ COFF(oc) + lrn_off[dhw] ]});
                                }
                            }
                        }
                    }
                    bool const central = (/* d == od && */ h == oh && w == ow);
                    for (dim_t oc=clo; oc<chi; ++oc) {
                        const acc_data_t omega = k
                            + alpha_inv_summands * om[oc-clo];
                        const acc_data_t tmp = fast_negative_powf(omega, beta)
                            * (acc_data_t)diff_dst[COFF(oc)];
                        if (central) A[oc-clo] = tmp;
                        B[oc-clo] += (src[COFF(oc)] * tmp / omega);
                    }
                }
#undef COFF
            } // A[], B[] calculated
            { // nchw simplification
                dim_t const chanoff0 = offset<tag>( data_d, mb,stride_mb,
                        clo,C, 0,D, oh,H, ow,W ); // D==1
#define COFF(c) (chanoff0 + (c-clo) * H*W) /* nchw */
                for (dim_t oc=clo; oc<chi; ++oc) {
                    diff_src[COFF(oc)] = static_cast<data_t>(
                            A[oc-clo] - (2.0f * alpha_inv_summands * beta)
                            * B[oc-clo] * src[COFF(oc)] );
                }
#undef COFF
            }
        });
    }
#endif
#if 1
    else if (!across_channels) { // i.e. within channels, lrn window on d,h,w
        //printf("nchw-bwd-within generic\n");
        // cleanup code (not so much work optimizing yet)
        typedef CoordsForNd<6,uint64_t,uint64_t> Coords;
        float const alpha_inv_summands = static_cast<float>(alpha) / static_cast<float>(summands);
        // for func call versions... faster, but still sluggish
        // perhaps optimize "is_dense && no inner blocks && unpadded"? (like nchw/nhwc)
        auto get_omega_vec_clohi = [&](
                dim_t __restrict__ *central_off,
                acc_data_t __restrict__ *sum,
                dim_t const mb, dim_t const clo, dim_t const chi,
                dim_t const d, dim_t const h, dim_t const w) {
            for (dim_t c=clo; c<chi; ++c) sum[c-clo] = acc_data_t{0};
            Coords ov;
            dim_t central_pos = 0;
            if (ndims <= 2) {
                ov.init(mb,mb+1, 0,1);
            }else{
                DEFINE_HALFSIZE_RANGE(w_st, w_en, w, 0, W);
                if(ndims <= 3){
                    ov.init(mb,mb+1, 0,1, w_st,w_en);
                    central_pos = (w-w_st);
                }else{
                    DEFINE_HALFSIZE_RANGE(h_st, h_en, h, 0, H);
                    if(ndims <= 4){
                        ov.init(mb,mb+1, 0,1, h_st,h_en, w_st,w_en);
                        central_pos = (h-h_st)*(w_en-w_st) + (w-w_st);
                    }else{
                        DEFINE_HALFSIZE_RANGE(d_st, d_en, d, 0, D);
                        ov.init(mb,mb+1, 0,1, d_st,d_en, h_st,h_en, w_st,w_en);
                        central_pos = ((d-d_st)*(h_en-h_st) + (h-h_st))*(w_en-w_st) + (w-w_st);
                    }
                }
            }
            NOVEC for ( ; ov; ++ov) {
                const unsigned vl=ov.get_vl(); // vl ~ d,h,w coords
                auto const pos = ov.get_pos();
                bool has_central_pos = (pos <= central_pos && central_pos < vl);
                for(dim_t c=clo; c<chi; ++c)
                {
                    ShortLoop() for(unsigned i=0U; i<vl; ++i)
                        ov.vp[1][i] = c; // manual "channel" set
                    dim_t lrnp[MVL];
                    data_opt.vec_off_v(ov.base(), (dim_t *)&lrnp[0], vl);
                    ShortLoop() for(unsigned i=0U; i<vl; ++i) {
                        sum[c-clo] += SQUARE(acc_data_t{ src[lrnp[i]] });
                    }
                    if (has_central_pos) {
                        central_off[c-clo] = lrnp[central_pos-pos];
                    }
                }
                ShortLoop() for(unsigned i=0U; i<vl; ++i)
                    ov.vp[1][i] = 0; // restore "channel" for ++ov
            }
            for (dim_t c=clo; c<chi; ++c) {
                sum[c-clo] = k + alpha_inv_summands * sum[c-clo];
                //printf(" omega[%ld,%ld,%ld,%ld,%ld]=%f\n",mb,c,d,h,w,sum[c-clo]);
                //assert( sum[c-clo] == get_omega(mb,c,d,h,w) );
            }
        };
#define OM_INLINE 1
        // 0~215 ms, 1~86 ms (2.5x speedup)
#if !OM_INLINE
        // for formula versions (vectorizes well)
        auto get_omega_within = [&]( //dim_t *central_off,
                dim_t const mb, dim_t const oc, dim_t const od,
                dim_t const oh, dim_t const ow) {
            DEFINE_HALFSIZE_RANGE(d_st, d_en, od, 0, D);
            DEFINE_HALFSIZE_RANGE(h_st, h_en, oh, 0, H);
            DEFINE_HALFSIZE_RANGE(w_st, w_en, ow, 0, W);
            acc_data_t sum = 0;
            for_(dim_t d = d_st; d < d_en; ++d)
                for_(dim_t h = h_st; h < h_en; ++h)
                for (dim_t w = w_st; w < w_en; ++w) {
                    auto const off = offset<tag>( data_d, mb,stride_mb,
                            oc,C, d,D, h,H, w,W);
                    sum += SQUARE( acc_data_t{ src[off] } );
                    //if (d==od && h==oh && w==ow) *central_off = off;
                }
            return (acc_data_t)(k + alpha_inv_summands * sum);
        };
#endif

        dim_t const blksz = stack_friendly_blksz(C);
        parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                [&](dim_t const mb, dim_t const c_blk, dim_t const od, dim_t const oh, dim_t const ow)
        {
            dim_t clo = c_blk * blksz;
            dim_t chi = nstl::min(clo + blksz, C);
            dim_t const cspan = chi - clo;
            // comp-time array bound [vs cspan] as ++ vectorization workaround
            //acc_data_t A[cspan], B[cspan];
            acc_data_t A[stack_channels], B[stack_channels];
            for(dim_t c=0; c<cspan; ++c) {
                A[c] = acc_data_t{0};
                B[c] = acc_data_t{0};
            }

            {
                DEFINE_HALFSIZE_RANGE(d_st, d_en, od, 0, D);
                DEFINE_HALFSIZE_RANGE(h_st, h_en, oh, 0, H);
                DEFINE_HALFSIZE_RANGE(w_st, w_en, ow, 0, W);
                for_(dim_t d = d_st; d < d_en; ++d)
                for_(dim_t h = h_st; h < h_en; ++h)
                for (dim_t w = w_st; w < w_en; ++w) {
                    dim_t chanoff[cspan];
                    acc_data_t om[cspan];
                    if (formula) {
                        for (dim_t oc=clo; oc<chi; ++oc)
                            chanoff[oc-clo] = offset<tag>( data_d, mb,stride_mb,
                                    oc,C, d,D, h,H, w,W );
#if !OM_INLINE
                        for (dim_t oc=clo; oc<chi; ++oc) {
                            om[oc-clo] = get_omega_within(mb, oc, d, h, w);
                        }
#else
                        DEFINE_HALFSIZE_RANGE(ld_st, ld_en, d, 0, D);
                        DEFINE_HALFSIZE_RANGE(lh_st, lh_en, h, 0, H);
                        DEFINE_HALFSIZE_RANGE(lw_st, lw_en, w, 0, W);
                        for (dim_t oc=clo; oc<chi; ++oc) om[oc-clo] = acc_data_t{0};
                        if (tag == nchw) {
                            // oc loop here: 86 829 13569 ms ** best for nchw **
                            // ..nhwc..      119 1189 ms
                            // ..nChw8/16c   135/139 1482/1465 ms
                            for_(dim_t oc=clo; oc<chi; ++oc)
                            for_(dim_t ld = ld_st; ld < ld_en; ++ld)
                            for_(dim_t lh = lh_st; lh < lh_en; ++lh)
                            for (dim_t lw = lw_st; lw < lw_en; ++lw)
                                om[oc-clo] += SQUARE( acc_data_t{ src[
                                        offset<tag>( data_d, mb,stride_mb,
                                                oc,C, ld,D, lh,H, lw,W) ] } );
                        }else{ // nhwc or 8c/16c
                            // oc loop here: 155 2619 42825 ms
                            // ..nhwc..      11  23   ms            ** best for nhwc **
                            // ..nChw8/16c   13.1/13.3  52/57 ms
                            for_(dim_t ld = ld_st; ld < ld_en; ++ld)
                            for_(dim_t lh = lh_st; lh < lh_en; ++lh)
                            for_(dim_t lw = lw_st; lw < lw_en; ++lw)
                            for (dim_t oc=clo; oc<chi; ++oc)
                                om[oc-clo] += SQUARE( acc_data_t{ src[
                                        offset<tag>( data_d, mb,stride_mb,
                                                oc,C, ld,D, lh,H, lw,W) ] } );
                        }
                        for (dim_t oc=clo; oc<chi; ++oc) {
                            om[oc-clo] = k + alpha_inv_summands * om[oc-clo];
                        }
#endif
                    }else{ // this way, 13280 ms --> 330 ms
                        get_omega_vec_clohi(chanoff,om,mb,clo,chi,d,h,w);
                    }

                    bool const central = (d == od && h == oh && w == ow);
                    for (dim_t oc=clo; oc<chi; ++oc) {
                        const acc_data_t omega = om[oc-clo];
                        const acc_data_t tmp = fast_negative_powf(omega, beta)
                                * (acc_data_t)diff_dst[chanoff[oc-clo]];
                        if (central) A[oc-clo] = tmp;
                        B[oc-clo] += (src[chanoff[oc-clo]] * tmp / omega);
                    }
                }
                {
                    dim_t outoff[cspan];
                    if (formula) {
                        for (dim_t oc=clo; oc<chi; ++oc)
                            outoff[oc-clo] = offset<tag>( data_d, mb,stride_mb,
                                    oc,C, od,D, oh,H, ow,W );
                    }else{
                        Coords lw;
                        //channel_offsets(data_opt, outoff, mb, clo, chi, d, h, w);
                        if(ndims >= 5) lw.init(mb,mb+1, clo,chi, od,od+1, oh,oh+1, ow,ow+1);
                        else if(ndims >= 4) lw.init(mb,mb+1, clo,chi, oh,oh+1, ow,ow+1);
                        else if(ndims >= 3) lw.init(mb,mb+1, clo,chi, ow,ow+1);
                        else lw.init(mb,mb+1, clo,chi);
                        for( ; lw; ++lw){
                            data_opt.vec_off_v(
                                    lw.base(), // VecPos& vector register data
                                    (dim_t*)&outoff[lw.get_pos()], // outputs (seq)
                                    lw.get_vl(), // register len, num ouptuts
                                    false/*is_pos_padded*/);
                        }
                    }

                    for (dim_t oc=clo; oc<chi; ++oc) {
                        diff_src[outoff[oc-clo]] = static_cast<data_t>(
                                A[oc-clo] - (2.0f * alpha_inv_summands * beta)
                                * B[oc-clo] * src[outoff[oc-clo]] );
                    }
                }
            }
        });
    }
#endif

    // old methods =========================================================
    else{
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
        auto ker = [&](data_t * const d, dim_t const mb, dim_t const oc,
                dim_t const od, dim_t const oh, dim_t const ow) {
            acc_data_t A = 0, B = 0;
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
                    //printf(" omega[%ld,%ld,%ld,%ld,%ld]=%f\n",mb,oc,d,h,w,omega);
                    const acc_data_t omega_in_beta
                            = fast_negative_powf(omega, beta);
                    const acc_data_t tmp
                            = omega_in_beta * (acc_data_t)diff_dst[off];
                    if (d == od && h == oh && w == ow) {
                        A = tmp;
                        //printf(" ker A[%ld,%ld,%ld,%ld,%ld]=%f\n",mb,oc,od,oh,ow,double{tmp});
                    }
                    B += (src[off] * tmp / omega);
                }
                //printf(" ker B[%ld,%ld,%ld,%ld,%ld]=%f\n",mb,oc,od,oh,ow,double{B});
            }
            data_t central = src[offset<tag>(data_d, mb,stride_mb, oc,C, od,D, oh,H, ow,W)];
            B *= (2.0f * alpha * beta * central / summands);
            *d = static_cast<data_t>(A - B);
        };

        if (tag == nChw16c || tag == nChw8c) {
            parallel_nd(MB, utils::div_up(C, blksize), H, W,
                    [&](dim_t mb, dim_t c_blk, dim_t h, dim_t w)
            {
                dim_t c = c_blk * blksize;
                //const dim_t off = mb * stride_mb + c * H * W
                //        + (h * W + w) * blksize;
                const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, 0,1, h,H, w,W);
                PRAGMA_OMP_SIMD()
                for (dim_t cc = 0; cc < nstl::min(blksize, C - c); ++cc)
                    ker(&diff_src[off + cc], mb, c + cc, 0, h, w);
            });
        } else if (tag == nhwc || tag == nchw) {
#if 0
            parallel_nd(MB, C, H, W, [&](dim_t mb, dim_t c, dim_t h, dim_t w) {
                const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, 0,1, h,H, w,W);
                ker(&diff_src[off], mb, c, 0, h, w);
            });
#else
            dim_t const blksz = stack_friendly_blksz(C);
            parallel_nd(MB, utils::div_up(C, blksz), D, H, W,
                    [&](dim_t mb, dim_t c_blk, dim_t d, dim_t h, dim_t w)
            {
                dim_t clo = c_blk * blksz;
                dim_t chi = nstl::min(clo + blksz, C);
                for (dim_t c=clo; c<chi; ++c) {
                    const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, 0,1, h,H, w,W);
                    ker(&diff_src[off], mb, c, d, h, w);
                }
            });
#endif
        } else {
            parallel_nd(MB, C, D, H, W,
                    [&](dim_t mb, dim_t c, dim_t d, dim_t h, dim_t w)
            {
                const dim_t off = offset<tag>(data_d, mb,stride_mb, c,C, d,D, h,H, w,W);
                ker(&diff_src[off], mb, c, d, h, w);
            });
        }
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

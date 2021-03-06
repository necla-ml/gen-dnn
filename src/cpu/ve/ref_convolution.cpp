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

/** \file
 * ref:any fwd convolution
 */
// conv.in timings for ref:any of conv.in beginning at ref:any ih90 tests:
// 237.2 80.23 57.70 325.7 325.7 1.60
//
// older timings (dev codes)
// errors:
// 0 : rconv-0.log OK, mistrusted 3 of conv.in mode=C Real Time 17.934
// 1 : rconv-1.log seg fault in --conv g32ic32ih112oc32oh112kh3ph1n"mobilenet:conv2_1/dw"
// 1 : rconv-1.log OK RTime 15.98 s
// 2 : rconv-2.log OK 15.72 s
// 3 : rconv-3.log OK 15.43 s
// 4 : rconv-4.log OK 15.23 s
// 5 : rconv-5.log OK  3.52 s
// 6 : rconv-6.log OK  2.42 s
// conv-gemm.in
// gemm :       190,213,145,265 (191,194,120)
//   add --skip-impl=gemm to run ref impls?
//   NO. comment them out in cpu_convolution_list.cpp
//   Perhaps benchdnn should use iterator api to continue trying
//   to find a non-skipped impl.
//
//   Cannot skip gemm convolution and still run the ref impl (lower) !)
// conv.in (much smaller tests when no gemm impl present)
// -1 : 48.5984 75.4461 68.4999 107.548 31.2228 24.5472 25.612  24.4907 203.698
//  0 : 48.6771 84.7707 87.7413 108.005 40.8471 24.7904 25.6382 24.2934 215.264
//  1 : xx.5149 51.7053 43.977  73.167  39.4347 18.7586 18.9639 18.6456 132.379
//  2 : 32.7245 40.2169 38.188  63.3137 36.6195 16.373  16.5362 16.4141 102.588
//  3 : 31.4836 38.6193 31.9966 59.813  32.5218 15.7574 15.9223 15.7973 95.3827
//  --- add a FWD_D test
//  3 : 31.3331 38.3888 31.7553 59.4035 59.3386 32.1725 15.4924 15.4991 15.491 92.2239
//  4 : 30.8901 37.9487 31.4165 58.203 58.1731 32.5846 15.2864 15.2907 15.2818 89.9907
// 4 : 30.9838 39.6846 32.9677 58.3858 58.3249 32.5824 15.3221 15.3263 15.3218 89.7068
// fix test_convolution_forward_u8s8s32 and test_convolution_eltwise_forward_x8s8f32s32 bugs
// for "most advanced" of each impl ...
// ...........................................
//  3     : 41.21 47.58 39.59 76.27 76.19
//        : 14477 3208. 190.8 18254 18259 34.30 postops-plain: 20.47 20.47 20.47 120.1
//  4plain: 38.61 42.98 38.35  68.23 68.16
//     any: 13528 3081. 182.6 17049 17050 33.01 postops-plain: 19.09 19.11 19.10 103.5
//  5     : 41.06 46.42 39.19 75.12 75.06  3.38 postops-plain: 20.33 20.34 20.33 118.1
//     any: 377.6 168.4 144.3 524.2 524.1 
//  6plain: 42.34 50.19 44.38  79.06 79.03
//          179.1 92.30 72.30 256.04 255.92 (wcrd for inner)
//  6any  : 240.82 86.59 66.90 337.87 337.93 1.70 postops-plain 20.91 20.96 20.87 125.91
//        : 233.44 78.32 55.64 321.82 321.73 1.51 postops-plain 87.41 87.41 87.41,628.67
//   --> use wcrd only for generic kernel (vectorize offset calcs)
//       because ker_plain6 vectorizes well with nc++
//   --> revert to 4 for ker_plain? what is the main diff?
//6:-1 : 43.82 54.13 44.51 83.62  83.58
//  any: 248.5 85.25 58.90 341.5 332.45 1.63 postops-plain: 21.69 21.71 21.71 135.1
//6:0  : 76.53 78.86 60.23 140.6 140.5 
//  any: 233.7 79.04 57.25 322.4 322.2 1.52 postops-plain: 37.77 37.77 37.77 264.3
//6:1  : 173.1 96.14 77.00 252.1 251.1
//  any: 233.6 79.36 57.51 321.9 321.8 1.51 postops-plain 85.06 85.55 84.42 616.0
//6:2  : 181.4 100.6 78.92 259.2 250.2 259.2
//  any: 233.7 78.35 56.91 322.0 321.9 1.51 postops-plain 88.61 88.46 88.61 637.3
//  --> use 4 for plain, and 6:1 for small improvement of 'any'
//6:0 with unsigned IKLIMS macro: (above used div_floor)
//       40.78 45.75 38.11 74.46 74.41
//       235.9 81.27 57.63 325.6 325.6 1.57 postops-plain: 20.19 10.19 10.19 116.5
// Enabling more IKLIMS test cases, I find that NOT using the fn call leads to
// miscompilation !!!  Only hoist_ApiB(...) fn call avoided all segfaults.

// current conv.in timings
// plain: 37.77 40.37 34.07 66.81 66.76
//  any : 240.4 84.05 66.69 334.9 334.9 1.68 postops-plain 18.71 18.72 18.71 99.30

#include "cpu/ve/ref_convolution_util.hpp"
#include "cpu/ve/hoist.hpp"     // nc++: hoist linear conditions out of loops
#include <iostream> // tmp debug


#include "common/ve/memory_desc_wrapper_opt.hpp"

#define PRAG(...) PragmaQuote(_NEC __VA_ARGS__)
#define LISTVEC PRAG(list_vector)
#define VFOR(VAR,LIM) ShortLoop() for(int VAR = 0; VAR < LIM; ++VAR)

#if 0
static int verbose=0; // 0 = print all, increase to reduce verbosity
static int file_errors=0;
static bool chk( bool cond, char const* msg, char const* file, int const lineno ){
    if (!cond){
        printf("@@@ error: %s : [%s:%d]\n", msg, file, lineno);
        ++file_errors;
        //exit(1);
    }
    return cond;
}
static bool trivial( int const verb, bool const cond, char const* msg,
                             char const* file, int const lineno ){
        if (verb > verbose && cond){
                    printf("@@@ trivial: %s : [%s:%d]\n", msg, file, lineno); fflush(stdout);
                        }

            return cond;
}
//#define MUST( COND )
#define MUST( COND )    chk(    (COND), #COND, __PRETTY_FUNCTION__, __LINE__)
#define PRINTF(...)     do{ printf(__VA_ARGS__); fflush(stdout);}while(0)
#define TRIVIAL( COND ) COND
//#define TRIVIAL( COND ) trivial(1, (COND), #COND, __PRETTY_FUNCTION__, __LINE__)

#if defined(NDEBUG)
#define DPRINTF(...)
#define DMUST(...) 1
#else
#define DPRINTF(...)  do{printf(__VA_ARGS__); fflush(stdout);}while(0)
#define DMUST(...)    MUST(__VA_ARGS__)
#endif

inline ALWAYS_INLINE void hoist_ApiBx(
        int& ilo, int& ihi,                     // sub-for-loop outputs
        const int ibeg, const int iend,         // orig for(i=ibeg;i<iend;++i) <-- stride 1
        const int a,    const int b,            // linear fn o=a+ib
        const int obeg, const int oend)         // linear fn range [obeg,oend)
{
    // div_floor approach for int args, not the unsigned generalization
    int err = 0;
    if(!MUST( b > 0 )) ++err;
    ilo = div_floor( obeg -a+b-1, b );
    if(!MUST( a + (ilo-1) * b < obeg )) ++err;
    if(!MUST( a + (ilo  ) * b >= obeg )) ++err;
    ihi = div_floor( oend -a+b-1, b );
    if(!MUST( a + (ihi-1) * b < oend )) ++err;
    if(!MUST( a + (ihi  ) * b >= oend )) ++err;
    if(err) printf(" hoist err: ibeg,iend=%d,%d  a,b=%d,%d  obeg,oend=%d,%d  ilo=%d ihi=%d\n",
            ibeg,iend,a,b,obeg,oend,ilo,ihi);
    if( ilo < ibeg ) ilo = ibeg;
    else if( ilo > iend ) ilo = iend; // intentionally NOT enforced
    if( ihi >= iend ) ihi = iend;
    else if( ihi < ibeg ) ihi = ibeg; // intentionally NOT enforced
}
#define CONV_CHECK(EXPR) do {if (!(EXPR)) {printf(" FAILED: " #EXPR "\n");}} while(0)

// vector window-tile access macros
#define DHW_W 0
#define DHW_H 1
#define DHW_D 2
// which = 0,1,2 for w, h, d respectively
// following COULD be done reasonably, but nc++ precalculates
// most pointers, using mem load instead "lea" calc (could use single reg).
// so still fair amount of register spill,restore and mem loads
#define DHW_SHIFT(which,idx)    dhw[((which*3  )*MVL)+idx]
#define DHW_ST(which,idx)       dhw[((which*3+1)*MVL)+idx]
#define DHW_EN(which,idx)       dhw[((which*3+2)*MVL)+idx]

#define DEFINE_IN_RANGE_VEC(idx, which, ksz, isz) \
    DHW_ST(which,idx) = (DHW_SHIFT(which,idx)       >   0 \
            ? DHW_SHIFT(which,idx): 0); \
    DHW_EN(which,idx) = (DHW_SHIFT(which,idx) + ksz < isz \
            ? DHW_SHIFT(which,idx) + ksz: isz); \
    if (DHW_EN(which,idx) < DHW_ST(which,idx)) \
            DHW_EN(which,idx) = DHW_ST(which,idx)

// macro version of window_precalc
#define POOL_WINDOW_LOOP(d_spatial, dvl, STRIDE, PAD, KSZ, ISZ, p_off, p_st, p_en, ssz) do \
{ \
    ShortLoop() for(int i=0; i<dvl; ++i) { \
        p_off[i] = d_spatial[i]/*od or oh or ow*/ * STRIDE - PAD; \
        p_st[i] = (p_off[i] > 0? p_off[i]: 0); \
        p_en[i] = (p_off[i] + KSZ < ISZ? p_off[i] + KSZ: ISZ); \
        if (p_en[i] < p_st[i]) p_en[i] = p_st[i]; \
        ssz[i] *= p_en[i] - p_st[i]; \
    } \
} while(0)

// alternate precalc loop, that backward avg-pooling really prefers !

// expects SD,KD,ID,padF, etc int consts defined as usual
// dcrd info passed in as dm~dcrd.get_dim() dvl~dcrd.get_vl(),
// and d_spatial0 ~ (int32_t*)&dcrd.vp[2][0].
#define POOL_WINDOW_PRECALC(dm, dvl, d_spatial0, ssz, dhw) do \
{ \
    int const* d_spatial = d_spatial0; \
    int * p_off; /* input coord offset (from destination coord) */ \
    int * p_st; /* input coord start */ \
    int * p_en; /* input coord end, >= start*/ \
    ShortLoop() for(int i=0; i<dvl; ++i) \
        ssz[i] = 1; \
    if (dm >= 5) { \
        p_off = dhw + 6*MVL; \
        p_st  = dhw + 7*MVL; \
        p_en  = dhw + 8*MVL; \
        POOL_WINDOW_LOOP(d_spatial, dvl, SD, padF, KD, ID, \
                p_off, p_st, p_en, ssz); \
        d_spatial += MVL; /*dcrd.MaxVl*/ \
    } \
    if (dm >= 4) { \
        p_off = dhw + 3*MVL; \
        p_st  = dhw + 4*MVL; \
        p_en  = dhw + 5*MVL; \
        POOL_WINDOW_LOOP(d_spatial, dvl, SH, padT, KH, IH, \
                p_off, p_st, p_en, ssz); \
        d_spatial += MVL; /*dcrd.MaxVl*/ \
    } \
    p_off = dhw + 0*MVL; \
    p_st  = dhw + 1*MVL; \
    p_en  = dhw + 2*MVL; \
    POOL_WINDOW_LOOP(d_spatial, dvl, SW, padL, KW, IW, \
            p_off, p_st, p_en, ssz); \
} while(0)

// SCRD_LIMITS macro uses POOL_WINDOW_PRECALC constants to set up
// iterator limits for pooling window overlapped with src
//
// pack up the i'th overlapped pooling window limits into rlo,rhi vectors.
// mb_ptr ~ &dcrd.vp[0][0]
// dhw ~ precalc offset,start,end vector data for ovlp window limits
// ssz ~ precalc window num-elements
// dm ~ dimension (3,4,5)
// scrd ~ (output) initialized CONV WINDOW src-coordinate-iterator mb,icxg[,d[,h]],w
// wcrd ~ [g,]oc,ic[,kd,[,kh]],kw
#define SCRD_LIMITS(i, mb_ptr, dhw, ssz, dm, scrd) do \
{ \
    int32_t* slo = (int32_t*)scrd.raw_lo(); \
    int32_t* shi = (int32_t*)scrd.raw_hi(); \
    auto const* crds = &mb_ptr[i]; \
    slo[0] = *crds; /*MB*/ \
    shi[0] = *crds+1; \
    slo[1] = 0; \
    shi[1] = 1; /*placehoder for ic < ICxG*/ \
    auto const* precalc = &dhw[i]; /* for(w,h,d): shift, start ,end vectors*/ \
    if (dm >= 5) { \
        slo[2] = precalc[(DHW_D*3+1)*MVL];  \
        shi[2] = precalc[(DHW_D*3+2)*MVL]; \
        slo[3] = precalc[(DHW_H*3+1)*MVL]; \
        shi[3] = precalc[(DHW_H*3+2)*MVL]; \
        slo[4] = precalc[(DHW_W*3+1)*MVL]; \
        shi[4] = precalc[(DHW_W*3+2)*MVL]; \
    } else if (dm >= 4) { \
        slo[2] = precalc[(DHW_H*3+1)*MVL]; \
        shi[2] = precalc[(DHW_H*3+2)*MVL]; \
        slo[3] = precalc[(DHW_W*3+1)*MVL]; \
        shi[3] = precalc[(DHW_W*3+2)*MVL]; \
    } else { \
        slo[2] = precalc[(DHW_W*3+1)*MVL]; \
        shi[2] = precalc[(DHW_W*3+2)*MVL];  \
    } \
    *scrd.raw_sz() = ssz[i]; \
    *scrd.raw_dim() = dm; \
    scrd.init_nd(0); \
} while(0)
#endif

namespace dnnl {
namespace impl {
namespace cpu {

using math::get_bias;

//typedef CoordsForNd<6,uint64_t,uint64_t> Coords;
// let's use 32-bit Crd (Pos can still be u64)
// oh. Pos u64 stilll required by memory_desc_wrapper_opt

// 32-bit coordinate ranges, 64-bit logical/physical offsets, iterable
typedef CoordsForNd<6,uint32_t,int64_t> Coords32;
// src-pixel coords (mb,oc,id,ih,iw when they are valid)

// Sometimes just need raw coordinate memory.
// (generated from other Coords32 etc.).
typedef memory_desc_wrapper_opt::VecPos32 VecPos32;
// a.k.a. CoordRegs<uint32_t, 6>,
// cast to int32_t* sometimes avoids VE conversion nonsense

//namespace {

//}//anon::

template <data_type_t src_type, data_type_t wei_type, data_type_t dst_type,
        data_type_t acc_type>
void ref_convolution_fwd_t<src_type, wei_type, dst_type,
        acc_type>::execute_forward_any(const exec_ctx_t &ctx) const {
    //printf("\nFWD_IMPL=%d\n", (int)FWD_IMPL); fflush(stdout);
    auto src = CTX_IN_MEM(const src_data_t *, DNNL_ARG_SRC);
    auto weights = CTX_IN_MEM(const wei_data_t *, DNNL_ARG_WEIGHTS);
    auto bias = CTX_IN_MEM(const char *, DNNL_ARG_BIAS);
    auto dst = CTX_OUT_MEM(dst_data_t *, DNNL_ARG_DST);

    typedef typename ref_convolution_fwd_t<src_type, wei_type, dst_type,
            acc_type>::pd_t mypd_t;
    mypd_t const* mypd = pd();
    //assert( mypd->ker_type() >= 0 || mypd->ker_type() <= 1 ); // ker vs ker_plain
    assert( mypd->ker_type() == pd_t::any); // "ref:any"

    const memory_desc_wrapper src_d(mypd->src_md());
    const memory_desc_wrapper dst_d(mypd->dst_md());
    const memory_desc_wrapper weights_d(mypd->weights_md(0));
    const memory_desc_wrapper bias_d(mypd->weights_md(1));

    const bool with_groups = mypd->with_groups();

    const int G = mypd->G();
    const int MB = mypd->MB();
    const int OD = mypd->OD();
    const int OH = mypd->OH();
    const int OW = mypd->OW();
    const int ID = mypd->ID();
    const int IH = mypd->IH();
    const int IW = mypd->IW();

    const int OCxG = mypd->OC();        // all channels
    const int ICxG = mypd->IC();
    const int OC = OCxG / G;            // channels per group
    const int IC = ICxG / G;
    //printf(" g%dic%doc%d IC,OC(per group)=%d,%d\n", G,ICxG,OCxG, IC,OC);
    const int KD = mypd->KD();
    const int KH = mypd->KH();
    const int KW = mypd->KW();

    const int KSD = mypd->KSD();
    const int KSH = mypd->KSH();
    const int KSW = mypd->KSW();

    const int KDD = mypd->KDD() + 1;
    const int KDH = mypd->KDH() + 1;
    const int KDW = mypd->KDW() + 1;

    const int padFront = mypd->padFront();
    const int padT = mypd->padT();
    const int padL = mypd->padL();

    const int ndims = mypd->desc()->src_desc.ndims;

    using namespace data_type;
    bool constexpr is_int_conv = utils::one_of(src_type, s32, s8, u8);

    // scale_idx_mult = 1 for per_oc scales and 0, otherwise
    const int scale_idx_mult
            = mypd->attr()->output_scales_.mask_ == (1 << 1);
    const float *scales = mypd->attr()->output_scales_.scales_;

    const post_ops_t& ops = mypd->attr()->post_ops_;

    auto maybe_postops_vec3 = [&](float *a, float const *dst_float, int const dvl) {
        NOVEC //;PRAG(unroll(dnnl_post_ops::capacity))//;
        for (int idx = 0; idx < ops.len_; ++idx) {
            const auto &e = ops.entry_[idx];
            if (e.kind == dnnl_sum) {
                VFOR(i,dvl) {
                    a[i] += e.sum.scale * dst_float[i];
                }
            } else {
#if 0 //orig
                VFOR(i,dvl) {
                    a[i] = eltwises_[idx]->compute_scalar(a[i]);
                }
#else
                //if BUGS, be aware that nc++ optimization may "cheat" on extreme values
                // last such: check swish input -alpha*x > log_float_max
                eltwises_[idx]->compute_vec_reg(a, a, dvl);
                //using cvt = Cvt<data_t, is_int_dt>; // postops are pure-float!
#endif
            }
        }
    };

    // Sum and post ops:

#if 0
    // make offset calls "look the same". We suffer a fn call anyway for the offset.
    auto off_abxg = (with_groups
            ? (ndims == 5? offg5d: ndims == 4? offg4d:
                ndims == 3? offg3d: oops)
            : (ndims == 5? off5d: ndims == 4? off4d:
                ndims == 3? off3d: oops));
    auto off_abx = (ndims == 5? off5d: ndims == 4? off4d:
                ndims == 3? off3d: oops);

    // help compiler optimize the code
    // constants for plain layouts kernel
    const dnnl_dims_t &src_str = src_d.blocking_desc().strides;
    const dim_t src_ic_stride = src_str[1];
    const dim_t src_id_stride = (ndims == 5) ? src_str[2] : 0;
    const dim_t src_ih_stride = (ndims >= 4) ? src_str[ndims - 2] : 0;
    const dim_t src_iw_stride = (ndims >= 3) ? src_str[ndims - 1] : 0;
    const dnnl_dims_t &weights_str = weights_d.blocking_desc().strides;
    const int gr_shift = with_groups ? 1 : 0;
    const dim_t weights_ic_stride = weights_str[1 + gr_shift];
    const dim_t weights_kd_stride
            = (ndims == 5) ? weights_str[2 + gr_shift] : 0;
    const dim_t weights_kh_stride
            = (ndims >= 4) ? weights_str[ndims - 2 + gr_shift] : 0;
    const dim_t weights_kw_stride
            = (ndims >= 3) ? weights_str[ndims - 1 + gr_shift] : 0;
#endif

    // common "iterator" setup
    // 32-bit coordinate ranges, 64-bit logical/physical offsets
    auto elems = (size_t)MB * OCxG * OD * OH * OW;
    auto dst_dopt = memory_desc_wrapper_opt(dst_d.md_);
    auto bias_dopt = memory_desc_wrapper_opt(
            bias? bias_d.md_: dst_d.md_/*useless*/);
    auto src_dopt = memory_desc_wrapper_opt(src_d.md_);
    auto weights_dopt = memory_desc_wrapper_opt(weights_d.md_);
    auto const bias_data_type = mypd->desc()->bias_desc.data_type;
    if (bias) {
        assert( bias_data_type == data_type::s8
                || bias_data_type == data_type::u8
                || bias_data_type == data_type::s32
                || bias_data_type == data_type::f32);
        assert( bias_d.ndims() == 1 );
    } else {
        assert( bias_data_type == data_type::undef );
    }

#define OPT6 0
#define LIM6 0 // -1,0 OK, but 1,2,3 (1,2 check out equiv during -1) may segfault
    // my guess is that this is a miscompilation with nc++-3.0.27

#if LIM6==0 // div_floor "old faithful" avoids segfault of inline version of this macro.
#define IKLIMS(ocrd, STRIDE, PAD, DILATION, KSZ, ISZ, k_st, k_en, i_0) do \
            { \
                int const A = (int)ocrd * (STRIDE) - (PAD); \
                int const B = (int)(DILATION); \
                assert( B > 0 ); \
                hoist_ApiB( k_st, k_en, \
                        /*kh in   */ 0, KSZ, \
                        /*ih=A+kB */ A, B, \
                        /*ih in   */ 0, ISZ); \
                /*if (k_st > k_en) k_st = k_en;*/ \
                i_0 = A; \
                if (k_en < k_st) k_en = k_st; \
            } while(0)
#elif LIM6==1 // signed integer simplification (ranges [0,KSZ) [0,ISZ) positive)
#define IKLIMS(ocrd, STRIDE, DILATION, PAD, KSZ, ISZ, k_st, k_en, i_0) do \
            { \
                i_0 = (int)ocrd * (STRIDE) - (PAD); \
                k_en = DILATION - 1 - i_0; \
                k_st = k_en / DILATION; \
                /* */ \
                /* nc++ is NOT using cms opcode !!! */ \
                /*if (k_st < 0) k_st = 0;*/ \
                k_st = ((int)(k_st) < int{0}? int{0}: (int)k_st); \
                /*k_st = (k_st >= 0? k_st: 0);*/ \
                /*even worse: k_st = nstl::max(0,k_st);*/ \
                /* */ \
                /*not needed: if (k_st > KSZ) k_st = KSZ;*/ \
                k_en = (k_en + ISZ) / DILATION; \
                /*if (k_en > KSZ) k_en = KSZ;*/ \
                k_en = (k_en > KSZ? KSZ: k_en); \
                if (k_en < k_st) k_en = k_st; \
            } while(0)
#elif LIM6==20 // signed integer simplification
#define IKLIMS(ocrd, STRIDE, DILATION, PAD, KSZ, ISZ, k_st, k_en, i_0) do \
            { \
                int const A = (int)ocrd * (STRIDE) - (PAD); \
                int const B = (int)(DILATION); \
                int kk_en = B - A - 1; \
                int kk_st = kk_en / B; \
                kk_st = (kk_st < 0? 0: kk_st); \
                kk_en = (kk_en + (int)(ISZ)) / B; \
                kk_en = (kk_en > (int)(KSZ)? (int)(KSZ) \
                        : kk_en < kk_st? kk_st \
                        : kk_en); \
                k_st = kk_st; \
                k_en = kk_en; \
                i_0 = A; \
            } while(0)
#elif LIM6==2 // signed integer simplification
#define IKLIMS(ocrd, STRIDE, DILATION, PAD, KSZ, ISZ, k_st, k_en, i_0) do \
            { \
                int const A = (int)ocrd * (STRIDE) - (PAD); \
                int const B = (int)(DILATION); \
                int const x = B - A - 1; \
                int const y = x / B; \
                int const z = (x + ISZ) / B; \
                k_st = (y < 0? 0: y); \
                k_en = (z > KSZ? KSZ \
                        : z < k_st? k_st \
                        : z); \
                i_0 = A; \
                asm("###"); \
                assert( k_st <= k_en ); \
            } while(0)
#elif LIM6==3 // unsigned-safe macro version (BUGGY) (XXX should fix, someday)
#define IKLIMS(ocrd, STRIDE, DILATION, PAD, KSZ, ISZ, k_st, k_en, i_st) do \
            { \
                k_st = (ocrd * STRIDE < PAD \
                        ? (DILATION - 1 + (PAD - ocrd*STRIDE)) / DILATION \
                        : 0); \
                i_st = ocrd * STRIDE - PAD + /* k_st=0 * DILATION */ ; \
                int iLast = ISZ; \
                k_en = (ocrd * STRIDE + KSZ * DILATION < iLast + PAD + DILATION ? KSZ \
                        : (ocrd * STRIDE >= iLast + PAD ? 0 \
                            : ((iLast + PAD - ocrd * STRIDE) + DILATION - 1) / DILATION)); \
                if (k_en < k_st) k_en = k_st; \
            } while(0)
#else // div_floor function, with debug alt. version
    // although kk_st,kk_en check out as OK, inlining that method leads to segfaults !!!
#define IKLIMS(ocrd, STRIDE, PAD, DILATION, KSZ, ISZ, k_st, k_en, i_0) do \
            { \
                int const A = (int)ocrd * (STRIDE) - (PAD); \
                int const B = (int)(DILATION); \
                assert( B > 0 ); \
                hoist_ApiB( k_st, k_en, \
                        /*kh in   */ 0, KSZ, \
                        /*ih=A+kB */ A, B, \
                        /*ih in   */ 0, ISZ); \
                if (k_en < k_st) k_en = k_st; \
                i_0 = A; \
                /* verify new alg */ \
                int kk_en = DILATION - A - 1; \
                int kk_st = kk_en / DILATION; \
                kk_st = (kk_st < 0? 0: kk_st); \
                int iLast = ISZ; \
                kk_en = (kk_en + iLast) / DILATION; \
                kk_en = (kk_en > KSZ? KSZ: kk_en); \
                if (kk_en < kk_st) kk_en = kk_st; \
                /*if (kk_en < kk_st) kk_en = kk_st;*/ \
                /* kk_en will not agree sometimes when k_en < 0 */ \
                if ((kk_st != k_st) || (kk_en != k_en)) { \
                    printf(" ocrd,S,P,D, K,I=%d,%d,%d,%d %d,%d A=%d B=%d k[%d,%d) kk[%d,%d)\n", \
                            (int)(ocrd),(int)(STRIDE),(int)(PAD),(int)(DILATION), (int)(KSZ),(int)(ISZ), \
                            A,B, (int)(k_st),(int)(k_en), kk_st,kk_en); \
                    exit(1); \
                } \
                if (k_en < k_st) k_en = k_st; \
            } while(0)
            // ih0 = A + k0*B --> k0 = div_floor(ih0-A +B-1, B)
#endif

    auto ker6 = [=](int g, int mb, int oc, int od, int oh, int ow) {
        acc_data_t d = 0;

        // macro based on ve/hoist.hpp derivations
        // unlike pooling, we now support dilation
        int kd_st=0, kd_en=1, id_0=0; // id_0 a.k.a A or ocrd*STRIDE-PAD
        int kh_st=0, kh_en=1, ih_0=0;
        int kw_st=0, kw_en=1, iw_0=0;
        
        Coords32::pos_t sz = Coords32::pos_t{1};
        if (ndims >= 5) {
            IKLIMS(od, KSD, padFront, KDD, KD, ID, kd_st, kd_en, id_0);
            sz *= kd_en - kd_st;
        }
        if (ndims >= 4) {
            IKLIMS(oh, KSH, padT    , KDH, KH, IH, kh_st, kh_en, ih_0);
            sz *= kh_en - kh_st;
        }
        if (1) { //ndims >= 3
            IKLIMS(ow, KSW, padL    , KDW, KW, IW, kw_st, kw_en, iw_0);
            sz *= kw_en - kw_st;
        }
        if (sz == 0) // no iterations in kernel weight loop?
            return d;

        auto const dm = dst_d.ndims(); // MB, OCxG, OD?, OH?, OW
        assert( weights_d.ndims() == dm + (with_groups? 1: 0) );
        //       dcrd  // mb, OCxG, [[od,] oh,] ow
        Coords32 wcrd; // [g,] oc, ic, [[kd,] kh,] kw
        {
            auto * rlo = wcrd.raw_lo();
            auto * rhi = wcrd.raw_hi();
            // XXX move out, set g,oc,IC ranges just once (const)
            int nd=0;
            if (with_groups) {
                *rlo++ = g;
                *rhi++ = g+1;
                ++nd;
            }
            *rlo++ = oc;
            *rhi++ = oc+1;
            ++nd;
            *rlo++ = 0;
            *rhi++ = IC;
            ++nd;
            sz *= IC;
            if (dm >= 5) {
                *rlo++ = kd_st;
                *rhi++ = kd_en;
                //sz *= kd_en - kd_st;
                ++nd;
            }
            if (dm >= 4) {
                *rlo++ = kh_st;
                *rhi++ = kh_en;
                //sz *= kh_en - kh_st;
                ++nd;
            }
            if (1) {
                *rlo++ = kw_st;
                *rhi++ = kw_en;
                //sz *= kw_en - kw_st;
                ++nd;
            }
            *wcrd.raw_sz() = sz;
            *wcrd.raw_dim() = weights_d.ndims();
            //if(0){ std::cout<<wcrd.lim_str("wcrd")<<std::endl; }
            assert( nd == wcrd.get_dim() );
            wcrd.init_nd(0);
        }

        // move out XXX and set mb just once
        VecPos32 svp; //mb, g*IC+ic, id,ih,iw linear function of kd,kh,kw
        {
            int const wvl = wcrd.get_vl(); // may only decrease later
            VFOR(w,wvl) svp.vp[0][w] = mb; // this dim remains const
        }

        //int const* restrict const vw = &wcrd.vp[0][0]; // XXX
        // saw segfault next line:
        for( ; wcrd; ++wcrd) // wcrd ~ [g,]oc,ic[,kd,[,kh]],kw
        {
            //std::cout<<wcrd.coord_str("wcrd")<<std::endl;
            int const wvl = wcrd.get_vl();
            VFOR(w,wvl) { //vec : wcrd --> svp src coords
                //svp.vp[0][i] = mb; // this dim remains const
                int wdim=0;
                if (with_groups) { // g * IC + ic
                    svp.vp[1][w] = wcrd.vp[0][w] * IC + wcrd.vp[2][w];
                    wdim=3;
                }else{
                    svp.vp[1][w] = wcrd.vp[1][w];
                    wdim=2;
                }
                int sdim=2;
                if (dm >= 5) {
                    svp.vp[sdim][w] = id_0 + wcrd.vp[wdim][w] * KDD;
                    ++sdim; ++wdim;
                }
                if (dm >= 4) {
                    svp.vp[sdim][w] = ih_0 + wcrd.vp[wdim][w] * KDH;
                    ++sdim; ++wdim;
                }
                if (1 /*dm >= 3*/) {
                    svp.vp[sdim][w] = iw_0 + wcrd.vp[wdim][w] * KDW;
                    ++sdim; ++wdim;
                }
            }

            dim_t wei_off[MVL];
            dim_t src_off[MVL];
            // vtmp: wcrd and svp are OK to clobber during wei/src_off calc
            weights_dopt.vec_off_vtmp(wcrd.base(), &wei_off[0], wvl, false/*pad*/);
            src_dopt.vec_off_vtmp(svp, &src_off[0], wvl, false/*pad*/);
            VFOR(w, wvl) { //vec
                acc_data_t const ss = src[src_off[w]];
                acc_data_t const ww = weights[wei_off[w]];
                d += ss * ww;
            }
        }
        return d;
    };

#undef IKLIMS

    auto kern6 = [&](int ithr, int nthr) {
        size_t start, end;
        balance211(elems, nthr, ithr, start, end);
        auto const dm = dst_d.ndims(); // MB, OCxG, OD?, OH?, OW
        assert( dm >= 3 && dm <= 5 );

        Coords32 dcrd(dst_dopt.dims(), dm, start, end);
        // avoid mixing unsigned and int ops VE
        int const* const restrict dcrd_outer0 = reinterpret_cast<int const*>
                (&dcrd.vp[0][0]); 

        int dim_zeros[MVL]; // Coords32, so int loop index OK
        {
            // dcrd.get_vl() never increases, so can init outside dcrd-loop
            int dvl=dcrd.get_vl();
            VFOR(i,dvl) dim_zeros[i] = 0;
        }


        NOVEC for ( ; dcrd; ++dcrd) { // in vec-length chunks of dst coords
            int const dvl = dcrd.get_vl();
            dim_t dst_off[MVL]; // vectorized phys-offset calc
            float dst_gather[MVL]; // gather dst values (MIGHT be used for post_ops)
            dst_dopt.vec_off_v(dcrd.base(), &dst_off[0], dvl, false/*pad*/);
            VFOR(i,dvl) dst_gather[i] = dst[dst_off[i]];

            typedef int const *coord_register_t; // VE: avoid mix signed/unsigned
#define COORD_VEC_REGISTER(VAR, ...) coord_register_t VAR = (__VA_ARGS__)
            auto dcrd_i = dcrd_outer0 + MVL; // also 1 dim before spatial coords
            COORD_VEC_REGISTER(v_ocxg, dcrd_outer0 + MVL);

            float a[MVL]; // accumulator
            if (bias) {
                // bias "coord" is the 1-D set of dcrd.vp[1][i] "OCxG" values
                // low-level offsets from logical bias coords in a VecPos32
                VecPos32 bias_vp; 
                dim_t bias_off[MVL];
                VFOR(i,dvl) bias_vp.vp[0][i] = v_ocxg[i];
                bias_dopt.vec_off_vtmp(bias_vp, &bias_off[0],
                        dvl, false/*pad*/);
                // bias is const char* : coerce the gather & cvt to float a[]
#define CASE(dt) case dt: VFOR(i,dvl) a[i] = (float)((const prec_traits<dt>::type *)bias)[bias_off[i]]; break
                switch(bias_data_type) {
                    CASE(data_type::s8);
                    CASE(data_type::u8);
                    CASE(data_type::s32);
                    CASE(data_type::f32);
                }
#undef CASE
            } else {
                VFOR(i,dvl) a[i] = 0.f;
            }

#if 0
            // spatial coords of src, dst as raw int pointers
            //      VE mixed ops w/ unsigned and signed are clunky
            static_assert( sizeof(dcrd.vp[dm-1][0]) == sizeof(int),
                    "require VecPos32");
            int const* const mb_ptr = reinterpret_cast<int const*>
                    (&dcrd.vp[0][0]); 
            int const* const o_spatial0 = reinterpret_cast<int const*>
                    (&dcrd.vp[2][0]);
            //int const* const s_spatial0 = reinterpret_cast<int const*>
            //        (&scrd.vp[2][0]); 
            //int const* const w_spatial0 = reinterpret_cast<int const*>
            //        (&wcrd.vp[2+ (with_groups?1:0)][0]); 

            // what type? data_t dst_reg[MVL]; VREG(dst_reg); // oh, we write this as mem :(
            float dst_reg[MVL]; VREG(dst_reg); // oh, we write this as mem :(
            //int kk_dhw[3*MVL]; // if (ws), max-coord memory (vector kpos post-calc)

            // vector precalc constants (vector calc --> mem)
            // ssz ~ source pooling window (tile overlap with unpadded src)
            // dhw ~ shift, start, end iterator (easy vector calc)
            int ssz[MVL]; // ssz < kern ovlp <= KD*KH*KW (small)
            int dhw[12*MVL]; // nc++ slightly prefers single array over 9
            // precalc ssz and dhw vectors for pool window ovlp w/ src
            //POOL_WINDOW_PRECALC(dm, dvl, o_spatial0, /*outputs*/ ssz, dhw);
            // macro version of window_precalc
            // Each destination spatial pixel has a source (and weight) pre-image tile.
            // Calculate 3 vectors of destination window info per spatial dimension.
            //
            // - ls_sz ~ src (/weights) tile coord span.
            // - ls_st ~ logical source start coord; in interval [0,ID|IH|IW)
            // - lk_st ~ logical kernel start coord; in interval [0,KD|KH|KW)
#define CONV_WINDOW_LOOP(d_spatial, dvl, STRIDE, DILATION, PAD, KSZ, ISZ, lscrd, p_st, p_en, ssz) do \
            { \
                ShortLoop() for(int i=0; i<dvl; ++i) { \
                    int lscrd = d_spatial[i]/*od or oh or ow*/ * STRIDE - PAD; \
                    ls_st[i] = (lscrd > 0? lscrd: 0); \
                    int ls_en = (lscrd + KSZ < ISZ? lscrd + KSZ: ISZ); \
                    if (ls_en < ls_st[i]) ls_en = ls_st[i]; \
                    ls_sz[i] = ls_en - ls_st[i]; \
                    ssz[i] *= ls_sz[i]; \
                } \
            } while(0)

            // expects SD,KD,ID,padF, etc int consts defined as usual
            // dcrd info passed in as dm~dcrd.get_dim() dvl~dcrd.get_vl(),
            // and d_spatial0 ~ (int32_t*)&dcrd.vp[2][0].
#define CONV_WINDOW_PRECALC(dm, dvl, d_spatial0, ssz, dhw) do \
            { \
                int const* d_spatial = d_spatial0; \
                int * p_off; /* input coord offset (from destination coord) */ \
                int * p_st; /* input coord start */ \
                int * p_en; /* input coord end, >= start*/ \
                ShortLoop() for(int i=0; i<dvl; ++i) \
                ssz[i] = 1; \
                if (dm >= 5) { \
                    p_off = dhw + 8*MVL; \
                    p_st  = dhw + 9*MVL; \
                    p_en  = dhw + 10*MVL; \
                    k_st  = dhw + 11*MVL; \
                    POOL_WINDOW_LOOP(d_spatial, dvl, SD, padF, KD, ID, \
                            p_off, p_st, p_en, ssz, k_st); \
                    d_spatial += MVL; /*dcrd.MaxVl*/ \
                } \
                if (dm >= 4) { \
                    p_off = dhw + 4*MVL; \
                    p_st  = dhw + 5*MVL; \
                    p_en  = dhw + 6*MVL; \
                    k_st  = dhw + 7*MVL; \
                    POOL_WINDOW_LOOP(d_spatial, dvl, SH, padT, KH, IH, \
                            p_off, p_st, p_en, ssz, k_st); \
                    d_spatial += MVL; /*dcrd.MaxVl*/ \
                } \
                p_off = dhw + 0*MVL; \
                p_st  = dhw + 1*MVL; \
                p_en  = dhw + 2*MVL; \
                p_en  = dhw + 3*MVL; \
                POOL_WINDOW_LOOP(d_spatial, dvl, SW, padL, KW, IW, \
                        p_off, p_st, p_en, ssz, k_st); \
            } while(0)
#endif

            {
                int v_g[MVL];
                int v_oc[MVL];
                VFOR(i,dvl) {
                    v_g[i] = v_ocxg[i] / OC;
                    v_oc[i] = v_ocxg[i] % OC;
                }
                COORD_VEC_REGISTER(v_mb, dcrd_outer0);
                // v_ocxg already set
                COORD_VEC_REGISTER(v_od, (dm >= 5? (dcrd_i+=MVL): dim_zeros));
                COORD_VEC_REGISTER(v_oh, (dm >= 4? (dcrd_i+=MVL): dim_zeros));
                COORD_VEC_REGISTER(v_ow, dcrd_i+=MVL); // always exists, dm >= 3
#undef COORD_VEC_REGISTER
                NOVEC VFOR(i,dvl) { // fn calls, novec
                    a[i] += ker6(v_g[i], v_mb[i], v_oc[i], v_od[i], v_oh[i], v_ow[i]);
                }
            }

            //maybe_oscale(a, g, oc);
            //VFOR(i,dvl) a[i] *= scales[v_ocxg[i] * scale_idx_mult];
            if (scale_idx_mult) VFOR(i,dvl) a[i] *= scales[v_ocxg[i]];
            else                VFOR(i,dvl) a[i] *= scales[0];

            maybe_postops_vec3(a, dst_gather, dvl); // faster as lambda? go figure

            VFOR(i,dvl) {
                // technically wrong for OPT==1, but faster !!!
                using cvt_dst = Cvt<dst_data_t, is_int_conv>;
                dst_gather[i] = cvt_dst::qs(a[i]);
                dst[dst_off[i]] = dst_gather[i];
            }
        }
    };

    bool constexpr force_sequential = 0; // 1 for debug
    parallel((force_sequential? 1: 0), kern6);
}

using namespace data_type;

template struct ref_convolution_fwd_t<f32>;
template struct ref_convolution_fwd_t<bf16,bf16,bf16,f32>;
template struct ref_convolution_fwd_t<bf16,bf16,f32,f32>;

template struct ref_convolution_fwd_t<u8, s8, f32, s32>;
template struct ref_convolution_fwd_t<u8, s8, s32, s32>;
template struct ref_convolution_fwd_t<u8, s8, s8, s32>;
template struct ref_convolution_fwd_t<u8, s8, u8, s32>;
template struct ref_convolution_fwd_t<s8, s8, f32, s32>;
template struct ref_convolution_fwd_t<s8, s8, s32, s32>;
template struct ref_convolution_fwd_t<s8, s8, s8, s32>;
template struct ref_convolution_fwd_t<s8, s8, u8, s32>;

} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino=+2s,l0,\:4,N-s

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
// internal compiler error?
//#pragma _NEC options "-floop-split"

#include <assert.h>
#include <float.h>
#include <math.h>

#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/type_helpers.hpp"
#include "common/bfloat16.hpp"

#include "cpu/ref_softmax.hpp"

// offset calc method : newer approaches...

// Dense loops are OK, but calls to 'off_l' did not vectorize
//      VE speedup on sm.in: avg ~ 68x (at high channels can be 100-200x faster)

// Choose vectorization
//      default vectorization
#include "common/ve/memory_desc_wrapper_opt.hpp"
#define memory_desc_wrapper memory_desc_wrapper_opt
//      experimental features
//#include "common/ve/memory_desc_wrapper_opt_dev.hpp"
//#define memory_desc_wrapper memory_desc_wrapper_opt_dev

// before code cleanup --> ve/dev/ref_softmax.cpp
//
// investigate whether nc++ uses 'shortloop' hint well
// 0 : segregate vec reg, MEDIUM and longer lengths
// 1 : code for MEDIUM and longer
// 2 : treat everything in just the longer, blocked loop
// sm.in results: WHICH==1 is definitely better than WHICH==0
//              presumably nc++ is not handling VREG as well as sx++ did
// WHICH==2 (blocking the channels, some double-calc of vec_off)
//          is really not too much worse than WHICH==1
//          and decreases code size a bit. (ref code candidate?)
//
// code for WHICH==0 removed [slower on VE] see src/common/ve/dev/
#define WHICH 1 /* best setting for VE, 2 loop cases */


// enable the new offset calc methods...
// XXX messy -- sometimes VE-specific, but sometimes want this for x86 too ?
//      src/cpu/ref_softmax.cpp with only VE workarounds,
//      and keep all vec optimization in src/cpu/ve/ref_softmax.cpp

#if defined(__ve)
// settings for VE [these also incorporate bug workarounds for nc++]
#define VE_FWD_GEN   1
#define VE_BWD_GEN   1
#ifndef MVL
#define MVL 256         /* max simd vector length */
#endif
#else
// TODO test/adapt VE vectorization mods to x86 (perhaps only WHICH==2 case)
#define VE_FWD_GEN   0
#define VE_BWD_GEN   0
#ifndef MVL
#define MVL 32          /* x86 ~ 'simd vector length'*/
#endif
#endif

#define MEDIUM (16*MVL) /*potentially 16 vec regs*/
// Does gcc want WHICH==2 and MEDIUM==MVL instead?

#define SOFTMAX_PRT 0 // prt dense/generic and outer/channels/inner

namespace dnnl {
namespace impl {
namespace cpu {

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::execute_forward_dense(
        const exec_ctx_t &ctx) const {
    auto src = CTX_IN_MEM(const data_t *, DNNL_ARG_SRC);
    auto dst = CTX_OUT_MEM(data_t *, DNNL_ARG_DST);
#if SOFTMAX_PRT
    fprintf(stderr,"sofmax_fwd_dense outer %d channels %d inner %d\n",
            (int)outer_size_,(int)channels_,(int)inner_size_);
#endif

    const auto ou_stride = pd()->outer_stride();

    parallel_nd(outer_size_, [&](int ou) {
        const data_t *src_data = src + ou * ou_stride;
        data_t *dst_data = dst + ou * ou_stride;
        float space_max = -FLT_MAX;
        float space_denom = 0;
#if defined(__ve)
        // max
        for (int c = 0; c < channels_; ++c) { // nc++ issue w/ nstl::max?
            space_max = (src_data[c] > space_max? (float)src_data[c]: space_max);
        }
        // sub + exp + sum // VE does fine vectorizing this
        if (pd()->is_softmax()) {
            for (int c = 0; c < channels_; ++c) {
                space_denom += dst_data[c] = expf(src_data[c] - space_max);
            }
        } else if (pd()->is_logsoftmax()) {
            for (int c = 0; c < channels_; ++c) {
                float D = dst_data[c] = src_data[c] - space_max;
                space_denom += expf(D);
            }
        }
        // scal // nc++ workaround (move cond out of loop)
        if (pd()->is_softmax()) {
            space_denom = space_denom ? (1.f / space_denom) : 1.f;
            // '*=', '-='  :  not avail for bfloat16
            for (int c = 0; c < channels_; ++c) {
                dst_data[c] = dst_data[c] * space_denom;
            }
        } else if (pd()->is_logsoftmax()) {
            space_denom = logf(space_denom);
            for (int c = 0; c < channels_; ++c) {
                dst_data[c] = dst_data[c] - space_denom;
            }
        }

#else // x86/arm original version

        constexpr int unroll_factor = 32;
#if !defined(__INTEL_COMPILER)
        // The code below makes the compiler generate maxps instruction.
        // rather than maxss, which is generated for the 'else' code path
        auto max_wrapper = [](float a, float b) { return nstl::max(a, b); };
        auto min_wrapper = [](int a, int b) { return nstl::min(a, b); };

        if (channels_ < unroll_factor) {
            float max_val = -FLT_MAX;
            for (int i = 0; i < channels_; i++) {
                max_val = max_wrapper(max_val, src_data[i]);
            }
            space_max = max_val;
        } else {
            float max_values[unroll_factor];

            for (int i = 0; i < unroll_factor; i++) {
                max_values[i] = src_data[i];
            }
            for (int i = unroll_factor; i < channels_; i += unroll_factor) {
                int offset = min_wrapper(i, channels_ - unroll_factor);
                for (int j = 0; j < unroll_factor; j++) {
                    max_values[j]
                            = max_wrapper(max_values[j], src_data[offset + j]);
                }
            }
            float max_val = -FLT_MAX;
            for (int i = 0; i < unroll_factor; i++) {
                max_val = max_wrapper(max_val, max_values[i]);
            }
            space_max = max_val;
        }
#else /* ic++ or nc++ */
        // Intel(R) C++ Compiler generates the maxps + shuffle pattern
        // for the max search which works faster
        for (int c = 0; c < channels_; ++c)
            space_max = nstl::max(space_max, (float)src_data[c]);
#endif

        // sub + exp + sum
        int tail = channels_ % unroll_factor;
        for (int i = 0; i < channels_ - tail; i += unroll_factor) {
            PRAGMA_OMP_SIMD()
            for (int j = 0; j < unroll_factor; j++) {
                if (pd()->is_softmax()) {
                    float D = expf(src_data[i + j] - space_max);
                    space_denom += D;
                    dst_data[i + j] = D;
                } else if (pd()->is_logsoftmax()) {
                    float D = src_data[i + j] - space_max;
                    space_denom += expf(D);
                    dst_data[i + j] = D;
                }
            }
        }
        for (int i = channels_ - tail; i < channels_; i++) {
            if (pd()->is_softmax()) {
                float D = expf(src_data[i] - space_max);
                space_denom += D;
                dst_data[i] = D;
            } else if (pd()->is_logsoftmax()) {
                float D = src_data[i] - space_max;
                space_denom += expf(D);
                dst_data[i] = D;
            }
        }

        // scal
        if (pd()->is_softmax()) {
            space_denom = space_denom ? (1.f / space_denom) : 1.f;
        } else if (pd()->is_logsoftmax()) {
            space_denom = logf(space_denom);
        }
        for (int c = 0; c < channels_; ++c) {
            if (pd()->is_softmax()) {
                dst_data[c] = dst_data[c] * space_denom;
            } else if (pd()->is_logsoftmax()) {
                dst_data[c] = dst_data[c] - space_denom;
            }
        }
#endif // VE workaround vs x86 original
    });
}

template <impl::data_type_t data_type>
void ref_softmax_fwd_t<data_type>::execute_forward_generic(
        const exec_ctx_t &ctx) const {

    auto src = CTX_IN_MEM(const data_t *, DNNL_ARG_SRC);
    auto dst = CTX_OUT_MEM(data_t *, DNNL_ARG_DST);
    auto const is_softmax = pd()->is_softmax();
    auto const is_logsoftmax = pd()->is_logsoftmax();

    const memory_desc_wrapper data_d(pd()->src_md());
#if SOFTMAX_PRT
    fprintf(stderr,"sofmax_fwd_generic outer %d channels %d inner %d\n",
            (int)outer_size_,(int)channels_,(int)inner_size_);
#endif

    parallel_nd(outer_size_, [&](int ou) {
        float space_max_val = 0, space_denom_val = 0;
        float *space_max = &space_max_val, *space_denom = &space_denom_val;
        if (inner_size_ > 1) {
            using namespace memory_tracking::names;
            space_max = ctx.get_scratchpad_grantor().template get<float>(
                                key_softmax_reduction)
                    + ou * 2 * inner_size_;
            space_denom = space_max + inner_size_;
        }

        utils::array_set(space_max, -FLT_MAX, inner_size_);
        utils::array_set(space_denom, 0, inner_size_);

        for (int in = 0; in < inner_size_; in++) {
            dim_t const ou_in_offset = ou * channels_ * inner_size_ + in;
#if VE_FWD_GEN
            // large-block
            // But we expect NON-BLOCKED formats for VE, which **do** have constant
            // channel-stride, and should be optimized (between "dense", inner~1
            // and this "generic", in>1 cases).
            if (channels_ == 0) {
                // ignore side effect of setting space_denom, space_max
                // XXX remove space_denom and space_max entirely?
                ;
            } else if( WHICH >= 1 && channels_ <= MEDIUM ) {
#define Medium_ PragmaQuote(_NEC loop_count(MEDIUM))
                size_t coff[MEDIUM];
                if (channels_ > 1) {   //
                    dim_t l_off[MEDIUM];
                    // VREG(l_off); if can inline vec_off_l (asm?)
                    // logical offsets, "as if dense" inner dims
                    Medium_ for (int c = 0; c < channels_; ++c)
                            l_off[c] = ou_in_offset + c * inner_size_;
                    data_d.vec_off_l( &l_off[0], channels_, (dim_t*)&coff[0] );
                    //                [is_pos_padded=false]
                }else{
                    Medium_ for (int c = 0; c < channels_; ++c)
                        // default: is_pos_padded=false
                        coff[c] = data_d.off_l(ou_in_offset + c * inner_size_);
                }
                float smax = src[coff[0]];
                Medium_ for (int c = 0; c < channels_; c++)
                    if( src[coff[c]] > smax ) smax = src[coff[c]];

                float denom = 0;
                if (is_softmax) {
                    Medium_ for (int c = 0; c < channels_; c++) {
                        float D = expf(src[coff[c]] - smax);
                        denom += D;
                        dst[coff[c]] = D;
                    }
                } else if (is_logsoftmax) {
                    Medium_ for (int c = 0; c < channels_; c++) {
                        float D = src[coff[c]] - smax;
                        denom += expf(D);
                        dst[coff[c]] = D;
                    }
                }
                space_max[in] = smax;
                if (is_logsoftmax) denom = logf(denom);

                // VE: both "Partially vectorized" until IVDEP (even wtih list_vector hint)
                if (is_softmax) {
                    float const mul = 1.0 / denom;
                    IVDEP() Medium_ for (int c = 0; c < channels_; c++)
                        dst[coff[c]] = dst[coff[c]] * mul;
                } else if (is_logsoftmax) {
                    IVDEP() Medium_ for (int c = 0; c < channels_; c++)
                        dst[coff[c]] = dst[coff[c]] - denom;
                }
                space_denom[in] = denom;
#undef Medium_
            } else { // WHICH >= 2 || channels_ > MEDIUM
#define OUTER_
#define Medium_ PragmaQuote(_NEC loop_count(MEDIUM))
#define Medium_lv_ Medium_ PragmaQuote(_NEC list_vector)
                // the initial max means a full scalar pass (not caching off_l func values)
                float smax = -FLT_MAX;
                OUTER_ for (int c0 = 0; c0 < channels_; c0+=MEDIUM) {
                    dim_t data_off[MEDIUM];
                    int const cmax=( channels_ - c0 > MEDIUM? MEDIUM: channels_ - c0 );
                    if (cmax > 1) {
                        dim_t l_off[MEDIUM]; // use cmax <= MEDIUM
                        Medium_ for (int c = 0; c < cmax; ++c)
                            l_off[c] = ou_in_offset + (c0 + c) * inner_size_;
                        data_d.vec_off_l( &l_off[0], cmax, &data_off[0] ); // is_pos_padded=false
                    } else {
                        Medium_ for(int c=0; c<cmax; ++c){ // unvectorizable func call
                            data_off[c] = data_d.off_l(ou_in_offset + (c0 + c) * inner_size_);
                        }
                    }
                    Medium_lv_ for(int c=0; c<cmax; ++c){ // unvectorizable func call
                        if( src[data_off[c]] > smax ) smax = src[data_off[c]];
                    }
                }
                float sdenom = 0.0;
                OUTER_ for (int c0 = 0; c0 < channels_; c0+=MEDIUM) {
                    dim_t data_off[MEDIUM];
                    int const cmax=( channels_ - c0 > MEDIUM? MEDIUM: channels_ - c0 );
                    if (cmax > 1) {
                        dim_t l_off[MEDIUM]; // use cmax <= MEDIUM
                        Medium_ for (int c = 0; c < cmax; ++c)
                            l_off[c] = ou_in_offset + (c0 + c) * inner_size_;
                        data_d.vec_off_l( &l_off[0], cmax, &data_off[0] ); // is_pos_padded=false
                    } else {
                        for(int c=0; c<cmax; ++c){ // unvectorizable func call
                            data_off[c] = data_d.off_l(ou_in_offset + (c0 + c) * inner_size_);
                        }
                    }
                    if (is_softmax) {
                        Medium_lv_ for(int c=0; c<cmax; ++c){ // pd-> blocks vectorization
                            float const D = expf(src[data_off[c]] - smax);
                            sdenom += D;
                            dst[data_off[c]] = D;
                        }
                    } else if (is_logsoftmax) {
                        Medium_lv_ for(int c=0; c<cmax; ++c){ // pd-> blocks vectorization
                            float const D = src[data_off[c]] - smax;
                            sdenom += expf(D);
                            dst[data_off[c]] = D;
                        }
                    }
                }
                space_max[in] = smax; // but actually do not need scratchpad? XXX

                if (is_softmax) {
                    space_denom[in] = sdenom; // but actually do not need to store XXX
                    sdenom = 1.0 / sdenom;
                } else if (is_logsoftmax) {
                    sdenom = logf(sdenom);
                    space_denom[in] = sdenom; // but actually do not need to store XXX
                }

                PragmaQuote(_NEC novovertake)
                OUTER_ for (int c0 = 0; c0 < channels_; c0+=MEDIUM) {
                    int const cmax=( c0 < channels_- MEDIUM? MEDIUM: channels_ - c0 );
                    dim_t data_off[MEDIUM];
                    if (cmax > 1) {
                        dim_t l_off[MEDIUM]; // use cmax <= MEDIUM
                        Medium_ for (int c = 0; c < cmax; ++c)
                            l_off[c] = ou_in_offset + (c0 + c) * inner_size_;
                        data_d.vec_off_l( &l_off[0], cmax, &data_off[0] ); // is_pos_padded=false
                    } else {
                        Medium_ for(int c=0; c<cmax; ++c){ // unvectorizable func call
                            data_off[c] = data_d.off_l(ou_in_offset + (c0 + c) * inner_size_);
                        }
                    }
                    if (is_softmax) {
                        Medium_lv_ for (int c = 0; c < cmax; c++) {
                            dst[data_off[c]] = dst[data_off[c]] * sdenom;
                        }
                    } else if (is_logsoftmax) {
                        Medium_lv_ for (int c = 0; c < cmax; c++) {
                            dst[data_off[c]] = dst[data_off[c]] - sdenom;
                        }
                    }
                }
#undef Medium_
#undef Medium_lv_
#undef OUTER_
            }
#else // VE_FWD_GEN
            { // original way was JUST the following
                for (int c = 0; c < channels_; c++) {
                    size_t off = data_d.off_l(ou_in_offset + c * inner_size_);
                    space_max[in] = nstl::max(space_max[in], (float)src[off]);
                }

                for (int c = 0; c < channels_; c++) {
                    size_t off = data_d.off_l(ou_in_offset + c * inner_size_);
                    if (is_softmax) {
                        float D = expf(src[off] - space_max[in]);
                        space_denom[in] += D;
                        dst[off] = D;
                    } else if (is_logsoftmax) {
                        float D = src[off] - space_max[in];
                        space_denom[in] += expf(D);
                        dst[off] = D;
                    }
                }

                if (is_logsoftmax) {
                    space_denom[in] = logf(space_denom[in]);
                }

                for (int c = 0; c < channels_; c++) {
                    size_t off = data_d.off_l(ou_in_offset + c * inner_size_);
                    if (is_softmax) {
                        dst[off] = dst[off] / space_denom[in];
                    } else if (is_logsoftmax) {
                        dst[off] = dst[off] - space_denom[in];
                    }
                }
            } // end original way
#endif // VE_FWD_GEN
        }
    });
}

template struct ref_softmax_fwd_t<data_type::bf16>;
template struct ref_softmax_fwd_t<data_type::f32>;

// softmax along last physical dimension
template <impl::data_type_t data_type>
void ref_softmax_bwd_t<data_type>::execute_backward_dense(
        const exec_ctx_t &ctx) const {
    auto dst = CTX_IN_MEM(const data_t *, DNNL_ARG_DST);
    auto diff_dst = CTX_IN_MEM(const data_t *, DNNL_ARG_DIFF_DST);
    auto diff_src = CTX_OUT_MEM(data_t *, DNNL_ARG_DIFF_SRC);

    const auto ou_stride = pd()->outer_stride();

#if SOFTMAX_PRT
    fprintf(stderr,"sofmax_bwd_dense outer %d channels %d inner %d\n",
            (int)outer_size_,(int)channels_,(int)inner_size_);
#endif
    parallel_nd(outer_size_, [&](int ou) {
        float sbr = 0;
        size_t off = ou * ou_stride;
        if (pd()->is_softmax()) {
            for (size_t loff = off; loff < off + channels_; ++loff)
                sbr += diff_dst[loff] * dst[loff];
            for (size_t loff = off; loff < off + channels_; ++loff)
                diff_src[loff] = dst[loff] * (diff_dst[loff] - sbr);
        } else if (pd()->is_logsoftmax()) {
            for (size_t loff = off; loff < off + channels_; ++loff)
                sbr += diff_dst[loff];
            for (size_t loff = off; loff < off + channels_; ++loff)
                diff_src[loff] = diff_dst[loff] - expf(dst[loff]) * sbr;
        }
    });
}

template <impl::data_type_t data_type>
void ref_softmax_bwd_t<data_type>::execute_backward_generic(
        const exec_ctx_t &ctx) const {
    auto dst = CTX_IN_MEM(const data_t *, DNNL_ARG_DST);
    auto diff_dst = CTX_IN_MEM(const data_t *, DNNL_ARG_DIFF_DST);
    auto diff_src = CTX_OUT_MEM(data_t *, DNNL_ARG_DIFF_SRC);

    const memory_desc_wrapper diff_d(pd()->diff_src_md());
    const memory_desc_wrapper data_d(pd()->dst_md());

#if SOFTMAX_PRT
    fprintf(stderr,"sofmax_bwd_generic outer %d channels %d inner %d\n",
            (int)outer_size_,(int)channels_,(int)inner_size_);
#endif
#if ! VE_BWD_GEN
    // original code [slow]
    parallel_nd(outer_size_, inner_size_, [&](int ou, int in) {
        dim_t ou_in_offset = ou * channels_ * inner_size_ + in;
        float sbr = 0;
        for (int c = 0; c < channels_; ++c) {
            auto off_diff = diff_d.off_l(ou_in_offset + c * inner_size_);
            if (pd()->is_softmax()) {
                auto off_data = data_d.off_l(ou_in_offset + c * inner_size_);
                sbr += diff_dst[off_diff] * dst[off_data];
            } else if (pd()->is_logsoftmax()) {
                sbr += diff_dst[off_diff];
            }
        }

        for (int c = 0; c < channels_; ++c) {
            auto off_diff = diff_d.off_l(ou_in_offset + c * inner_size_);
            auto off_data = data_d.off_l(ou_in_offset + c * inner_size_);
            if (pd()->is_softmax()) {
                diff_src[off_diff] = dst[off_data] * (diff_dst[off_diff] - sbr);
            } else if (pd()->is_logsoftmax()) {
                diff_src[off_diff]
                        = diff_dst[off_diff] - expf(dst[off_data]) * sbr;
            }
        }
    });
#else // VE version (vectorize over channels_)
    // (some precision issues w/ nc++ fast exp ??
    parallel_nd(outer_size_, inner_size_, [&](int ou, int in) {
        dim_t const ou_in_offset = ou * channels_ * inner_size_ + in;
        if (pd()->is_softmax()) {
            typedef data_t acc_t;
            acc_t sbr = (data_t)0;
            //acc_t sbr = (acc_t)0;
#if VE_BWD_GEN // rewrite to allow some vectorization
            // time 91.8669 --> 0.104004 for
            // ./vetest.sh -B build-vej --benchdnn --softmax --verbose=10 --dir=BWD_D --axis=0 8192x64
            // which fails accuracy checks.
            if( WHICH >= 1 && channels_ <= MEDIUM ){
                //asm("## medium");
#define Medium_ PragmaQuote(_NEC loop_count(MEDIUM))
                dim_t diff_off[MEDIUM];
                dim_t data_off[MEDIUM];
                if (channels_ > 1) {
                    dim_t l_off[MEDIUM];
                    Medium_ for (int c = 0; c < channels_; ++c)
                        l_off[c] = ou_in_offset + c * inner_size_;
                    data_d.vec_off_l( &l_off[0], channels_, &data_off[0] );
                    diff_d.vec_off_l( &l_off[0], channels_, &diff_off[0] );
                } else {
                    Medium_ for (int c = 0; c < channels_; ++c) { // Unvectorized
                        size_t const coff = ou_in_offset + c * inner_size_;
                        diff_off[c] = diff_d.off_l(coff);
                        data_off[c] = data_d.off_l(coff);
                    }
                }
                //PragmaQuote(_NEC nofuse)
                Medium_ for (int c = 0; c < channels_; ++c) {
                    sbr += (acc_t)(diff_dst[diff_off[c]]) * dst[data_off[c]];
                }
                Medium_ for (int c = 0; c < channels_; ++c) {
                    diff_src[diff_off[c]] = dst[data_off[c]]
                            * (diff_dst[diff_off[c]] - sbr);
                }
#undef Medium_
            }else{ // WHICH >= 2 || channels_ > MEDIUM
                //asm("## long");
#define Medium_ PragmaQuote(_NEC loop_count(MEDIUM))
                for (int c0 = 0; c0 < channels_; c0+=MEDIUM) {
                    dim_t diff_off[MEDIUM];
                    dim_t data_off[MEDIUM];
                    int const cmax=( c0 < channels_- MEDIUM? MEDIUM: channels_ - c0 );
                    if (cmax > 1) {
                        dim_t l_off[MEDIUM];
                        Medium_ for (int c = 0; c < cmax; ++c)
                            l_off[c] = ou_in_offset + (c0 + c) * inner_size_;
                        data_d.vec_off_l( &l_off[0], cmax, &data_off[0] );
                        diff_d.vec_off_l( &l_off[0], cmax, &diff_off[0] );
                    } else {
                        Medium_ for(int c=0; c<cmax; ++c){ // unvectorizable func call
                            size_t const coff = ou_in_offset + (c0 + c) * inner_size_;
                            diff_off[c] = diff_d.off_l(coff);
                            data_off[c] = data_d.off_l(coff);
                        }
                    }
                    //PragmaQuote(_NEC vob)
                    Medium_ for(int c=0; c<cmax; ++c){
                        sbr += (acc_t)(diff_dst[diff_off[c]]) * dst[data_off[c]];
                    }
                }
                for (int c0 = 0; c0 < channels_; c0+=MEDIUM) {
                    dim_t diff_off[MEDIUM];
                    dim_t data_off[MEDIUM];
                    int const cmax=( c0 < channels_- MEDIUM? MEDIUM: channels_ - c0 );
                    if (cmax > 1) {
                        dim_t l_off[MEDIUM];
                        Medium_ for (int c = 0; c < cmax; ++c)
                            l_off[c] = ou_in_offset + (c0 + c) * inner_size_;
                        data_d.vec_off_l( &l_off[0], cmax, &data_off[0] );
                        diff_d.vec_off_l( &l_off[0], cmax, &diff_off[0] );
                    } else {
                        Medium_ for(int c=0; c<cmax; ++c){ // unvectorizable func call
                            size_t const coff = ou_in_offset + (c0 + c) * inner_size_;
                            diff_off[c] = diff_d.off_l(coff);
                            data_off[c] = data_d.off_l(coff);
                        }
                    }
                    //PragmaQuote(_NEC vob)
                    Medium_ for(int c=0; c<cmax; ++c){
                        diff_src[diff_off[c]] = dst[data_off[c]]
                                * (diff_dst[diff_off[c]] - sbr);
                    }
                }
#undef Medium_
            }
#else // original : offset function calls ==> scalar loops
            for (int c = 0; c < channels_; ++c) {
                auto off_diff = diff_d.off_l(ou_in_offset + c * inner_size_);
                auto off_data = data_d.off_l(ou_in_offset + c * inner_size_);
                sbr += diff_dst[off_diff] * dst[off_data];
            }
            for (int c = 0; c < channels_; ++c) {
                auto off_diff = diff_d.off_l(ou_in_offset + c * inner_size_);
                auto off_data = data_d.off_l(ou_in_offset + c * inner_size_);
                diff_src[off_diff] = dst[off_data] * (diff_dst[off_diff] - sbr);
            }
#endif
        } else if (pd()->is_logsoftmax()) {
            //typedef data_t acc_t; // double was 100x slower and no accuracty improvement
            typedef float acc_t;
            acc_t sbr = (data_t)0;
#if VE_BWD_GEN // rewrite to allow some vectorization
            // time 91.8669 --> 0.104004 for
            // ./vetest.sh -B build-vej --benchdnn --softmax --verbose=10 --dir=BWD_D --axis=0 8192x64
            // which fails accuracy checks.
            if( WHICH >= 1 && channels_ <= MEDIUM ){
                //asm("## logmedium");
#define Medium_ PragmaQuote(_NEC loop_count(MEDIUM))
                dim_t diff_off[MEDIUM];
                dim_t data_off[MEDIUM];
                float data_exp[MEDIUM];
                if (channels_ > 1) {
                    dim_t l_off[MEDIUM];
                    Medium_ for (int c = 0; c < channels_; ++c)
                        l_off[c] = ou_in_offset + c * inner_size_;
                    data_d.vec_off_l( &l_off[0], channels_, &data_off[0] );
                    diff_d.vec_off_l( &l_off[0], channels_, &diff_off[0] );
                } else {
                    Medium_ for (int c = 0; c < channels_; ++c) { // Unvectorized
                        size_t const coff = ou_in_offset + c * inner_size_;
                        data_off[c] = data_d.off_l(coff);
                        diff_off[c] = diff_d.off_l(coff);
                    }
                }
                // nc++ DOES have an __vec_expf ... how to get it?
                {
                    data_t vdst[MEDIUM];
                    Medium_ for (int c = 0; c < channels_; ++c) { // sneaky: allows __vec_expf
                        // NO data_exp[c] = expf( dst[data_off[c]] );
                        vdst[c] = dst[data_off[c]];
                        data_exp[c] = expf( vdst[c] );
                    }
                }
                Medium_ for (int c = 0; c < channels_; ++c) {
                    sbr += diff_dst[diff_off[c]];
                }
                Medium_ for (int c = 0; c < channels_; ++c) {
                    diff_src[diff_off[c]] = diff_dst[diff_off[c]]
                            - data_exp[c] * sbr;
                }
#undef Medium_
            }else{ // WHICH >= 2 || channels_ > MEDIUM
                //asm("## loglong"); // ouch. double calc of offsets
#define Medium_ PragmaQuote(_NEC loop_count(MEDIUM))
                for (int c0 = 0; c0 < channels_; c0+=MEDIUM) {
                    dim_t diff_off[MEDIUM];
                    int const cmax=( c0 < channels_- MEDIUM? MEDIUM: channels_ - c0 );
                    if (cmax > 1) {
                        dim_t l_off[MEDIUM];
                        Medium_ for (int c = 0; c < cmax; ++c)
                            l_off[c] = ou_in_offset + (c0 + c) * inner_size_;
                        diff_d.vec_off_l( &l_off[0], cmax, &diff_off[0] );
                    } else {
                        Medium_ for(int c=0; c<cmax; ++c){ // unvectorizable func call
                            size_t const coff = ou_in_offset + (c0 + c) * inner_size_;
                            diff_off[c] = diff_d.off_l(coff);
                        }
                    }
                    Medium_ for(int c=0; c<cmax; ++c){
                        sbr += diff_dst[diff_off[c]];
                    }
                } // blockwise sbr adds mul & div corrections (keep as original!)
                for (int c0 = 0; c0 < channels_; c0+=MEDIUM) {
                    dim_t diff_off[MEDIUM];
                    dim_t data_off[MEDIUM];
                    float data_exp[MEDIUM];
                    int const cmax=( c0 < channels_- MEDIUM? MEDIUM: channels_ - c0 );
                    if (cmax > 1) {
                        dim_t l_off[MEDIUM];
                        Medium_ for (int c = 0; c < cmax; ++c)
                            l_off[c] = ou_in_offset + (c0 + c) * inner_size_;
                        data_d.vec_off_l( &l_off[0], cmax, &data_off[0] );
                        diff_d.vec_off_l( &l_off[0], cmax, &diff_off[0] );
                    } else {
                        Medium_ for(int c=0; c<cmax; ++c){ // unvectorizable func calls
                            size_t const coff = ou_in_offset + (c0 + c) * inner_size_;
                            data_off[c] = data_d.off_l(coff);
                            diff_off[c] = diff_d.off_l(coff);
                        }
                    }
                    {
                        data_t vdst[MEDIUM];
                        Medium_ for(int c=0; c<cmax; ++c){
                            vdst[c] = dst[data_off[c]];   // gather
                            data_exp[c] = expf(vdst[c]);  // __vec_expf
                        }
                    }
                    Medium_ for(int c=0; c<cmax; ++c){ // gather, load, fma, scatter
                        diff_src[diff_off[c]] = diff_dst[diff_off[c]]
                                - data_exp[c] * sbr;
                    }
                }
#undef Medium_
            }
#else // original bwd logsoftmax: offset function calls ==> scalar loops
            // still unvectorized because of off_l on VE XXX TODO
            for (int c = 0; c < channels_; ++c) {
                auto off_diff = diff_d.off_l(ou_in_offset + c * inner_size_);
                sbr += diff_dst[off_diff];
            }
            for (int c = 0; c < channels_; ++c) {
                auto off_diff = diff_d.off_l(ou_in_offset + c * inner_size_);
                auto off_data = data_d.off_l(ou_in_offset + c * inner_size_);
                diff_src[off_diff] = diff_dst[off_diff]
                        - expf(dst[off_data]) * sbr;
            }
#endif // VE_BWD_GEN
        }
    });
#endif // orig vs VE vectorize-over-channels
}

template struct ref_softmax_bwd_t<data_type::bf16>;
template struct ref_softmax_bwd_t<data_type::f32>;

} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino=+2s,^=l0,\:0,N-s

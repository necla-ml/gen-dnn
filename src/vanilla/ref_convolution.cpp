/*******************************************************************************
* Copyright 2016-2017 Intel Corporation
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

#include "ref_convolution.hpp"
#include "mkldnn_traits.hpp"
#include "math_utils.hpp"

#if !defined(DBG_CONV)
#if !defined(NDEBUG)
#define DBG_CONV 1
#else
#define DBG_CONV 0
#endif
#endif

#if DBG_CONV
#include "mkldnn_io.h"
#include "string.h"
#include <unordered_set> // to track new ref_convolutions (potential to optimize?)
#endif

namespace mkldnn {
namespace impl {
namespace cpu {

using math::saturate;

#if DBG_CONV
// There are mkldnn_memory_format_max^3 possible combinations, of which only
// a small percentage commonly occur.  We can restrict register optimized cases
// into a lookup table XXX eventually.
template<mkldnn_memory_format_t s, mkldnn_memory_format_t wb, mkldnn_memory_format_t d>
inline int constexpr cmem_fmt_tag() {
    mkldnn_memory_format_t const m=mkldnn_memory_format_max;
    return ((s)*m + wb)*m + d;
}
inline int mem_fmt_tag(mkldnn_memory_format_t s, mkldnn_memory_format_t wb, mkldnn_memory_format_t d){
    mkldnn_memory_format_t const m=mkldnn_memory_format_max;
    return ((s)*m + wb)*m + d;
}
// maintain a registry, and avoid outputting duplicates
static std::unordered_set<int> seen;

/** char after the last ':'. \return ptr within \c msg; or \c msg if no ':'. */
static inline char const* after_last_colon(const char* msg) {
    const char * last_colon = strrchr(msg, ':');
    return last_colon? last_colon+1: msg;
}
#endif

template <bool with_relu, data_type_t src_type, data_type_t wei_type,
         data_type_t dst_type, data_type_t acc_type>
_ref_convolution_fwd_t<with_relu, src_type, wei_type, dst_type, acc_type>
::_ref_convolution_fwd_t (const pd_t *pd, const input_vector &inputs,
                          const output_vector &outputs)
: cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd) {
#if DBG_CONV
    mkldnn_memory_format_t const sfmt = conf_.src_pd()->desc()->format;
    mkldnn_memory_format_t const wbfmt = conf_.weights_pd()->desc()->format;
    mkldnn_memory_format_t const dfmt = conf_.dst_pd()->desc()->format;
    int tag = mem_fmt_tag(sfmt,wbfmt,dfmt);
    auto ins = seen.insert( tag );
#define LEN 500
    if(ins.second){ // a one-line message
        printf("\n *** NEW FMTS for %s *** src %s,  wei|bia %s,  dst %s\n",
                    mkldnn_primitive_desc_shorten(conf_.name()),
                    after_last_colon(mkldnn_name_memory_format( sfmt )),
                    after_last_colon(mkldnn_name_memory_format( wbfmt )),
                    after_last_colon(mkldnn_name_memory_format( dfmt )));
    }
    if(ins.second){ // extra detail
        char sbuf[LEN], wbuf[LEN], dbuf[LEN]; // bias layout same as weights
        mkldnn_name_memory_desc( conf_.src_pd()->desc(),     sbuf, LEN );
        mkldnn_name_memory_desc( conf_.weights_pd()->desc(), wbuf, LEN );
        mkldnn_name_memory_desc( conf_.dst_pd()->desc(),     dbuf, LEN );
        printf(" _ref_conv( src        :fmt=%s\n"
               "          , weight/bias:fmt=%s\n"
               "          , dst        :fmt=%s\n",
               &sbuf[0], &wbuf[0], &dbuf[0]);
    }
#undef LEN
#endif /* DBG_CONV */
}

template <bool with_relu, data_type_t src_type, data_type_t wei_type,
         data_type_t dst_type, data_type_t acc_type>
void _ref_convolution_fwd_t<with_relu, src_type, wei_type, dst_type, acc_type>
        ::execute_forward() {
    auto src = reinterpret_cast<const src_data_t *>(this->input_memory(0));
    auto weights = reinterpret_cast<const wei_data_t *>(this->input_memory(1));
    auto bias = reinterpret_cast<const char *>(this->input_memory(2));
    auto dst = reinterpret_cast<dst_data_t *>(this->memory());

    const memory_desc_wrapper src_d(conf_.src_pd());
    const memory_desc_wrapper dst_d(conf_.dst_pd());
    const memory_desc_wrapper weights_d(conf_.weights_pd(0));
    const memory_desc_wrapper bias_d(conf_.weights_pd(1));

    const bool with_groups = conf_.with_groups();

    const int G = conf_.G();
    const int MB = conf_.MB();
    const int OH = conf_.OH();
    const int OW = conf_.OW();
    const int IH = conf_.IH();
    const int IW = conf_.IW();

    const int OC = conf_.OC() / G;
    const int IC = conf_.IC() / G;
    const int KH = conf_.KH();
    const int KW = conf_.KW();

    const int KSH = conf_.KSH();
    const int KSW = conf_.KSW();

    const int KDH = conf_.KDH();
    const int KDW = conf_.KDW();

    const int padT = conf_.padT();
    const int padL = conf_.padL();

    const float nslope = conf_.negative_slope();

    auto ker = [=](acc_data_t &d, int g, int mb, int oc, int oh, int ow) {
        for (int ic = 0; ic < IC; ++ic) {
            for (int kh = 0; kh < KH; ++kh) {
                for (int kw = 0; kw < KW; ++kw) {
                    const int ih = oh * KSH - padT + kh * (1 + KDH);
                    const int iw = ow * KSW - padL + kw * (1 + KDW);
                    // kh = (**ih** - oh*KSH + padT ) / (1+KDH)
                    // So lims of 'kh' loop are
                    // ih==0 --> kh = max(0,  (   - oh*KSH + padT) / (1+KDH)) )
                    // ih==IH -> kh = min(IH, (IH - oh*KSH + padT) / (1+KDH) )
                    // [see benchdnn ref_conv3.cpp (?)]

                    if (ih < 0 || ih >= IH) continue;
                    if (iw < 0 || iw >= IW) continue;

                    d += (acc_data_t)src[src_d.off(mb, g*IC + ic, ih, iw)]
                        * (with_groups
                                ? weights[weights_d.off(g, oc, ic, kh, kw)]
                                : weights[weights_d.off(oc, ic, kh, kw)]);
                }
            }
        }
    };

    auto get_bias = [=, &bias](size_t off) -> acc_data_t {
#       define CASE(dt) case dt: \
            return (acc_data_t)(*((const prec_traits<dt>::type *)bias + off))
        switch (conf_.cdesc()->bias_desc.data_type) {
        CASE(data_type::s8);
        CASE(data_type::u8);
        CASE(data_type::s32);
        CASE(data_type::f32);
        default: assert(!"unimplemented");
        }
#       undef CASE
        return 0;
    };

    OMP(parallel for collapse(5) schedule(static))
    for (int g = 0; g < G; ++g) {
        for (int mb = 0; mb < MB; ++mb) {
            for (int oc = 0; oc < OC; ++oc) {
                for (int oh = 0; oh < OH; ++oh) {
                    for (int ow = 0; ow < OW; ++ow) {
                        acc_data_t a = bias
                            ? get_bias(bias_d.off(g*OC + oc)) : (acc_data_t)0;
                        ker(a, g, mb, oc, oh, ow);
                        if (with_relu && a < (acc_data_t)0)
                            a = (acc_data_t)((float)a * nslope);
                        dst[dst_d.off(mb, g*OC + oc, oh, ow)]
                            = saturate<dst_data_t>(a);
                    }
                }
            }
        }
    }
}

template <data_type_t diff_src_type, data_type_t wei_type,
         data_type_t diff_dst_type, data_type_t acc_type>
void ref_convolution_bwd_data_t<diff_src_type, wei_type, diff_dst_type,
     acc_type>::execute_backward_data() {
    auto diff_dst = reinterpret_cast<const diff_dst_data_t*>(
            this->input_memory(0));
    auto weights = reinterpret_cast<const wei_data_t*>(this->input_memory(1));
    auto diff_src = reinterpret_cast<diff_src_data_t*>(this->memory());

    const memory_desc_wrapper diff_dst_d(conf_.diff_dst_pd());
    const memory_desc_wrapper diff_src_d(conf_.diff_src_pd());
    const memory_desc_wrapper weights_d(conf_.weights_pd(0));

    const bool with_groups = conf_.with_groups();

    const int G = conf_.G();
    const int MB = conf_.MB();
    const int OH = conf_.OH();
    const int OW = conf_.OW();
    const int IH = conf_.IH();
    const int IW = conf_.IW();

    const int OC = conf_.OC() / G;
    const int IC = conf_.IC() / G;
    const int KH = conf_.KH();
    const int KW = conf_.KW();

    const int KSH = conf_.KSH();
    const int KSW = conf_.KSW();

    const int KDH = conf_.KDH();
    const int KDW = conf_.KDW();

    const int padT = conf_.padT();
    const int padL = conf_.padL();

    auto ker = [=](acc_data_t &d, int g, int mb, int ic, int ih, int iw) {
        for (int oc = 0; oc < OC; ++oc) {
            for (int kh = 0; kh < KH; ++kh) {
                for (int kw = 0; kw < KW; ++kw) {
                    if (iw + padL < kw * (1 + KDW)
                       || ih + padT < kh * (1 + KDH))
                        continue;
                    int ow = iw - kw * (1 + KDW) + padL;
                    int oh = ih - kh * (1 + KDH) + padT;
                    if (ow % KSW != 0 || oh % KSH != 0)
                        continue;

                    ow /= KSW;
                    oh /= KSH;

                    if (oh < OH && ow < OW) {
                        d += (acc_data_t)diff_dst[diff_dst_d.off(mb, g*OC + oc,
                                oh, ow)] * (with_groups
                                    ? weights[weights_d.off(g, oc, ic, kh, kw)]
                                    : weights[weights_d.off(oc, ic, kh, kw)]);
                    }
                }
            }
        }
    };

    OMP(parallel for collapse(5) schedule(static))
    for (int g = 0; g < G; ++g) {
        for (int mb = 0; mb < MB; ++mb) {
            for (int ic = 0; ic < IC; ++ic) {
                for (int ih = 0; ih < IH; ++ih) {
                    for (int iw = 0; iw < IW; ++iw) {
                        auto ds_idx = diff_src_d.off(mb, g*IC + ic, ih, iw);
                        acc_data_t a = acc_data_t(0);
                        ker(a, g, mb, ic, ih, iw);
                        diff_src[ds_idx] = saturate<diff_src_data_t>(a);
                    }
                }
            }
        }
    }
}

template <data_type_t src_type, data_type_t diff_wei_type,
         data_type_t diff_dst_type, data_type_t acc_type>
void ref_convolution_bwd_weights_t<src_type, diff_wei_type, diff_dst_type,
     acc_type>::execute_backward_weights() {
    auto src = reinterpret_cast<const src_data_t *>(this->input_memory(0));
    auto diff_dst = reinterpret_cast<const diff_dst_data_t *>(
            this->input_memory(1));
    auto diff_weights = reinterpret_cast<diff_wei_data_t*>(this->memory(0));
    auto diff_bias = reinterpret_cast<diff_wei_data_t *>(this->memory(1));

    const memory_desc_wrapper src_d(conf_.src_pd());
    const memory_desc_wrapper diff_dst_d(conf_.diff_dst_pd());
    const memory_desc_wrapper diff_weights_d(conf_.diff_weights_pd(0));
    const memory_desc_wrapper diff_bias_d(conf_.diff_weights_pd(1));

    const bool with_groups = conf_.with_groups();

    const int G = conf_.G();
    const int MB = conf_.MB();
    const int OH = conf_.OH();
    const int OW = conf_.OW();
    const int IH = conf_.IH();
    const int IW = conf_.IW();

    const int OC = conf_.OC() / G;
    const int IC = conf_.IC() / G;
    const int KH = conf_.KH();
    const int KW = conf_.KW();

    const int KSH = conf_.KSH();
    const int KSW = conf_.KSW();

    const int KDH = conf_.KDH();
    const int KDW = conf_.KDW();

    const int padT = conf_.padT();
    const int padL = conf_.padL();

    auto ker = [=](acc_data_t &d, int g, int oc, int ic, int kh, int kw) {
        for (int mb = 0; mb < MB; ++mb) {
            for (int oh = 0; oh < OH; ++oh) {
                for (int ow = 0; ow < OW; ++ow) {
                    if (ow*KSW + kw * (1 + KDW) < padL
                            || oh*KSH + kh * (1 + KDH) < padT
                            || ow*KSW + kw * (1 + KDW) >= IW + padL
                            || oh*KSH + kh * (1 + KDH) >= IH + padT)
                        continue;

                    int ih = oh*KSH - padT + kh * (1 + KDH);
                    int iw = ow*KSW - padL + kw * (1 + KDW);

                    d += (acc_data_t)diff_dst[diff_dst_d.off(mb, g*OC + oc, oh,
                            ow)] * src[src_d.off(mb, g*IC + ic, ih, iw)];
                }
            }
        }
    };

    auto ker_bias = [=](acc_data_t &d, int g, int oc) {
        for (int mb = 0; mb < MB; ++mb) {
            for (int oh = 0; oh < OH; ++oh) {
                for (int ow = 0; ow < OW; ++ow) {
                    d += (acc_data_t)diff_dst[diff_dst_d.off(mb, g*OC + oc, oh,
                            ow)];
                }
            }
        }
    };

    OMP(parallel for collapse(2) schedule(static))
    for (int g = 0; g < G; ++g) {
        for (int oc = 0; oc < OC; ++oc) {
            if (diff_bias) {
                acc_data_t db = 0;
                ker_bias(db, g, oc);
                diff_bias[diff_bias_d.off(g*OC+oc)]
                    = saturate<diff_wei_data_t>(db);
            }

            for (int ic = 0; ic < IC; ++ic) {
                for (int kh = 0; kh < KH; ++kh) {
                    for (int kw = 0; kw < KW; ++kw) {
                        acc_data_t dw = 0;
                        ker(dw, g, oc, ic, kh, kw);

                        auto idx = with_groups
                            ? diff_weights_d.off(g, oc, ic, kh, kw)
                            : diff_weights_d.off(oc, ic, kh, kw);
                        diff_weights[idx] = saturate<diff_wei_data_t>(dw);
                    }
                }
            }
        }
    }
}

using namespace data_type;

template struct _ref_convolution_fwd_t<false, f32>;
template struct _ref_convolution_fwd_t<true, f32>;
template struct _ref_convolution_fwd_t<false, s16, s16, s32, s32>;
template struct _ref_convolution_fwd_t<true, s16, s16, s32, s32>;
template struct _ref_convolution_fwd_t<false, u8, s8, s32, s32>;
template struct _ref_convolution_fwd_t<true, u8, s8, s32, s32>;
template struct _ref_convolution_fwd_t<false, u8, s8, s8, s32>;
template struct _ref_convolution_fwd_t<true, u8, s8, s8, s32>;
template struct _ref_convolution_fwd_t<false, u8, s8, u8, s32>;
template struct _ref_convolution_fwd_t<true, u8, s8, u8, s32>;

template struct ref_convolution_bwd_data_t<f32, f32, f32, f32>;
template struct ref_convolution_bwd_data_t<s32, s16, s16, s32>;

template struct ref_convolution_bwd_weights_t<f32, f32, f32, f32>;
template struct ref_convolution_bwd_weights_t<s16, s32, s16, s32>;

}
}
}

// vim: et ts=4 sw=4 cindent cino=^=l0,\:0,N-s

/*******************************************************************************
* Copyright 2017-2019 Intel Corporation
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

#include "cpu_barrier.hpp"

#if BARRIER_SHOULD_THROW
// nothing to do (barrier throws if nthr > 1, o/w does nothing)

#else // provide a nontrivial barrier impl

#if TARGET_VE
//#include "C++/xatomic.h" // things like std::_Atomic_fetch_add_8, and _Atomic_counter_t
#include <memory> // shared pointer support defines memory_order_seq_cst

#elif !TARGET_X86_JIT
#include <atomic> // the usual place we expect memory_order_seq_cst

#endif

namespace dnnl {
namespace impl {
namespace cpu {

namespace simple_barrier {

#if TARGET_X86_JIT // several barrier implementations
void generate(
        jit_generator &code, Xbyak::Reg64 reg_ctx, Xbyak::Reg64 reg_nthr) {
#define BAR_CTR_OFF offsetof(ctx_t, ctr)
#define BAR_SENSE_OFF offsetof(ctx_t, sense)
    using namespace Xbyak;

    Xbyak::Reg64 reg_tmp = [&]() {
        /* returns register which is neither reg_ctx nor reg_nthr */
        Xbyak::Reg64 regs[] = {util::rax, util::rbx, util::rcx};
        for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i)
            if (!utils::one_of(regs[i], reg_ctx, reg_nthr)) return regs[i];
        return regs[0]; /* should not happen */
    }();

    Label barrier_exit_label, barrier_exit_restore_label, spin_label;

    code.cmp(reg_nthr, 1);
    code.jbe(barrier_exit_label);

    code.push(reg_tmp);

    /* take and save current sense */
    code.mov(reg_tmp, code.ptr[reg_ctx + BAR_SENSE_OFF]);
    code.push(reg_tmp);
    code.mov(reg_tmp, 1);

    if (mayiuse(avx512_mic)) {
        code.prefetchwt1(code.ptr[reg_ctx + BAR_CTR_OFF]);
        code.prefetchwt1(code.ptr[reg_ctx + BAR_CTR_OFF]);
    }

    code.lock();
    code.xadd(code.ptr[reg_ctx + BAR_CTR_OFF], reg_tmp);
    code.add(reg_tmp, 1);
    code.cmp(reg_tmp, reg_nthr);
    code.pop(reg_tmp); /* restore previous sense */
    code.jne(spin_label);

    /* the last thread {{{ */
    code.mov(code.qword[reg_ctx + BAR_CTR_OFF], 0); // reset ctx

    // notify waiting threads
    code.not_(reg_tmp);
    code.mov(code.ptr[reg_ctx + BAR_SENSE_OFF], reg_tmp);
    code.jmp(barrier_exit_restore_label);
    /* }}} the last thread */

    code.CodeGenerator::L(spin_label);
    code.pause();
    code.cmp(reg_tmp, code.ptr[reg_ctx + BAR_SENSE_OFF]);
    code.je(spin_label);

    code.CodeGenerator::L(barrier_exit_restore_label);
    code.pop(reg_tmp);

    code.CodeGenerator::L(barrier_exit_label);
#undef BAR_CTR_OFF
#undef BAR_SENSE_OFF
}

/** jit barrier generator */
struct jit_t : public jit_generator {
    void (*barrier)(ctx_t *ctx, size_t nthr);

    jit_t() {
        generate(*this, abi_param1, abi_param2);
        ret();
        barrier = reinterpret_cast<decltype(barrier)>(
                const_cast<uint8_t *>(this->getCode()));
    }

    DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_t)
};

void barrier(ctx_t *ctx, int nthr) {
    static jit_t j; /* XXX: constructed on load ... */
    j.barrier(ctx, nthr);
}

#elif TARGET_X86 /* and no jit */
void barrier(ctx_t *ctx, int nthr) {
    // assume ctx_init zeroes out ctx
    if (nthr > 1) {
        size_t sense_sav;

        /* take and save current sense */
        register size_t tmp = ctx->sense;
        *&sense_sav = tmp; // "push"

        tmp = 1U;
        //__sync_fetch_and_add( &ctx->ctr, tmp );
        //tmp += 1U;
        // simpler:
        __atomic_add_fetch(&ctx->ctr, tmp,
                std::memory_order_seq_cst); // maybe this one exists?

        bool last_thread = (tmp == static_cast<size_t>(nthr));
        tmp = *&sense_sav; // "pop"
        if (last_thread) {
            ctx->ctr = 0U; // reset thread counter
            // notify waiting threads
            tmp = !tmp;
            ctx->sense = tmp;
            //goto barrier_exit_restore_label;
        } else {
            //spin_label:
            while (ctx->sense == tmp) {
                __builtin_ia32_pause(); // gcc perhaps has this
            }
        }
        //barrier_exit_restore_label:
        // (nothing to do)
        //barrier_exit_label:
    }
}

#elif defined(__ve) //TARGET_VE
//#warning "Aurora: if xatomic0.h usable, consider _Atomic_counter_t"
// XXX check __NEC_VERSION__ (ex. nc++ -dM -E empty.cpp --> #define __NEC_VERSION__ 30025)
//     because level atomic support has been improving in recent compiler versions
// XXX check that ncc uses proper fences for volatile variables
void barrier(ctx_t *ctx, int nthr) {
    if (nthr > 1) {
        volatile size_t sense_sav;

        /* take and save current sense */
        /*register*/ size_t tmp = ctx->sense; // (register is deprecated)
        *&sense_sav = tmp; // "push"

        tmp = 1U;
        //__sync_fetch_and_add( &ctx->ctr, tmp );
        //tmp += 1U;
        //__sync_add_and_fetch( &ctx->ctr, tmp);
        assert(sizeof(ctx->ctr) == 8);
        //std::_Atomic_fetch_add_8( reinterpret_cast<volatile std::_Uint8_t*>(&ctx->ctr), tmp, std::memory_order_seq_cst );
        //__atomic_add_fetch( &ctx->ctr, tmp, __ATOMIC_SEQ_CST); // maybe this one exists?
        __atomic_add_fetch(&ctx->ctr, tmp,
                std::memory_order_seq_cst); // maybe this one exists?

        bool last_thread = (tmp == static_cast<size_t>(nthr));
        tmp = *&sense_sav; // "pop"
        if (last_thread) {
            ctx->ctr = 0U; // reset thread counter
            // notify waiting threads
            tmp = !tmp;
            ctx->sense = tmp;
        } else {
            // XXX do we need to explicitly clear caches w/ FENCE?
            // XXX should this use the .nc "Not cached on ADB" suffix for the load?
            while (ctx->sense == tmp) {
                asm("nop" :::);
            }
        }
    }
}

#else
#error "barrier not implemented for this build target yet"

#endif // barrier implementations

} // namespace simple_barrier

} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif // DNNL_CPU_THREADING_RUNTIME == DNNL_RUNTIME_SEQ
// vim: et ts=4 sw=4 cindent cino=+2s,^=l0,\:0,N-s

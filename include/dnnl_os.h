/*******************************************************************************
 * Copyright 2017 NEC Labs America
*
* Licensed under the Apache License, Version 2.0 (the "License"); you may not
* use this file except in compliance with the License.  You may obtain a copy
* of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
* WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
* License for the specific language governing permissions and limitations under
* the License.
*******************************************************************************/
/** \file handle various compiler/os retrictions.
 *
 * \deprecated -- move this into src/common/opt_pragmas
 *
 * - These provide OS workarounds - libc pecularities, - and compiler
 * workarounds - like support for \e restrict or \e alignment - and C++ OpenMP
 * support pragmas.
 *
 * Note: These macros <B>do not</B> begin with "DNNL_", so this file is \b not
 * really part of the public API.  It aids <EM>code readability</EM> by
 * avoiding ugly \#if blocks.
 *
 * You can think of it as settings that are even more common than code in
 * src/common, and it is \e public for re-use when access to the full set of
 * <TT>src/common</TT> is perhaps not required.
 *
 * OpenMP C++ Pragma macros because different different \c DNNL_CPU targets)
 * have widely varying degrees of OpenMP support.
 *
 */
#ifndef DNNL_OS_H
#define DNNL_OS_H

// How can I avoid this, and still be useful ?
#include "dnnl_config.h"        // TODO use DNNL_TARGET_FOO conditionals
// this also includes dnnl_types.h, so DNNL_RUNTIME_CPU build flags are available

//#include "os_common.hpp" // not available -- we use mkldnn public API only.
#if 1
#if defined(__ve)
#define strnlen strnlen_s
#endif

// How is the restrict keyword handled? (disallow it as you encounter errors, please)
#if defined(_SX) // deprecated

#elif defined(__ve)
// restrict is allowed
#ifndef __restrict
#define __restrict restrict /* ve/musl/include/stdlib.h uses __restrict !!! */
#endif

#elif defined(__INTEL_COMPILER) || defined(__GNUC__)
#define restrict /*no-restrict*/

#elif defined(WIN32)
// ???
#else
// ???
#endif // restrict keyword handling

// Any restrictions on the alignas attribute?
#ifdef __ve
#define alignas(x) alignas((x) > 16 ? 16 : (x))
#endif
#endif

// ENABLE_OPT_PRAGMAS
//    set to 0 to debug pragma-related incorrect assumptions
#if !defined(ENABLE_OPT_PRAGMAS)
/** new way to control OpenMP usage is via cmake DNNL_CPU_RUNTIME.
 * Even if you ask for OpenMP, \ref dnnl_os.h must be aware of your compiler to
 * generate the appropriate supported set of directives.
 *
 * \e vim note: adding '\\\\;' is an ugly kludge to avoid next-line indent
 * after prefixing a for loop with an OpenMP macro.
 */
#define ENABLE_OPT_PRAGMAS (DNNL_CPU_THREADING_RUNTIME == DNNL_RUNTIME_OMP)
//#define ENABLE_OPT_PRAGMAS 0 /* may help debug */
//#define ENABLE_OPT_PRAGMAS 1 /* old default (auto-determine) */
#endif

// ENABLE_OMP defaults to 1
#if !defined(ENABLE_OMP)
#if defined(_SX)
#elif defined(__ve) // OMP is not yet supported by ncc/nc++
//#define ENABLE_OMP 0  // at Dec. 25th 2017 release, ncc may support OMP
#elif defined(__INTEL_COMPILER)
#elif defined(__GNUC__)
#else
#endif
#if !defined(ENABLE_OMP)
#define ENABLE_OMP 1
#endif
#endif


// -------- compiler-specific pragmas --------
// __ve compile does something with pragma omp, but it is not officially supported,
// so we use C++11 _Pragma to emit pragmas from macros and customize pragmas to
// particular compilers.
//
// Allocation directives:
//   VREG          : hint that array fits into one simd register
//                   There may be many conditions on array access!
//   ALLOC_ON_VREG : hint that array fits into multiple simd registers
//   ALLOC_ON_ADB  : hint that array should be "cached" in special memory bank.
//
// Loop directives apply to an IMMEDIATELY FOLLOWING loop:
//   ShortLoop : hint that for-loop limit is less than max simd register length
//   RETAIN    : hint that array should be kept accesible (cached)
//   IVDEP     : pretend all ptrs are independent (restrict)
//
// TODO: SX pre-loop macros must be SINGLE ones, because sxcc REQUIRES
//       multiple #pragma cdir to be combined, comma-separated.
//       So you can only use ONE pre-loop macro.  If 2 macros,
//       compiler docs say **both** will be ignored!
//
// FIXME  SX alloc_on_vreg 2nd arg must be a compile-time constant
//
// Oh! ALLOC_ON_VREG cannot "decay" into RETAIN, because syntax is different
// -----------------------------------
#ifdef _MSC_VER // uses __pragma takes an unquoted arg UNTESTED (see z_magic.hpp)
#define BENCHDNN_MPRAGMA(...) __pragma(__VA_ARGS__)
#define BENCHDNN_EVAL(...) __VA_ARGS__
#define PragmaQuote(...) BENCHDNN_MPRAGMA(BENCHDNN_STRINGIZE(__VA_ARGS__))
#else
#define BENCHDNN_MPRAGMA(str) _Pragma(str)
#define BENCHDNN_STRINGIZE(...) #__VA_ARGS__
#define PragmaQuote(...) BENCHDNN_MPRAGMA(BENCHDNN_STRINGIZE(__VA_ARGS__))
#endif

//
// deprecated / unused
//#   define PRAGMASIMD(...) PragmaQuote(simd __VA_ARGS__)
//

#if ENABLE_OPT_PRAGMAS && defined(_SX)
// SX preprocessor generates _Pragma(XXX) and sxc++ might be ignoring
//    *some*, based on failure to produce some warning messages.
//#warning "SX optimization pragmas IN EFFECT"
#   define VREG(...) PragmaQuote(cdir vreg(__VA_ARGS__))
#   define ALLOC_ON_VREG(...) PragmaQuote(cdir alloc_on_vreg(__VA_ARGS__))
#   define ALLOC_ON_ADB(...) PragmaQuote(cdir alloc_on_adb(__VA_ARGS__))
// Is there a pre-for-loop RETAIN for SX? For now, kludge as on_adb.
#   define RETAIN(...) PragmaQuote(cdir on_adb(__VA_ARGS__))
#   define RETAIN1st(var,...) PragmaQuote(cdir on_adb(var))
#   define ShortLoop() _Pragma("cdir shortloop")
#   define ShortLoopTest() /*?*/
#   define IVDEP() _Pragma("cdir nodep")
#   define UNROLL(x)

#elif ENABLE_OPT_PRAGMAS && defined(__ve)
//#   warning "__ve optimization pragmas IN EFFECT"
#   define VREG(...) PragmaQuote(_NEC vreg(__VA_ARGS__))
#   define ALLOC_ON_VREG(...)
#   define ALLOC_ON_ADB(...)
#   define RETAIN(...) PragmaQuote(_NEC retain(__VA_ARGS__))
#   define RETAIN1st(var,...) PragmaQuote(_NEC retain(var))
#   define ShortLoop() _Pragma("_NEC shortloop")
#   define ShortLoopTest() _Pragma("_NEC shortloop_reduction")
#   define IVDEP() _Pragma("_NEC ivdep")
#   define UNROLL(x) PragmaQuote(_NEC unroll(x))
//# define PRAGMA_OMP_SIMD(...) // default: not supported

#elif ENABLE_OPT_PRAGMAS && defined(__INTEL_COMPILER)
// restrict keyword requires the "-restrict" CFLAG; __restrict__ works anyway
#   define restrict __restrict__
#   define IVDEP() _Pragma("ivdep")
#   define UNROLL(x) PragmaQuote(unroll(x))
//  TODO:
#   define VREG(...)
#   define ALLOC_ON_VREG(...)
#   define ALLOC_ON_ADB(...)
#   define RETAIN(...)
#   define ShortLoop()
#   define ShortLoopTest()

#elif ENABLE_OPT_PRAGMAS && defined(_MSC_VER) && !defined(__clang__) && !defined(__INTEL_COMPILER)
//--------------------------------------------
//  taken from MSVC code in mkldnn_thread.hpp
//# warning "MSVC still supports omp 2.0 only"
#   define collapse(x)
#   define simdlen(x)
//#  define PRAGMA_OMP_SIMD(...) ... below
//--------------------------------------------
#   define VREG(...)
#   define ALLOC_ON_VREG(...)
#   define ALLOC_ON_ADB(...)
#   define RETAIN(...)
#   define ShortLoop()
#   define ShortLoopTest()

#elif ENABLE_OPT_PRAGMAS && defined(__GNUC__)
//#warning "__GNUC optimization pragmas IN EFFECT"
#   define VREG(...)
#   define ALLOC_ON_VREG(...)
#   define ALLOC_ON_ADB(...)
#   define RETAIN(...)
#   define ShortLoop()
#   define ShortLoopTest()
#   define IVDEP() _Pragma("GCC ivdep")
#   define UNROLL(x) PragmaQuote(GCC unroll x)

#else /* A new system might begin by ignoring the optimization pragmas */
#   warning "Please check if _Pragma macros can be defined for this platorm"
#   define VREG(...)
#   define ALLOC_ON_VREG(...)
#   define ALLOC_ON_ADB(...)
#   define RETAIN(...)
#   define ShortLoop()
#   define ShortLoopTest()
#   define IVDEP()
#   define UNROLL(x)

#endif

// PRAGMA_OMP_SIMD move up from src/common/dnnl_thread.hpp ? NO !!!
//#if ENABLE_OPT_PRAGMAS
//#   define PRAGMA_OMP(...) PragmaQuote(omp __VA_ARGS__)
//#   if defined(__ve)
//#      warning "__ve enabling #pragma omp"
//#   endif
//#   if defined(_SX) // no support for "simd" pragmas
//#   elif defined(_MSC_VER) && !defined(__clang__) && !defined(__INTEL_COMPILER)
//#      define collapse(x)
//#   else // defined(__GNUC) or intel or ...
//#      if defined(__ve)
//#         warning "__ve enabling #pragma [omp] simd"
//#      endif
//#      define OMPSIMD(...) PragmaQuote(omp simd __VA_ARGS__)
//#      ifndef PRAGMA_OMP_SIMD // original was in mkldnn_thread.hpp
//#         define PRAGMA_OMP_SIMD(...) PragmaQuote(omp simd __VA_ARGS__)
//#      endif
//#   endif
//#endif
#define OMPSIMD(...) ("OMPSIMD deprecated" ^ "use PRAGMA_OMP_SIMD instead")
#define OMP(...) ("OMP deprecated" ^ "use PRAGMA_OMP instead")

//#ifndef PRAGMA_OMP
//#   define PRAGMA_OMP(...)
//#endif
//#ifndef OMPSIMD
//#   define OMPSIMD(...)
//#endif
#ifndef PRAGMASIMD
#   define PRAGMASIMD(...)
#endif

// process simdlen; it is supported for Clang >= 3.9; ICC >= 17.0; GCC >= 6.1
// No support on Windows.
//#if (defined(__clang_major__) \
//        && (__clang_major__ < 3 \
//                || (__clang_major__ == 3 && __clang_minor__ < 9))) \
//        || (defined(__INTEL_COMPILER) && __INTEL_COMPILER < 1700) \
//        || (!defined(__INTEL_COMPILER) && !defined(__clang__) \
//                && (defined(_MSC_VER) || __GNUC__ < 6 \
//                        || (__GNUC__ == 6 && __GNUC_MINOR__ < 1)))
//#define simdlen(x)
//#endif // long simdlen if

//#ifndef OMP
//#   define OMP(...)
//#if defined(REF_LRN_HPP) // mostly ignore: show for cpu_engine compile at least
//#   warning "not enabling #pragma omp (mkldnn_os.h)"
//#endif
//#endif
#define OMP(...) ("OMP macro deprecated" ^ "use PRAGMA_OMP instead")

// vim: et ts=4 sw=4 cindent cino=+2s,^=l0,\:0,N-s
#endif // DNNL_OS_H

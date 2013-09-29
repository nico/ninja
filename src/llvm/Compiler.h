//===-- llvm/Support/Compiler.h - Compiler abstraction support --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines several macros, based on the current compiler.  This allows
// use of compiler-specific features in a way that remains portable.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_COMPILER_H
#define LLVM_SUPPORT_COMPILER_H

#ifndef __has_feature
# define __has_feature(x) 0
#endif

#if (__has_feature(cxx_deleted_functions) \
     || defined(__GXX_EXPERIMENTAL_CXX0X__))
     // No version of MSVC currently supports this.
#define LLVM_DELETED_FUNCTION = delete
#else
#define LLVM_DELETED_FUNCTION
#endif

#if __has_feature(cxx_override_control) \
    || (defined(_MSC_VER) && _MSC_VER >= 1700)
#define LLVM_OVERRIDE override
#else
#define LLVM_OVERRIDE
#endif

#if (__GNUC__ >= 4)
#define LLVM_LIKELY(EXPR) __builtin_expect((bool)(EXPR), true)
#define LLVM_UNLIKELY(EXPR) __builtin_expect((bool)(EXPR), false)
#else
#define LLVM_LIKELY(EXPR) (EXPR)
#define LLVM_UNLIKELY(EXPR) (EXPR)
#endif

#if defined(HAVE_SANITIZER_MSAN_INTERFACE_H)
# include <sanitizer/msan_interface.h>
#else
# define __msan_allocated_memory(p, size)
# define __msan_unpoison(p, size)
#endif

#endif

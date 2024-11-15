/*
 * macros.h
 *
 * Copyright (c) 2024, NLnet Labs. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#ifndef MACROS_H
#define MACROS_H

#if defined __GNUC__
# define have_gnuc(major, minor) \
  ((__GNUC__ > major) || (__GNUC__ == major && __GNUC_MINORE__ >= minor))
#else
# define have_gnuc(major, minor) (0)
#endif

#if defined(__has_attribute)
# define has_attribute(params) __has_attribute(params)
#else
# define has_attribute(params) (0)
#endif

#if defined(__has_builtin)
# define has_builtin(params) __has_builtin(params)
#else
# define has_builtin(params) (0)
#endif

#if has_builtin(__builtin_expect)
# define likely(params) __builtin_expect(!!(params), 1)
# define unlikely(params) __builtin_expect(!!(params), 0)
#else
# define likely(params) (params)
# define unlikely(params) (params)
#endif

#if has_attribute(always_inline) || have_gnuc(3, 1) && ! defined __NO_INLINE__
  // Compilation using GCC 4.2.1 without optimizations fails.
  //   sorry, unimplemented: inlining failed in call to ...
  // GCC 4.1.2 and GCC 4.30 compile forward declared functions annotated
  // with __attribute__((always_inline)) without problems. Test if
  // __NO_INLINE__ is defined and define macro accordingly.
# define always_inline inline __attribute__((always_inline))
#else
# define always_inline inline
#endif

#if has_attribute(noinline) || have_gnuc(2, 96)
# define never_inline __attribute__((noinline))
#else
# define never_inline
#endif

#if has_attribute(nonnull)
# define nonnull(params) __attribute__((__nonnull__ params))
# define nonnull_all __attribute__((__nonnull__))
#else
# define nonnull(params)
# define nonnull_all
#endif

#if has_attribute(warn_unused_result)
# define warn_unused_result __attribute__((warn_unused_result))
#else
# define warn_unused_result
#endif

#endif // MACROS_H

/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_int.h
 *	Internal umbrella header.  Pulls in the headers an internal
 *	source file needs in 90% of cases.
 */

#ifndef XTC_INT_H
#define XTC_INT_H

/*
 * Feature-test macros — must come before any system header.  We aim
 * for POSIX.1-2008 with BSD/illumos extensions enabled, since we
 * touch mmap flags, ucontext, pthread_setname_np, and a handful of
 * other extensions.
 */
#ifndef _XOPEN_SOURCE
# define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE 1
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
# ifndef __BSD_VISIBLE
#  define __BSD_VISIBLE 1
# endif
#endif
#if defined(__sun) || defined(__illumos__)
# ifndef __EXTENSIONS__
#  define __EXTENSIONS__ 1
# endif
#endif

#if defined(__has_include)
# if __has_include("xtc_config.h")
#  include "xtc_config.h"
# endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "xtc.h"
#include "os_atomic.h"
#include "os_alloc.h"
#include "os_thread.h"
#include "os_time.h"
#include "os_cpu.h"

#endif /* XTC_INT_H */

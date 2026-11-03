/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/real/inc/xtc_config.h
 *
 *	The configuration the real-code CBMC harnesses compile the library
 *	sources under: a Linux epoll build with no TLS.  It stands in for
 *	the configure-generated header so the tier needs no build tree.
 *	Only what the harnessed sources test matters; the version macros
 *	are placeholders (no harness checks them).
 */
#ifndef XTC_CONFIG_H
#define XTC_CONFIG_H

#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define XTC_IO_BACKEND_EPOLL 1
#define XTC_TLS_BACKEND_NONE 1
#define XTC_VERSION_MAJOR 0
#define XTC_VERSION_MINOR 0
#define XTC_VERSION_PATCH 0
#define XTC_VERSION_STRING "0.0.0"

#endif /* XTC_CONFIG_H */

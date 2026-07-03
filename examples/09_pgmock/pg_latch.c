/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/09_pgmock/pg_latch.c
 *	Translation-unit anchor for pg_latch.h.  The Latch glue is a
 *	handful of static-inline wrappers over the proc mailbox and
 *	xtc_proc_wait_fd (see pg_latch.h); this file forces one
 *	non-inline emission so the glue compiles as its own object
 *	and links cleanly into the mock and the smoke test.
 */

#include "pg_latch.h"

/* Force emission of the inline wrappers in this TU (also a link-time
 * proof they compile standalone). */
extern int (*pg_latch_force_link)(int, uint32_t, int64_t, uint32_t *);
int (*pg_latch_force_link)(int, uint32_t, int64_t, uint32_t *) =
    pg_wait_latch_or_socket;

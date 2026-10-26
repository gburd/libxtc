/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_sqlxtc_evict.c
 *	DST: test_sim_sqlxtc_reopen's open / close / crash harness with a
 *	buffer pool far smaller than the store.  The crash tests size the
 *	pool so nothing is ever evicted; here ~1 KB rows over a 32-frame
 *	pool make every segment evict dirty pages (written back by the
 *	trickler or the evicting fiber) and reload them, between clean
 *	closes, trusted reopens and crash recoveries.  The oracle is the
 *	same: a page lost or reloaded stale shows as a lost or wrong row,
 *	or as an index that misses rows.
 *
 *	usage: test_sim_sqlxtc_evict [base [count]]   (default 1 6)
 */
#define SIM_FRAMES 32
#define SIM_PAD    1000
#define SIM_NAME   "eviction (32-frame pool)"
#include "test_sim_sqlxtc_reopen.c"

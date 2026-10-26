/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_sqlxtc_gc.c
 *	DST: version GC and pruning with snapshots held open across them.
 *	test_sim_sqlxtc_reopen's harness (included) plus a reader fiber that
 *	holds snapshot transactions open across the lanes' commits and a
 *	collector fiber that runs xstore_gc; the lanes' writes prune on
 *	write (autovacuum is on in every sx_storage_open).  Each snapshot
 *	reads the table, waits while UPDATEs and DELETEs commit and the
 *	horizon moves, and reads it again by scan and through the index:
 *	it must read exactly its versions.  Crash cuts and clean closes are
 *	the reopen tier's, so pruned / collected state also survives
 *	recovery and reopen against the oracle.
 *
 *	usage: test_sim_sqlxtc_gc [base [count]]   (default 1 6)
 */
#define SIM_GC     1
#define SIM_NAME   "version GC with snapshots held"
#define SIM_TEST   "test_sim_sqlxtc_gc"
#include "test_sim_sqlxtc_reopen.c"

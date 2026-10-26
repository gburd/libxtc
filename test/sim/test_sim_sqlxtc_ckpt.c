/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_sqlxtc_ckpt.c
 *	DST: the online log checkpoint (xstore_checkpoint_wal_online, the
 *	wal_ockpt_* phases) while commits continue, with crash cuts landing
 *	in and after it.  test_sim_sqlxtc_reopen's harness (included) plus a
 *	checkpointer fiber that runs online checkpoints back to back on the
 *	writer's loop while the lanes commit.  Buggify yields between the
 *	checkpoint's phases (xstore.ockpt.dump / .catchup / .finish) let
 *	commits and the seeded crash land between the quiesced begin, the
 *	dump, each catch-up round and the quiesced finish (copy, fsync,
 *	rename, rebind).  The oracle is the reopen tier's: after any cut,
 *	crash recovery from the (possibly just-swapped) log must restore
 *	every acked op exactly.
 *
 *	usage: test_sim_sqlxtc_ckpt [base [count]]   (default 1 6)
 */
#define SIM_CKPT   1
#define SIM_PAD    200
#define SIM_NAME   "online checkpoint under commits"
#define SIM_TEST   "test_sim_sqlxtc_ckpt"
#include "test_sim_sqlxtc_reopen.c"

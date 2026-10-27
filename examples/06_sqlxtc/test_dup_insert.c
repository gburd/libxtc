/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_dup_insert.c
 *	Concurrent autocommit INSERTs of the same primary key.  NCLI procs,
 *	each with its own connection, meet at a barrier and then all INSERT
 *	key k (value = client id), for KEYS keys, on 1 and 4 loops.  Every
 *	commit parks on the group-commit fsync, so all of them are in
 *	flight together.  Exactly one INSERT per key may succeed; the
 *	others must fail, and the row must hold the winner's value.
 *	Before the fix the uniqueness probe read only committed rows and
 *	the write skipped the first-committer-wins check: every client was
 *	acknowledged and all but one of the rows was silently overwritten.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "engine.h"
#include "wal.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NCLI 8
#define KEYS 100

static bt_t        *g_bt;
static wal_t       *g_wal;
static sx_db       *g_c[NCLI];
static _Atomic int  g_bar, g_left;
static _Atomic long g_ok[KEYS];          /* acked INSERTs per key */
static _Atomic long g_winner[KEYS];      /* client id of the last ack */
static _Atomic long g_errs, g_other;

static void
barrier(int *k)
{
	int target = NCLI * ++(*k);
	atomic_fetch_add(&g_bar, 1);
	while (atomic_load(&g_bar) < target)
		(void)xtc_proc_sleep(20LL * 1000);
}

static void
worker(void *arg)
{
	long me = (long)arg;
	char q[96];
	const char *err;
	int key, k = 0;
	for (key = 0; key < KEYS; key++) {
		barrier(&k);
		snprintf(q, sizeof q, "INSERT INTO d VALUES(%d,%ld)", key, me);
		if (sx_exec(g_c[me], q, NULL) == SX_OK) {
			atomic_fetch_add(&g_ok[key], 1);
			atomic_store(&g_winner[key], me);
		} else {
			atomic_fetch_add(&g_errs, 1);
			err = sx_errmsg(g_c[me]);
			if (err == NULL || strstr(err, "UNIQUE constraint failed: d.id") == NULL)
				atomic_fetch_add(&g_other, 1);
		}
	}
	if (atomic_fetch_sub(&g_left, 1) == 1)
		(void)wal_writer_stop(g_wal);
}

int
main(void)
{
	char logp[256], pg[256];
	wal_opts_t wo = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	xtc_exec_t *ex = NULL;
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "dupins" };
	xtc_pid_t pid;
	sx_stmt *st;
	long multi, lost, zero, rows;
	int fd, nloops, key;
	long i;

	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	for (nloops = 1; nloops <= 4; nloops += 3) {
		t_tmpl(logp, sizeof logp, "sqlxtc-dupins-log");
		t_tmpl(pg, sizeof pg, "sqlxtc-dupins-pg");
		if ((fd = mkstemp(logp)) < 0) return 1;
		close(fd);
		if ((fd = mkstemp(pg)) < 0) return 1;
		close(fd);
		wo.path = logp;
		if (wal_open(&wo, &g_wal) != XTC_OK) return 1;
		bo.path = pg; bo.page_size = 4096; bo.n_frames = 256;
		if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &g_bt) != XTC_OK)
			return 1;
		xstore_set_wal((struct wal *)g_wal);
		for (i = 0; i < NCLI; i++)
			if (sx_open_bt(g_bt, &g_c[i]) != SX_OK) return 1;
		CK(sx_exec(g_c[0], "CREATE TABLE d(id INTEGER PRIMARY KEY, w INTEGER)", NULL) == SX_OK);
		atomic_store(&g_bar, 0);
		atomic_store(&g_left, NCLI);
		atomic_store(&g_errs, 0);
		atomic_store(&g_other, 0);
		for (key = 0; key < KEYS; key++) {
			atomic_store(&g_ok[key], 0);
			atomic_store(&g_winner[key], -1);
		}
		if (xtc_exec_init(&ex, nloops) != XTC_OK) return 1;
		if (wal_writer_spawn(g_wal, xtc_exec_loop(ex, 0), NULL) != XTC_OK) return 1;
		for (i = 0; i < NCLI; i++)
			if (xtc_proc_spawn(xtc_exec_loop(ex, (int)(i % nloops)), worker,
			    (void *)i, &po, &pid) != XTC_OK)
				return 1;
		if (xtc_exec_run(ex) != XTC_OK) return 1;
		(void)xtc_exec_fini(ex);

		multi = lost = zero = rows = 0;
		for (key = 0; key < KEYS; key++) {
			char q[64];
			long got = -2;
			snprintf(q, sizeof q, "SELECT w FROM d WHERE id=%d", key);
			if (sx_prepare(g_c[0], q, -1, &st, NULL) == SX_OK) {
				if (sx_step(st) == SX_ROW) {
					got = (long)sx_column_int64(st, 0);
					rows++;
				}
				sx_finalize(st);
			}
			if (atomic_load(&g_ok[key]) > 1) multi++;
			if (atomic_load(&g_ok[key]) == 0) zero++;
			if (got != atomic_load(&g_winner[key])) lost++;
		}
		printf("  [%d loop%s] keys=%d clients=%d keys-with->1-ack=%ld "
		    "keys-with-0-acks=%ld acked-value-not-stored=%ld rows=%ld "
		    "errors=%ld (non-unique-msg %ld)\n", nloops,
		    nloops > 1 ? "s" : "", KEYS, NCLI, multi, zero, lost, rows,
		    atomic_load(&g_errs), atomic_load(&g_other));
		CK(multi == 0);
		CK(zero == 0);
		CK(lost == 0);
		CK(rows == KEYS);
		CK(atomic_load(&g_errs) == (long)KEYS * (NCLI - 1));
		CK(atomic_load(&g_other) == 0);
		for (i = 0; i < NCLI; i++) sx_close(g_c[i]);
		xstore_set_wal(NULL);
		bt_close(g_bt);
		bm_destroy(bm);
		wal_close(g_wal);
		unlink(logp); unlink(pg);
	}
	if (g_fail) { fprintf(stderr, "FAIL: test_dup_insert\n"); return 1; }
	printf("  ok   concurrent same-key autocommit INSERTs: exactly one wins\n");
	return 0;
}

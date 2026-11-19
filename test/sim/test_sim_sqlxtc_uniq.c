/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_sqlxtc_uniq.c
 *	DST: a UNIQUE column holds over keys of every length, under
 *	concurrent writers, and an index lookup finds every row.
 *
 *	Per seed, inside xtc_sim_exec_run on N_LOOPS loops: a table with a
 *	UNIQUE TEXT column and a plain index on the same column.  NW writer
 *	procs, each with its own connection, insert values drawn from a
 *	small seeded pool, so most INSERTs collide with another writer's.
 *	The pool mixes short values, values just over bt_max_key (497 on
 *	4 KiB pages) and up to 3000 bytes, and pairs that share their first
 *	490 bytes, so long values meet both duplicates and near-duplicates.
 *	Half the writes run in BEGIN..COMMIT (the commit-time re-check),
 *	half autocommit; some UPDATE an existing row to a pool value; each
 *	writer parks a seeded few microseconds between statements.  Before
 *	one write in eight a writer arms xstore_uniq_fail_inject(1), so the
 *	next UNIQUE probe of the committed index (any writer's) fails as a
 *	page read can.  A writer that sees "UNIQUE constraint failed: t.s"
 *	or "index error: cannot check UNIQUE t.s" carries on (the write was
 *	refused); any other error fails the run.
 *
 *	Invariants (each a FAIL line naming the seed):
 *	  UNIQUE   no two live rows carry one value (counted by a table scan
 *	           with the index defeated);
 *	  INDEX    for every pool value, its count through the index equals
 *	           its count by the scan, and xstore_index_check reports no
 *	           entry missing;
 *	  REPLAY   the same seed gives the same result text.
 *
 *	Planted bugs (scripts/dst-bug-inject.sh): 72 (UNIQPROBE) reads a
 *	failed UNIQUE probe as "no duplicate", as 2.2.0 did, so a long
 *	duplicate is accepted; 73 (LONGKEY) skips the entry of a key
 *	longer than one B-tree key, as 2.2.0 did, so the index misses rows.
 *	usage: test_sim_sqlxtc_uniq [base [count]]   (default 1 8)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_runtime.h"
#include "sim_int.h"

#include "bufmgr.h"
#include "btree.h"
#include "engine.h"
#include "xstore.h"

#define N_LOOPS 3
#define NW 4     /* writer procs */
#define NOPS 24  /* statements per writer */
#define NPOOL 10 /* distinct values */
#define VMAX 3000
#define OUTCAP 2048

static bt_t *g_bt;
static char g_pool[NPOOL][VMAX + 1];
static char g_out[OUTCAP];
static char g_why[256];
static int g_left, g_ok;
static long g_dups; /* UNIQUE refusals seen */
static long g_perr; /* writes refused by a failed probe */

/* Value i of the pool: a short one, or 498..3000 bytes of a letter with
 * a tag at the end (so values 2k and 2k + 1 of one length share all but
 * their last 8 bytes). */
static void
pool_make(void)
{
	static const int len[NPOOL] = { 3, 7, 498, 498, 699, 699, 1200, 1200,
		3000, 3000 };
	int i;
	for (i = 0; i < NPOOL; i++) {
		if (len[i] < 8) {
			snprintf(g_pool[i], sizeof g_pool[i], "s%d", i);
			continue;
		}
		memset(g_pool[i], 'a' + i / 2, (size_t)len[i]);
		snprintf(g_pool[i] + len[i] - 8, 9, "%08d", i);
	}
}

static uint64_t
draw(uint64_t n)
{
	return __xtc_sim_rng_range(XTC_SIM_RNG_APP, n);
}

/* Run sql with up to two binds (an int64 k, then a pool value v; -1 for
 * none).  Returns the step's code. */
static int
run(sx_db *h, const char *sql, int64_t k, int v)
{
	sx_stmt *st = NULL;
	int rc, b = 1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -1;
	if (v >= 0)
		sx_bind_text(st, b++, g_pool[v], -1);
	if (k >= 0)
		sx_bind_int64(st, b, k);
	rc = sx_step(st);
	sx_finalize(st);
	return rc;
}

static int64_t
count_q(sx_db *h, const char *sql, int v)
{
	sx_stmt *st = NULL;
	int64_t n = -1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	sx_bind_text(st, 1, g_pool[v], -1);
	if (sx_step(st) == SX_ROW)
		n = sx_column_int64(st, 0);
	sx_finalize(st);
	return n;
}

/* A failed statement is fine only if it is a UNIQUE refusal. */
static int
uniq_or_fail(sx_db *h, int rc, const char *what)
{
	const char *m = sx_errmsg(h);
	if (rc == SX_DONE || rc == SX_OK)
		return 0;
	if (m != NULL && strstr(m, "UNIQUE constraint failed: t.s") != NULL) {
		g_dups++;
		return 0;
	}
	if (m != NULL &&
	    strstr(m, "index error: cannot check UNIQUE t.s") != NULL) {
		g_perr++;
		return 0;
	}
	if (g_why[0] == '\0')
		snprintf(g_why, sizeof g_why, "%s failed: rc %d \"%s\"", what,
		    rc, m != NULL ? m : "");
	return -1;
}

static void
writer(void *arg)
{
	long me = (long)arg;
	sx_db *h = NULL;
	int i, v, rc, txn;
	int64_t k;
	if (sx_open_bt(g_bt, &h) != SX_OK)
		goto out;
	for (i = 0; i < NOPS && g_why[0] == '\0'; i++) {
		v = (int)draw(NPOOL);
		k = me * 1000 + i;
		txn = (int)draw(2);
		if (txn && sx_exec(h, "BEGIN", NULL) != SX_OK)
			break;
		if (draw(8) == 0)
			xstore_uniq_fail_inject(1);
		if (draw(4) == 0)
			rc = run(h, "UPDATE t SET s = ? WHERE k = ?",
			    (me * 1000) + (int64_t)draw((uint64_t)i + 1), v);
		else
			rc = run(h, "INSERT INTO t(s, k) VALUES(?, ?)", k, v);
		if (uniq_or_fail(h, rc, "write") != 0)
			break;
		(void)xtc_proc_sleep((int64_t)(1 + draw(300)) * 1000);
		if (txn) {
			rc = sx_exec(h, rc == SX_DONE ? "COMMIT" : "ROLLBACK",
			    NULL);
			if (uniq_or_fail(h, rc, "COMMIT") != 0)
				break;
		}
	}
out:
	if (h != NULL)
		sx_close(h);
	xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst);
}

/* After the writers: the invariants, and the result text. */
static void
checker(void *arg)
{
	xtc_exec_t *e = arg;
	sx_db *h = NULL;
	uint64_t miss = 0, stale = 0;
	int64_t a, b;
	size_t l;
	int v;
	while (xtc_atomic_load_i32(&g_left, xtc_mo_seq_cst) > 0)
		(void)xtc_proc_sleep(1000LL * 1000);
	if (g_why[0] != '\0' || sx_open_bt(g_bt, &h) != SX_OK)
		goto out;
	for (v = 0; v < NPOOL; v++) {
		a = count_q(h, "SELECT count(*) FROM t WHERE s = ?", v);
		b = count_q(h, "SELECT count(*) FROM t WHERE s || '' = ?", v);
		if (b > 1) {
			snprintf(g_why, sizeof g_why,
			    "UNIQUE: value %d (%d bytes) "
			    "is held by %lld rows",
			    v, (int)strlen(g_pool[v]), (long long)b);
			goto out;
		}
		if (a != b) {
			snprintf(g_why, sizeof g_why,
			    "INDEX: value %d (%d bytes): "
			    "index %lld, scan %lld",
			    v, (int)strlen(g_pool[v]), (long long)a,
			    (long long)b);
			goto out;
		}
		l = strlen(g_out);
		snprintf(g_out + l, OUTCAP - l, "%lld,", (long long)b);
	}
	(void)xstore_index_check(g_bt, &miss, &stale);
	if (miss != 0) {
		snprintf(g_why, sizeof g_why,
		    "INDEX: xstore_index_check: %llu "
		    "entries missing",
		    (unsigned long long)miss);
		goto out;
	}
	l = strlen(g_out);
	snprintf(g_out + l, OUTCAP - l, "d%ld,e%ld", g_dups, g_perr);
	g_ok = 1;
out:
	if (h != NULL) {
		(void)sx_exec(h, "DROP TABLE IF EXISTS t", NULL);
		sx_close(h);
	}
	(void)xtc_exec_stop(e);
}

static void
setup(void *arg)
{
	xtc_exec_t *e = arg;
	sx_db *h = NULL;
	long i;
	if (sx_open_bt(g_bt, &h) != SX_OK ||
	    sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, s TEXT UNIQUE)",
	        NULL) != SX_OK ||
	    sx_exec(h, "CREATE INDEX t_s ON t(s)", NULL) != SX_OK) {
		snprintf(g_why, sizeof g_why, "create failed");
		if (h != NULL)
			sx_close(h);
		(void)xtc_exec_stop(e);
		return;
	}
	sx_close(h);
	xtc_atomic_store_i32(&g_left, NW, xtc_mo_seq_cst);
	for (i = 0; i < NW; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (int)(i % N_LOOPS)),
		    writer, (void *)i, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), checker, e, NULL, NULL);
}

static int
run_one(uint64_t seed, char *out)
{
	xtc_exec_t *e = NULL;
	int rc;
	g_out[0] = '\0';
	g_why[0] = '\0';
	g_ok = 0;
	g_dups = g_perr = 0;
	if (xtc_exec_create(&(xtc_exec_opts_t) { .size =
	                                             sizeof(xtc_exec_opts_t),
	                        .n_loops = N_LOOPS },
	        &e) != XTC_OK)
		return -1;
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), setup, e, NULL, NULL);
	rc = xtc_sim_exec_run(e, seed, 50000000);
	xstore_uniq_fail_inject(0); /* an armed failure outlived the run */
	xtc_exec_destroy(e);
	snprintf(out, OUTCAP, "%s", g_out);
	return (rc == XTC_OK && g_ok) ? 0 : -1;
}

/* A fresh, empty tree in the same page file, and the commit clock back
 * at its start (see test_sim_sqlxtc_gcidx). */
static int
fresh(bm_t **bm, bm_opts_t *bo)
{
	if (g_bt != NULL)
		bt_close(g_bt);
	if (*bm != NULL)
		bm_destroy(*bm);
	g_bt = NULL;
	*bm = NULL;
	xstore_clock_reset();
	bo->reopen = 0;
	if (truncate(bo->path, 0) != 0 || bm_create(bo, bm) != XTC_OK ||
	    bt_open(*bm, &g_bt) != XTC_OK)
		return -1;
	return 0;
}

int
main(int argc, char **argv)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	char btf[512];
	const char *td = getenv("TMPDIR");
	static char a[OUTCAP], b[OUTCAP];
	uint64_t base = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
	uint64_t count = argc > 2 ? strtoull(argv[2], NULL, 0) : 8, s;
	int fd, fails = 0;

	/* SQL on a fiber needs sqlxtc's 512 KiB stack (main.c). */
	if (xtc_runtime_set_stack_size(512u * 1024u) != XTC_OK)
		return 1;
	snprintf(btf, sizeof btf, "%s/xtc-uniq-bt-XXXXXX",
	    td != NULL && td[0] != '\0' ? td : "/tmp");
	fd = mkstemp(btf);
	if (fd < 0)
		return 1;
	close(fd);
	pool_make();
	bo.path = btf;
	bo.page_size = 4096;
	bo.n_frames = 512;
	for (s = base; s < base + count; s++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull * (s + 1);
		int ra, rb;
		ra = fresh(&bm, &bo) != 0 ? -1 : run_one(seed, a);
		rb = ra != 0 || fresh(&bm, &bo) != 0 ? -1 : run_one(seed, b);
		if (ra != 0 || rb != 0) {
			printf("  FAIL: seed=%llu (0x%016llx): %s\n",
			    (unsigned long long)s, (unsigned long long)seed,
			    g_why[0] ? g_why : "run error or guard trip");
			printf("  corpus row: test_sim_sqlxtc_uniq %llu 1 "
			       "<description, fixing commit>\n",
			    (unsigned long long)s);
			fails++;
			continue;
		}
		if (strcmp(a, b) != 0) {
			printf(
			    "  FAIL: seed=%llu: REPLAY: two runs differ (%s vs "
			    "%s)\n",
			    (unsigned long long)s, a, b);
			fails++;
		}
	}
	if (g_bt != NULL)
		bt_close(g_bt);
	if (bm != NULL)
		bm_destroy(bm);
	unlink(btf);
	if (fails) {
		printf("FAIL: %d of %llu seeds\n", fails,
		    (unsigned long long)count);
		return 1;
	}
	printf("OK: UNIQUE holds and the index finds every row over keys of "
	       "3..3000 bytes -- %llu seeds, %d writers, replay clean\n",
	    (unsigned long long)count, NW);
	return 0;
}

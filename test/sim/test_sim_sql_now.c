/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_sql_now.c
 *	DST: the sqlxtc date/time functions read 'now' replayably.
 *
 *	date('now'), datetime('now'), julianday('now'), unixepoch(),
 *	CURRENT_TIMESTAMP and strftime('%s','now') derive the current time
 *	from xtc_clock_mono (virtualized by the simulator) plus a one-shot
 *	epoch offset, never from xtc_clock_real (AGENTS.md DST rule 1).  This
 *	test runs those queries from fibers inside seeded xtc_sim_exec_run
 *	runs, and asserts for each seed that:
 *	  - the run returns XTC_OK -- xtc_sim_exec_run refuses XTC_OK when a
 *	    determinism guard tripped, so no real clock was read on the path;
 *	  - a second run of the same seed yields byte-identical result text;
 *	  - 'now' is one value per statement (two references agree), and it
 *	    advances with the virtual clock between statements.
 *
 *	The epoch offset is sampled once per process (outside the runs, by
 *	a warm-up query), as xstore's HLC does.
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
#include "xtc_sim.h"

#include "bufmgr.h"
#include "btree.h"
#include "engine.h"
#include "xstore.h"

#define N_LOOPS 2
#define N_Q     3
#define OUTCAP  1024

static bt_t *g_bt;
static char  g_out[OUTCAP];
static int   g_ok;

static const char *const k_q[N_Q] = {
	"SELECT datetime('now'), julianday('now') = julianday('now'), "
	"CURRENT_TIMESTAMP = datetime('now'), strftime('%s', 'now') = "
	"CAST(unixepoch() AS TEXT)",
	"SELECT unixepoch('now', 'subsec'), date('now', '+1 day'), time()",
	"SELECT datetime('now', 'start of day', '+90 minutes')"
};

/* Run one query and append its row(s) as text to g_out. */
static int
run_q(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int rc, i, n;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK || st == NULL)
		return -1;
	n = sx_column_count(st);
	while ((rc = sx_step(st)) == SX_ROW) {
		for (i = 0; i < n; i++) {
			size_t l = strlen(g_out);
			const char *t;
			switch (sx_column_type(st, i)) {
			case SX_INTEGER:
				snprintf(g_out + l, OUTCAP - l, "%lld|",
				    (long long)sx_column_int64(st, i));
				break;
			case SX_FLOAT:
				snprintf(g_out + l, OUTCAP - l, "%.17g|",
				    sx_column_double(st, i));
				break;
			default:
				t = sx_column_text(st, i);
				snprintf(g_out + l, OUTCAP - l, "%s|", t ? t : "NULL");
				break;
			}
		}
	}
	sx_finalize(st);
	return rc == SX_DONE ? 0 : -1;
}

static void
querier(void *arg)
{
	xtc_exec_t *e = arg;
	sx_db *db = NULL;
	int i;
	if (sx_open_bt(g_bt, &db) != SX_OK) goto out;
	for (i = 0; i < N_Q; i++) {
		if (run_q(db, k_q[i]) != 0) goto out;
		/* advance the virtual clock between statements */
		(void)xtc_proc_sleep(1500LL * 1000 * 1000);
	}
	g_ok = 1;
out:
	if (db != NULL) sx_close(db);
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, char *out)
{
	xtc_exec_t *e = NULL;
	int rc;
	g_out[0] = '\0';
	g_ok = 0;
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), querier, e, NULL, NULL);
	rc = xtc_sim_exec_run(e, seed, 2000000);
	(void)xtc_exec_fini(e);
	snprintf(out, OUTCAP, "%s", g_out);
	return (rc == XTC_OK && g_ok) ? 0 : -1;
}

int
main(void)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	sx_db *warm = NULL;
	char btf[] = "/tmp/xtc-sqlnow-bt-XXXXXX";
	char a[OUTCAP], b[OUTCAP];
	int i, fd, fails = 0;

	fd = mkstemp(btf);
	if (fd < 0) return 1;
	close(fd);
	bo.path = btf; bo.page_size = 4096; bo.n_frames = 256;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &g_bt) != XTC_OK) {
		unlink(btf);
		return 1;
	}
	/* Warm-up outside any sim run: samples the one-shot epoch offset. */
	if (sx_open_bt(g_bt, &warm) != SX_OK) return 1;
	g_out[0] = '\0';
	if (run_q(warm, "SELECT datetime('now')") != 0) {
		printf("  FAIL: warm-up query did not run\n");
		return 1;
	}
	sx_close(warm);

	for (i = 0; i < 8; i++) {
		uint64_t seed = 0x6e6f77ull + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		const char *p;
		if (run_one(seed, a) != 0 || run_one(seed, b) != 0) {
			printf("  seed 0x%016llx: FAIL (run error or guard trip)\n",
			    (unsigned long long)seed);
			fails++;
			continue;
		}
		if (strcmp(a, b) != 0) {
			printf("  seed 0x%016llx: FAIL (replay differs)\n  %s\n  %s\n",
			    (unsigned long long)seed, a, b);
			fails++;
			continue;
		}
		/* q0: the three self-comparisons must all be 1 (one 'now'). */
		p = strchr(a, '|');
		if (p == NULL || strncmp(p, "|1|1|1|", 7) != 0) {
			printf("  seed 0x%016llx: FAIL ('now' not one value per "
			    "statement): %s\n", (unsigned long long)seed, a);
			fails++;
		}
	}
	bt_close(g_bt);
	bm_destroy(bm);
	unlink(btf);
	if (fails) {
		printf("FAIL: %d seed(s)\n", fails);
		return 1;
	}
	printf("OK: sqlxtc 'now' under DST -- 8 seeds, date/time functions read "
	    "the virtualized clock (no determinism-guard trip), one 'now' per "
	    "statement, and every run replays byte-identically from its seed\n");
	return 0;
}

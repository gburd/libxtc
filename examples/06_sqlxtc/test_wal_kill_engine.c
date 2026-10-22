/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_kill_engine.c
 *	Kill fibers inside COMMIT through the whole engine (sx_exec ->
 *	xs_commit_ctx -> xs_wal_log -> wal_commit), with the background
 *	storage procs running and periodic checkpoints on.
 *
 *	NC connection procs across NL loops each insert rows (slot, seq)
 *	in explicit transactions; a boss kills a random one inside COMMIT
 *	(or anywhere) every few hundred microseconds and respawns it.  The
 *	watchdog fails the run when nothing commits for HANG_S seconds.
 *	Then the boss commits again, the engine is abandoned (a crash), and
 *	reopened: every row whose COMMIT returned SX_OK must be there.
 *
 *	usage: test_wal_kill_engine [ms [seed [any|mask]]]
 *	  any: kill anywhere, not only inside COMMIT; mask: COMMIT runs under
 *	  xtc_mask_enter (a consumer-side workaround, for comparison)
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "wal.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NL      4
#define NC      16
#define MAXSEQ  (1u << 16)
#define HANG_S  8

struct slot {
	_Atomic int      phase;      /* 1 inside COMMIT */
	_Atomic uint32_t seq;
	xtc_pid_t        pid;
	int              live;
	uint8_t         *acked;      /* acked[seq] = 1 once COMMIT returned OK */
};

static char             g_path[256];
static xtc_exec_t      *g_exec;
static struct slot      g_slot[NC + 1];
static _Atomic int      g_stop, g_done, g_final_ok;
static _Atomic uint64_t g_commits, g_kills, g_errs;
static uint64_t         g_seed;
static int64_t          g_ms;
static int              g_any, g_mask;

static uint64_t
rng(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

/* One transaction of two rows for `slot`.  Returns sx_exec's COMMIT rc,
 * or -1 if it never reached COMMIT. */
static int
txn(sx_db *h, int slot)
{
	struct slot *s = &g_slot[slot];
	uint32_t seq = atomic_fetch_add(&s->seq, 1);
	char sql[128];
	int rc;
	if (seq >= MAXSEQ)
		return -2;
	if (sx_exec(h, "BEGIN", NULL) != SX_OK)
		return -1;
	snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%lld, %u, 'a%u')",
	    (long long)slot * MAXSEQ * 2 + seq * 2, seq, seq);
	if (sx_exec(h, sql, NULL) != SX_OK)
		goto bad;
	snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%lld, %u, 'b%u')",
	    (long long)slot * MAXSEQ * 2 + seq * 2 + 1, seq, seq);
	if (sx_exec(h, sql, NULL) != SX_OK)
		goto bad;
	if (g_mask)
		(void)xtc_mask_enter();      /* diagnostic: COMMIT unkillable */
	atomic_store(&s->phase, 1);
	rc = sx_exec(h, "COMMIT", NULL);
	atomic_store(&s->phase, 0);
	if (rc == SX_OK)
		s->acked[seq] = 1;          /* before a latched kill unwinds */
	if (g_mask)
		(void)xtc_mask_leave();
	if (rc == SX_OK) {
		atomic_fetch_add(&g_commits, 1);
	} else if (rc != SX_BUSY) {
		atomic_fetch_add(&g_errs, 1);
	}
	return rc;
bad:
	(void)sx_exec(h, "ROLLBACK", NULL);
	atomic_fetch_add(&g_errs, 1);
	return -1;
}

static void
close_db(void *arg)
{
	sx_close(arg);
}

static void
conn(void *arg)
{
	int slot = (int)(intptr_t)arg;
	sx_db *h = NULL;
	if (sx_open(":memory:", &h) != SX_OK) {
		atomic_fetch_add(&g_errs, 1);
		return;
	}
	/* The connection proc owns the handle; a kill unwinds past the
	 * close, as the server's dropped connection would. */
	(void)xtc_proc_at_exit(close_db, h);
	while (!atomic_load(&g_stop))
		if (txn(h, slot) == -2)
			break;
}

static void *
watchdog(void *arg)
{
	uint64_t last = 0;
	int64_t since = xtc_clock_mono();
	(void)arg;
	while (!atomic_load(&g_done)) {
		uint64_t c = atomic_load(&g_commits);
		int64_t now = xtc_clock_mono();
		int i;
		if (c != last) {
			last = c;
			since = now;
		} else if (now - since > (int64_t)HANG_S * 1000000000LL) {
			struct wal *w = sx_storage_wal();
			fprintf(stderr, "HANG (seed %llu): no commit for %d s; commits "
			    "%llu kills %llu durable_lsn %llu\n",
			    (unsigned long long)g_seed, HANG_S,
			    (unsigned long long)c,
			    (unsigned long long)atomic_load(&g_kills),
			    (unsigned long long)(w ? wal_durable_lsn(w) : 0));
			for (i = 0; i <= NC; i++)
				if (atomic_load(&g_slot[i].phase))
					fprintf(stderr, "  slot %d inside COMMIT\n", i);
			fprintf(stderr, "FAIL: test_wal_kill_engine: hang\n");
			if (getenv("WAL_KILL_ABORT") != NULL)
				abort();
			_exit(3);
		}
		(void)xtc_sleep_ns(50LL * 1000 * 1000);
	}
	return NULL;
}

static int
spawn_slot(int i)
{
	xtc_proc_opts_t o = { .name = "conn" };
	uint64_t ref;
	if (xtc_proc_spawn_monitor(xtc_exec_loop(g_exec, i % NL), conn,
	    (void *)(intptr_t)i, &o, &g_slot[i].pid, &ref) != XTC_OK)
		return -1;
	g_slot[i].live = 1;
	return 0;
}

static int
reap(int respawn)
{
	void *m;
	size_t n;
	int i, live = 0;
	while (xtc_recv(&m, &n, 0) == XTC_OK) {
		xtc_pid_t who;
		if (m != NULL && xtc_down_decode(m, n, &who, NULL) == XTC_OK)
			for (i = 0; i < NC; i++)
				if (g_slot[i].live && xtc_pid_eq(g_slot[i].pid, who)) {
					g_slot[i].live = 0;
					if (respawn)
						(void)spawn_slot(i);
				}
		xtc_free(m);
	}
	for (i = 0; i < NC; i++)
		live += g_slot[i].live;
	return live;
}

static void
boss(void *arg)
{
	uint64_t r = g_seed;
	int64_t end = xtc_clock_mono() + g_ms * 1000000LL;
	sx_db *h = NULL;
	int i, ok = 0;

	(void)arg;
	for (i = 0; i < NC; i++)
		(void)spawn_slot(i);
	while (xtc_clock_mono() < end) {
		int k = (int)(rng(&r) % NC);
		(void)xtc_proc_sleep((int64_t)(200 + rng(&r) % 800) * 1000);
		(void)reap(1);
		if (!g_slot[k].live || (!g_any && !atomic_load(&g_slot[k].phase)))
			continue;
		if (xtc_exit_pid(g_slot[k].pid, 7) == XTC_OK)
			atomic_fetch_add(&g_kills, 1);
	}
	atomic_store(&g_stop, 1);
	while (reap(0) > 0)
		(void)xtc_proc_sleep(1000LL * 1000);
	if (sx_open(":memory:", &h) == SX_OK) {
		for (i = 0; i < 20; i++)
			ok += txn(h, NC) == SX_OK;
		sx_close(h);
	}
	atomic_store(&g_final_ok, ok == 20);
	sx_storage_quiesce();
}

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

int
main(int argc, char **argv)
{
	const char *sfx[] = { "", "-wal", ".dwb", "-wal.ocompact", "-wal.compact" };
	char side[320], sql[128];
	xtc_proc_opts_t bo = { .name = "boss" };
	xtc_pid_t bp;
	pthread_t wd;
	sx_db *h = NULL;
	uint64_t acked = 0, lost = 0;
	int fd, i, fail = 0;
	uint32_t q;

	g_ms = argc > 1 ? atol(argv[1]) : 1500;
	g_seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x5eed;
	g_any = argc > 3 && strcmp(argv[3], "any") == 0;
	g_mask = argc > 3 && strcmp(argv[3], "mask") == 0;
	if (g_seed == 0)
		g_seed = 1;
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK)
		return 1;
	for (i = 0; i <= NC; i++)
		if ((g_slot[i].acked = calloc(MAXSEQ, 1)) == NULL)
			return 1;
	t_tmpl(g_path, sizeof g_path, "sqlxtc-walkill-eng");
	if ((fd = mkstemp(g_path)) < 0)
		return 1;
	close(fd);
	unlink(g_path);
	if (sx_init() != SX_OK)
		return 1;
	sx_storage_set_checkpoint(4ull << 20);   /* checkpoints during the run */
	if (sx_storage_open(g_path, 4096) != SX_OK || sx_open(":memory:", &h) != SX_OK)
		return 1;
	if (sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, seq INTEGER, v TEXT)",
	    NULL) != SX_OK)
		return 1;
	sx_close(h);
	if (xtc_exec_init(&g_exec, NL) != XTC_OK ||
	    sx_storage_run(xtc_exec_loop(g_exec, 0)) != SX_OK ||
	    xtc_proc_spawn(xtc_exec_loop(g_exec, 0), boss, NULL, &bo, &bp) != XTC_OK)
		return 1;
	(void)pthread_create(&wd, NULL, watchdog, NULL);
	(void)xtc_exec_run(g_exec);
	atomic_store(&g_done, 1);
	(void)pthread_join(wd, NULL);
	(void)xtc_exec_fini(g_exec);

	sx_storage_abandon();                    /* crash */
	if (sx_storage_open(g_path, 4096) != SX_OK || sx_open(":memory:", &h) != SX_OK)
		return 1;
	for (i = 0; i <= NC; i++)
		for (q = 0; q < MAXSEQ; q++) {
			if (!g_slot[i].acked[q])
				continue;
			acked++;
			snprintf(sql, sizeof sql, "SELECT count(*) FROM t WHERE k "
			    "BETWEEN %lld AND %lld", (long long)i * MAXSEQ * 2 + q * 2,
			    (long long)i * MAXSEQ * 2 + q * 2 + 1);
			if (one(h, sql) != 2 && lost++ < 5)
				fprintf(stderr, "  LOST slot %d seq %u\n", i, q);
		}
	sx_close(h);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < (int)(sizeof sfx / sizeof sfx[0]); i++) {
		snprintf(side, sizeof side, "%s%s", g_path, sfx[i]);
		unlink(side);
	}
	printf("  engine: kills %llu acked %llu lost %llu errs %llu checkpoints "
	    "%llu final %s\n", (unsigned long long)atomic_load(&g_kills),
	    (unsigned long long)acked, (unsigned long long)lost,
	    (unsigned long long)atomic_load(&g_errs),
	    (unsigned long long)sx_storage_checkpoints(),
	    atomic_load(&g_final_ok) ? "ok" : "FAILED");
	if (lost || atomic_load(&g_errs) || !atomic_load(&g_final_ok) ||
	    atomic_load(&g_kills) == 0)
		fail = 1;
	if (fail) {
		fprintf(stderr, "FAIL: test_wal_kill_engine\n");
		return 1;
	}
	printf("  ok   fibers killed inside COMMIT never hang the engine or lose "
	    "an acked commit\n");
	return 0;
}

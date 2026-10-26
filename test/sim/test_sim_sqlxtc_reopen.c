/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_sqlxtc_reopen.c
 *	DST: the sqlxtc storage engine's OPEN / CLOSE paths -- clean close
 *	followed by a TRUSTED reopen (no replay, indexes trusted), and crash
 *	recovery (rebuild from the log) -- interleaved, under the
 *	deterministic scheduler, judged against an oracle the test keeps.
 *
 *	The structure is test_sim_crash_recover's (fork-isolated runs, a
 *	seeded crash that halts the run with xtc_exec_stop, the log cut at
 *	the durable frontier), extended to many rounds over ONE store.  Per
 *	seed, the parent runs ROUNDS rounds; each round is a fresh child
 *	process (a process that crashes is gone, as in production) that
 *	runs 1-2 segments:
 *
 *	  open    sx_storage_open: a trusted reopen when the last segment
 *	          closed cleanly, else crash recovery from the log;
 *	  verify  every row against the oracle (exact columns), every
 *	          in-flight-at-crash op resolved all-or-nothing, the
 *	          per-value row count through the index equal to the
 *	          oracle's, and xstore_index_check: 0 rows missing from any
 *	          index;
 *	  run     NW lane fibers over N_LOOPS loops with the background
 *	          storage procs (WAL writer, checkpoint proc, page provider,
 *	          trickler: sx_storage_run) commit a seeded number of ops
 *	          (INSERT; BEGIN INSERT+UPDATE COMMIT; UPDATE; DELETE) under
 *	          xtc_sim_exec_run with seeded I/O latency;
 *	  end     clean: the in-run index check, then sx_storage_close (must
 *	          succeed); the next segment reopens in-process.  Crash (the
 *	          last segment, on a seeded coin): a lane stops the executor
 *	          at a seeded op count, the log is cut at its durable LSN and
 *	          the child exits without closing.
 *
 *	A last child verifies the final state.  The oracle (rows, op status)
 *	lives in shared memory the children update as ops are attempted and
 *	acknowledged.  Invariants, each a "FAIL:" line naming seed, round,
 *	segment and the open kind:
 *	  DURABILITY  every acked op's effect is present;
 *	  ATOMICITY   an op in flight at the crash is all or nothing;
 *	  EXACTNESS   no row the oracle does not hold, every column right;
 *	  INDEX       0 rows missing from an index, index counts = oracle;
 *	  LIVENESS    every run quiesces (no deadlock / step budget);
 *	  REPLAY      the same seed yields the identical fingerprint.
 *
 *	usage: test_sim_sqlxtc_reopen [base [count]]   (default 1 6)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#include "bufmgr.h"
#include "btree.h"
#include "wal.h"
#include "engine.h"
#include "xstore.h"

#define N_LOOPS   3
#define NW        4               /* lanes (one fiber each) */
#define ROUNDS    4               /* child processes per seed (+ a final verify) */
#define OPS_MIN   12              /* ops per lane per segment: MIN + rnd % SPAN */
#define OPS_SPAN  12
#define MAXSEQ    256             /* lane sequence numbers per seed */
#define MAXID     (MAXSEQ * NW)
#define NG        7               /* distinct g values (the indexed column) */
#define FRAMES    512
#define N_IDX     2               /* u UNIQUE + t_g */
#define MAX_STEPS 20000000L

enum { K_INS, K_TXN, K_UPD, K_DEL };
enum { S_NONE, S_ACKED, S_UNKNOWN, S_APPLIED, S_GONE, S_FAILED };

struct mrow { uint8_t present; int32_t g, w; };

/* The oracle, shared by the parent and every child of one seed. */
struct shared {
	int64_t     next_s[NW];
	uint8_t     st[MAXID];
	uint8_t     kind[MAXID];
	int32_t     tgt[MAXID];
	struct mrow row[MAXID];
	int         created;
	int         last_clean;       /* the last segment closed cleanly */
	uint64_t    acked, crashes, cleans, trusted_opens, recover_opens;
	int         failed;
};

static struct shared *g_sh;
static char       g_path[512];
static xtc_exec_t *g_exec;
static uint64_t   g_seed;
static int        g_round, g_seg;
static _Atomic long g_step, g_crash_at;
static _Atomic int  g_crashed, g_left;
static int        g_ops;

static uint64_t
mix(uint64_t x)
{
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

static int64_t
id_of(int64_t s, int l)
{
	return s * NW + l;
}

/* Op kind for lane sequence s, and its target's sequence. */
static int
op_of(int64_t s, int64_t *ts)
{
	switch (s % 5) {
	case 0: case 1: *ts = -1; return K_INS;
	case 2: *ts = s - 2; return K_TXN;   /* s-2 is an insert (s%5 == 0) */
	case 3: *ts = s - 3; return K_UPD;   /* s-3 is an insert (s%5 == 0) */
	default: *ts = s - 3; return K_DEL;  /* s-3 is an insert (s%5 == 1) */
	}
}

#define FAILF(...) do { fprintf(stderr, "FAIL: seed=%llu round=%d seg=%d ", \
	(unsigned long long)g_seed, g_round, g_seg); fprintf(stderr, __VA_ARGS__); \
	fprintf(stderr, "\n"); g_sh->failed = 1; } while (0)

/* Apply op `id` to the oracle. */
static void
model_apply(int64_t id)
{
	struct mrow *r = &g_sh->row[id];
	int32_t t = g_sh->tgt[id];
	switch (g_sh->kind[id]) {
	case K_INS:
		r->present = 1; r->g = (int32_t)(id % NG); r->w = 0;
		break;
	case K_TXN:
		r->present = 1; r->g = (int32_t)(id % NG); r->w = 0;
		if (g_sh->row[t].present)
			g_sh->row[t].w = (int32_t)id;
		break;
	case K_UPD:
		if (g_sh->row[t].present) {
			g_sh->row[t].g = (g_sh->row[t].g + 3) % NG;
			g_sh->row[t].w = (int32_t)id;
		}
		break;
	default:
		g_sh->row[t].present = 0;
		break;
	}
}

static int
run_op(sx_db *h, int kind, int64_t id, int64_t tgt)
{
	char sql[200];
	int rc;
	switch (kind) {
	case K_INS:
		snprintf(sql, sizeof sql, "INSERT INTO t(k, u, g, w) VALUES(%lld, "
		    "'u%lld', %lld, 0)", (long long)id, (long long)id,
		    (long long)(id % NG));
		return sx_exec(h, sql, NULL);
	case K_TXN:
		if ((rc = sx_exec(h, "BEGIN", NULL)) != SX_OK)
			return rc;
		snprintf(sql, sizeof sql, "INSERT INTO t(k, u, g, w) VALUES(%lld, "
		    "'u%lld', %lld, 0)", (long long)id, (long long)id,
		    (long long)(id % NG));
		if ((rc = sx_exec(h, sql, NULL)) == SX_OK) {
			snprintf(sql, sizeof sql, "UPDATE t SET w = %lld WHERE k = %lld",
			    (long long)id, (long long)tgt);
			rc = sx_exec(h, sql, NULL);
		}
		if (rc == SX_OK)
			rc = sx_exec(h, "COMMIT", NULL);
		if (rc != SX_OK)
			(void)sx_exec(h, "ROLLBACK", NULL);
		return rc;
	case K_UPD:
		snprintf(sql, sizeof sql, "UPDATE t SET g = (g + 3) %% %d, w = %lld "
		    "WHERE k = %lld", NG, (long long)id, (long long)tgt);
		return sx_exec(h, sql, NULL);
	default:
		snprintf(sql, sizeof sql, "DELETE FROM t WHERE k = %lld",
		    (long long)tgt);
		return sx_exec(h, sql, NULL);
	}
}

static void
lane_proc(void *arg)
{
	int l = (int)(intptr_t)arg, k;
	sx_db *h = NULL;
	if (sx_open(":memory:", &h) != SX_OK) {
		FAILF("lane %d: sx_open failed", l);
		goto out;
	}
	for (k = 0; k < g_ops; k++) {
		int64_t s = g_sh->next_s[l], ts, id, tgt;
		long step;
		int kind, rc;
		if (s >= MAXSEQ)
			break;
		step = atomic_fetch_add(&g_step, 1);
		if (step >= atomic_load(&g_crash_at) &&
		    atomic_load(&g_crash_at) >= 0) {
			int z = 0;
			if (atomic_compare_exchange_strong(&g_crashed, &z, 1))
				(void)xtc_exec_stop(g_exec);
			break;                  /* the crash: this op never starts */
		}
		kind = op_of(s, &ts);
		id = id_of(s, l);
		tgt = ts < 0 ? -1 : id_of(ts, l);
		g_sh->kind[id] = (uint8_t)kind;
		g_sh->tgt[id] = (int32_t)tgt;
		g_sh->st[id] = S_UNKNOWN;
		g_sh->next_s[l] = s + 1;
		rc = run_op(h, kind, id, tgt);
		if (rc == SX_OK) {
			g_sh->st[id] = S_ACKED;
			g_sh->acked++;
			model_apply(id);
		} else {
			g_sh->st[id] = S_FAILED;
			FAILF("op %lld (kind %d) refused by the engine (rc %d)",
			    (long long)id, kind, rc);
		}
		if ((k & 3) == 0)
			xtc_yield();
	}
	sx_close(h);
out:
	if (atomic_fetch_sub(&g_left, 1) == 1 && !atomic_load(&g_crashed))
		sx_storage_quiesce();
}

/* Observed table state (child-local). */
static struct mrow g_obs[MAXID];

static int64_t
count_g(sx_db *h, int g)
{
	sx_stmt *st = NULL;
	int64_t c = -1;
	if (sx_prepare(h, "SELECT count(*) FROM t WHERE g = ?", -1, &st,
	    NULL) != SX_OK)
		return -1;
	sx_bind_int64(st, 1, g);
	if (sx_step(st) == SX_ROW)
		c = sx_column_int64(st, 0);
	sx_finalize(st);
	return c;
}

/*
 * Verify the open store against the oracle; resolve every op left
 * UNKNOWN by a crash first (all or nothing).  `kind` names the open.
 * Returns 0, or -1 after printing FAIL lines.
 */
static int
verify(const char *kind)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	uint64_t miss = 0, stale = 0;
	int64_t id, gc[NG];
	int n, g;

	memset(g_obs, 0, sizeof g_obs);
	memset(gc, 0, sizeof gc);
	if (sx_open(":memory:", &h) != SX_OK) {
		FAILF("%s: sx_open failed", kind);
		return -1;
	}
	if (sx_prepare(h, "SELECT k, u, g, w FROM t ORDER BY k", -1, &st,
	    NULL) != SX_OK) {
		FAILF("%s: table t is gone", kind);
		sx_close(h);
		return -1;
	}
	while (sx_step(st) == SX_ROW) {
		char want[32];
		const char *u = (const char *)sx_column_text(st, 1);
		id = sx_column_int64(st, 0);
		if (id < 0 || id >= MAXID || g_sh->st[id] == S_NONE) {
			FAILF("%s: row k=%lld was never attempted", kind,
			    (long long)id);
			continue;
		}
		if (g_obs[id].present)
			FAILF("%s: row k=%lld twice", kind, (long long)id);
		snprintf(want, sizeof want, "u%lld", (long long)id);
		if (u == NULL || strcmp(u, want) != 0)
			FAILF("%s: row k=%lld has u=%s", kind, (long long)id,
			    u ? u : "NULL");
		g_obs[id].present = 1;
		g_obs[id].g = (int32_t)sx_column_int64(st, 2);
		g_obs[id].w = (int32_t)sx_column_int64(st, 3);
	}
	sx_finalize(st);

	/* Resolve the ops a crash left in flight (one per lane at most). */
	for (id = 0; id < MAXID; id++) {
		int32_t t = g_sh->tgt[id];
		int app = 0;
		if (g_sh->st[id] != S_UNKNOWN)
			continue;
		switch (g_sh->kind[id]) {
		case K_INS:
			app = g_obs[id].present;
			break;
		case K_TXN:
			app = g_obs[id].present;
			if (g_sh->row[t].present &&
			    app != (g_obs[t].present && g_obs[t].w == id))
				FAILF("%s: ATOMICITY: txn %lld torn (insert %s, "
				    "update of %d %s)", kind, (long long)id,
				    app ? "present" : "absent", t,
				    app ? "missing" : "present");
			break;
		case K_UPD:
			app = g_sh->row[t].present && g_obs[t].present &&
			    g_obs[t].w == id;
			break;
		default:
			app = g_sh->row[t].present && !g_obs[t].present;
			break;
		}
		g_sh->st[id] = app ? S_APPLIED : S_GONE;
		if (app)
			model_apply(id);
	}

	for (id = 0; id < MAXID; id++) {
		struct mrow *m = &g_sh->row[id], *o = &g_obs[id];
		if (m->present && !o->present)
			FAILF("%s: DURABILITY: row %lld lost (op status %d)", kind,
			    (long long)id, g_sh->st[id]);
		else if (!m->present && o->present)
			FAILF("%s: row %lld present, oracle says absent (status %d)",
			    kind, (long long)id, g_sh->st[id]);
		else if (m->present && (m->g != o->g || m->w != o->w))
			FAILF("%s: row %lld is (g %d, w %d), want (g %d, w %d)",
			    kind, (long long)id, o->g, o->w, m->g, m->w);
		if (m->present)
			gc[m->g]++;
	}
	for (g = 0; g < NG; g++) {
		int64_t c = count_g(h, g);
		if (c != gc[g])
			FAILF("%s: INDEX: count(g=%d) is %lld, oracle %lld", kind,
			    g, (long long)c, (long long)gc[g]);
	}
	sx_close(h);
	n = xstore_index_check(sx_storage_bt(), &miss, &stale);
	if (n != N_IDX || miss != 0)
		FAILF("%s: INDEX: %llu rows missing from an index (%d indexes "
		    "checked, want %d)", kind, (unsigned long long)miss, n, N_IDX);
	return g_sh->failed ? -1 : 0;
}

/*
 * The crash cut: keep the log's frames up to the durable LSN (what the
 * group-commit writer fsync-confirmed), as test_sim_crash_recover does.
 * The sim writes bytes at submit time, so frames past the frontier are
 * physically there; a real crash may lose them.  Zero-filled
 * preallocation past the end reads as lsn 0 and ends the scan.
 */
static int
wal_cut_durable(const char *path, uint64_t durable_lsn)
{
	int fd;
	off_t off = 0, keep = 0;
	uint8_t hdr[12];
	uint64_t prev = 0;
	struct stat sb;

	if ((fd = open(path, O_RDWR)) < 0)
		return -1;
	if (fstat(fd, &sb) != 0) {
		close(fd);
		return -1;
	}
	for (;;) {
		uint64_t lsn;
		uint32_t len;
		off_t end;
		if (pread(fd, hdr, sizeof hdr, off) != (ssize_t)sizeof hdr)
			break;
		memcpy(&lsn, hdr, 8);
		memcpy(&len, hdr + 8, 4);
		end = off + (off_t)sizeof hdr + (off_t)len + 8;   /* + crc */
		if (lsn == 0 || lsn <= prev || sb.st_size < end ||
		    lsn > durable_lsn)
			break;
		prev = lsn;
		off = end;
		keep = off;
	}
	if (ftruncate(fd, keep) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* One workload segment on the open store.  Returns 1 if it crashed,
 * 0 after a clean close, -1 on failure. */
static int
segment(uint64_t rs, int crash)
{
	xtc_proc_opts_t po = { 0 };
	xtc_pid_t pid;
	uint64_t miss = 0, stale = 0;
	int l, rc, n;

	g_ops = OPS_MIN + (int)(mix(rs ^ 0x6F7073ull) % OPS_SPAN);
	atomic_store(&g_step, 0);
	atomic_store(&g_crashed, 0);
	atomic_store(&g_left, NW);
	atomic_store(&g_crash_at, crash ?
	    (long)(mix(rs ^ 0x637574ull) % (uint64_t)(NW * g_ops + 1)) : -1L);
	if (xtc_exec_init(&g_exec, N_LOOPS) != XTC_OK) {
		FAILF("xtc_exec_init failed");
		return -1;
	}
	if (sx_storage_run(xtc_exec_loop(g_exec, 0)) != SX_OK) {
		FAILF("sx_storage_run failed");
		return -1;
	}
	po.name = "lane";
	for (l = 0; l < NW; l++)
		if (xtc_proc_spawn(xtc_exec_loop(g_exec, l % N_LOOPS), lane_proc,
		    (void *)(intptr_t)l, &po, &pid) != XTC_OK) {
			FAILF("spawn lane %d failed", l);
			return -1;
		}
	xtc_sim_io_faults_enable(20 * 1000LL, 300 * 1000LL, 0);
	rc = xtc_sim_exec_run(g_exec, rs, MAX_STEPS);
	xtc_sim_io_faults_disable();
	if (rc != XTC_OK) {
		FAILF("LIVENESS: sim run returned %d (%s)", rc,
		    rc == XTC_E_DEADLK ? "deadlock: every proc parked" :
		    rc == XTC_E_AGAIN ? "step budget exhausted" : "error");
		return -1;
	}
	if (atomic_load(&g_crashed)) {
		char wp[1100];
		uint64_t dlsn = wal_durable_lsn(sx_storage_wal());
		snprintf(wp, sizeof wp, "%s", sx_storage_wal_path());
		if (wal_cut_durable(wp, dlsn) != 0) {
			FAILF("cannot cut the log");
			return -1;
		}
		g_sh->crashes++;
		g_sh->last_clean = 0;
		return 1;                  /* the process dies here: no close */
	}
	(void)xtc_exec_fini(g_exec);
	g_exec = NULL;
	n = xstore_index_check(sx_storage_bt(), &miss, &stale);
	if (n != N_IDX || miss != 0)
		FAILF("in run, before a clean close: INDEX: %llu rows missing "
		    "from an index (%d indexes checked)",
		    (unsigned long long)miss, n);
	if (sx_storage_close() != SX_OK) {
		FAILF("sx_storage_close failed");
		return -1;
	}
	g_sh->cleans++;
	g_sh->last_clean = 1;
	return g_sh->failed ? -1 : 0;
}

/*
 * The commit clock samples its one-shot wall-clock epoch offset on the
 * first commit of a process (xstore.c hlc_phys_ms); inside a sim run
 * that real-clock read trips the determinism guard.  So every child
 * commits once OFF the sim, right after its open, to a side table the
 * oracle ignores (test_sim_sql_now warms the same offset).
 */
static int
warm_clock(void)
{
	sx_db *h = NULL;
	int rc;
	if (sx_open(":memory:", &h) != SX_OK)
		return -1;
	rc = sx_exec(h, "CREATE TABLE IF NOT EXISTS warm(k INTEGER PRIMARY KEY, "
	    "v INTEGER)", NULL);
	if (rc == SX_OK)
		rc = sx_exec(h, "DELETE FROM warm WHERE k = 1", NULL);
	if (rc == SX_OK)
		rc = sx_exec(h, "INSERT INTO warm(k, v) VALUES(1, 0)", NULL);
	sx_close(h);
	if (rc != SX_OK)
		FAILF("warm-up commit failed (rc %d)", rc);
	return rc == SX_OK ? 0 : -1;
}

/* Open the store, recording which kind of open this is. */
static int
open_store(const char **kind)
{
	*kind = g_sh->last_clean ? "after a clean reopen" : "after crash recovery";
	if (!g_sh->created)
		*kind = "after create";
	if (sx_storage_open(g_path, FRAMES) != SX_OK) {
		FAILF("%s: sx_storage_open failed", *kind);
		return -1;
	}
	if (g_sh->last_clean)
		g_sh->trusted_opens++;
	else
		g_sh->recover_opens++;
	if (!g_sh->created) {
		sx_db *h = NULL;
		if (sx_open(":memory:", &h) != SX_OK ||
		    sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, u TEXT UNIQUE, "
		    "g INTEGER, w INTEGER)", NULL) != SX_OK ||
		    sx_exec(h, "CREATE INDEX t_g ON t(g)", NULL) != SX_OK) {
			FAILF("create failed");
			return -1;
		}
		sx_close(h);
		g_sh->created = 1;
	}
	return warm_clock();
}

/* Child: one round of 1-2 segments; exit 0 / 3 (failed). */
static int
round_child(int last)
{
	uint64_t rs = mix(g_seed * 1000003ull + (uint64_t)g_round);
	int nseg = last ? 1 : 1 + (int)(mix(rs) % 2);
	int crash = !last && (mix(rs ^ 0x63ull) & 1);
	const char *kind;
	int r;

	if (xtc_set_stack_size(512u * 1024u) != XTC_OK || sx_init() != SX_OK)
		return 3;
	sx_storage_set_checkpoint(1ull << 40);   /* no online checkpoint here */
	for (g_seg = 0; g_seg < nseg; g_seg++) {
		if (open_store(&kind) != 0 || verify(kind) != 0)
			return 3;
		if (last) {
			(void)sx_storage_close();
			return 0;
		}
		r = segment(mix(rs + (uint64_t)g_seg + 1),
		    crash && g_seg == nseg - 1);
		if (r < 0)
			return 3;
		if (r == 1)
			return 0;          /* crashed */
	}
	return 0;
}

static const char *const k_sfx[] = { "", "-wal", ".dwb", "-wal.compact",
	"-wal.ocompact" };

/* Run one seed: ROUNDS rounds + a final verify.  0 ok, else 1; *fp is
 * the replay fingerprint. */
static int
run_seed(uint64_t seed, uint64_t *fp)
{
	const char *td = getenv("TMPDIR");
	char p2[600];
	int fd, r, st, ok = 1;
	pid_t p;
	uint64_t h = 0xCBF29CE484222325ull;
	int64_t id;
	size_t i;

	g_seed = seed;
	memset(g_sh, 0, sizeof *g_sh);
	snprintf(g_path, sizeof g_path, "%s/sim_reopen_XXXXXX",
	    td != NULL && td[0] != '\0' ? td : "/tmp");
	if ((fd = mkstemp(g_path)) < 0)
		return 1;
	close(fd);
	for (r = 0; r <= ROUNDS && ok; r++) {
		g_round = r;
		fflush(stdout);
		fflush(stderr);
		p = fork();
		if (p == 0)
			_exit(round_child(r == ROUNDS));
		if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st) ||
		    WEXITSTATUS(st) != 0 || g_sh->failed) {
			if (!g_sh->failed)
				fprintf(stderr, "FAIL: seed=%llu round=%d: child "
				    "died (status 0x%x)\n",
				    (unsigned long long)seed, r, st);
			ok = 0;
		}
	}
	for (i = 0; i < sizeof k_sfx / sizeof k_sfx[0]; i++) {
		snprintf(p2, sizeof p2, "%s%s", g_path, k_sfx[i]);
		(void)unlink(p2);
	}
	for (id = 0; id < MAXID; id++) {
		h = (h ^ (uint64_t)g_sh->row[id].present) * 0x100000001B3ull;
		h = (h ^ (uint64_t)(uint32_t)g_sh->row[id].g) * 0x100000001B3ull;
		h = (h ^ (uint64_t)(uint32_t)g_sh->row[id].w) * 0x100000001B3ull;
		h = (h ^ g_sh->st[id]) * 0x100000001B3ull;
	}
	h = (h ^ g_sh->acked) * 0x100000001B3ull;
	h = (h ^ g_sh->crashes) * 0x100000001B3ull;
	*fp = h;
	return ok ? 0 : 1;
}

int
main(int argc, char **argv)
{
	uint64_t base = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
	int count = argc > 2 ? atoi(argv[2]) : 6, k;
	uint64_t acked = 0, crashes = 0, cleans = 0, tr = 0, rec = 0;

	g_sh = mmap(NULL, sizeof *g_sh, PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANON, -1, 0);
	if (g_sh == MAP_FAILED)
		return 1;
	for (k = 0; k < count; k++) {
		uint64_t seed = base + (uint64_t)k, f1 = 0, f2 = 0;
		if (run_seed(seed, &f1) != 0)
			return 1;
		acked += g_sh->acked;
		crashes += g_sh->crashes;
		cleans += g_sh->cleans;
		tr += g_sh->trusted_opens;
		rec += g_sh->recover_opens;
		if (run_seed(seed, &f2) != 0)
			return 1;
		if (f1 != f2) {
			printf("FAIL: seed=%llu REPLAY: fingerprint %016llx vs "
			    "%016llx\n", (unsigned long long)seed,
			    (unsigned long long)f1, (unsigned long long)f2);
			return 1;
		}
	}
	printf("sweep: %d seeds x2 (replayed): %llu acked ops, %llu crashes, "
	    "%llu clean closes, %llu trusted reopens, %llu recoveries\n", count,
	    (unsigned long long)acked, (unsigned long long)crashes,
	    (unsigned long long)cleans, (unsigned long long)tr,
	    (unsigned long long)rec);
	printf("OK: sqlxtc open/close under DST -- clean close + trusted reopen "
	    "and crash recovery interleaved; every acked op durable, in-flight "
	    "ops all-or-nothing, rows exact, 0 index entries missing, replayed "
	    "(%d seeds from %llu)\n", count, (unsigned long long)base);
	return 0;
}

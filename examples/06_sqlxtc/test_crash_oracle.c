/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_crash_oracle.c
 *	A crash oracle for the storage engine's open / close / recovery
 *	paths: every acknowledged commit survives, nothing appears that was
 *	never attempted, transactions are atomic, and the indexes are
 *	complete -- judged against a model the parent keeps, not against
 *	another engine.
 *
 *	The parent never opens the store.  Per seed it runs rounds; each
 *	round forks a worker child that opens the store (a recovery, or a
 *	trusted reopen with the recorded log tail when the last close was
 *	clean), runs NLANE committer fibers with online log checkpoints on
 *	(a 64 KB trigger), cleanly closes and reopens the store in-process
 *	0-2 times, and either closes cleanly at the end or is SIGKILLed by
 *	the parent at a seeded time (by its own pid).  Before each commit
 *	the child sends the parent "attempt id", after a successful one
 *	"acked id", over a pipe.  After a round (always after the last; at
 *	random after a kill, else the next worker does the recovery) a
 *	verify child opens the store, dumps every row, runs
 *	xstore_index_check, counts rows per indexed value through the index
 *	and probes UNIQUE, and closes cleanly; the parent checks it:
 *	  - every acked op's effect is present;
 *	  - no row exists whose insert was never attempted, none twice;
 *	  - an op in flight at the kill is all or nothing (the transaction
 *	    that inserts row i and updates an earlier row: both or neither);
 *	  - every row matches the model exactly (columns u, g, w);
 *	  - 0 missing index entries; index counts = scan counts; a duplicate
 *	    of an existing UNIQUE value is refused.
 *	Ops, by the lane-local sequence number s (row id = s * NLANE + lane):
 *	  s%10 0-5  INSERT row          (autocommit)
 *	  s%10 6,7  BEGIN; INSERT row; UPDATE row of s-6 (w, g); COMMIT
 *	  s%10 8    UPDATE row of s-6   (autocommit; w, g)
 *	  s%10 9    DELETE row of s-6   (autocommit)
 *	A target in an earlier round makes the op a plain INSERT, so every
 *	target was acked earlier on the same lane.
 *	Usage: test_crash_oracle [seeds [first-seed]] (default 3 1).
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

/* Declared here too so the oracle builds against an engine whose
 * xstore.h predates it (a shim then provides it). */
int xstore_index_check(bt_t *bt, uint64_t *missing, uint64_t *stale);

#define NLANE    4
#define MAXSEQ   60000             /* lane sequence numbers per seed */
#define MAXID    (MAXSEQ * NLANE)
#define FRAMES   2048
#define NG       17

enum { K_INS, K_TXN, K_UPD, K_DEL };
enum { S_NONE, S_ACKED, S_UNKNOWN, S_APPLIED, S_GONE };   /* op status */

struct msg { int32_t tag; int32_t kind; int64_t id; int64_t tgt; };

static char g_path[256];
static int g_wfd = -1;             /* child: pipe to the parent */

/* ---- child side --------------------------------------------------- */

static void
send_msg(int tag, int kind, int64_t id, int64_t tgt)
{
	struct msg m;
	ssize_t n;
	m.tag = tag; m.kind = kind; m.id = id; m.tgt = tgt;
	do {
		n = write(g_wfd, &m, sizeof m);   /* <= PIPE_BUF: atomic */
	} while (n < 0 && errno == EINTR);
}

struct seg { int64_t a, b, round_start; };
static struct seg g_seg;
static _Atomic int g_left;

static int
op_of(int64_t s, int64_t round_start, int64_t *tseq)
{
	int r = (int)(s % 10);
	*tseq = s - 6;
	if (r < 6 || *tseq < round_start)
		return K_INS;
	return r <= 7 ? K_TXN : r == 8 ? K_UPD : K_DEL;
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
		if ((rc = sx_exec(h, "BEGIN", NULL)) != SX_OK) return rc;
		snprintf(sql, sizeof sql, "INSERT INTO t(k, u, g, w) VALUES(%lld, "
		    "'u%lld', %lld, 0)", (long long)id, (long long)id,
		    (long long)(id % NG));
		if ((rc = sx_exec(h, sql, NULL)) == SX_OK) {
			snprintf(sql, sizeof sql, "UPDATE t SET w = %lld, g = (g + 5) %% %d "
			    "WHERE k = %lld", (long long)id, NG, (long long)tgt);
			rc = sx_exec(h, sql, NULL);
		}
		if (rc == SX_OK)
			rc = sx_exec(h, "COMMIT", NULL);
		if (rc != SX_OK)
			(void)sx_exec(h, "ROLLBACK", NULL);
		return rc;
	case K_UPD:
		snprintf(sql, sizeof sql, "UPDATE t SET w = %lld, g = (g + 3) %% %d "
		    "WHERE k = %lld", (long long)id, NG, (long long)tgt);
		return sx_exec(h, sql, NULL);
	default:
		snprintf(sql, sizeof sql, "DELETE FROM t WHERE k = %lld",
		    (long long)tgt);
		return sx_exec(h, sql, NULL);
	}
}

static void
lane(void *arg)
{
	long ln = (long)arg;
	sx_db *h = NULL;
	int64_t s, tseq;
	int kind;
	if (sx_open(":memory:", &h) == SX_OK) {
		for (s = g_seg.a; s < g_seg.b; s++) {
			int64_t id = s * NLANE + ln, tgt;
			kind = op_of(s, g_seg.round_start, &tseq);
			tgt = kind == K_INS ? -1 : tseq * NLANE + ln;
			send_msg('A', kind, id, tgt);
			if (run_op(h, kind, id, tgt) == SX_OK)
				send_msg('K', kind, id, tgt);
			else
				send_msg('F', kind, id, tgt);   /* attempted, refused */
			if ((s & 63) == 0)
				send_msg('C', 0, (int64_t)sx_storage_checkpoints(), 0);
		}
		sx_close(h);
	}
	if (atomic_fetch_sub(&g_left, 1) == 1)
		sx_storage_quiesce();
}

/* argv-less worker: segments [a, b) of the lane sequence, with clean
 * close + reopen between segments. */
static int
worker(int64_t round_start, const int64_t *cuts, int ncut, int create)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "lane" };
	xtc_pid_t pid;
	uint64_t miss, stale;
	int i, l, n;
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	if (sx_init() != SX_OK) return 1;
	sx_storage_set_checkpoint(64u << 10);   /* online checkpoints */
	if (sx_storage_open(g_path, FRAMES) != SX_OK) return 1;
	if (create) {
		sx_db *h = NULL;
		if (sx_open(":memory:", &h) != SX_OK ||
		    sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, u TEXT UNIQUE, "
		    "g INTEGER, w INTEGER)", NULL) != SX_OK ||
		    sx_exec(h, "CREATE INDEX t_g ON t(g)", NULL) != SX_OK ||
		    sx_exec(h, "CREATE INDEX t_gw ON t(g, w)", NULL) != SX_OK)
			return 1;
		sx_close(h);
	}
	miss = stale = 0;              /* the open's result: recovery or reopen */
	n = xstore_index_check((bt_t *)sx_storage_bt(), &miss, &stale);
	send_msg('Y', n, (int64_t)miss, (int64_t)stale);
	send_msg('R', 0, 0, 0);
	for (i = 0; i + 1 < ncut; i++) {
		g_seg.a = cuts[i]; g_seg.b = cuts[i + 1];
		g_seg.round_start = round_start;
		if (i > 0) {
			if (sx_storage_open(g_path, FRAMES) != SX_OK) return 1;
			miss = stale = 0;
			n = xstore_index_check((bt_t *)sx_storage_bt(), &miss, &stale);
			send_msg('Y', n, (int64_t)miss, (int64_t)stale);
			send_msg('O', 0, 0, 0);          /* an in-process reopen */
		}
		atomic_store(&g_left, NLANE);
		if (xtc_loop_init(&loop) != XTC_OK) return 1;
		if (sx_storage_run(loop) != SX_OK) return 1;
		for (l = 0; l < NLANE; l++)
			if (xtc_proc_spawn(loop, lane, (void *)(long)l, &po, &pid) != XTC_OK)
				return 1;
		if (xtc_loop_run(loop) != XTC_OK) return 1;
		(void)xtc_loop_fini(loop);
		send_msg('C', 0, (int64_t)sx_storage_checkpoints(), 0);
		/* In-run completeness, before the close (a rebuild at the next
		 * open would hide a loss): commits are stopped here. */
		miss = stale = 0;
		n = xstore_index_check((bt_t *)sx_storage_bt(), &miss, &stale);
		send_msg('X', n, (int64_t)miss, (int64_t)stale);
		if (sx_storage_close() != SX_OK) return 1;
	}
	send_msg('E', 0, 0, 0);
	return 0;
}

/* Verify child: dump rows and index facts to `out` (text). */
static int
verifier(const char *out)
{
	FILE *f = fopen(out, "w");
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	uint64_t miss = 0, stale = 0;
	int g, n, bad = 0, probes = 0;
	char sql[160];
	if (f == NULL) return 1;
	if (sx_init() != SX_OK || sx_storage_open(g_path, FRAMES) != SX_OK)
		return 1;
	if (sx_open(":memory:", &h) != SX_OK) return 1;
	if (sx_prepare(h, "SELECT k, u, g, w FROM t ORDER BY k", -1, &st,
	    NULL) != SX_OK) {
		fprintf(f, "NOTABLE\n");      /* no table: nothing was ever durable */
	} else {
		while (sx_step(st) == SX_ROW) {
			const char *u = (const char *)sx_column_text(st, 1);
			fprintf(f, "R %lld %s %lld %lld\n",
			    (long long)sx_column_int64(st, 0), u ? u : "NULL",
			    (long long)sx_column_int64(st, 2),
			    (long long)sx_column_int64(st, 3));
		}
		sx_finalize(st);
		for (g = 0; g < NG; g++) {
			int64_t c = -1;
			if (sx_prepare(h, "SELECT count(*) FROM t WHERE g = ?", -1, &st,
			    NULL) == SX_OK) {
				sx_bind_int64(st, 1, g);
				if (sx_step(st) == SX_ROW) c = sx_column_int64(st, 0);
				sx_finalize(st);
			}
			fprintf(f, "G %d %lld\n", g, (long long)c);
		}
		/* UNIQUE: a duplicate of an existing u must be refused */
		if (sx_prepare(h, "SELECT u FROM t ORDER BY k LIMIT 20", -1, &st,
		    NULL) == SX_OK) {
			char us[20][32];
			n = 0;
			while (n < 20 && sx_step(st) == SX_ROW) {
				const char *u = (const char *)sx_column_text(st, 0);
				snprintf(us[n++], sizeof us[0], "%s", u ? u : "");
			}
			sx_finalize(st);
			for (g = 0; g < n; g++) {
				snprintf(sql, sizeof sql, "INSERT INTO t(k, u, g, w) VALUES("
				    "%d, '%s', 0, 0)", 900000000 + g, us[g]);
				probes++;
				if (sx_exec(h, sql, NULL) != SX_CONSTRAINT) bad++;
			}
		}
		fprintf(f, "U %d %d\n", probes, bad);
		n = xstore_index_check((bt_t *)sx_storage_bt(), &miss, &stale);
		fprintf(f, "I %d %llu %llu\n", n, (unsigned long long)miss,
		    (unsigned long long)stale);
	}
	sx_close(h);
	if (sx_storage_close() != SX_OK) fprintf(f, "CLOSEFAIL\n");
	fprintf(f, "END\n");
	fclose(f);
	return 0;
}

/* ---- parent side -------------------------------------------------- */

static uint8_t  g_st[MAXID];       /* op status */
static uint8_t  g_kind[MAXID];
static int64_t  g_tgt[MAXID];
static int      g_fails;
static uint64_t g_rng;

static uint64_t
rnd(void)
{
	g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
	return g_rng;
}

#define BAD(...) do { fprintf(stderr, "  ORACLE FAIL seed %llu: ", \
	(unsigned long long)seed); fprintf(stderr, __VA_ARGS__); \
	fprintf(stderr, "\n"); g_fails++; } while (0)

struct row { int present; char u[64]; int64_t g, w; };
static struct row g_row[MAXID];

static int64_t
mod(int64_t a) { return ((a % NG) + NG) % NG; }

/* Run a verify child, parse its dump into g_row.  0 ok. */
static int
verify(uint64_t seed, int64_t maxid, uint64_t *rows_out)
{
	char out[300], line[200];
	FILE *f;
	pid_t p;
	int status, gcount[NG], gidx[NG], ended = 0, g;
	int64_t k, gg, ww, i;
	char u[64];
	uint64_t nrows = 0;
	snprintf(out, sizeof out, "%s.verify", g_path);
	p = fork();
	if (p == 0)
		_exit(verifier(out));
	if (p < 0 || waitpid(p, &status, 0) != p || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		BAD("verify child failed (status %d)", status);
		return -1;
	}
	memset(g_row, 0, sizeof g_row[0] * (size_t)maxid);
	memset(gcount, 0, sizeof gcount);
	for (g = 0; g < NG; g++) gidx[g] = -1;
	if ((f = fopen(out, "r")) == NULL) return -1;
	while (fgets(line, sizeof line, f) != NULL) {
		long long a, b, c, d;
		if (sscanf(line, "R %lld %63s %lld %lld", &a, u, &c, &d) == 4) {
			k = a; gg = c; ww = d;
			if (k < 0 || k >= maxid) { BAD("row k=%lld never attempted", a); continue; }
			if (g_row[k].present) BAD("row k=%lld twice", a);
			g_row[k].present = 1;
			snprintf(g_row[k].u, sizeof g_row[k].u, "%s", u);
			g_row[k].g = gg; g_row[k].w = ww;
			if (gg >= 0 && gg < NG) gcount[gg]++;
			nrows++;
		} else if (sscanf(line, "G %lld %lld", &a, &b) == 2) {
			if (a >= 0 && a < NG) gidx[a] = (int)b;
		} else if (sscanf(line, "U %lld %lld", &a, &b) == 2) {
			if (b != 0) BAD("%lld of %lld duplicate UNIQUE inserts accepted", b, a);
		} else if (sscanf(line, "I %lld %lld %lld", &a, &b, &c) == 3) {
			if (a != 3) BAD("index check covered %lld indexes, want 3", a);
			if (b != 0) BAD("%lld rows missing from an index", b);
		} else if (strncmp(line, "NOTABLE", 7) == 0) {
			BAD("table t is gone (it was created before any kill)");
		} else if (strncmp(line, "CLOSEFAIL", 9) == 0) {
			BAD("verify close failed");
		} else if (strncmp(line, "END", 3) == 0) {
			ended = 1;
		}
	}
	fclose(f);
	unlink(out);
	if (!ended) BAD("truncated verify dump");
	for (g = 0; g < NG; g++)
		if (gidx[g] != gcount[g])
			BAD("index count for g=%d is %d, scan has %d", g, gidx[g], gcount[g]);
	/* model check */
	for (i = 0; i < maxid; i++) {
		int st = g_st[i];
		if (st == S_UNKNOWN) {          /* in flight at a kill: observe it */
			int app;
			switch (g_kind[i]) {
			case K_INS: app = g_row[i].present; break;
			case K_TXN:
				app = g_row[i].present;
				if (app != (g_row[g_tgt[i]].present && g_row[g_tgt[i]].w == i))
					BAD("txn %lld torn: row %s, update of %lld %s",
					    (long long)i, app ? "present" : "absent",
					    (long long)g_tgt[i], app ? "missing" : "present");
				break;
			case K_UPD: app = g_row[g_tgt[i]].w == i; break;
			default:    app = !g_row[g_tgt[i]].present; break;
			}
			g_st[i] = app ? S_APPLIED : S_GONE;
		}
	}
	for (i = 0; i < maxid; i++) {
		int own = g_st[i] == S_ACKED || g_st[i] == S_APPLIED;
		int want = 0, deleted = 0;
		int64_t wg = mod(i), ww2 = 0;
		int64_t j = i + 6 * NLANE;       /* the only op that can target row i */
		if (g_kind[i] == K_INS || g_kind[i] == K_TXN)
			want = own;
		if (want && j < maxid && g_kind[j] != K_INS && g_tgt[j] == i &&
		    (g_st[j] == S_ACKED || g_st[j] == S_APPLIED)) {
			if (g_kind[j] == K_TXN) { ww2 = j; wg = mod(wg + 5); }
			else if (g_kind[j] == K_UPD) { ww2 = j; wg = mod(wg + 3); }
			else deleted = 1;
		}
		if (deleted) want = 0;
		if (want && !g_row[i].present)
			BAD("acked row %lld lost (op status %d)", (long long)i, g_st[i]);
		else if (!want && g_row[i].present)
			BAD("row %lld present, model says absent (status %d kind %d)",
			    (long long)i, g_st[i], g_kind[i]);
		else if (want) {
			char eu[32];
			snprintf(eu, sizeof eu, "u%lld", (long long)i);
			if (strcmp(eu, g_row[i].u) != 0 || g_row[i].g != wg ||
			    g_row[i].w != ww2)
				BAD("row %lld is (%s,%lld,%lld), want (%s,%lld,%lld)",
				    (long long)i, g_row[i].u, (long long)g_row[i].g,
				    (long long)g_row[i].w, eu, (long long)wg, (long long)ww2);
		}
	}
	*rows_out = nrows;
	return 0;
}

/* One worker round; returns 1 if it was killed. */
static int
round_run(uint64_t seed, int64_t start, int64_t len, int create,
    uint64_t *ckpts, int *reopens, uint64_t *acked, int64_t *maxid)
{
	int fds[2], status, killed = 0, ready = 0, ncut = 2, i;
	int64_t cuts[4];
	int64_t kill_ms = (rnd() % 4 == 0) ? -1 : (int64_t)(rnd() % 1500) + 5;
	int64_t t0 = 0;
	uint64_t rck = 0;                  /* checkpoints by this process */
	pid_t p;
	cuts[0] = start;
	for (i = (int)(rnd() % 3); i > 0; i--)    /* in-process clean reopens */
		cuts[ncut - 1] = start + (int64_t)(rnd() % (uint64_t)len), ncut++;
	cuts[ncut - 1] = start + len;
	for (i = 1; i < ncut; i++)                 /* sort the cut points */
		if (cuts[i] < cuts[i - 1]) { int64_t t = cuts[i]; cuts[i] = cuts[i - 1];
			cuts[i - 1] = t; i = 0; }
	if (pipe(fds) != 0) return -1;
	p = fork();
	if (p == 0) {
		close(fds[0]);
		g_wfd = fds[1];
		_exit(worker(start, cuts, ncut, create));
	}
	close(fds[1]);
	for (;;) {
		struct pollfd pf = { .fd = fds[0], .events = POLLIN };
		struct msg m;
		int to = -1, r;
		if (ready && kill_ms >= 0) {
			int64_t left = kill_ms - (xtc_clock_mono() - t0) / 1000000;
			if (left <= 0) {
				(void)kill(p, SIGKILL);     /* the worker itself */
				killed = 1;
				kill_ms = -1;
				continue;
			}
			to = (int)left;
		}
		r = poll(&pf, 1, to);
		if (r == 0) continue;
		if (r < 0 && errno == EINTR) continue;
		if (read(fds[0], &m, sizeof m) != (ssize_t)sizeof m) break;   /* EOF */
		if (m.tag == 'R') { ready = 1; t0 = xtc_clock_mono(); }
		else if (m.tag == 'Y') {
			if ((m.kind != 3 && !create) || m.id != 0)
				BAD("right after an open: %lld rows missing from an index "
				    "(%d indexes checked)", (long long)m.id, m.kind);
		} else if (m.tag == 'X') {
			if (m.kind != 3 || m.id != 0)
				BAD("in run, before a clean close: %lld rows missing from "
				    "an index (%d indexes checked)", (long long)m.id, m.kind);
		}
		else if (m.tag == 'O') (*reopens)++;
		else if (m.tag == 'C') { if ((uint64_t)m.id > rck) rck = (uint64_t)m.id; }
		else if (m.id >= 0 && m.id < MAXID) {
			if (m.tag == 'A') {
				g_st[m.id] = S_UNKNOWN; g_kind[m.id] = (uint8_t)m.kind;
				g_tgt[m.id] = m.tgt;
				if (m.id + 1 > *maxid) *maxid = m.id + 1;
			} else if (m.tag == 'K') {
				g_st[m.id] = S_ACKED; (*acked)++;
			} else if (m.tag == 'F') {
				BAD("op %lld refused by the engine", (long long)m.id);
			}
		}
	}
	close(fds[0]);
	*ckpts += rck;
	if (waitpid(p, &status, 0) != p) return -1;
	if (kill(p, 0) == 0) BAD("worker still alive after waitpid");
	if (!killed && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
		BAD("worker exited with status %d", status);
	if (killed && WIFEXITED(status) && WEXITSTATUS(status) == 0)
		killed = 0;                        /* it had finished first */
	return killed;
}

int
main(int argc, char **argv)
{
	int nseeds = argc > 1 ? atoi(argv[1]) : 3;
	uint64_t first = argc > 2 ? strtoull(argv[2], NULL, 0) : 1, seed;
	uint64_t tot_acked = 0, tot_ckpt = 0, tot_rows = 0;
	int tot_kills = 0, tot_reopens = 0, tot_verifies = 0, fd, s;
	char p2[320];
	const char *sfx[] = { "", "-wal", ".dwb", "-wal.compact", "-wal.ocompact" };
	t_tmpl(g_path, sizeof g_path, "sqlxtc-oracle");
	if ((fd = mkstemp(g_path)) < 0) return 1;
	close(fd);
	for (seed = first; seed < first + (uint64_t)nseeds; seed++) {
		int64_t start = 0, maxid = 0;
		int rounds, r, k;
		uint64_t rows = 0;
		for (s = 0; s < 5; s++) {
			snprintf(p2, sizeof p2, "%s%s", g_path, sfx[s]);
			unlink(p2);
		}
		memset(g_st, 0, sizeof g_st);
		memset(g_kind, 0, sizeof g_kind);
		g_rng = seed * 0x9E3779B97F4A7C15ull | 1;
		rounds = 3 + (int)(rnd() % 4);
		for (r = 0; r < rounds; r++) {
			int64_t len = 100 + (int64_t)(rnd() % 1200);
			len = (len + 9) / 10 * 10;
			if (start + len > MAXSEQ) break;
			k = round_run(seed, start, len, r == 0, &tot_ckpt, &tot_reopens,
			    &tot_acked, &maxid);
			if (k < 0) { BAD("round setup failed"); break; }
			tot_kills += k;
			start += len;
			if (r == rounds - 1 || !k || rnd() % 2) {
				if (verify(seed, maxid, &rows) == 0) tot_verifies++;
			}
		}
		tot_rows += rows;
		printf("  seed %llu: %d rounds, last verify %llu rows\n",
		    (unsigned long long)seed, rounds, (unsigned long long)rows);
		if (g_fails) break;
	}
	for (s = 0; s < 5; s++) {
		snprintf(p2, sizeof p2, "%s%s", g_path, sfx[s]);
		unlink(p2);
	}
	printf("  %d seeds: %llu acked commits, %d SIGKILLs, %d in-process clean "
	    "reopens, %llu online checkpoints, %d verifies -- %d violations\n",
	    nseeds, (unsigned long long)tot_acked, tot_kills, tot_reopens,
	    (unsigned long long)tot_ckpt, tot_verifies, g_fails);
	if (g_fails) {
		fprintf(stderr, "FAIL: test_crash_oracle\n");
		return 1;
	}
	printf("  ok   every acked commit survived, nothing invented, in-flight "
	    "ops atomic, indexes complete\n");
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_wal_kill.c
 *	DST: killing committers mid-commit never wedges the sqlxtc log
 *	(examples/06_sqlxtc/wal.c) and never loses an acknowledged commit.
 *
 *	One run, for a seed: N_CMT committer procs across N_LOOPS loops
 *	commit records naming (slot, seq) through wal_commit, with seeded
 *	AIO latency (every pwrite / fdatasync parks), the write-back crash
 *	model armed, wal.c's buggify yield sites on (they put a yield --
 *	a kill delivery point -- inside the copy window, before the
 *	leader's I/O and inside the direct big-record write) and, per seed,
 *	the pessimal scheduler and completion swizzle.  A killer proc
 *	kills random committers (xtc_exit_pid, or a linked child dying),
 *	half the time only ones inside wal_commit, and sometimes the
 *	writer proc; a monitor respawns killed slots.  Then the killer
 *	waits for every committer, commits FINAL_N more records itself
 *	and stops the writer.
 *
 *	Invariants (each a FAIL line naming the seed):
 *	  LIVENESS   the run reaches quiescence: no XTC_E_DEADLK (every
 *	             proc parked with no waker -- a dead baton holder) and
 *	             no exhausted step budget (a flush spinning on a
 *	             completion hole); the log accepts the final commits;
 *	             wal_writer_stop returns within 1 s of virtual time.
 *	  DURABILITY after cutting the log at the last fdatasync'd byte
 *	             (the crash model), every ACKED commit is present at
 *	             its acked LSN with its exact payload.
 *	  REPLAY     the same seed yields the identical fingerprint.
 *
 *	wal.c is included so a failure can print the log's internal state.
 *	usage: test_sim_wal_kill [base [count]]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../examples/06_sqlxtc/wal.c"

#include "xtc_exec.h"
#include "xtc_sim.h"

#define N_LOOPS   3
#define N_CMT     6
#define QUOTA     48              /* records each slot commits */
#define N_SLOTS   (N_CMT + 1)     /* + the killer's final commits */
#define FINAL     N_CMT
#define FINAL_N   8
#define N_KILLS   40
#define BIG_LEN   (2u << 20) + 77 /* > WAL_RING / 4: wal_append_direct */
#define MAX_STEPS 4000000L

struct slot {
	int       phase;              /* 1 inside wal_commit */
	uint32_t  seq;                /* next sequence number */
	xtc_pid_t pid;
	int       live;
	uint64_t  ack[QUOTA + FINAL_N];
	uint64_t  found[QUOTA + FINAL_N];
};

static wal_t      *g_wal;
static xtc_exec_t *g_exec;
static struct slot g_slot[N_SLOTS];
static int         g_big;          /* this seed logs big records */
static int         g_link;         /* this seed kills by link */
static int         g_kill_writer;  /* this seed kills the writer proc */
static int         g_errs;         /* acked but not durable, rc != OK */
static int         g_final_ok;
static int64_t     g_stop_ns;      /* virtual time wal_writer_stop took */
static uint64_t    g_kills, g_acked;   /* g_kills: exit_pid + link deaths */

static uint64_t
draw(uint64_t n)
{
	return __xtc_sim_rng_range(XTC_SIM_RNG_APP, n);
}

static uint32_t
rec_len(int slot, uint32_t seq)
{
	uint32_t h = (uint32_t)slot * 2654435761u ^ seq * 40503u;
	if (g_big && slot < N_CMT && h % 23 == 0)
		return BIG_LEN;
	return 16 + h % 200;
}

static uint8_t
rec_byte(int slot, uint32_t seq, uint32_t i)
{
	return (uint8_t)(slot * 31 + seq * 7 + i);
}

static int
commit_one(int slot, uint8_t *buf)
{
	struct slot *s = &g_slot[slot];
	uint32_t seq = s->seq++, len = rec_len(slot, seq), i, sl = (uint32_t)slot;
	uint64_t lsn = 0;
	int rc;

	memcpy(buf, &sl, 4);
	memcpy(buf + 4, &seq, 4);
	for (i = 8; i < len; i++)
		buf[i] = rec_byte(slot, seq, i);
	s->phase = 1;
	rc = wal_commit(g_wal, buf, len, &lsn);
	s->phase = 0;
	if (rc != XTC_OK || lsn > wal_durable_lsn(g_wal))
		g_errs++;
	if (rc == XTC_OK) {
		s->ack[seq] = lsn;
		g_acked++;
	}
	return rc;
}

static void
linked_child(void *arg)
{
	(void)xtc_proc_sleep((int64_t)(uintptr_t)arg);
	(void)xtc_exit_self(9);          /* abnormal: kills the linked peer */
}

static void
committer(void *arg)
{
	int slot = (int)(intptr_t)arg;
	uint8_t *buf = malloc(BIG_LEN);
	if (buf == NULL)
		return;
	(void)xtc_proc_at_exit(free, buf);
	while (g_slot[slot].seq < QUOTA) {
		if (g_link && draw(4) == 0) {
			xtc_proc_opts_t o = { .size = sizeof(xtc_proc_opts_t), .name = "lnk" };
			xtc_pid_t c;
			(void)xtc_proc_spawn_link(xtc_exec_loop(g_exec,
			    (int)draw(N_LOOPS)), linked_child,
			    (void *)(uintptr_t)(draw(300) * 1000), &o, &c);
		}
		(void)commit_one(slot, buf);
	}
}

static int
spawn_slot(int i)
{
	xtc_proc_opts_t o = { .size = sizeof(xtc_proc_opts_t), .name = "cmt" };
	uint64_t ref;
	if (xtc_proc_spawn_monitor(xtc_exec_loop(g_exec, 1 + i % (N_LOOPS - 1)),
	    committer, (void *)(intptr_t)i, &o, &g_slot[i].pid, &ref) != XTC_OK)
		return -1;
	g_slot[i].live = 1;
	return 0;
}

/* Reap DOWNs, respawning a slot with quota left.  Returns slots live. */
static int
reap(void)
{
	void *m;
	size_t n;
	int i, live = 0;
	while (xtc_recv(&m, &n, 0) == XTC_OK) {
		xtc_pid_t who;
		int why = 0;
		if (m != NULL && xtc_down_decode(m, n, &who, &why) == XTC_OK)
			for (i = 0; i < N_CMT; i++)
				if (g_slot[i].live && xtc_pid_eq(g_slot[i].pid, who)) {
					g_slot[i].live = 0;
					if (g_link && why != 0)
						g_kills++;    /* a linked child took it down */
					if (g_slot[i].seq < QUOTA)
						(void)spawn_slot(i);
				}
		xtc_free(m);
	}
	for (i = 0; i < N_CMT; i++)
		live += g_slot[i].live;
	return live;
}

static void
killer(void *arg)
{
	uint8_t *buf = malloc(BIG_LEN);
	int i, k, only_in_commit = (int)draw(2), wk = (int)draw(N_KILLS);
	int64_t t0;

	(void)arg;
	if (buf == NULL)
		return;
	for (i = 0; i < N_CMT; i++)
		(void)spawn_slot(i);
	for (i = 0; i < N_KILLS; i++) {
		(void)xtc_proc_sleep((int64_t)draw(400) * 1000);
		(void)reap();
		if (g_kill_writer && i == wk &&
		    xtc_exit_pid(wal_writer_pid(g_wal), 7) == XTC_OK)
			g_kills++;
		k = (int)draw(N_CMT);
		if (g_link || !g_slot[k].live ||
		    (only_in_commit && !g_slot[k].phase))
			continue;
		if (xtc_exit_pid(g_slot[k].pid, 7) == XTC_OK)
			g_kills++;
	}
	while (reap() > 0)
		(void)xtc_proc_sleep(1000LL * 1000);
	for (i = 0; i < FINAL_N; i++)
		if (commit_one(FINAL, buf) != XTC_OK)
			break;
	g_final_ok = i == FINAL_N;
	free(buf);
	t0 = xtc_clock_mono();
	(void)wal_writer_stop(g_wal);
	g_stop_ns = xtc_clock_mono() - t0;
}

static int
scan_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	const uint8_t *p = rec;
	uint32_t slot, seq, i;
	(void)user;
	if (len < 8)
		return 0;
	memcpy(&slot, p, 4);
	memcpy(&seq, p + 4, 4);
	if (slot >= N_SLOTS || seq >= QUOTA + FINAL_N ||
	    len != rec_len((int)slot, seq)) {
		g_errs++;
		return 0;
	}
	for (i = 8; i < len; i++)
		if (p[i] != rec_byte((int)slot, seq, i)) {
			g_errs++;
			return 0;
		}
	g_slot[slot].found[seq] = lsn;
	return 0;
}

static void
dump_state(void)
{
	struct wal_waiter *q;
	int n = 0, i;
	fprintf(stderr, "      flushing %d direct %d next_lsn %llu copied_lsn %llu "
	    "durable_lsn %llu wq %s", g_wal->flushing, g_wal->direct,
	    (unsigned long long)g_wal->next_lsn,
	    (unsigned long long)g_wal->copied_lsn,
	    (unsigned long long)atomic_load(&g_wal->durable_lsn),
	    g_wal->wq == NULL ? "empty" : "");
	for (q = g_wal->wq; q != NULL; q = q->next)
		n++;
	if (n)
		fprintf(stderr, "%d waiters", n);
	fprintf(stderr, "\n      in wal_commit:");
	for (i = 0; i < N_SLOTS; i++)
		if (g_slot[i].phase)
			fprintf(stderr, " slot%d(live %d)", i, g_slot[i].live);
	fprintf(stderr, "\n");
}

struct result {
	int      rc;
	uint64_t fp;
	uint64_t acked, kills;
	int      buggified;
};

/* One seeded run.  Returns 0, or 1 on an invariant violation (the
 * process state is then not torn down: a wedged log has parked fibers
 * pointing into it). */
static int
run_one(uint64_t seed, struct result *res)
{
	char path[256];
	const char *td = getenv("TMPDIR");
	wal_opts_t wo;
	xtc_proc_opts_t ko = { .size = sizeof(xtc_proc_opts_t), .name = "killer" };
	xtc_pid_t kp;
	uint64_t fp = 0xCBF29CE484222325ull, durable;
	int fd, i, rc, lost = 0;
	uint32_t q;

	memset(g_slot, 0, sizeof g_slot);
	g_errs = 0;
	g_final_ok = 0;
	g_stop_ns = 0;
	g_kills = g_acked = 0;
	/* Per-seed shape from the seed itself, outside the sim streams. */
	g_big = (int)(seed >> 7 & 1);
	g_link = (seed >> 9 & 3) == 0;
	g_kill_writer = (int)(seed >> 11 & 1);

	snprintf(path, sizeof path, "%s/sim_walkill_XXXXXX",
	    td != NULL && td[0] != '\0' ? td : "/tmp");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	if (wal_open(&wo, &g_wal) != XTC_OK ||
	    xtc_exec_init(&g_exec, N_LOOPS) != XTC_OK)
		return 1;
	xtc_sim_io_wb_enable(1);
	xtc_sim_io_faults_enable(20 * 1000LL, 300 * 1000LL, 0);
	xtc_sim_buggify_enable(600);
	if ((wal_writer_spawn(g_wal, xtc_exec_loop(g_exec, 0), NULL)) != XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(g_exec, 0), killer, NULL, &ko, &kp) !=
	    XTC_OK)
		return 1;
	/* Adversarial scheduling on part of the sweep (reset per run by
	 * xtc_sim_deactivate, so set after nothing else can clear it). */
	if (seed >> 3 & 1)
		xtc_sim_sched_pessimal(400);
	if (seed >> 5 & 1)
		xtc_sim_swizzle_enable(200);
	rc = xtc_sim_exec_run(g_exec, seed, MAX_STEPS);
	res->buggified = xtc_sim_buggify_active_count();
	xtc_sim_buggify_disable();
	xtc_sim_io_faults_disable();
	res->rc = rc;
	res->acked = g_acked;
	res->kills = g_kills;
	if (rc != XTC_OK || !g_final_ok || g_stop_ns > 1000LL * 1000 * 1000) {
		printf("  seed 0x%016llx: FAIL LIVENESS: %s (rc %d, final commits "
		    "%s, writer stop %lld ms, kills %llu, acked %llu, big %d link %d "
		    "writer-kill %d)\n", (unsigned long long)seed,
		    rc == XTC_E_DEADLK ? "deadlock: every proc parked, no waker" :
		    rc == XTC_E_AGAIN ? "step budget exhausted (no quiescence)" :
		    rc != XTC_OK ? "sim run error" :
		    !g_final_ok ? "log refused commits after the kills" :
		    "wal_writer_stop stalled", rc, g_final_ok ? "ok" : "FAILED",
		    (long long)(g_stop_ns / 1000000),
		    (unsigned long long)g_kills, (unsigned long long)g_acked,
		    g_big, g_link, g_kill_writer);
		dump_state();
		return 1;
	}
	(void)xtc_exec_fini(g_exec);
	g_exec = NULL;

	/* The crash: only what an fdatasync covered survives. */
	durable = xtc_sim_io_durable_end(wal_fd(g_wal));
	xtc_sim_io_wb_enable(0);
	wal_close(g_wal);
	g_wal = NULL;
	if (truncate(path, (off_t)durable) != 0 ||
	    wal_scan(path, scan_cb, NULL) != XTC_OK)
		g_errs++;
	unlink(path);
	for (i = 0; i < N_SLOTS; i++)
		for (q = 0; q < QUOTA + FINAL_N; q++) {
			if (g_slot[i].ack[q] != 0 &&
			    g_slot[i].found[q] != g_slot[i].ack[q] && lost++ == 0)
				printf("  seed 0x%016llx: FAIL DURABILITY: acked slot %d "
				    "seq %u lsn %llu missing after the crash cut "
				    "(found lsn %llu)\n", (unsigned long long)seed, i, q,
				    (unsigned long long)g_slot[i].ack[q],
				    (unsigned long long)g_slot[i].found[q]);
			fp = (fp ^ g_slot[i].ack[q]) * 0x100000001B3ull;
			fp = (fp ^ g_slot[i].found[q]) * 0x100000001B3ull;
		}
	if (g_errs && !lost)
		printf("  seed 0x%016llx: FAIL: %d commit errors / bad records\n",
		    (unsigned long long)seed, g_errs);
	res->fp = (fp ^ g_kills) * 0x100000001B3ull;
	return lost || g_errs;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x77616c6b;      /* "walk" */
	int n = 24, i, fails = 0, max_bug = 0;
	uint64_t tot_kills = 0, tot_acked = 0;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	printf("== wal kill DST: %d seeds from base 0x%llx ==\n", n,
	    (unsigned long long)base);
	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		struct result a, b;
		if (run_one(seed, &a) != 0) {
			printf("  replay: test_sim_wal_kill 0x%llx 1\n",
			    (unsigned long long)seed);
			fflush(stdout);
			_exit(1);                 /* wedged state: no teardown */
		}
		if (run_one(seed, &b) != 0 || a.fp != b.fp || a.rc != b.rc) {
			printf("  seed 0x%016llx: FAIL REPLAY: fingerprint %llx != "
			    "%llx\n", (unsigned long long)seed,
			    (unsigned long long)a.fp, (unsigned long long)b.fp);
			fflush(stdout);
			_exit(1);
		}
		tot_kills += a.kills;
		tot_acked += a.acked;
		if (a.buggified > max_bug)
			max_bug = a.buggified;
	}
	if (tot_kills == 0) {
		printf("FAIL: no kill was delivered across the sweep\n");
		fails++;
	}
	if (fails)
		return 1;
	printf("OK: %d seeds, %llu kills, %llu acked commits, all durable after "
	    "the crash cut, no wedge, replay identical (up to %d buggify sites "
	    "active)\n", n, (unsigned long long)tot_kills,
	    (unsigned long long)tot_acked, max_bug);
	return 0;
}

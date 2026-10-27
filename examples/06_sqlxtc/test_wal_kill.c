/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_kill.c
 *	Kill committers mid-commit, a lot, and check the log survives.
 *
 *	NC committer procs on an NL-loop executor commit records whose
 *	payload names (slot, seq).  A boss proc monitors them, kills a
 *	random committer that is inside wal_commit every few microseconds
 *	(xtc_exit_pid, or a linked child dying, per mode) and respawns
 *	the slot.  A watchdog thread fails the run when no commit is
 *	acknowledged for HANG_S seconds and dumps the log's state.  At the
 *	end the boss stops killing, waits for every committer, then
 *	commits again itself: the log must still accept commits.
 *
 *	Asserts: no hang; every ACKED commit is in the log (wal_scan) at
 *	its acked LSN with its exact payload, and at or below the durable
 *	LSN; the log keeps accepting commits after the kills.
 *
 *	wal.c is included so the watchdog can print its internal state.
 *
 *	usage: test_wal_kill [mode [ms [seed]]]   (default all 600 0x5eed)
 *	  mode: exit   xtc_exit_pid on committers inside wal_commit
 *	        any    xtc_exit_pid on any committer, any time
 *	        link   a linked child exits abnormally (link kill)
 *	        big    exit, with some records over a quarter of the ring
 *	        mid    exit, 256 KiB records (long copies) + off-loop threads
 *	        preempt  exit + involuntary preemption on
 *	        preempt-nokill  involuntary preemption, no kills (control)
 *	        writer the writer proc killed; wal_writer_stop must not stall
 *	        all    every mode above in turn (the default)
 */

#include "wal.c"

#include "xtc_exec.h"
#include "xtc_preempt.h"
#include "t_tmp.h"

#define NL        4
#define NC        24
#define NT        4               /* off-loop thread committers (mid) */
#define MAXSEQ    (1u << 17)
#define NS        (NC + NT + 1)   /* + the boss's final-commit slot */
#define FINAL     (NC + NT)
#define HANG_S    5
#define BIG_LEN   (3u << 20)      /* > WAL_RING / 4: wal_append_direct */
#define MID_LEN   (256u << 10)

struct slot {
	_Atomic int      phase;       /* 1 while inside wal_commit */
	_Atomic uint32_t seq;         /* next sequence number */
	xtc_pid_t        pid;         /* boss-owned */
	int              live;        /* boss-owned */
	uint64_t        *ack;         /* ack[seq] = acked LSN, 0 = none */
};

static struct {
	const char *mode;
	int         kill_any, link, big, mid, preempt, nokill, writer;
} g_cfg;

static wal_t           *g_wal;
static xtc_exec_t      *g_exec;
static struct slot      g_slot[NS];
static _Atomic int      g_stop;          /* stop killing + committing */
static _Atomic int      g_done;          /* run finished (watchdog off) */
static _Atomic uint64_t g_acked;         /* acked commits (progress) */
static _Atomic uint64_t g_kills, g_respawn, g_errs;
static _Atomic int      g_final_ok;
static uint64_t         g_seed;
static int64_t          g_ms;

static uint64_t
rng(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static uint32_t
rec_len(int slot, uint32_t seq)
{
	uint32_t h = (uint32_t)slot * 2654435761u ^ seq * 40503u;
	if (g_cfg.big && h % 61 == 0)
		return BIG_LEN + h % 4096;
	if (g_cfg.mid && h % 5 == 0)
		return MID_LEN + h % 4096;
	return 24 + h % 300;
}

static uint8_t
rec_byte(int slot, uint32_t seq, uint32_t i)
{
	return (uint8_t)(slot * 31 + seq * 7 + i);
}

/* payload: u32 slot, u32 seq, then pattern bytes */
static void
rec_fill(uint8_t *p, int slot, uint32_t seq, uint32_t len)
{
	uint32_t i, s = (uint32_t)slot;
	memcpy(p, &s, 4);
	memcpy(p + 4, &seq, 4);
	for (i = 8; i < len; i++)
		p[i] = rec_byte(slot, seq, i);
}

/* One commit for `slot`; returns wal_commit's rc. */
static int
commit_one(int slot, uint8_t *buf)
{
	struct slot *s = &g_slot[slot];
	uint32_t seq = atomic_fetch_add(&s->seq, 1);
	uint32_t len;
	uint64_t lsn = 0;
	int rc;

	if (seq >= MAXSEQ)
		return XTC_E_RESOURCE;
	len = rec_len(slot, seq);
	rec_fill(buf, slot, seq, len);
	atomic_store(&s->phase, 1);
	rc = wal_commit(g_wal, buf, len, &lsn);
	atomic_store(&s->phase, 0);
	if (rc == XTC_OK) {
		s->ack[seq] = lsn;
		if (lsn > wal_durable_lsn(g_wal))
			atomic_fetch_add(&g_errs, 1);   /* acked, not durable */
		atomic_fetch_add(&g_acked, 1);
	} else {
		atomic_fetch_add(&g_errs, 1);
	}
	return rc;
}

static void
linked_child(void *arg)
{
	(void)xtc_proc_sleep((int64_t)(uintptr_t)arg);
	(void)xtc_exit_self(9);              /* abnormal: kills the linked peer */
}

static void
committer(void *arg)
{
	int slot = (int)(intptr_t)arg;
	uint64_t r = g_seed ^ (uint64_t)(slot + 1) * 0x9E3779B97F4A7C15ull;
	/* xtc_malloc, not malloc: under involuntary preemption a fiber
	 * preempted inside glibc's arena lock wedges its loop-mates (libxtc's
	 * allocator brackets the preemption timer; raw malloc does not). */
	uint8_t *buf = xtc_malloc(BIG_LEN + 4096);
	int n = 0;

	if (buf == NULL)
		return;
	(void)xtc_proc_at_exit(xtc_free, buf);   /* a kill unwinds past it */
	while (!atomic_load(&g_stop)) {
		if (g_cfg.link && (n++ & 7) == 0) {
			xtc_proc_opts_t o = { .size = sizeof(xtc_proc_opts_t), .name = "lnk" };
			xtc_pid_t c;
			(void)xtc_proc_spawn_link(xtc_exec_loop(g_exec,
			    (int)(rng(&r) % NL)), linked_child,
			    (void *)(uintptr_t)(rng(&r) % 400000), &o, &c);
		}
		if (commit_one(slot, buf) == XTC_E_RESOURCE)
			break;
	}
}

static void
dump_state(const char *why)
{
	wal_t *w = g_wal;
	struct wal_waiter *q;
	int n = 0, locked, i;

	fprintf(stderr, "\n=== %s (mode %s seed %llu) ===\n", why, g_cfg.mode,
	    (unsigned long long)g_seed);
	locked = pthread_mutex_trylock(&w->fl_mu) == 0;
	fprintf(stderr, "fl_mu %s; res_mu %s\n", locked ? "free" : "HELD",
	    pthread_mutex_trylock(&w->res_mu) == 0 ?
	    (pthread_mutex_unlock(&w->res_mu), "free") : "HELD");
	fprintf(stderr, "flushing %d direct %d next_lsn %llu copied_lsn %llu "
	    "durable_lsn %llu\n", w->flushing, w->direct,
	    (unsigned long long)w->next_lsn, (unsigned long long)w->copied_lsn,
	    (unsigned long long)atomic_load(&w->durable_lsn));
	fprintf(stderr, "res_pos %llu copied_pos %llu written %llu durable_off "
	    "%llu failed %d\n", (unsigned long long)w->res_pos,
	    (unsigned long long)w->copied_pos,
	    (unsigned long long)atomic_load(&w->written),
	    (unsigned long long)atomic_load(&w->durable_off),
	    atomic_load(&w->failed));
	for (q = w->wq; q != NULL; q = q->next, n++)
		if (n < 8)
			fprintf(stderr, "  wq[%d] need %llu baton %d\n", n,
			    (unsigned long long)q->need, q->baton);
	fprintf(stderr, "wq length %d (live committers %d)\n", n, NC);
	fprintf(stderr, "next slot lsn %llu has %llu\n",
	    (unsigned long long)(w->copied_lsn + 1),
	    (unsigned long long)atomic_load(&w->slot[(w->copied_lsn + 1) &
	    (WAL_SLOTS - 1)].lsn));
	for (i = 0; i < NS; i++)
		if (atomic_load(&g_slot[i].phase))
			fprintf(stderr, "  slot %d in wal_commit (seq %u)\n", i,
			    atomic_load(&g_slot[i].seq));
	fprintf(stderr, "acked %llu kills %llu respawns %llu errs %llu\n",
	    (unsigned long long)atomic_load(&g_acked),
	    (unsigned long long)atomic_load(&g_kills),
	    (unsigned long long)atomic_load(&g_respawn),
	    (unsigned long long)atomic_load(&g_errs));
	if (locked)
		(void)pthread_mutex_unlock(&w->fl_mu);
}

/* Fails the run (exit 3) when no commit is acked for HANG_S seconds. */
static void *
watchdog(void *arg)
{
	uint64_t last = 0;
	int64_t since = xtc_clock_mono();
	(void)arg;
	while (!atomic_load(&g_done)) {
		uint64_t a = atomic_load(&g_acked);
		int64_t now = xtc_clock_mono();
		if (a != last) {
			last = a;
			since = now;
		} else if (now - since > (int64_t)HANG_S * 1000000000LL) {
			dump_state("HANG: no commit acknowledged");
			fprintf(stderr, "FAIL: test_wal_kill %s: hang\n", g_cfg.mode);
			if (getenv("WAL_KILL_ABORT") != NULL)
				abort();          /* leave a core / stop under gdb */
			_exit(3);
		}
		(void)xtc_sleep_ns(50LL * 1000 * 1000);
	}
	return NULL;
}

static void *
thread_committer(void *arg)
{
	int slot = (int)(intptr_t)arg;
	uint8_t *buf = malloc(BIG_LEN + 4096);
	if (buf == NULL)
		return NULL;
	while (!atomic_load(&g_stop))
		if (commit_one(slot, buf) == XTC_E_RESOURCE)
			break;
	free(buf);
	return NULL;
}

static int
spawn_slot(int i)
{
	xtc_proc_opts_t o = { .size = sizeof(xtc_proc_opts_t), .name = "cmt" };
	uint64_t ref;
	if (xtc_proc_spawn_monitor(xtc_exec_loop(g_exec, i % NL), committer,
	    (void *)(intptr_t)i, &o, &g_slot[i].pid, &ref) != XTC_OK)
		return -1;
	g_slot[i].live = 1;
	return 0;
}

/* Reap DOWNs; respawn unless stopping.  Returns committers still live. */
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
					if (respawn && spawn_slot(i) == 0)
						atomic_fetch_add(&g_respawn, 1);
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
	uint8_t *buf = xtc_malloc(BIG_LEN + 4096);
	int64_t t0;
	int i, wkilled = 0;

	(void)arg;
	for (i = 0; i < NC; i++)
		(void)spawn_slot(i);
	while (xtc_clock_mono() < end) {
		int k = (int)(rng(&r) % NC);
		(void)xtc_proc_sleep((int64_t)(rng(&r) % 300) * 1000);
		(void)reap(1);
		if (g_cfg.writer && !wkilled &&
		    xtc_clock_mono() > end - g_ms * 500000LL) {
			/* the writer proc, parked in xtc_recv, killed mid-run */
			if (xtc_exit_pid(wal_writer_pid(g_wal), 7) == XTC_OK)
				atomic_fetch_add(&g_kills, 1);
			wkilled = 1;
		}
		if (!g_slot[k].live || g_cfg.link || g_cfg.nokill)
			continue;
		if (!g_cfg.kill_any && !atomic_load(&g_slot[k].phase))
			continue;
		if (xtc_exit_pid(g_slot[k].pid, 7) == XTC_OK)
			atomic_fetch_add(&g_kills, 1);
	}
	atomic_store(&g_stop, 1);
	while (reap(0) > 0)
		(void)xtc_proc_sleep(1000LL * 1000);
	/* The log must still accept commits after every kill. */
	for (i = 0; buf != NULL && i < 50; i++)
		if (commit_one(FINAL, buf) != XTC_OK)
			break;
	xtc_free(buf);
	t0 = xtc_clock_mono();
	(void)wal_writer_stop(g_wal);
	t0 = xtc_clock_mono() - t0;
	if (t0 > 2000LL * 1000 * 1000)
		fprintf(stderr, "  wal_writer_stop took %lld ms\n",
		    (long long)(t0 / 1000000));
	atomic_store(&g_final_ok, i == 50 && t0 <= 2000LL * 1000 * 1000);
}

static int
scan_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	uint64_t **found = user;
	const uint8_t *p = rec;
	uint32_t slot, seq, i;
	if (len < 8)
		return 0;
	memcpy(&slot, p, 4);
	memcpy(&seq, p + 4, 4);
	if (slot >= NS || seq >= MAXSEQ || len != rec_len((int)slot, seq)) {
		atomic_fetch_add(&g_errs, 1);
		return 0;
	}
	for (i = 8; i < len; i++)
		if (p[i] != rec_byte((int)slot, seq, i)) {
			atomic_fetch_add(&g_errs, 1);
			return 0;
		}
	found[slot][seq] = lsn;
	return 0;
}

/* Where does the scan stop, and why?  Prints the first bad frame. */
static void
scan_diag(const char *path)
{
	int fd = open(path, O_RDONLY);
	off_t o = 0, fsize;
	uint64_t lsn, n = 0;
	uint32_t len;
	uint8_t hdr[WAL_REC_HDR + 8];
	if (fd < 0)
		return;
	fsize = wal_fsize(fd);
	while (wal_frame_ok(fd, o, fsize, &lsn, &len)) {
		o += (off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC;
		n++;
	}
	memset(hdr, 0, sizeof hdr);
	(void)pread(fd, hdr, sizeof hdr, o);
	memcpy(&lsn, hdr, 8);
	memcpy(&len, hdr + 8, 4);
	fprintf(stderr, "  scan: %llu frames, stops at %lld of %lld (log end %llu); "
	    "there: lsn %llu len %u slot %u seq %u\n", (unsigned long long)n,
	    (long long)o, (long long)fsize, (unsigned long long)wal_size(g_wal),
	    (unsigned long long)lsn, len, hdr[12] | hdr[13] << 8,
	    hdr[16] | hdr[17] << 8);
	if (len > 8 && len < 4096) {
		uint8_t b[4096 + 32];
		uint32_t sl, sq, i, bad = 0;
		(void)pread(fd, b, len + WAL_REC_HDR + WAL_REC_CRC, o);
		memcpy(&sl, b + 12, 4);
		memcpy(&sq, b + 16, 4);
		for (i = 8; i < len; i++)
			if (b[12 + i] != rec_byte((int)sl, sq, i)) {
				if (bad++ < 40)
					fprintf(stderr, "    body[%u] (file off %lld, ring "
					    "addr mod 32 = %llu) got %02x want %02x\n", i,
					    (long long)(o + 12 + i),
					    (unsigned long long)((uintptr_t)(g_wal->ring +
					    ((o + 12 + i) & (WAL_RING - 1))) & 31),
					    b[12 + i], rec_byte((int)sl, sq, i));
			}
		fprintf(stderr, "    %u bad body bytes; len ok %d\n", bad,
		    len == rec_len((int)sl, sq));
	}
	close(fd);
}

static int
run_mode(const char *mode)
{
	char path[256];
	wal_opts_t wo;
	xtc_exec_t *exec = NULL;
	xtc_proc_opts_t bo = { .size = sizeof(xtc_proc_opts_t), .name = "boss" };
	xtc_pid_t bp;
	pthread_t wd, th[NT];
	uint64_t *found[NS];
	uint64_t acked = 0, lost = 0, extra = 0;
	int i, fd, fail = 0;
	uint32_t q;

	memset(&g_cfg, 0, sizeof g_cfg);
	g_cfg.mode = mode;
	g_cfg.kill_any = strcmp(mode, "any") == 0;
	g_cfg.link = strcmp(mode, "link") == 0;
	g_cfg.big = strcmp(mode, "big") == 0;
	g_cfg.mid = strcmp(mode, "mid") == 0;
	g_cfg.preempt = strncmp(mode, "preempt", 7) == 0;
	g_cfg.nokill = strcmp(mode, "preempt-nokill") == 0;
	g_cfg.writer = strcmp(mode, "writer") == 0;
	atomic_store(&g_stop, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_acked, 0);
	atomic_store(&g_kills, 0);
	atomic_store(&g_respawn, 0);
	atomic_store(&g_errs, 0);
	atomic_store(&g_final_ok, 0);
	for (i = 0; i < NS; i++) {
		memset(&g_slot[i], 0, sizeof g_slot[i]);
		g_slot[i].ack = calloc(MAXSEQ, sizeof(uint64_t));
		found[i] = calloc(MAXSEQ, sizeof(uint64_t));
		if (g_slot[i].ack == NULL || found[i] == NULL)
			return 1;
	}
	t_tmpl(path, sizeof path, "sqlxtc-walkill");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	if (wal_open(&wo, &g_wal) != XTC_OK || xtc_exec_init(&exec, NL) != XTC_OK)
		return 1;
	g_exec = exec;
	if (g_cfg.preempt) {
		(void)xtc_exec_set_preempt(exec, 50LL * 1000);
		xtc_preempt_set_involuntary(1);
	}
	(void)wal_writer_spawn(g_wal, xtc_exec_loop(exec, 0), NULL);
	if (xtc_proc_spawn(xtc_exec_loop(exec, 0), boss, exec, &bo, &bp) != XTC_OK)
		return 1;
	(void)pthread_create(&wd, NULL, watchdog, NULL);
	for (i = 0; g_cfg.mid && i < NT; i++)
		(void)pthread_create(&th[i], NULL, thread_committer,
		    (void *)(intptr_t)(NC + i));
	(void)xtc_exec_run(exec);
	for (i = 0; g_cfg.mid && i < NT; i++)
		(void)pthread_join(th[i], NULL);
	atomic_store(&g_done, 1);
	(void)pthread_join(wd, NULL);
	(void)xtc_exec_fini(exec);
	if (g_cfg.preempt)
		xtc_preempt_set_involuntary(0);

	if (wal_scan(path, scan_cb, found) != XTC_OK)
		fail = 1;
	for (i = 0; i < NS; i++)
		for (q = 0; q < MAXSEQ; q++) {
			if (g_slot[i].ack[q] != 0) {
				acked++;
				if (found[i][q] != g_slot[i].ack[q]) {
					if (lost++ < 5)
						fprintf(stderr, "  LOST slot %d seq %u lsn "
						    "%llu (found %llu)\n", i, q,
						    (unsigned long long)g_slot[i].ack[q],
						    (unsigned long long)found[i][q]);
				}
			} else if (found[i][q] != 0) {
				extra++;          /* unacked (killed) but logged: legal */
			}
		}
	printf("  %-7s kills %llu respawns %llu acked %llu unacked-logged %llu "
	    "errs %llu lost %llu final %s\n", mode,
	    (unsigned long long)atomic_load(&g_kills),
	    (unsigned long long)atomic_load(&g_respawn),
	    (unsigned long long)acked, (unsigned long long)extra,
	    (unsigned long long)atomic_load(&g_errs), (unsigned long long)lost,
	    atomic_load(&g_final_ok) ? "ok" : "FAILED");
	if (lost)
		scan_diag(path);
	if (lost || atomic_load(&g_errs) || !atomic_load(&g_final_ok) ||
	    (!g_cfg.nokill && atomic_load(&g_kills) + atomic_load(&g_respawn) == 0))
		fail = 1;
	wal_close(g_wal);
	g_wal = NULL;
	unlink(path);
	for (i = 0; i < NS; i++) {
		free(g_slot[i].ack);
		free(found[i]);
	}
	return fail;
}

int
main(int argc, char **argv)
{
	static const char *all[] = { "exit", "any", "link", "big", "mid", "writer",
	    NULL };
	const char *mode = argc > 1 ? argv[1] : "all";
	int i, fail = 0;

	g_ms = argc > 2 ? atol(argv[2]) : 600;
	g_seed = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x5eed;
	if (g_seed == 0)
		g_seed = 1;
	if (strcmp(mode, "all") == 0) {
		for (i = 0; all[i] != NULL; i++)
			fail |= run_mode(all[i]);
	} else {
		fail = run_mode(mode);
	}
	if (fail) {
		fprintf(stderr, "FAIL: test_wal_kill\n");
		return 1;
	}
	printf("  ok   killed committers never hang the log or lose an acked "
	    "commit\n");
	return 0;
}

/*
 * test_sim_down_kind -- DST of DOWN classification: every way a proc can
 * end reaches its monitor as the kind and code that way promises.
 *
 * A supervisor fiber on each loop of a multi-loop executor spawns and
 * monitors (atomically) one child per ending below, some of them on a
 * peer loop, with a seeded order, seeded yields and seeded kill reasons:
 *
 *   CLEAN   the body returns                     -> CLEAN
 *   EXIT    xtc_exit_self(code), code seeded from
 *           { 1, 7, -1, -2, INT_MIN }            -> EXIT,   exit_code code
 *   KILL    parked, xtc_exit_pid(child, r), r from
 *           { 0, 1, -1, -14, INT_MAX }           -> KILLED, exit_code r
 *   MKILL   killed while masked, honored at
 *           xtc_mask_leave                       -> KILLED, exit_code r
 *   FRAME   SIGSEGV in its own recovery frame,
 *           then xtc_exit_self(seeded code)      -> SIGNAL, signal 11
 *   INIT    an xtc_svr whose init fails with
 *           XTC_E_NOMEM                          -> EXIT,   exit_code -2
 *
 * The single-thread sim runs a real synchronous SIGSEGV deterministically
 * (the fault guard's siglongjmp is ordinary control flow on the sim
 * thread; test_sim_scope relies on the same), so FRAME is the real
 * contained-fault path.
 *
 * Assertions per seed: every child produced exactly one DOWN, each DOWN
 * matches its oracle, each xtc_exit_pid returned XTC_OK, and the run
 * replays byte-identically (counts and scheduler state hash).  A
 * mismatch prints the child, the expected and the observed kind/code.
 *
 * Planted bugs (src/inc/xtc_dst_inject.h) this test must catch:
 *   31 EXITNEG    the reason+1 kill/exit encoding: a kill with -1 is
 *                 dropped and xtc_exit_self(-1) reads CLEAN.
 *   32 FAULTKIND  the fault_sig override dropped: FRAME reads EXIT 11.
 *   33 KILLKIND   a kill reported as EXIT, not KILLED.
 *   34 INITEXIT   a failed svr init returns normally: INIT reads CLEAN.
 *
 * Usage: test_sim_down_kind [base [count]]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"
#include "xtc_svr.h"

#define N_LOOPS 3

enum { E_CLEAN, E_EXIT, E_KILL, E_MKILL, E_FRAME, E_INIT, N_ENDINGS };

static const char *const ending_name[N_ENDINGS] = { "CLEAN", "EXIT", "KILL",
	"MKILL", "FRAME", "INIT" };

static const int exit_codes[] = { 1, 7, -1, -2, INT_MIN };
static const int kill_reasons[] = { 0, 1, -1, -14, INT_MAX };

/* One child: its ending, the code it exits or is killed with, and what
 * its DOWN must say. */
struct child {
	int ending;
	int code;
	int want_kind;
	int want_code; /* exit_code, or signal for SIGNAL */
	int yields;
	xtc_pid_t pid;
	uint64_t ref;
	xtc_svr_t *svr;
	int seen;
};

struct sup {
	xtc_exec_t *e;
	int loop;
	struct child c[N_ENDINGS];
};

static struct sup g_sup[N_LOOPS];
static int g_bad;
static int g_downs;
static int g_kill_rc_bad;
static int g_sups_done;

/* Report the first few mismatches; later ones only count. */
static void
bad(const struct sup *s, const struct child *c, const xtc_down_info_t *di)
{
	if (xtc_atomic_fetch_add_i32(&g_bad, 1, xtc_mo_seq_cst) < 4)
		printf("    loop %d %s(code %d): want kind %d code %d, got "
		       "kind %d exit_code %d signal %d\n",
		    s->loop, ending_name[c->ending], c->code, c->want_kind,
		    c->want_code, di != NULL ? (int)di->kind : -1,
		    di != NULL ? di->exit_code : 0,
		    di != NULL ? di->signal : 0);
}

static void
child_body(void *arg)
{
	struct child *c = arg;
	volatile uintptr_t addr = 0x10;
	void *m;
	size_t sz;
	int i, sig;

	for (i = 0; i < c->yields; i++)
		xtc_yield();
	switch (c->ending) {
	case E_EXIT:
		(void)xtc_exit_self(c->code);
		return;
	case E_KILL:
		for (;;) {
			if (xtc_recv(&m, &sz, -1) == XTC_OK)
				xtc_free(m);
		}
	case E_MKILL:
		(void)xtc_mask_enter();
		(void)xtc_proc_sleep(50LL * 1000 * 1000);
		(void)xtc_mask_leave(); /* the latched kill unwinds here */
		return;
	case E_FRAME:
		sig = xtc_proc_recovery_arm();
		if (sig == 0)
			*(volatile int *)addr = 1; /* boom */
		(void)xtc_exit_self(c->code);
		return;
	default:
		return;
	}
}

static int
nomem_init(void *st)
{
	(void)st;
	return XTC_E_NOMEM;
}

static const xtc_svr_callbacks_t NOMEM_CB = {
	.size = sizeof(xtc_svr_callbacks_t),
	.init = nomem_init,
};

/* Draw this supervisor's children: the code, the yields and the loop
 * each runs on come from the APP stream, so the scheduler streams are
 * untouched by the draw. */
static void
draw(struct sup *s)
{
	int i;
	for (i = 0; i < N_ENDINGS; i++) {
		struct child *c = &s->c[i];
		memset(c, 0, sizeof *c);
		c->ending = i;
		c->yields = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4);
		switch (i) {
		case E_CLEAN:
			c->want_kind = XTC_DOWN_KIND_CLEAN;
			break;
		case E_EXIT:
			c->code =
			    exit_codes[__xtc_sim_rng_range(XTC_SIM_RNG_APP,
			        sizeof exit_codes / sizeof exit_codes[0])];
			c->want_kind = XTC_DOWN_KIND_EXIT;
			c->want_code = c->code;
			break;
		case E_KILL:
		case E_MKILL:
			c->code =
			    kill_reasons[__xtc_sim_rng_range(XTC_SIM_RNG_APP,
			        sizeof kill_reasons / sizeof kill_reasons[0])];
			c->want_kind = XTC_DOWN_KIND_KILLED;
			c->want_code = c->code;
			break;
		case E_FRAME:
			c->code = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 3);
			c->want_kind = XTC_DOWN_KIND_SIGNAL;
			c->want_code = SIGSEGV;
			break;
		case E_INIT:
			c->want_kind = XTC_DOWN_KIND_EXIT;
			c->want_code = XTC_E_NOMEM;
			break;
		}
	}
}

static void
check_down(struct sup *s, const void *m, size_t n)
{
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	int i, got;
	if (xtc_down_decode_ex(m, n, &di) != XTC_OK) {
		bad(s, &s->c[0], NULL);
		return;
	}
	for (i = 0; i < N_ENDINGS; i++) {
		struct child *c = &s->c[i];
		if (c->ref != di.ref)
			continue;
		c->seen++;
		got =
		    di.kind == XTC_DOWN_KIND_SIGNAL ? di.signal : di.exit_code;
		if (c->seen != 1 || (int)di.kind != c->want_kind ||
		    got != c->want_code || di.reason != got)
			bad(s, c, &di);
		xtc_atomic_fetch_add_i32(&g_downs, 1, xtc_mo_seq_cst);
		return;
	}
	bad(s, &s->c[0], &di); /* a DOWN for no child of ours */
}

static void
spawn_one(struct sup *s, struct child *c)
{
	unsigned li = (unsigned)__xtc_sim_rng_range(XTC_SIM_RNG_APP, N_LOOPS);
	xtc_loop_t *l = xtc_exec_loop(s->e, li);
	if (c->ending == E_INIT) {
		/* The server cannot run before this fiber yields when it is
		 * on our loop; on a peer loop it may, so the monitor can see
		 * NOPROC.  Keep the server on our own loop. */
		l = xtc_exec_loop(s->e, (unsigned)s->loop);
		if (xtc_svr_start(l, &NOMEM_CB, NULL, NULL, &c->svr) !=
		        XTC_OK ||
		    xtc_monitor(xtc_svr_pid(c->svr), &c->ref) != XTC_OK)
			bad(s, c, NULL);
		return;
	}
	if (xtc_proc_spawn_monitor(l, child_body, c, NULL, &c->pid, &c->ref) !=
	    XTC_OK)
		bad(s, c, NULL);
}

static void
supervisor(void *arg)
{
	struct sup *s = arg;
	int order[N_ENDINGS];
	int i, j, t, want = N_ENDINGS, seen = 0, tries;

	draw(s);
	for (i = 0; i < N_ENDINGS; i++)
		order[i] = i;
	for (i = N_ENDINGS - 1; i > 0; i--) {
		j = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, (uint64_t)i + 1);
		t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	for (i = 0; i < N_ENDINGS; i++)
		spawn_one(s, &s->c[order[i]]);

	/* Let the killable children park, then kill them. */
	(void)xtc_proc_sleep(5LL * 1000 * 1000);
	for (i = 0; i < N_ENDINGS; i++) {
		struct child *c = &s->c[order[i]];
		if (c->ending != E_KILL && c->ending != E_MKILL)
			continue;
		if (xtc_exit_pid(c->pid, c->code) != XTC_OK)
			xtc_atomic_fetch_add_i32(&g_kill_rc_bad, 1,
			    xtc_mo_seq_cst);
	}

	for (tries = 0; tries < 400 && seen < want; tries++) {
		void *m = NULL;
		size_t n = 0;
		if (xtc_recv(&m, &n, 10LL * 1000 * 1000) != XTC_OK)
			continue;
		check_down(s, m, n);
		seen++;
		xtc_free(m);
	}
	for (i = 0; i < N_ENDINGS; i++) {
		struct child *c = &s->c[i];
		if (c->seen == 0)
			bad(s, c, NULL);
		if (c->svr != NULL)
			(void)xtc_svr_join(c->svr, 0);
		/* A dropped kill leaves its child parked: release it so the
		 * run quiesces and the miss is reported, not a hang. */
		if (c->seen == 0 && !xtc_pid_is_none(c->pid))
			(void)xtc_exit_pid(c->pid, 99);
	}
	if (xtc_atomic_fetch_add_i32(&g_sups_done, 1, xtc_mo_seq_cst) + 1 >=
	    N_LOOPS)
		(void)xtc_exec_stop(s->e);
}

static int
run_one(uint64_t seed, int *out_downs, int *out_bad, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	xtc_atomic_store_i32(&g_bad, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_downs, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_kill_rc_bad, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_sups_done, 0, xtc_mo_seq_cst);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(e, 1);
	for (i = 0; i < N_LOOPS; i++) {
		g_sup[i].e = e;
		g_sup[i].loop = i;
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)i), supervisor,
		    &g_sup[i], NULL, NULL);
	}
	rc = xtc_sim_exec_run(e, seed, 20000000);
	*out_downs = xtc_atomic_load_i32(&g_downs, xtc_mo_seq_cst);
	*out_bad = xtc_atomic_load_i32(&g_bad, xtc_mo_seq_cst) +
	    xtc_atomic_load_i32(&g_kill_rc_bad, xtc_mo_seq_cst);
	*out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x646b; /* "dk" */
	int n = 24, i, fails = 0;
	int want = N_LOOPS * N_ENDINGS;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	if (xtc_fault_guard_install() != XTC_OK) {
		printf("FAIL: xtc_fault_guard_install\n");
		return 1;
	}
	printf("== DOWN classification DST: %d seeds from base 0x%llx ==\n", n,
	    (unsigned long long)base);
	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int downs = 0, nbad = 0, downs2 = 0, nbad2 = 0, rc, rc2;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &downs, &nbad, &st);
		if (rc == XTC_OK && nbad == 0 && downs == want) {
			rc2 = run_one(seed, &downs2, &nbad2, &st2);
			if (rc2 != rc || downs2 != downs || nbad2 != nbad ||
			    st2 != st) {
				printf(
				    "  seed 0x%016llx: FAIL replay diverged\n",
				    (unsigned long long)seed);
				fails++;
			}
			continue;
		}
		printf("  seed 0x%016llx: FAIL a DOWN misclassified or missing "
		       "(downs=%d/%d bad=%d rc=%d)\n",
		    (unsigned long long)seed, downs, want, nbad, rc);
		fails++;
	}
	if (fails == 0) {
		printf(
		    "OK: DOWN classification DST -- %d seeds x %d children "
		    "(clean, exit, kill, masked kill, user-frame fault, "
		    "failed svr init): every DOWN carried its kind and code, "
		    "all replay\n",
		    n, want);
		return 0;
	}
	printf("FAIL: %d/%d down-kind seeds failed\n", fails, n);
	return 1;
}

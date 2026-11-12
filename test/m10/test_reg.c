/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_reg.c -- verifies M10.5 process registry.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_int.h"
#include "os_thread.h"   /* the visit-lifetime test's second thread */

#include <string.h>

#include <stdint.h>
#include <stdio.h>

/* Mirror of reg.c's FNV-1a over a NUL-terminated name and its bucket
 * count, so the collision test can find names that land in the same
 * bucket without reaching into reg.c internals. */
#define REG_NBUCKETS 256u
static uint32_t
reg_hash(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s != '\0'; s++) {
		h ^= (uint8_t)*s;
		h *= 16777619u;
	}
	return h;
}

static MunitResult
test_reg_basic(const MunitParameter p[], void *d)
{
	xtc_reg_t *r;
	xtc_pid_t pid_a = { 0, 1, 1 }, pid_b = { 0, 2, 1 };
	xtc_pid_t got;
	(void)p; (void)d;
	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, 0);

	munit_assert_int(xtc_reg_register(r, "alpha", pid_a), ==, XTC_OK);
	munit_assert_int(xtc_reg_register(r, "beta",  pid_b), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, 2);

	/* Duplicate name rejected. */
	munit_assert_int(xtc_reg_register(r, "alpha", pid_b), ==, XTC_E_INVAL);

	/* whereis */
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_a));
	munit_assert_int(xtc_reg_whereis(r, "beta", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_b));
	munit_assert_int(xtc_reg_whereis(r, "missing", &got), ==, XTC_E_INVAL);

	/* unregister */
	munit_assert_int(xtc_reg_unregister(r, "alpha"), ==, XTC_OK);
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_unregister(r, "alpha"), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_count(r), ==, 1);

	/* Re-register under new pid OK. */
	munit_assert_int(xtc_reg_register(r, "alpha", pid_b), ==, XTC_OK);
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_b));

	xtc_reg_destroy(r);
	return MUNIT_OK;
}

/*
 * Collision test: register several names that all hash to the same
 * bucket (hash & (REG_NBUCKETS - 1)), and verify each is found and
 * removed independently.  Exercises chain walking and unlink-by-link
 * at the head, middle, and tail of a chain.
 */
static MunitResult
test_reg_collisions(const MunitParameter p[], void *d)
{
#define NCOLL 5
	xtc_reg_t *r;
	char names[NCOLL][32];
	xtc_pid_t pids[NCOLL];
	xtc_pid_t got;
	uint32_t target;
	int found = 0;
	long k;
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);

	/* Find NCOLL distinct names whose hash lands in one bucket. */
	target = reg_hash("k0") & (REG_NBUCKETS - 1);
	for (k = 0; found < NCOLL && k < 5000000L; k++) {
		char cand[32];
		(void)snprintf(cand, sizeof cand, "k%ld", k);
		if ((reg_hash(cand) & (REG_NBUCKETS - 1)) == target) {
			(void)snprintf(names[found], sizeof names[found],
			    "%s", cand);
			pids[found].loop_id = 0;
			pids[found].local_id = (uint32_t)(found + 1);
			pids[found].gen = 1;
			found++;
		}
	}
	munit_assert_int(found, ==, NCOLL);

	/* All collide in the same bucket. */
	for (i = 1; i < NCOLL; i++)
		munit_assert_uint32(reg_hash(names[i]) & (REG_NBUCKETS - 1),
		    ==, target);

	/* Register all, then confirm each resolves to its own pid. */
	for (i = 0; i < NCOLL; i++)
		munit_assert_int(xtc_reg_register(r, names[i], pids[i]), ==,
		    XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, NCOLL);
	for (i = 0; i < NCOLL; i++) {
		munit_assert_int(xtc_reg_whereis(r, names[i], &got), ==,
		    XTC_OK);
		munit_assert_true(xtc_pid_eq(got, pids[i]));
	}

	/* Remove the first-inserted (chain tail) and a middle node; the
	 * rest must still resolve. */
	munit_assert_int(xtc_reg_unregister(r, names[0]), ==, XTC_OK);
	munit_assert_int(xtc_reg_unregister(r, names[2]), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, NCOLL - 2);
	munit_assert_int(xtc_reg_whereis(r, names[0], &got), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_whereis(r, names[2], &got), ==, XTC_E_INVAL);
	for (i = 0; i < NCOLL; i++) {
		if (i == 0 || i == 2) continue;
		munit_assert_int(xtc_reg_whereis(r, names[i], &got), ==,
		    XTC_OK);
		munit_assert_true(xtc_pid_eq(got, pids[i]));
	}

	/* Remove the remaining nodes; bucket empties cleanly. */
	munit_assert_int(xtc_reg_unregister(r, names[1]), ==, XTC_OK);
	munit_assert_int(xtc_reg_unregister(r, names[3]), ==, XTC_OK);
	munit_assert_int(xtc_reg_unregister(r, names[4]), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, 0);

	xtc_reg_destroy(r);
	return MUNIT_OK;
#undef NCOLL
}

/*
 * Scale test: register N names, look every one up, then unregister
 * them all.  Demonstrates the table works (and stays correct) far
 * past the bucket count, with chains hashed across all buckets.
 */
static MunitResult
test_reg_scale(const MunitParameter p[], void *d)
{
#define NSCALE 5000
	xtc_reg_t *r;
	xtc_pid_t got;
	char name[32];
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);

	for (i = 0; i < NSCALE; i++) {
		xtc_pid_t pid = { 0, (uint16_t)(i + 1), (uint32_t)(i + 1) };
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_register(r, name, pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_reg_count(r), ==, NSCALE);

	/* Every name resolves to the pid it was registered with. */
	for (i = 0; i < NSCALE; i++) {
		xtc_pid_t want = { 0, (uint16_t)(i + 1), (uint32_t)(i + 1) };
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_whereis(r, name, &got), ==, XTC_OK);
		munit_assert_true(xtc_pid_eq(got, want));
	}

	/* A name that was never registered misses. */
	munit_assert_int(xtc_reg_whereis(r, "svc-none", &got), ==, XTC_E_INVAL);

	/* Unregister all; count returns to zero. */
	for (i = 0; i < NSCALE; i++) {
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_unregister(r, name), ==, XTC_OK);
	}
	munit_assert_int(xtc_reg_count(r), ==, 0);
	munit_assert_int(xtc_reg_whereis(r, "svc-0", &got), ==, XTC_E_INVAL);

	xtc_reg_destroy(r);
	return MUNIT_OK;
#undef NSCALE
}

static int
count_cb(xtc_pid_t pid, void *user)
{
	int *n = user;
	(void)pid;
	(*n)++;
	return 0;
}

static MunitResult
test_reg_dup_keys(const MunitParameter p[], void *d)
{
	xtc_reg_t *r = NULL;
	xtc_pid_t a = { 0, 1, 1 }, b = { 0, 2, 1 }, c = { 0, 3, 1 };
	int n;
	(void)p; (void)d;
	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);

	/* Three pids join one group key. */
	munit_assert_int(xtc_reg_register_dup(r, "grp", a), ==, XTC_OK);
	munit_assert_int(xtc_reg_register_dup(r, "grp", b), ==, XTC_OK);
	munit_assert_int(xtc_reg_register_dup(r, "grp", c), ==, XTC_OK);
	/* Idempotent re-add. */
	munit_assert_int(xtc_reg_register_dup(r, "grp", a), ==, XTC_OK);
	n = 0;
	munit_assert_int(xtc_reg_members(r, "grp", count_cb, &n), ==, 3);
	munit_assert_int(n, ==, 3);

	/* Leave one. */
	munit_assert_int(xtc_reg_unregister_pid(r, "grp", b), ==, XTC_OK);
	n = 0;
	munit_assert_int(xtc_reg_members(r, "grp", count_cb, &n), ==, 2);
	/* Leaving a non-member is XTC_E_INVAL. */
	munit_assert_int(xtc_reg_unregister_pid(r, "grp", b), ==, XTC_E_INVAL);

	/* drop_pid: a pid in several groups + a unique name is removed from
	 * all of them in one call. */
	munit_assert_int(xtc_reg_register_dup(r, "g2", a), ==, XTC_OK);
	munit_assert_int(xtc_reg_register(r, "uniqueA", a), ==, XTC_OK);
	/* a is now in: grp, g2, and the unique name uniqueA (3 entries). */
	munit_assert_int(xtc_reg_drop_pid(r, a), ==, 3);
	/* c remains in grp. */
	n = 0;
	munit_assert_int(xtc_reg_members(r, "grp", count_cb, &n), ==, 1);
	munit_assert_int(xtc_reg_drop_pid(r, a), ==, 0);   /* idempotent */

	xtc_reg_destroy(r);
	return MUNIT_OK;
}

/* ---------- crash-aware registry (reaper auto-drops on DOWN) ---------- */

struct ca_ctx {
	xtc_reg_t  *reg;
	xtc_loop_t *loop;               /* checker stops it when done */
	int worker_gone_seen;           /* checker: entry absent after crash */
};

/* A worker that registers itself for monitored cleanup, then exits
 * (simulating a crash -- the entry must be auto-dropped). */
static void
ca_worker(void *arg)
{
	struct ca_ctx *c = arg;
	(void)xtc_reg_register_mon(c->reg, "worker", xtc_self());
	/* exit immediately; the reaper's monitor -> DOWN -> drop_pid */
}

/* Checker: waits for the reaper to process the monitor request and the
 * worker's DOWN, then verifies "worker" is no longer resolvable. */
static void
ca_checker(void *arg)
{
	struct ca_ctx *c = arg;
	xtc_pid_t pid;
	int i;
	/* Poll: the reaper must (a) receive+arm the monitor, (b) receive the
	 * DOWN, (c) drop_pid.  Give it several scheduler turns. */
	for (i = 0; i < 200; i++) {
		if (xtc_reg_whereis(c->reg, "worker", &pid) != XTC_OK) {
			xtc_atomic_store_i32(&c->worker_gone_seen, 1,
			    xtc_mo_seq_cst);
			break;
		}
		xtc_proc_sleep(5LL * 1000 * 1000);   /* 5ms */
	}
	(void)xtc_loop_stop(c->loop);   /* let the blocked reaper's loop exit */
}

static MunitResult
test_reg_crash_aware(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_reg_t *r = NULL;
	struct ca_ctx c;
	xtc_pid_t reaper;
	(void)p; (void)d;

	memset(&c, 0, sizeof c);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);
	c.reg = r;
	c.loop = loop;

	/* Spawn the reaper first so it publishes its pid before the worker
	 * registers-for-monitoring. */
	munit_assert_int(xtc_proc_spawn(loop, xtc_reg_reaper, r, NULL,
	    &reaper), ==, XTC_OK);
	/* Let the reaper register itself. */
	munit_assert_int(xtc_proc_spawn(loop, ca_worker, &c, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, ca_checker, &c, NULL, NULL),
	    ==, XTC_OK);

	/* The reaper loops forever (recv -1); the checker stops the loop
	 * once it has its answer, so xtc_loop_run returns. */
	(void)xtc_loop_run(loop);

	munit_assert_int(xtc_atomic_load_i32(&c.worker_gone_seen,
	                     xtc_mo_seq_cst),
	    ==, 1);

	xtc_reg_destroy(r);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/*
 * PLAN 19.27.12: a MONITORED registration must never be left without
 * monitoring.  The reaper's mailbox is capped at 1 and the reaper is not
 * scheduled until we yield, so every enrollment after the first cannot be
 * delivered.  The fixed contract: xtc_reg_register_mon either establishes
 * coverage and returns XTC_OK, or rolls the name back and returns an
 * error.  So after every worker has died, NO name may remain whose
 * register_mon call returned XTC_OK -- and no name at all may remain for a
 * call that failed.
 *
 * Fails on the unfixed library: all three calls return XTC_OK and two
 * names outlive their pids.
 */
static xtc_reg_t *g_rm_reg;
static int g_rm_rc[3], g_rm_stale_ok, g_rm_present_failed, g_rm_failed;

static void
rm_worker(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(20LL * 1000 * 1000);
}

static void
rm_driver(void *a)
{
	xtc_loop_t *loop = a;
	xtc_proc_opts_t o;
	xtc_pid_t reaper, w[3], dummy;
	char nm[8];
	int i;

	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.mailbox_cap = 1;
	(void)xtc_proc_spawn(loop, xtc_reg_reaper, g_rm_reg, &o, &reaper);
	(void)xtc_proc_sleep(1LL * 1000 * 1000);   /* let it publish itself */
	for (i = 0; i < 3; i++) {
		(void)xtc_proc_spawn(loop, rm_worker, NULL, NULL, &w[i]);
		snprintf(nm, sizeof nm, "w%d", i);
		g_rm_rc[i] = xtc_reg_register_mon(g_rm_reg, nm, w[i]);
		if (g_rm_rc[i] != XTC_OK)
			g_rm_failed++;
	}
	(void)xtc_proc_sleep(200LL * 1000 * 1000);  /* workers die */
	for (i = 0; i < 3; i++) {
		int present;
		snprintf(nm, sizeof nm, "w%d", i);
		present = (xtc_reg_whereis(g_rm_reg, nm, &dummy) == XTC_OK);
		if (present && g_rm_rc[i] == XTC_OK) g_rm_stale_ok++;
		if (present && g_rm_rc[i] != XTC_OK) g_rm_present_failed++;
	}
	(void)xtc_exit_pid(reaper, 0);
}

static MunitResult
test_reg_mon_no_silent_loss(const MunitParameter p[], void *d)
{
	xtc_loop_t *l = NULL;
	(void)p; (void)d;
	g_rm_stale_ok = g_rm_present_failed = g_rm_failed = 0;
	munit_assert_int(xtc_reg_create(&g_rm_reg), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&l), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(l, rm_driver, l, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(l), ==, XTC_OK);
	(void)xtc_loop_fini(l);
	xtc_reg_destroy(g_rm_reg);

	/* The scenario must actually overflow enrollment -- otherwise the
	 * no-stale property below would pass vacuously. */
	munit_assert_int(g_rm_failed, >=, 1);
	/* The property: nothing registered "successfully" outlives its pid,
	 * and a failed registration leaves nothing behind. */
	munit_assert_int(g_rm_stale_ok, ==, 0);
	munit_assert_int(g_rm_present_failed, ==, 0);
	return MUNIT_OK;
}

/*
 * E-M11: lookups stay O(1) as the table grows.  Before 1.55.1 the 256
 * buckets were fixed, so at 200k names every whereis walked a ~780-node
 * chain: ~100x the cost at 1k names (measured 47 ns at 100 names, 5.1 us
 * at 100k).  The table now doubles at load factor 1.  Asserts both
 * correctness across many grows (unique + dup keys, drop_pid) and that
 * the per-lookup cost at 200k names is within 25x of that at 1k (best
 * of 3 timings).  The residual growth is cache misses on a table that no
 * longer fits in cache, not chain walks; the fixed table was ~700x.
 */
static int64_t
reg_lookup_ns(xtc_reg_t *r, int n)
{
	char name[32];
	xtc_pid_t got;
	int64_t best = INT64_MAX, t0, t;
	int rep, i;
	for (rep = 0; rep < 3; rep++) {
		t0 = xtc_clock_mono();
		for (i = 0; i < 100000; i++) {
			(void)snprintf(name, sizeof name, "svc-%d",
			    (int)(((int64_t)i * 7919) % n));
			munit_assert_int(xtc_reg_whereis(r, name, &got), ==,
			    XTC_OK);
		}
		t = xtc_clock_mono() - t0;
		if (t < best) best = t;
	}
	return best / 100000;
}

static int
reg_count_member(xtc_pid_t pid, void *user)
{
	(void)pid;
	(*(int *)user)++;
	return 0;
}

static MunitResult
test_reg_resize(const MunitParameter p[], void *d)
{
#define NBIG 200000
	xtc_reg_t *r;
	xtc_pid_t got, member = { 0, 7, 7 };
	char name[32];
	int64_t small_ns, big_ns;
	int i, seen = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);
	for (i = 0; i < 1000; i++) {
		xtc_pid_t pid = { 0, 1, (uint32_t)(i + 1) };
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_register(r, name, pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_reg_register_dup(r, "group", member), ==, XTC_OK);
	small_ns = reg_lookup_ns(r, 1000);
	for (i = 1000; i < NBIG; i++) {
		xtc_pid_t pid = { 0, 1, (uint32_t)(i + 1) };
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_register(r, name, pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_reg_count(r), ==, NBIG + 1);
	for (i = 0; i < NBIG; i++) {
		xtc_pid_t want = { 0, 1, (uint32_t)(i + 1) };
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_whereis(r, name, &got), ==, XTC_OK);
		munit_assert_true(xtc_pid_eq(got, want));
	}
	/* Duplicate a registered name fails; the dup entry survived the
	 * rehashes. */
	munit_assert_int(xtc_reg_register(r, "svc-12345", member), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_reg_members(r, "group", reg_count_member, &seen),
	    ==, 1);
	munit_assert_int(xtc_reg_drop_pid(r, member), ==, 1);
	big_ns = reg_lookup_ns(r, NBIG);
	munit_logf(MUNIT_LOG_INFO, "whereis: %lld ns at 1k, %lld ns at %d",
	    (long long)small_ns, (long long)big_ns, NBIG);
	munit_assert_int64(big_ns, <=, 25 * (small_ns > 0 ? small_ns : 1));

	for (i = 0; i < NBIG; i++) {
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_unregister(r, name), ==, XTC_OK);
	}
	munit_assert_int(xtc_reg_count(r), ==, 0);
	xtc_reg_destroy(r);
	return MUNIT_OK;
#undef NBIG
}

/*
 * xtc_reg_put / xtc_reg_visit (2.2): an entry carries a caller-owned
 * value, and visit hands it to a callback under the registry lock.
 */
struct visit_acc {
	int       n;
	xtc_pid_t pid;
	void     *value;
	int       stop_after;
};

static int
visit_acc_fn(xtc_pid_t pid, void *value, void *user)
{
	struct visit_acc *a = user;
	a->n++;
	a->pid = pid;
	a->value = value;
	return a->stop_after != 0 && a->n >= a->stop_after;
}

static MunitResult
test_reg_put_visit(const MunitParameter p[], void *d)
{
	xtc_reg_t *r;
	xtc_pid_t pa = { 0, 1, 1 }, pb = { 0, 2, 1 }, got;
	int va = 1, vb = 2, n = -1;
	struct visit_acc acc;
	(void)p; (void)d;
	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);

	/* A unique name: the second put is XTC_E_EXIST, and the value comes
	 * back through visit.  whereis still resolves the pid. */
	munit_assert_int(xtc_reg_put(r, "b1", pa, &va, 0), ==, XTC_OK);
	munit_assert_int(xtc_reg_put(r, "b1", pb, &vb, 0), ==, XTC_E_EXIST);
	munit_assert_int(xtc_reg_register(r, "b1", pb), ==, XTC_E_INVAL);
	memset(&acc, 0, sizeof acc);
	munit_assert_int(xtc_reg_visit(r, "b1", visit_acc_fn, &acc, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 1);
	munit_assert_true(xtc_pid_eq(acc.pid, pa));
	munit_assert_ptr_equal(acc.value, &va);
	munit_assert_int(xtc_reg_whereis(r, "b1", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pa));

	/* A miss is XTC_E_NOTFOUND with a count of 0, and fn never runs. */
	memset(&acc, 0, sizeof acc);
	munit_assert_int(xtc_reg_visit(r, "nobody", visit_acc_fn, &acc, &n),
	    ==, XTC_E_NOTFOUND);
	munit_assert_int(n, ==, 0);
	munit_assert_int(acc.n, ==, 0);

	/* Removal by any of the existing calls removes the value with it. */
	munit_assert_int(xtc_reg_unregister(r, "b1"), ==, XTC_OK);
	munit_assert_int(xtc_reg_visit(r, "b1", visit_acc_fn, &acc, NULL), ==,
	    XTC_E_NOTFOUND);

	/* Duplicate-key members, each with its own value; the same
	 * (key, pid) again is idempotent and keeps the first value; a
	 * nonzero return stops the walk. */
	munit_assert_int(xtc_reg_put(r, "grp", pa, &va, XTC_REG_DUP), ==,
	    XTC_OK);
	munit_assert_int(xtc_reg_put(r, "grp", pb, &vb, XTC_REG_DUP), ==,
	    XTC_OK);
	munit_assert_int(xtc_reg_put(r, "grp", pb, &va, XTC_REG_DUP), ==,
	    XTC_OK);
	memset(&acc, 0, sizeof acc);
	munit_assert_int(xtc_reg_visit(r, "grp", visit_acc_fn, &acc, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 2);
	memset(&acc, 0, sizeof acc);
	acc.stop_after = 1;
	munit_assert_int(xtc_reg_visit(r, "grp", visit_acc_fn, &acc, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 1);
	munit_assert_int(xtc_reg_drop_pid(r, pb), ==, 1);
	memset(&acc, 0, sizeof acc);
	munit_assert_int(xtc_reg_visit(r, "grp", visit_acc_fn, &acc, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 1);
	munit_assert_ptr_equal(acc.value, &va);

	/* Argument checks. */
	munit_assert_int(xtc_reg_put(r, "x", pa, NULL, 0x80u), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_reg_put(NULL, "x", pa, NULL, 0), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_visit(r, "x", NULL, NULL, &n), ==,
	    XTC_E_INVAL);
	munit_assert_int(n, ==, 0);
	xtc_reg_destroy(r);
	return MUNIT_OK;
}

/*
 * The lifetime guarantee the value slot exists for: an owner that
 * removes its entry before freeing the value cannot free it while a
 * visitor uses it.  One thread puts a fresh heap value, unregisters it
 * and frees it at once, in a loop; another visits the name and reads
 * through the value.  Under ASan a visitor that saw a freed value is a
 * heap-use-after-free; without ASan the value's magic word must always
 * be intact.  This is the PostgreSQL backend-interrupt pattern
 * (lookup + use under the registry lock).
 */
#define PV_MAGIC 0x5a17c0deu
struct pv_obj { unsigned magic; unsigned flips; };
static xtc_reg_t *g_pv_reg;
static int g_pv_stop, g_pv_bad, g_pv_hits;

static int
pv_visit_fn(xtc_pid_t pid, void *value, void *user)
{
	struct pv_obj *o = value;
	(void)pid; (void)user;
	if (o->magic != PV_MAGIC)
		g_pv_bad++;
	o->flips++;     /* the "set the interrupt bit" write */
	g_pv_hits++;
	return 0;
}

static void *
pv_visitor(void *arg)
{
	(void)arg;
	while (!xtc_atomic_load_i32(&g_pv_stop, xtc_mo_acquire))
		(void)xtc_reg_visit(g_pv_reg, "backend:7", pv_visit_fn, NULL,
		    NULL);
	return NULL;
}

static MunitResult
test_reg_visit_lifetime(const MunitParameter p[], void *d)
{
	__os_thread_t th = {0};
	xtc_pid_t pa = { 0, 7, 1 };
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_reg_create(&g_pv_reg), ==, XTC_OK);
	g_pv_stop = g_pv_bad = g_pv_hits = 0;
	munit_assert_int(__os_thread_create(&th, pv_visitor, NULL), ==, XTC_OK);
	for (i = 0; i < 200000; i++) {
		struct pv_obj *o;
		o = xtc_malloc(sizeof *o);
		munit_assert_not_null(o);
		o->magic = PV_MAGIC;
		o->flips = 0;
		munit_assert_int(xtc_reg_put(g_pv_reg, "backend:7", pa, o, 0),
		    ==, XTC_OK);
		munit_assert_int(xtc_reg_unregister(g_pv_reg, "backend:7"), ==,
		    XTC_OK);
		o->magic = 0;   /* poison, then free: a late visitor sees it */
		xtc_free(o);
	}
	xtc_atomic_store_i32(&g_pv_stop, 1, xtc_mo_release);
	munit_assert_int(__os_thread_join(&th, NULL), ==, XTC_OK);
	munit_assert_int(g_pv_bad, ==, 0);
	munit_logf(MUNIT_LOG_INFO, "visit_lifetime: %d visits", g_pv_hits);
	xtc_reg_destroy(g_pv_reg);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/reg_basic", test_reg_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_collisions", test_reg_collisions, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_scale", test_reg_scale, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_resize", test_reg_resize, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_dup_keys", test_reg_dup_keys, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_crash_aware", test_reg_crash_aware, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_mon_no_silent_loss", test_reg_mon_no_silent_loss, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_put_visit", test_reg_put_visit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_visit_lifetime", test_reg_visit_lifetime, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/reg", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m11/test_mctx.c -- verifies M11 memory contexts.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_mctx.h"
#include "xtc_int.h"

static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m;
	void *a, *b;
	(void)p; (void)d;

	munit_assert_int(xtc_mctx_create(NULL, "root", 0, &m), ==, XTC_OK);
	munit_assert_string_equal(xtc_mctx_name(m), "root");

	a = xtc_mctx_alloc(m, 100);
	munit_assert_not_null(a);
	memset(a, 'A', 100);

	b = xtc_mctx_calloc(m, 50, sizeof(int));
	munit_assert_not_null(b);
	{
		int i;
		for (i = 0; i < 50; i++)
			munit_assert_int(((int *)b)[i], ==, 0);
	}

	munit_assert_size(xtc_mctx_total_chunks(m), ==, 2);
	munit_assert_size(xtc_mctx_total_bytes(m), ==, 100 + 50 * sizeof(int));

	/* Free one early. */
	xtc_mctx_free(m, a);
	munit_assert_size(xtc_mctx_total_chunks(m), ==, 1);

	xtc_mctx_destroy(m);
	return MUNIT_OK;
}

/* Cleanup callback fires before chunks are freed. */
static int g_cleanup_count;
static void *g_cleanup_chunk;

static void
my_cleanup(void *u)
{
	(void)u;
	__os_atomic_fetch_add_i32(&g_cleanup_count, 1);
	/* The chunk is still alive when cleanup runs. */
	if (g_cleanup_chunk != NULL)
		((char *)g_cleanup_chunk)[0] = 'X';
}

static MunitResult
test_cleanup(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m;
	void *a;
	(void)p; (void)d;

	__os_atomic_store_i32(&g_cleanup_count, 0);

	munit_assert_int(xtc_mctx_create(NULL, "scratch", 0, &m), ==, XTC_OK);
	a = xtc_mctx_alloc(m, 64);
	g_cleanup_chunk = a;
	munit_assert_int(xtc_mctx_register_cleanup(m, my_cleanup, NULL),
	    ==, XTC_OK);

	xtc_mctx_destroy(m);
	munit_assert_int(__os_atomic_load_i32(&g_cleanup_count), ==, 1);
	return MUNIT_OK;
}

/* Hierarchy: parent -> child -> grandchild.  Destroying parent
 * destroys grandchild's chunks too. */
static MunitResult
test_hierarchy(const MunitParameter p[], void *d)
{
	xtc_mctx_t *parent, *child, *grand;
	(void)p; (void)d;

	munit_assert_int(xtc_mctx_create(NULL,   "p", 0, &parent), ==, XTC_OK);
	munit_assert_int(xtc_mctx_create(parent, "c", 0, &child),  ==, XTC_OK);
	munit_assert_int(xtc_mctx_create(child,  "g", 0, &grand),  ==, XTC_OK);

	(void)xtc_mctx_alloc(parent, 100);
	(void)xtc_mctx_alloc(child,  200);
	(void)xtc_mctx_alloc(grand,  300);

	munit_assert_size(xtc_mctx_total_bytes(parent), ==, 100);
	munit_assert_size(xtc_mctx_total_bytes(child),  ==, 200);
	munit_assert_size(xtc_mctx_total_bytes(grand),  ==, 300);

	xtc_mctx_destroy(parent);
	/* No leaks; tested via not-segfaulting + total tally. */
	return MUNIT_OK;
}

/* Reset frees chunks but keeps the context usable.  Children are
 * cascaded-reset, not destroyed. */
static int g_reset_cleanup;
static void reset_cb(void *u) { (void)u; g_reset_cleanup++; }

static MunitResult
test_reset(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m, *child;
	(void)p; (void)d;

	g_reset_cleanup = 0;
	munit_assert_int(xtc_mctx_create(NULL, "m", 0, &m), ==, XTC_OK);
	munit_assert_int(xtc_mctx_create(m,    "c", 0, &child), ==, XTC_OK);

	(void)xtc_mctx_alloc(m, 1000);
	(void)xtc_mctx_alloc(child, 500);
	(void)xtc_mctx_register_cleanup(m, reset_cb, NULL);

	xtc_mctx_reset(m);
	munit_assert_int(g_reset_cleanup, ==, 1);
	munit_assert_size(xtc_mctx_total_bytes(m), ==, 0);
	munit_assert_size(xtc_mctx_total_bytes(child), ==, 0);

	/* Context is still usable. */
	munit_assert_not_null(xtc_mctx_alloc(m, 50));
	munit_assert_size(xtc_mctx_total_bytes(m), ==, 50);

	xtc_mctx_destroy(m);
	return MUNIT_OK;
}

static MunitResult
test_strdup(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m;
	const char *s = "hello, world";
	char *copy;
	(void)p; (void)d;
	munit_assert_int(xtc_mctx_create(NULL, "m", 0, &m), ==, XTC_OK);
	copy = xtc_mctx_strdup(m, s);
	munit_assert_not_null(copy);
	munit_assert_string_equal(copy, s);
	munit_assert_ptr_not_equal(copy, s);
	xtc_mctx_destroy(m);
	return MUNIT_OK;
}

/* ---- regression: a cleanup callback may introspect its own context --
 *
 * Cleanups used to run while the context mutex was HELD, so a callback
 * that asked its own thread-safe context for total_bytes/_chunks
 * blocked on a non-recursive mutex it already owned -- a hard hang, not
 * a wrong answer.  Group arenas are always THREAD_SAFE, so the path was
 * always exposed.  Before the fix this test HANGS.
 *
 * It also pins the documented ordering: the chunks are still attached
 * and readable while the cleanup runs. */
static size_t g_intro_bytes;
static size_t g_intro_chunks;
static const char *g_intro_name;
static int g_intro_ran;

static void
introspecting_cleanup(void *u)
{
	xtc_mctx_t *m = u;
	/* Each of these takes the context's own mutex. */
	g_intro_bytes  = xtc_mctx_total_bytes(m);
	g_intro_chunks = xtc_mctx_total_chunks(m);
	g_intro_name   = xtc_mctx_name(m);
	g_intro_ran++;
}

static MunitResult
test_cleanup_introspects_self(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m, *child;
	(void)p; (void)d;

	g_intro_bytes = g_intro_chunks = 0;
	g_intro_name = NULL;
	g_intro_ran = 0;

	munit_assert_int(xtc_mctx_create(NULL, "introspect",
	    XTC_MCTX_THREAD_SAFE, &m), ==, XTC_OK);
	munit_assert_not_null(xtc_mctx_alloc(m, 128));
	munit_assert_int(xtc_mctx_register_cleanup(m, introspecting_cleanup, m),
	    ==, XTC_OK);

	/* Deadlocked here before the fix. */
	xtc_mctx_reset(m);

	munit_assert_int(g_intro_ran, ==, 1);
	/* Chunks are still live while the cleanup runs. */
	munit_assert_size(g_intro_bytes, ==, 128);
	munit_assert_size(g_intro_chunks, ==, 1);
	munit_assert_string_equal(g_intro_name, "introspect");
	/* ...and gone afterwards. */
	munit_assert_size(xtc_mctx_total_bytes(m), ==, 0);

	/* Detached-before-run means exactly once: a second reset does not
	 * re-fire the callback. */
	xtc_mctx_reset(m);
	munit_assert_int(g_intro_ran, ==, 1);

	/* Same hazard on the destroy path, and through a cascade: a CHILD's
	 * cleanup introspecting the child while the parent reset walks it. */
	munit_assert_int(xtc_mctx_create(m, "child", XTC_MCTX_THREAD_SAFE,
	    &child), ==, XTC_OK);
	munit_assert_not_null(xtc_mctx_alloc(child, 64));
	munit_assert_int(xtc_mctx_register_cleanup(child,
	    introspecting_cleanup, child), ==, XTC_OK);
	xtc_mctx_reset(m);
	munit_assert_int(g_intro_ran, ==, 2);
	munit_assert_size(g_intro_bytes, ==, 64);

	munit_assert_int(xtc_mctx_register_cleanup(m, introspecting_cleanup, m),
	    ==, XTC_OK);
	xtc_mctx_destroy(m);
	munit_assert_int(g_intro_ran, ==, 3);
	return MUNIT_OK;
}

/* ---- regression: joining an arena group DURING a discard ------------
 *
 * discard() snapshots the members, then UNLOCKS and does yielding waits
 * before resetting the arena.  A fiber that joined inside that window
 * used to succeed: it was never killed (not in the snapshot), its
 * allocations were wiped by the reset it did not know about, and
 * clearing g->n dropped it from tracking entirely -- a "success"
 * verdict over a live member holding dangling pointers.
 *
 * The group is now SEALED for the duration of the discard, so the late
 * join fails with XTC_E_AGAIN and never touches the arena.  Before the
 * fix the join returns XTC_OK and this test fails.
 */
static xtc_arena_group_t *g_ag;
static _Atomic int g_ag_member_ready;   /* victim has joined and parked */
static _Atomic int g_ag_verdict;        /* discard has returned */
static _Atomic int g_ag_late_rc = 999;  /* the late join's return code */
static _Atomic int g_ag_rejoin_rc = 999;
static _Atomic int g_ag_all_gone = -1;
static _Atomic int g_ag_discard_rc = 999;
static _Atomic int g_ag_chunks_after = -1;
static _Atomic int g_ag_size_after = -1;

/* The cohort member the discard will kill: joins, allocates, parks. */
static void
ag_victim_proc(void *arg)
{
	void *msg = NULL;
	size_t sz = 0;
	(void)arg;
	munit_assert_int(xtc_arena_group_add(g_ag), ==, XTC_OK);
	munit_assert_not_null(xtc_mctx_alloc(xtc_arena_group_mctx(g_ag), 32));
	atomic_store(&g_ag_member_ready, 1);
	(void)xtc_recv(&msg, &sz, -1);   /* killable park */
	xtc_free(msg);
}

/* Tries to join WHILE the discard is in its yielding wait. */
static void
ag_late_joiner_proc(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_ag_member_ready))
		(void)xtc_proc_sleep(200000);
	/* The victim is parked, so the supervisor is inside discard's wait
	 * by the time we get here (same loop, cooperative). */
	atomic_store(&g_ag_late_rc, xtc_arena_group_add(g_ag));
	/* Deliberately do NOT touch the arena: a refused join must not. */
	while (!atomic_load(&g_ag_verdict))
		(void)xtc_proc_sleep(200000);
	/* Once the verdict is in, the seal is lifted and the group is
	 * reusable by a fresh cohort. */
	atomic_store(&g_ag_rejoin_rc, xtc_arena_group_add(g_ag));
	atomic_store(&g_ag_size_after, xtc_arena_group_size(g_ag));
}

static void
ag_discarder_proc(void *arg)
{
	int all_gone = -1;
	(void)arg;
	while (!atomic_load(&g_ag_member_ready))
		(void)xtc_proc_sleep(200000);
	atomic_store(&g_ag_discard_rc,
	    xtc_arena_group_discard(g_ag, 9, 500000000LL, &all_gone));
	atomic_store(&g_ag_all_gone, all_gone);
	atomic_store(&g_ag_chunks_after,
	    (int)xtc_mctx_total_chunks(xtc_arena_group_mctx(g_ag)));
	atomic_store(&g_ag_verdict, 1);
}

static MunitResult
test_arena_group_join_during_discard(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts;
	xtc_pid_t pid;
	(void)p; (void)d;

	memset(&opts, 0, sizeof opts);
	atomic_store(&g_ag_member_ready, 0);
	atomic_store(&g_ag_verdict, 0);
	atomic_store(&g_ag_late_rc, 999);
	atomic_store(&g_ag_rejoin_rc, 999);
	atomic_store(&g_ag_all_gone, -1);
	atomic_store(&g_ag_discard_rc, 999);
	atomic_store(&g_ag_chunks_after, -1);
	atomic_store(&g_ag_size_after, -1);

	munit_assert_int(xtc_arena_group_create("ag-seal", &g_ag), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "ag-victim";
	munit_assert_int(xtc_proc_spawn(loop, ag_victim_proc, NULL, &opts,
	    &pid), ==, XTC_OK);
	opts.name = "ag-discarder";
	munit_assert_int(xtc_proc_spawn(loop, ag_discarder_proc, NULL, &opts,
	    &pid), ==, XTC_OK);
	opts.name = "ag-late";
	munit_assert_int(xtc_proc_spawn(loop, ag_late_joiner_proc, NULL, &opts,
	    &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_ag_verdict), ==, 1);
	munit_assert_int(atomic_load(&g_ag_discard_rc), ==, XTC_OK);
	/* THE REGRESSION: a join attempted during the discard is REFUSED.
	 * Before the fix this was XTC_OK and the joiner became a live member
	 * of a reset arena. */
	munit_assert_int(atomic_load(&g_ag_late_rc), ==, XTC_E_AGAIN);
	/* The cohort was killable, so the arena WAS reset -- which is only
	 * safe because the seal kept the late joiner out of it. */
	munit_assert_int(atomic_load(&g_ag_all_gone), ==, 1);
	munit_assert_int(atomic_load(&g_ag_chunks_after), ==, 0);
	/* The seal lifts at the verdict: the group is reusable. */
	munit_assert_int(atomic_load(&g_ag_rejoin_rc), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_ag_size_after), ==, 1);

	xtc_arena_group_destroy(g_ag);
	g_ag = NULL;
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * Arena-group member identity must be OWNER-QUALIFIED.  A pid is only
 * executor-relative: every standalone loop encodes loop_id 0, so procs
 * on two standalone loops routinely carry EQUAL pids.  The group used to
 * keep bare pids and ask the global, pid-only questions (xtc_proc_info,
 * xtc_exit_pid_deadline, pid equality), which mix the two procs up.
 * These three cases each failed before the group recorded the member's
 * owner loop at xtc_arena_group_add.
 */
static void ag_join_proc(void *arg)
{
	munit_assert_int(xtc_arena_group_add(arg), ==, XTC_OK);
}
static void ag_noop_proc(void *arg) { (void)arg; }

/* A reaped member is gone even though an UNRELATED proc on another
 * standalone loop, with an equal pid, is still in its table.  Before the
 * fix the global xtc_proc_info scan found the unrelated proc and
 * discard reported all_gone=0. */
static MunitResult
test_arena_group_reaped_vs_equal_pid(const MunitParameter p[], void *d)
{
	xtc_loop_t *a = NULL, *b = NULL;
	xtc_arena_group_t *g = NULL;
	xtc_pid_t pa, pb;
	int gone = -1;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&a), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&b), ==, XTC_OK);
	munit_assert_int(xtc_arena_group_create("ag-eqpid", &g), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(a, ag_join_proc, g, NULL, &pa), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(b, ag_noop_proc, NULL, NULL, &pb), ==,
	    XTC_OK);
	munit_assert_true(xtc_pid_eq(pa, pb));       /* the precondition */
	munit_assert_int(xtc_loop_run(a), ==, XTC_OK);  /* member reaped */
	munit_assert_int(xtc_arena_group_size(g), ==, 1);
	munit_assert_int(xtc_arena_group_discard(g, 1, 0, &gone), ==, XTC_OK);
	munit_assert_int(gone, ==, 1);
	munit_assert_int(xtc_loop_run(b), ==, XTC_OK);
	xtc_arena_group_destroy(g);
	munit_assert_int(xtc_loop_fini(a), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(b), ==, XTC_OK);
	return MUNIT_OK;
}

/* Two fibers on different standalone loops with equal pids are TWO
 * members.  Before the fix the second add matched the first by pid and
 * was silently dropped -- so a discard would never kill it and would
 * reset the arena under it. */
static MunitResult
test_arena_group_add_equal_pid_other_loop(const MunitParameter p[], void *d)
{
	xtc_loop_t *a = NULL, *b = NULL;
	xtc_arena_group_t *g = NULL;
	xtc_pid_t pa, pb;
	int gone = -1;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&a), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&b), ==, XTC_OK);
	munit_assert_int(xtc_arena_group_create("ag-dedup", &g), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(a, ag_join_proc, g, NULL, &pa), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(b, ag_join_proc, g, NULL, &pb), ==,
	    XTC_OK);
	munit_assert_true(xtc_pid_eq(pa, pb));
	munit_assert_int(xtc_loop_run(a), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(b), ==, XTC_OK);
	munit_assert_int(xtc_arena_group_size(g), ==, 2);
	munit_assert_int(xtc_arena_group_discard(g, 1, 0, &gone), ==, XTC_OK);
	munit_assert_int(gone, ==, 1);
	xtc_arena_group_destroy(g);
	munit_assert_int(xtc_loop_fini(a), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(b), ==, XTC_OK);
	return MUNIT_OK;
}

/* discard KILLS THE MEMBER, not a bystander with an equal pid on the
 * discarder's own standalone loop.  Member M runs on loop A (its own
 * thread); bystander U and discarder D run on loop B.  Before the fix
 * xtc_exit_pid_deadline resolved loop_id 0 to D's loop and killed U,
 * M survived, and discard timed out with all_gone=0. */
static xtc_arena_group_t *g_kg;
static _Atomic int g_kg_m_ready, g_kg_stop, g_kg_m_normal, g_kg_u_normal;
static _Atomic int g_kg_all_gone = -1;

static void
kg_member_proc(void *arg)
{
	(void)arg;
	munit_assert_int(xtc_arena_group_add(g_kg), ==, XTC_OK);
	atomic_store(&g_kg_m_ready, 1);
	while (!atomic_load(&g_kg_stop))
		(void)xtc_proc_sleep(1000000);   /* killable */
	atomic_store(&g_kg_m_normal, 1);
}

static void
kg_bystander_proc(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_kg_stop))
		(void)xtc_proc_sleep(1000000);   /* killable */
	atomic_store(&g_kg_u_normal, 1);
}

static void
kg_discarder_proc(void *arg)
{
	int gone = -1;
	(void)arg;
	while (!atomic_load(&g_kg_m_ready))
		(void)xtc_proc_sleep(1000000);
	munit_assert_int(xtc_arena_group_discard(g_kg, 7, 2000000000LL,
	    &gone), ==, XTC_OK);
	atomic_store(&g_kg_all_gone, gone);
	atomic_store(&g_kg_stop, 1);
}

static void *
kg_loop_thread(void *arg)
{
	(void)xtc_loop_run(arg);
	return NULL;
}

static MunitResult
test_arena_group_kill_targets_owner(const MunitParameter p[], void *d)
{
	xtc_loop_t *a = NULL, *b = NULL;
	xtc_pid_t pm, pu, pd;
	pthread_t th;
	(void)p; (void)d;

	atomic_store(&g_kg_m_ready, 0);
	atomic_store(&g_kg_stop, 0);
	atomic_store(&g_kg_m_normal, 0);
	atomic_store(&g_kg_u_normal, 0);
	atomic_store(&g_kg_all_gone, -1);
	munit_assert_int(xtc_loop_init(&a), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&b), ==, XTC_OK);
	munit_assert_int(xtc_arena_group_create("ag-kill", &g_kg), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(a, kg_member_proc, NULL, NULL, &pm),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(b, kg_bystander_proc, NULL, NULL,
	    &pu), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(pm, pu));
	munit_assert_int(xtc_proc_spawn(b, kg_discarder_proc, NULL, NULL,
	    &pd), ==, XTC_OK);
	munit_assert_int(pthread_create(&th, NULL, kg_loop_thread, a), ==, 0);
	munit_assert_int(xtc_loop_run(b), ==, XTC_OK);
	atomic_store(&g_kg_stop, 1);
	munit_assert_int(pthread_join(th, NULL), ==, 0);

	munit_assert_int(atomic_load(&g_kg_u_normal), ==, 1);  /* spared */
	munit_assert_int(atomic_load(&g_kg_m_normal), ==, 0);  /* killed */
	munit_assert_int(atomic_load(&g_kg_all_gone), ==, 1);

	xtc_arena_group_destroy(g_kg);
	g_kg = NULL;
	munit_assert_int(xtc_loop_fini(a), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(b), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic",      test_basic,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cleanup",    test_cleanup,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cleanup_introspects_self", test_cleanup_introspects_self, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hierarchy",  test_hierarchy, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reset",      test_reset,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/strdup",     test_strdup,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arena_group_join_during_discard", test_arena_group_join_during_discard, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arena_group_reaped_vs_equal_pid", test_arena_group_reaped_vs_equal_pid, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arena_group_add_equal_pid_other_loop", test_arena_group_add_equal_pid_other_loop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arena_group_kill_targets_owner", test_arena_group_kill_targets_owner, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m11/mctx", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

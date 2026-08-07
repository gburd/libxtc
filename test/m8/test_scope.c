/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m8/test_scope.c -- A1 resource scope / bracket + A2 cancellation
 *	masking.  The unit-level proof that finalizers run LIFO on every
 *	exit path (normal close, xtc_exit_self, async kill, contained
 *	crash) and that a kill delivered inside a masked region is deferred
 *	until the mask lifts -- so a resource acquired in a masked acquire
 *	always registers its release.  The seed-replayable version of the
 *	same guarantee lives in test/sim/test_sim_scope.c.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_proc.h"

/* ---------- shared observation state ---------- */

/* Finalizer order log: each finalizer appends its tag; the test asserts
 * the sequence is LIFO. */
#define LOG_MAX 16
static int   g_log[LOG_MAX];
static int   g_log_n;
static int   g_release_ran;
static int   g_use_ran;
static int   g_acquire_ran;

static void
log_reset(void)
{
	memset(g_log, 0, sizeof g_log);
	g_log_n = 0;
	g_release_ran = 0;
	g_use_ran = 0;
	g_acquire_ran = 0;
}

static void
fin_tag(void *arg)
{
	int tag = (int)(intptr_t)arg;
	if (g_log_n < LOG_MAX)
		g_log[g_log_n++] = tag;
}

/* ---------- 1. scope closes LIFO on the happy path ---------- */

static void
scope_normal_proc(void *arg)
{
	xtc_scope_t *s;
	(void)arg;
	s = xtc_scope_open();
	if (s == NULL)
		return;
	(void)xtc_scope_defer(s, fin_tag, (void *)(intptr_t)1);
	(void)xtc_scope_defer(s, fin_tag, (void *)(intptr_t)2);
	(void)xtc_scope_defer(s, fin_tag, (void *)(intptr_t)3);
	xtc_scope_close(s);
}

static MunitResult
test_scope_normal_lifo(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	(void)p; (void)d;
	log_reset();
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, scope_normal_proc, NULL, NULL,
	    &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(g_log_n, ==, 3);
	munit_assert_int(g_log[0], ==, 3);   /* LIFO */
	munit_assert_int(g_log[1], ==, 2);
	munit_assert_int(g_log[2], ==, 1);
	return MUNIT_OK;
}

/* ---------- 2. scope finalizers run on xtc_exit_self ---------- */

static void
scope_exit_proc(void *arg)
{
	xtc_scope_t *s;
	(void)arg;
	s = xtc_scope_open();
	if (s == NULL)
		return;
	(void)xtc_scope_defer(s, fin_tag, (void *)(intptr_t)10);
	(void)xtc_scope_defer(s, fin_tag, (void *)(intptr_t)20);
	/* Leave the scope OPEN and exit: the recovery-registry unwind must
	 * still run its finalizers LIFO. */
	(void)xtc_exit_self(0);
	/* NOTREACHED */
}

static MunitResult
test_scope_exit_self(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	(void)p; (void)d;
	log_reset();
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, scope_exit_proc, NULL, NULL,
	    &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(g_log_n, ==, 2);
	munit_assert_int(g_log[0], ==, 20);
	munit_assert_int(g_log[1], ==, 10);
	return MUNIT_OK;
}

/* ---------- 3. scope finalizers run on a contained crash ---------- */

static void
scope_crash_watcher(void *arg)
{
	xtc_pid_t *target = arg;
	uint64_t ref;
	void *m = NULL; size_t n = 0;
	if (xtc_monitor(*target, &ref) != XTC_OK)
		return;
	(void)xtc_recv(&m, &n, 2LL * 1000 * 1000 * 1000);
	if (m)
		xtc_free(m);
}

static void
scope_crash_proc(void *arg)
{
	xtc_scope_t *s;
	(void)arg;
	xtc_proc_recovery_arm_clean();   /* default: cleanup + exit on fault */
	s = xtc_scope_open();
	if (s == NULL)
		return;
	(void)xtc_scope_defer(s, fin_tag, (void *)(intptr_t)100);
	(void)xtc_scope_defer(s, fin_tag, (void *)(intptr_t)200);
	{
		volatile uintptr_t addr = 0x10;
		*(volatile int *)addr = 1;   /* boom -- contained fault */
	}
	xtc_scope_close(s);              /* NOTREACHED */
}

static MunitResult
test_scope_contained_crash(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t faulter, watcher;
	(void)p; (void)d;
#if defined(__SANITIZE_ADDRESS__)
	return MUNIT_SKIP;   /* ASan owns SIGSEGV */
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;
#  endif
#endif
	log_reset();
	munit_assert_int(xtc_fault_guard_install(), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, scope_crash_proc, NULL, NULL,
	    &faulter), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, scope_crash_watcher, &faulter,
	    NULL, &watcher), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	/* The contained crash still ran both finalizers, LIFO. */
	munit_assert_int(g_log_n, ==, 2);
	munit_assert_int(g_log[0], ==, 200);
	munit_assert_int(g_log[1], ==, 100);
	return MUNIT_OK;
}

/* ---------- 4. xtc_bracket runs release on normal + error ---------- */

static int
bk_acquire(void **res, void *ud)
{
	(void)ud;
	g_acquire_ran = 1;
	*res = (void *)(intptr_t)0xB00C;
	return XTC_OK;
}
static int
bk_use_ok(void *res, void *ud)
{
	(void)ud;
	munit_assert_ptr_equal(res, (void *)(intptr_t)0xB00C);
	g_use_ran = 1;
	return XTC_OK;
}
static int
bk_use_err(void *res, void *ud)
{
	(void)res; (void)ud;
	g_use_ran = 1;
	return XTC_E_INVAL;
}
static void
bk_release(void *res, void *ud)
{
	(void)ud;
	munit_assert_ptr_equal(res, (void *)(intptr_t)0xB00C);
	g_release_ran = 1;
}

static int g_bracket_rc;

static void
bracket_ok_proc(void *arg)
{
	(void)arg;
	g_bracket_rc = xtc_bracket(bk_acquire, bk_use_ok, bk_release, NULL);
}
static void
bracket_err_proc(void *arg)
{
	(void)arg;
	g_bracket_rc = xtc_bracket(bk_acquire, bk_use_err, bk_release, NULL);
}

static MunitResult
test_bracket_release_normal(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	(void)p; (void)d;
	log_reset();
	g_bracket_rc = 0x7fff;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, bracket_ok_proc, NULL, NULL,
	    &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(g_acquire_ran, ==, 1);
	munit_assert_int(g_use_ran, ==, 1);
	munit_assert_int(g_release_ran, ==, 1);
	munit_assert_int(g_bracket_rc, ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_bracket_release_on_error(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	(void)p; (void)d;
	log_reset();
	g_bracket_rc = 0x7fff;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, bracket_err_proc, NULL, NULL,
	    &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(g_release_ran, ==, 1);         /* released anyway */
	munit_assert_int(g_bracket_rc, ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ---------- 5. masking: kill deferred until unmask ---------- */

/* The masked body: while masked, spin a few yields.  Another proc (the
 * killer) sets kill_pending mid-way; the body must NOT be unwound until
 * xtc_uncancelable returns.  We record whether the body ran to
 * completion and whether the scope's finalizer ran. */
static _Atomic int g_masked_completed;
static _Atomic int g_masked_fin_ran;
static xtc_pid_t   g_mask_target;

static void
mask_fin(void *arg)
{
	(void)arg;
	atomic_store(&g_masked_fin_ran, 1);
}

static int
masked_body(void *ud)
{
	xtc_scope_t *s = ud;
	int i;
	/* Acquire a resource (defer its finalizer) INSIDE the masked
	 * region, then yield so the killer can fire.  The kill must be
	 * deferred: this body must complete and register cleanly. */
	(void)xtc_scope_defer(s, mask_fin, NULL);
	for (i = 0; i < 8; i++)
		xtc_yield();
	atomic_store(&g_masked_completed, 1);
	return XTC_OK;
}

static void
mask_target_proc(void *arg)
{
	xtc_scope_t *s;
	(void)arg;
	s = xtc_scope_open();
	if (s == NULL)
		return;
	(void)xtc_uncancelable(masked_body, s);
	/* On return from the masked region a deferred kill is honored at
	 * the next park; the scope's finalizer runs on the unwind. */
	xtc_scope_close(s);   /* may not reach if killed after unmask */
}

static void
mask_killer_proc(void *arg)
{
	int i;
	(void)arg;
	/* Let the target enter its masked region, then kill it. */
	for (i = 0; i < 3; i++)
		xtc_yield();
	(void)xtc_exit_pid(g_mask_target, 9);
}

static MunitResult
test_mask_defers_kill(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t killer;
	(void)p; (void)d;
	atomic_store(&g_masked_completed, 0);
	atomic_store(&g_masked_fin_ran, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, mask_target_proc, NULL, NULL,
	    &g_mask_target), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, mask_killer_proc, NULL, NULL,
	    &killer), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	/* The masked body completed despite the mid-region kill... */
	munit_assert_int(atomic_load(&g_masked_completed), ==, 1);
	/* ...and the resource's finalizer still ran (on close or unwind). */
	munit_assert_int(atomic_load(&g_masked_fin_ran), ==, 1);
	return MUNIT_OK;
}

/* ---------- 6. off-proc calls are safe no-ops / direct-run ---------- */

static MunitResult
test_offproc_safe(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* Off a proc: scope_open returns NULL, bracket honors the contract
	 * directly, uncancelable just runs the body. */
	munit_assert_ptr_null(xtc_scope_open());
	log_reset();
	munit_assert_int(xtc_bracket(bk_acquire, bk_use_ok, bk_release, NULL),
	    ==, XTC_OK);
	munit_assert_int(g_release_ran, ==, 1);
	munit_assert_int(xtc_cancel_requested(), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/scope_normal_lifo",     test_scope_normal_lifo,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/scope_exit_self",       test_scope_exit_self,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/scope_contained_crash", test_scope_contained_crash, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bracket_release_normal", test_bracket_release_normal, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bracket_release_on_error", test_bracket_release_on_error, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mask_defers_kill",      test_mask_defers_kill,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/offproc_safe",          test_offproc_safe,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m8/scope", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

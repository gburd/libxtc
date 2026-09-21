/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m9/test_alloc_audit.c -- xtc_alloc_audit per-proc leak detection.
 *	A leaker process allocates and never frees; a clean process
 *	frees everything.  After the loop drains, the auditor reports
 *	the leaker's outstanding allocations and none for the clean
 *	process.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_alloc_audit.h"
#include "os_alloc.h"

struct audit_state {
	xtc_pid_t leaker_pid;
	xtc_pid_t clean_pid;
	void     *leaked[3];
};

static void
leaker_proc(void *arg)
{
	struct audit_state *s = arg;
	int i;
	s->leaker_pid = xtc_self();
	for (i = 0; i < 3; i++) {
		s->leaked[i] = NULL;
		(void)__os_malloc(64, &s->leaked[i]);   /* never freed */
	}
}

static void
clean_proc(void *arg)
{
	struct audit_state *s = arg;
	void *p = NULL, *q = NULL;
	s->clean_pid = xtc_self();
	(void)__os_malloc(128, &p);
	(void)__os_malloc(256, &q);
	__os_free(p);
	__os_free(q);
}

static MunitResult
test_proc_leaks(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	struct audit_state s;
	xtc_pid_t a, b;
	size_t cnt = 0, bytes = 0;
	int i;
	(void)p; (void)d;

	memset(&s, 0, sizeof s);
	munit_assert_int(xtc_alloc_audit_enable(1), ==, XTC_OK);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "leaker";
	munit_assert_int(xtc_proc_spawn(loop, leaker_proc, &s, &opts, &a),
	    ==, XTC_OK);
	opts.name = "clean";
	munit_assert_int(xtc_proc_spawn(loop, clean_proc, &s, &opts, &b),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* The leaker's three 64-byte buffers are still live and
	 * attributed to it; the clean process leaked nothing. */
	xtc_alloc_audit_proc_leaks(s.leaker_pid, &cnt, &bytes);
	munit_assert_size(cnt, ==, 3);
	munit_assert_size(bytes, ==, 3 * 64);

	/* Global stats: at least the leaker's three live buffers are
	 * counted (other subsystems may hold live allocations too).  Both
	 * out-pointers optional -- exercise the NULL path too. */
	{
		size_t gcnt = 0, gbytes = 0;
		xtc_alloc_audit_stats(&gcnt, &gbytes);
		munit_assert_size(gcnt, >=, 3);
		munit_assert_size(gbytes, >=, 3 * 64);
		xtc_alloc_audit_stats(NULL, NULL);   /* no crash */
	}

	cnt = bytes = 1;
	xtc_alloc_audit_proc_leaks(s.clean_pid, &cnt, &bytes);
	munit_assert_size(cnt, ==, 0);
	munit_assert_size(bytes, ==, 0);

	/* Release the deliberately-leaked buffers so the run is clean. */
	for (i = 0; i < 3; i++)
		if (s.leaked[i]) __os_free(s.leaked[i]);
	xtc_alloc_audit_proc_leaks(s.leaker_pid, &cnt, NULL);
	munit_assert_size(cnt, ==, 0);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(xtc_alloc_audit_enable(0), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * A FAILED realloc must not lose the allocation from the auditor.
 * __a_realloc removed the record BEFORE calling downstream, so when the
 * downstream realloc returned NULL the ORIGINAL block was still live
 * (and still the caller's to free) yet no longer tracked -- the leak
 * checker reported clean exactly on an OOM path.  Fails without the
 * "re-key only after success" ordering in src/ptc/alloc_audit.c.
 *
 * The failure is induced with a hook that refuses realloc, NOT with a
 * huge size: ASan aborts on an allocation-size-too-big request by
 * default (CI sets abort_on_error=1), and a hook makes the OOM path
 * deterministic on every platform.
 */
static int oom_on_realloc;
static struct __os_alloc_hook down_h;

static void *oom_malloc(size_t s) { return down_h.malloc(s); }
static void *oom_calloc(size_t n, size_t s) { return down_h.calloc(n, s); }
static void  oom_free(void *q) { down_h.free(q); }
static void *oom_aligned(size_t a, size_t s) { return down_h.aligned(a, s); }
static void  oom_aligned_free(void *q) { down_h.aligned_free(q); }
static void *
oom_realloc(void *q, size_t s)
{
	if (oom_on_realloc)
		return NULL;                /* simulate OOM */
	return down_h.realloc(q, s);
}

static MunitResult
test_failed_realloc_keeps_record(const MunitParameter p[], void *d)
{
	struct __os_alloc_hook oom_h;
	void *p0 = NULL, *p1 = NULL;
	size_t cnt0 = 0, bytes0 = 0, cnt = 0, bytes = 0;
	int rc;
	(void)p; (void)d;

	/* Install the OOM-able hook UNDER the auditor: enable() captures
	 * whatever is active as its downstream. */
	munit_assert_int(__os_alloc_get_hook(&down_h), ==, XTC_OK);
	oom_h.malloc = oom_malloc;
	oom_h.calloc = oom_calloc;
	oom_h.realloc = oom_realloc;
	oom_h.free = oom_free;
	oom_h.aligned = oom_aligned;
	oom_h.aligned_free = oom_aligned_free;
	oom_on_realloc = 0;
	munit_assert_int(__os_alloc_set_hook(&oom_h), ==, XTC_OK);

	munit_assert_int(xtc_alloc_audit_enable(1), ==, XTC_OK);
	xtc_alloc_audit_stats(&cnt0, &bytes0);

	munit_assert_int(__os_malloc(64, &p0), ==, XTC_OK);
	xtc_alloc_audit_stats(&cnt, &bytes);
	munit_assert_size(cnt, ==, cnt0 + 1);
	munit_assert_size(bytes, ==, bytes0 + 64);

	/* Realloc fails; p0 is still live and still the caller's. */
	oom_on_realloc = 1;
	rc = __os_realloc(p0, 256, &p1);
	munit_assert_int(rc, ==, XTC_E_NOMEM);
	oom_on_realloc = 0;

	/* The live 64-byte block is STILL tracked, at its original size. */
	xtc_alloc_audit_stats(&cnt, &bytes);
	munit_assert_size(cnt, ==, cnt0 + 1);
	munit_assert_size(bytes, ==, bytes0 + 64);

	/* Freeing it drops the record as usual -- i.e. the record that
	 * survived really is the one describing p0, not a stale duplicate. */
	__os_free(p0);
	xtc_alloc_audit_stats(&cnt, &bytes);
	munit_assert_size(cnt, ==, cnt0);
	munit_assert_size(bytes, ==, bytes0);

	/* A SUCCEEDING realloc still re-keys to the new pointer/size. */
	munit_assert_int(__os_malloc(64, &p0), ==, XTC_OK);
	munit_assert_int(__os_realloc(p0, 256, &p1), ==, XTC_OK);
	xtc_alloc_audit_stats(&cnt, &bytes);
	munit_assert_size(cnt, ==, cnt0 + 1);
	munit_assert_size(bytes, ==, bytes0 + 256);
	__os_free(p1);
	xtc_alloc_audit_stats(&cnt, &bytes);
	munit_assert_size(cnt, ==, cnt0);

	munit_assert_int(xtc_alloc_audit_enable(0), ==, XTC_OK);
	munit_assert_int(__os_alloc_set_hook(&down_h), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/proc_leaks", test_proc_leaks, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/failed_realloc_keeps_record", test_failed_realloc_keeps_record,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m9/alloc_audit", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

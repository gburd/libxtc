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

static MunitTest tests[] = {
	{ "/proc_leaks", test_proc_leaks, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m9/alloc_audit", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

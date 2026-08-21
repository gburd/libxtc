/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m4/test_stack_guard.c
 *	Verifies the coroutine-stack guard page (memory-isolation tier b):
 *	a fiber that overflows its stack must fault on the PROT_NONE guard
 *	rather than silently scribbling into a neighbour's memory.  Run in
 *	a forked child because the expected outcome is a fatal signal.
 */

#include "munit.h"
#include "xtc.h"

#if defined(_WIN32)
static MunitResult
test_guard(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return MUNIT_SKIP;   /* fork()/signal model is POSIX-only */
}
#else

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* Detect AddressSanitizer on both gcc (__SANITIZE_ADDRESS__) and clang
 * (__has_feature(address_sanitizer)). */
#if defined(__SANITIZE_ADDRESS__)
#  define XTC_TEST_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define XTC_TEST_ASAN 1
#  endif
#endif

#include "xtc_loop.h"
#include "xtc_proc.h"

/* Overflow the fiber stack: a local far larger than the 64 KiB fiber
 * stack, touched low-to-high so the deepest (lowest) address -- below
 * the usable stack -- lands on the guard page. */
static void
overflow_fn(void *arg)
{
	volatile char big[1024 * 1024];
	size_t i;
	(void)arg;
	for (i = 0; i < sizeof big; i += 256)
		big[i] = (char)(i & 0xff);
	_exit(7);   /* reached only if the guard did NOT fault */
}

static MunitResult
test_guard(const MunitParameter p[], void *d)
{
	pid_t pid;
	int   st = 0;
	(void)p; (void)d;

#if defined(XTC_TEST_ASAN)
	/* AddressSanitizer replaces the PROT_NONE guard-page trap with its
	 * own shadow-memory instrumentation and SEGV handler, so this test
	 * does not exercise the real guard under ASan; worse, ASan may
	 * _exit() the child on the detected overflow rather than let a
	 * signal propagate, so waitpid() sees an ordinary exit and
	 * WIFSIGNALED() is false.  The guard page is verified in the
	 * non-sanitized build and in CI; skip here. */
	return MUNIT_SKIP;
#endif
	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		/* Child: drive a fiber that overflows; the guard should
		 * deliver SIGSEGV/SIGBUS and terminate us. */
		xtc_loop_t *loop = NULL;
		if (xtc_loop_init(&loop) != XTC_OK)
			_exit(8);
		(void)xtc_proc_spawn(loop, overflow_fn, NULL, NULL, NULL);
		(void)xtc_loop_run(loop);
		_exit(9);   /* survived: no fault */
	}

	munit_assert_int(waitpid(pid, &st, 0), ==, pid);
	/* The child must have died from a memory fault, not exited.  (Under
	 * ASan this point is unreachable -- the ASan build returns SKIP
	 * above, because ASan intercepts the guard-page fault itself.) */
	munit_assert_true(WIFSIGNALED(st));
	munit_assert_true(WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS);
	return MUNIT_OK;
}
#endif

static MunitTest tests[] = {
	{ "/overflow_faults", test_guard, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m4/stack_guard", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

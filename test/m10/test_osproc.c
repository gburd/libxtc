/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m10/test_osproc.c -- xtc_osproc_spawn (R3) verification.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_osproc.h"
#include "xtc_io.h"

#include <signal.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#if !defined(_WIN32)

/* ---- bad opts ---- */
static MunitResult
test_bad_opts(const MunitParameter p[], void *d)
{
	xtc_osproc_t *h = NULL;
	xtc_osproc_opts_t o;
	char *const argv[] = { "/bin/true", NULL };
	(void)p; (void)d;

	munit_assert_int(xtc_osproc_spawn(NULL, &h), ==, XTC_E_INVAL);
	memset(&o, 0, sizeof o);
	/* neither argv nor fn */
	munit_assert_int(xtc_osproc_spawn(&o, &h), ==, XTC_E_INVAL);
	/* both argv and fn */
	o.argv = argv;
	o.fn   = (int (*)(int, void *))1;
	munit_assert_int(xtc_osproc_spawn(&o, &h), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ---- exec a program; reap with try_wait (off-loop) ---- */
static int
run_exec(char *const argv[], int *exitcode)
{
	xtc_osproc_t *h = NULL;
	xtc_osproc_opts_t o;
	int spins, st = 0, rc;

	memset(&o, 0, sizeof o);
	o.argv = argv;
	if (xtc_osproc_spawn(&o, &h) != XTC_OK)
		return -1;
	munit_assert_int(xtc_osproc_pid(h), >, 0);
	for (spins = 0; spins < 5000; spins++) {
		rc = xtc_osproc_try_wait(h, &st);
		if (rc == XTC_OK) break;
		munit_assert_int(rc, ==, XTC_E_AGAIN);
		{ struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
	}
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_true(WIFEXITED(st));
	*exitcode = WEXITSTATUS(st);
	xtc_osproc_destroy(h);
	return 0;
}

static MunitResult
test_exec_true_false(const MunitParameter p[], void *d)
{
	/* Launch a program that exits 0, and one that exits 1.  Prefer
	 * /usr/bin/env (PATH-resolves the command, so it does not depend
	 * on where true/false live -- /bin on Linux, /usr/bin on macOS);
	 * fall back to /bin/sh -c, which every POSIX host provides.  (On
	 * Windows xtc_osproc is XTC_E_NOSYS; a future Win32 port would
	 * launch via cmd.exe /c.) */
	char *const env_t[] = { "/usr/bin/env", "true",  NULL };
	char *const env_f[] = { "/usr/bin/env", "false", NULL };
	char *const sh_t[]  = { "/bin/sh", "-c", "exit 0", NULL };
	char *const sh_f[]  = { "/bin/sh", "-c", "exit 1", NULL };
	int have_env = (access("/usr/bin/env", X_OK) == 0);
	int code = -1;
	(void)p; (void)d;

	munit_assert_int(run_exec(have_env ? env_t : sh_t, &code), ==, 0);
	munit_assert_int(code, ==, 0);
	munit_assert_int(run_exec(have_env ? env_f : sh_f, &code), ==, 0);
	munit_assert_int(code, ==, 1);
	return MUNIT_OK;
}

/* ---- fd-leak: a non-CLOEXEC parent fd must NOT reach the exec'd child
 * (CWE-403).  Dup the write end of a pipe to a fixed, high fd number
 * (unlikely to be reused by the shell for its own bookkeeping), then
 * exec a shell that attempts to write a sentinel byte to that fd.  If
 * the fd leaked, the write succeeds and the parent reads the sentinel;
 * with the pre-exec sweep the fd is closed, the write fails, and no
 * sentinel arrives.  We assert nothing arrives. */
/* Single digit: dash / POSIX sh only accept single-digit fd numbers in a
 * >&N redirect (fd >= 10 is a bash extension).  fds 8-9 are not used by
 * the shell for its own bookkeeping, so fd 9 survives to the echo. */
#define XTC_LEAK_FD 9
static MunitResult
test_no_fd_leak(const MunitParameter p[], void *d)
{
	int pfd[2];
	char arg[96];
	char *argv[4];
	int code = -1;
	char rb[4];
	ssize_t got;
	(void)p; (void)d;

	if (pipe(pfd) != 0)
		return MUNIT_SKIP;
	/* Move the write end to the fixed probe fd; keep read end for the
	 * parent.  dup2 clears CLOEXEC on the target, so this fd is exactly
	 * the kind of plain, inheritable descriptor the sweep must close. */
	if (dup2(pfd[1], XTC_LEAK_FD) != XTC_LEAK_FD) {
		(void)close(pfd[0]); (void)close(pfd[1]);
		return MUNIT_SKIP;
	}
	(void)close(pfd[1]);

	(void)snprintf(arg, sizeof arg,
	    "echo -n LEAK >&%d 2>/dev/null; exit 0", XTC_LEAK_FD);
	argv[0] = "/bin/sh"; argv[1] = "-c"; argv[2] = arg; argv[3] = NULL;

	munit_assert_int(run_exec(argv, &code), ==, 0);
	munit_assert_int(code, ==, 0);

	/* Close our probe write-fd so the read end sees EOF rather than
	 * blocking; then a non-blocking read must return 0 (EOF) -- i.e.
	 * the child wrote nothing because its copy of the fd was closed. */
	(void)close(XTC_LEAK_FD);
	{
		int fl = fcntl(pfd[0], F_GETFL, 0);
		(void)fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);
	}
	got = read(pfd[0], rb, sizeof rb);
	(void)close(pfd[0]);
	munit_assert_int((int)got, <=, 0);   /* 0 = EOF, no sentinel = no leak */
	return MUNIT_OK;
}

/* ---- fork callback + control socket + cooperative wait (on a loop) ---- */
static _Atomic int g_ok;

/* Child: send a known byte string on the control fd, then exit 7. */
static int
child_ping(int ctrl_fd, void *arg)
{
	const char *msg = "PING";
	(void)arg;
	if (ctrl_fd >= 0)
		{ ssize_t wr = write(ctrl_fd, msg, 4); (void)wr; }
	return 7;
}

static void
parent_proc(void *arg)
{
	xtc_osproc_t *h = NULL;
	xtc_osproc_opts_t o;
	int cfd, st = 0, rc;
	char buf[8];
	uint32_t rev = 0;
	size_t got = 0;
	(void)arg;

	memset(&o, 0, sizeof o);
	o.fn = child_ping;
	o.ctrl_socket = 1;
	if (xtc_osproc_spawn(&o, &h) != XTC_OK) return;

	cfd = xtc_osproc_ctrl_fd(h);
	if (cfd < 0) { xtc_osproc_destroy(h); return; }

	/* Read the 4-byte PING from the (non-blocking) parent end, parking
	 * on readability via the loop. */
	while (got < 4) {
		ssize_t n = read(cfd, buf + got, sizeof buf - got);
		if (n > 0) { got += (size_t)n; continue; }
		if (n == 0) break;            /* child closed */
		if (xtc_proc_wait_fd(cfd, XTC_IO_READABLE,
		    2000LL * 1000 * 1000, &rev) != XTC_OK)
			break;
	}

	rc = xtc_osproc_wait(h, &st, 2000LL * 1000 * 1000);
	if (rc == XTC_OK && got == 4 && memcmp(buf, "PING", 4) == 0 &&
	    WIFEXITED(st) && WEXITSTATUS(st) == 7)
		atomic_store(&g_ok, 1);

	xtc_osproc_destroy(h);
}

static MunitResult
test_fn_ctrl_wait(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	(void)p; (void)d;
	atomic_store(&g_ok, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, parent_proc, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_ok), ==, 1);
	return MUNIT_OK;
}

/* ---- signal a long-lived child ---- */
static _Atomic int g_sig_ok;

static int
child_sleep(int ctrl_fd, void *arg)
{
	(void)ctrl_fd; (void)arg;
	for (;;) { struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); }
	return 0;
}

static void
signal_proc(void *arg)
{
	xtc_osproc_t *h = NULL;
	xtc_osproc_opts_t o;
	int st = 0;
	(void)arg;

	memset(&o, 0, sizeof o);
	o.fn = child_sleep;
	if (xtc_osproc_spawn(&o, &h) != XTC_OK) return;

	munit_assert_int(xtc_osproc_signal(h, SIGTERM), ==, XTC_OK);
	if (xtc_osproc_wait(h, &st, 2000LL * 1000 * 1000) == XTC_OK &&
	    WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM)
		atomic_store(&g_sig_ok, 1);
	xtc_osproc_destroy(h);
}

static MunitResult
test_signal(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	(void)p; (void)d;
	atomic_store(&g_sig_ok, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, signal_proc, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_sig_ok), ==, 1);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/bad_opts",     test_bad_opts,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exec",         test_exec_true_false, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/no_fd_leak",   test_no_fd_leak,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fn_ctrl_wait", test_fn_ctrl_wait,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/signal",       test_signal,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

#else  /* _WIN32: xtc_osproc is XTC_E_NOSYS; smoke that the stubs link */

static MunitResult
test_nosys(const MunitParameter p[], void *d)
{
	xtc_osproc_t *h = NULL;
	xtc_osproc_opts_t o;
	char *const argv[] = { "cmd", NULL };
	(void)p; (void)d;
	memset(&o, 0, sizeof o);
	o.argv = argv;
	munit_assert_int(xtc_osproc_spawn(&o, &h), ==, XTC_E_NOSYS);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/nosys", test_nosys, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

#endif /* _WIN32 */

static const MunitSuite suite = {
	"/m10/osproc", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}

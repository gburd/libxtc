/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m12/test_dump.c -- xtc_dump runtime-state dump, XTC_ASSERT_F,
 * and the xtc_panic / XTC_PANIC abort-with-dump path.
 */

#define _GNU_SOURCE

#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_dump.h"

/* ---------- /dump/basic: dump shows loops, procs, mailbox ---------- */

static xtc_pid_t g_sleeper;

static void
dump_sleeper(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(300LL * 1000 * 1000);   /* park ~300ms */
}

static void
dump_driver(void *a)
{
	int fd = *(int *)a;
	const char *m = "x";

	/* Queue two messages at the parked sleeper so the dump shows a
	 * non-zero mailbox depth. */
	xtc_send(g_sleeper, m, 2);
	xtc_send(g_sleeper, m, 2);
	(void)xtc_proc_sleep(2LL * 1000 * 1000);     /* let sends land */

	xtc_dump(fd);
}

static MunitResult
test_dump_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t dpid;
	char tmpl[] = "/tmp/xtc_dump_XXXXXX";
	int fd, n;
	char buf[8192];
	(void)p; (void)d;

	fd = mkstemp(tmpl);
	munit_assert_int(fd, >=, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, dump_sleeper, NULL, &o,
	    &g_sleeper), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, dump_driver, &fd, &o, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	munit_assert_int(lseek(fd, 0, SEEK_SET), ==, 0);
	n = (int)read(fd, buf, sizeof buf - 1);
	munit_assert_int(n, >, 0);
	buf[n] = '\0';
	close(fd);
	unlink(tmpl);

	/* Structure: header, loops section, procs section, footer. */
	munit_assert_not_null(strstr(buf, "=== xtc runtime dump ==="));
	munit_assert_not_null(strstr(buf, "loops:"));
	munit_assert_not_null(strstr(buf, "procs:"));
	munit_assert_not_null(strstr(buf, "=== end dump ==="));
	/* A proc line uses the <loop.local.gen> pid form + mbox= field. */
	munit_assert_not_null(strstr(buf, "mbox="));
	munit_assert_not_null(strstr(buf, "park="));
	/* The parked sleeper should show its two queued messages. */
	munit_assert_not_null(strstr(buf, "mbox=2/"));
	/* Backtrace line present (frames where supported, else the note). */
	munit_assert_not_null(strstr(buf, "backtrace"));
	return MUNIT_OK;
}

/* ---------- /assert/pass: a true assertion is a no-op ---------- */

static MunitResult
test_assert_pass(const MunitParameter p[], void *d)
{
	int x = 5;
	(void)p; (void)d;
	XTC_ASSERT(x == 5);
	XTC_ASSERT_F(x > 0, "x must be positive, got %d", x);
	munit_assert_int(x, ==, 5);
	return MUNIT_OK;
}

/* ---------- /crash_handler: install is idempotent, returns XTC_OK ---- */
static MunitResult
test_crash_handler(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* Installs SIGSEGV/SIGABRT/etc fault handlers; idempotent. */
	munit_assert_int(xtc_crash_handler_install(), ==, XTC_OK);
	munit_assert_int(xtc_crash_handler_install(), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---------- /panic/aborts: XTC_PANIC dumps + aborts (forked) ---------- */

static MunitResult
test_panic_aborts(const MunitParameter p[], void *d)
{
	int pipefd[2];
	pid_t pid;
	int status;
	char buf[8192];
	ssize_t n;
	(void)p; (void)d;

	munit_assert_int(pipe(pipefd), ==, 0);

	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		/* Child: capture stderr, panic.  Should not return. */
		close(pipefd[0]);
		dup2(pipefd[1], STDERR_FILENO);
		XTC_PANIC("boom %d", 42);
		_exit(99);   /* unreachable */
	}

	/* Parent: drain the child's stderr to EOF, then reap.  Reading to
	 * EOF (rather than a single read) avoids killing the child with
	 * SIGPIPE if it is still writing the dump when we would otherwise
	 * close the pipe -- we must observe its abort(), not a broken pipe. */
	close(pipefd[1]);
	{
		size_t off = 0;
		for (;;) {
			ssize_t r = read(pipefd[0], buf + off,
			    sizeof buf - 1 - off);
			if (r <= 0)
				break;
			off += (size_t)r;
			if (off >= sizeof buf - 1)
				break;
		}
		n = (ssize_t)off;
	}
	close(pipefd[0]);
	munit_assert_int((int)n, >, 0);
	buf[n] = '\0';
	munit_assert_int(waitpid(pid, &status, 0), ==, pid);

	/* Child died via abort() (SIGABRT). */
	munit_assert_true(WIFSIGNALED(status));
	munit_assert_int(WTERMSIG(status), ==, SIGABRT);

	/* The panic banner carried the formatted message, and the dump ran. */
	munit_assert_not_null(strstr(buf, "xtc panic: boom 42"));
	munit_assert_not_null(strstr(buf, "=== xtc runtime dump ==="));
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/dump/basic",    test_dump_basic,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/crash_handler", test_crash_handler, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/assert/pass",   test_assert_pass,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/panic/aborts",  test_panic_aborts, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/dump", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char **argv)
{
	return munit_suite_main(&suite, NULL, argc, argv);
}

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m12/test_dump.c -- xtc_dump runtime-state dump, XTC_ASSERT_F,
 * and the xtc_panic / XTC_PANIC abort-with-dump path.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
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
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_pid_t dpid;
	char tmpl[] = "/tmp/xtc_dump_XXXXXX";
	int fd, n;
	char buf[8192];
	(void)p; (void)d;

	fd = mkstemp(tmpl);
	munit_assert_int(fd, >=, 0);

	munit_assert_int(xtc_loop_create(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, dump_sleeper, NULL, &o,
	    &g_sleeper), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, dump_driver, &fd, &o, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	xtc_loop_destroy(loop);

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

/* ---------- fault guard + crash handler compose ----------
 *
 * Each case runs in a forked child (it ends in a fatal signal) and
 * returns the child's stderr and wait status.  Through 2.2 the two
 * installers overwrote each other's handlers: with the crash handler
 * installed last a proc fault was not contained, and with the guard
 * last a fault off any proc died with no dump. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DUMP_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define DUMP_SANITIZER 1
#endif
#endif

static void
gc_fault(void)
{
	volatile uintptr_t addr = 0x10;
	*(volatile int *)addr = 1;
}

static void *
gc_thread_fault(void *arg)
{
	(void)arg;
	gc_fault();
	return NULL;
}

static int gc_down;

static void
gc_victim(void *arg)
{
	(void)arg;
	gc_fault();
}

static void
gc_watcher(void *arg)
{
	xtc_pid_t v;
	uint64_t ref;
	void *m = NULL;
	size_t n = 0;
	if (xtc_proc_spawn_monitor(arg, gc_victim, NULL, NULL, &v, &ref) !=
	    XTC_OK)
		return;
	if (xtc_recv(&m, &n, 5LL * 1000 * 1000 * 1000) == XTC_OK) {
		gc_down = 1;
		xtc_free(m);
	}
}

static void
gc_app_handler(int sig)
{
	static const char msg[] = "app handler ran\n";
	ssize_t w = write(STDERR_FILENO, msg, sizeof msg - 1);
	(void)w;
	(void)sig;
}

/* Child bodies, chosen by `which`. */
static void
gc_child(int which)
{
	xtc_loop_t *loop = NULL;
	pthread_t t;

	switch (which) {
	case 0:
		/* davros's order, with a second app's install repeated: the
		 * crash handler again after the guard must not take SIGSEGV
		 * back. */
		(void)xtc_crash_handler_install();
		(void)xtc_fault_guard_install();
		(void)xtc_crash_handler_install();
		if (xtc_loop_create(&loop) != XTC_OK)
			_exit(97);
		if (xtc_proc_spawn(loop, gc_watcher, loop, NULL, NULL) != XTC_OK)
			_exit(97);
		(void)xtc_loop_run(loop);
		_exit(gc_down ? 0 : 98);
		break;
	case 1:
		/* Guard first, crash handler second: a fault on a plain
		 * thread is not containable and must still dump. */
		(void)xtc_fault_guard_install();
		(void)xtc_crash_handler_install();
		break;
	default:
		/* An application handler the guard displaces still runs. */
		(void)signal(SIGSEGV, gc_app_handler);
		(void)xtc_fault_guard_install();
		break;
	}
	(void)pthread_create(&t, NULL, gc_thread_fault, NULL);
	(void)pthread_join(t, NULL);
	_exit(96);
}

static int
gc_run(int which, char *buf, size_t cap)
{
	int pipefd[2], status = 0;
	size_t off = 0;
	ssize_t r;
	pid_t pid;

	munit_assert_int(pipe(pipefd), ==, 0);
	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDERR_FILENO);
		gc_child(which);
	}
	close(pipefd[1]);
	while ((r = read(pipefd[0], buf + off, cap - 1 - off)) > 0 &&
	    off + (size_t)r < cap - 1)
		off += (size_t)r;
	if (r > 0)
		off += (size_t)r;
	buf[off] = '\0';
	close(pipefd[0]);
	munit_assert_int(waitpid(pid, &status, 0), ==, pid);
	return status;
}

static MunitResult
test_guard_and_crash_handler(const MunitParameter p[], void *d)
{
	char buf[16384];
	int st;
	(void)p;
	(void)d;
#if defined(DUMP_SANITIZER)
	return MUNIT_SKIP; /* the sanitizer owns SIGSEGV */
#else
	st = gc_run(0, buf, sizeof buf);
	munit_assert_true(WIFEXITED(st));
	munit_assert_int(WEXITSTATUS(st), ==, 0); /* contained, DOWN seen */

	st = gc_run(1, buf, sizeof buf);
	munit_assert_true(WIFSIGNALED(st));
	munit_assert_int(WTERMSIG(st), ==, SIGSEGV);
	munit_assert_not_null(strstr(buf, "*** xtc fatal signal 11 ***"));

	st = gc_run(2, buf, sizeof buf);
	munit_assert_true(WIFSIGNALED(st));
	munit_assert_int(WTERMSIG(st), ==, SIGSEGV);
	munit_assert_not_null(strstr(buf, "app handler ran"));
	return MUNIT_OK;
#endif
}

static MunitTest tests[] = {
	{ "/dump/basic",    test_dump_basic,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/crash_handler", test_crash_handler, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/assert/pass",   test_assert_pass,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/panic/aborts",  test_panic_aborts, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/guard_and_crash_handler", test_guard_and_crash_handler, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
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

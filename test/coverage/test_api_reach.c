/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/coverage/test_api_reach.c -- PLAN 19.27.15.
 *
 *	One behavioral test for each exported xtc_* function that no file
 *	under test/ referenced before this file existed.  Each case asserts
 *	what the function is documented to DO (a value, a state change, a
 *	rejected argument), not merely that it returns.  The merge gate
 *	test/dist/test_api_reach.sh keeps the set from growing back: every
 *	exported xtc_* symbol must be referenced by some test.
 *
 *	POSIX only (fork, socketpair, mkdtemp); the Windows MSVC build does
 *	not compile this file.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_cfg.h"
#include "xtc_dump.h"
#include "xtc_exec.h"
#include "xtc_fs.h"
#include "xtc_inject.h"
#include "xtc_io.h"
#include "xtc_log.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_sim.h"
#include "xtc_slab.h"
#include "xtc_stats.h"
#include "xtc_tail.h"
#include "xtc_tnt.h"
#include "xtc_xproc.h"
#include "xtc_int.h"
#include "loop_int.h"      /* xtc_sim_check: plant a corrupt field */
#include "os_tuning.h"     /* xtc_tuning_check: fixture-path seam */

#define MS (1000LL * 1000)

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
# define SANITIZER_OWNS_SIGSEGV 1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#  define SANITIZER_OWNS_SIGSEGV 1
# endif
#endif

/* ---- cfg: ref_get_int64 + the four untested session setters -------- */

static const char *const g_labels[] = { "low", "mid", "high", NULL };

static void
reg_kinds(void)
{
	xtc_cfg_spec_t s;
	memset(&s, 0, sizeof s);
	s.name = "ar.b"; s.kind = XTC_CFG_BOOL; s.dflt.d_bool = 0;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "ar.i64"; s.kind = XTC_CFG_INT64;
	s.dflt.d_int64 = 5000000000LL;
	s.min_int = 1; s.max_int = 10000000000LL;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "ar.d"; s.kind = XTC_CFG_DOUBLE; s.dflt.d_double = 0.5;
	s.min_double = 0.0; s.max_double = 1.0;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "ar.e"; s.kind = XTC_CFG_ENUM; s.dflt.d_enum = 0;
	s.enum_labels = g_labels; s.n_enum_labels = 3;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
}

static void
unreg_kinds(void)
{
	munit_assert_int(xtc_cfg_unregister("ar.b"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("ar.i64"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("ar.d"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("ar.e"), ==, XTC_OK);
}

static MunitResult
test_cfg_ref_get_int64(const MunitParameter p[], void *d)
{
	xtc_cfg_ref_t ref = NULL;
	int64_t v = 0;
	int iv = 0;
	(void)p; (void)d;
	reg_kinds();
	munit_assert_int(xtc_cfg_ref("ar.i64", &ref), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref_get_int64(ref, &v), ==, XTC_OK);
	munit_assert_llong(v, ==, 5000000000LL);          /* > INT_MAX */
	/* A live set is observed through the handle. */
	munit_assert_int(xtc_cfg_set_int64("ar.i64", 7000000000LL), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref_get_int64(ref, &v), ==, XTC_OK);
	munit_assert_llong(v, ==, 7000000000LL);
	/* Kind must match: an INT64 handle is not an INT, a BOOL is not an
	 * INT64. */
	munit_assert_int(xtc_cfg_ref_get_int(ref, &iv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ref("ar.b", &ref), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref_get_int64(ref, &v), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ref_get_int64(NULL, &v), ==, XTC_E_INVAL);
	unreg_kinds();
	return MUNIT_OK;
}

static MunitResult
test_cfg_ssn_set_kinds(const MunitParameter p[], void *d)
{
	xtc_cfg_session_t *s = NULL;
	xtc_cfg_ref_t ref = NULL;
	int b = -1, e = -1;
	int64_t i64 = 0;
	double dv = 0;
	(void)p; (void)d;
	reg_kinds();
	munit_assert_int(xtc_cfg_session_create(&s), ==, XTC_OK);

	/* NULL session with nothing bound has no target. */
	munit_assert_int(xtc_cfg_ssn_set_bool(NULL, "ar.b", 1,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_INVAL);

	munit_assert_ptr_null(xtc_cfg_session_bind(s));
	munit_assert_int(xtc_cfg_ssn_set_bool(NULL, "ar.b", 1,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_int64(s, "ar.i64", 9000000000LL,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_double(s, "ar.d", 0.25,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_enum(s, "ar.e", 2,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);

	/* The bound session sees its overrides (name-keyed and via ref). */
	munit_assert_int(xtc_cfg_get_bool("ar.b", &b), ==, XTC_OK);
	munit_assert_int(b, ==, 1);
	munit_assert_int(xtc_cfg_ref("ar.i64", &ref), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref_get_int64(ref, &i64), ==, XTC_OK);
	munit_assert_llong(i64, ==, 9000000000LL);
	munit_assert_int(xtc_cfg_get_double("ar.d", &dv), ==, XTC_OK);
	munit_assert_double(dv, ==, 0.25);
	munit_assert_int(xtc_cfg_get_enum("ar.e", &e), ==, XTC_OK);
	munit_assert_int(e, ==, 2);

	/* Bounds apply per kind (out of bounds is XTC_E_RANGE, the 1.50
	 * contract in xtc_cfg.h); a rejected set changes nothing. */
	munit_assert_int(xtc_cfg_ssn_set_bool(s, "ar.b", 2,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_ssn_set_int64(s, "ar.i64", 0,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);          /* < min */
	munit_assert_int(xtc_cfg_ssn_set_double(s, "ar.d", 1.5,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);          /* > max */
	munit_assert_int(xtc_cfg_ssn_set_enum(s, "ar.e", 3,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);          /* no label 3 */
	munit_assert_int(xtc_cfg_ssn_set_enum(s, "ar.e", -1,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);
	/* Kind mismatch: an int64 setter on a double knob. */
	munit_assert_int(xtc_cfg_ssn_set_int64(s, "ar.d", 5,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ssn_set_double(s, "ar.nope", 0.1,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_enum("ar.e", &e), ==, XTC_OK);
	munit_assert_int(e, ==, 2);

	/* Source precedence: a lower-ranked source is refused silently. */
	munit_assert_int(xtc_cfg_ssn_set_double(s, "ar.d", 0.75,
	    XTC_CFG_SRC_FILE), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_double("ar.d", &dv), ==, XTC_OK);
	munit_assert_double(dv, ==, 0.25);

	/* Unbound: the global values were never written. */
	(void)xtc_cfg_session_bind(NULL);
	munit_assert_int(xtc_cfg_get_bool("ar.b", &b), ==, XTC_OK);
	munit_assert_int(b, ==, 0);
	munit_assert_int(xtc_cfg_ref_get_int64(ref, &i64), ==, XTC_OK);
	munit_assert_llong(i64, ==, 5000000000LL);
	munit_assert_int(xtc_cfg_get_double("ar.d", &dv), ==, XTC_OK);
	munit_assert_double(dv, ==, 0.5);
	munit_assert_int(xtc_cfg_get_enum("ar.e", &e), ==, XTC_OK);
	munit_assert_int(e, ==, 0);

	xtc_cfg_session_destroy(s);
	unreg_kinds();
	return MUNIT_OK;
}

/* ---- fs: fdatasync ------------------------------------------------- */

static MunitResult
test_fs_fdatasync(const MunitParameter p[], void *d)
{
	char path[256];
	int fd = -1;
	size_t done = 0;
	char back[5] = { 0 };
	(void)p; (void)d;
	munit_assert_int(xtc_fs_tmpdir(path, sizeof path), ==, XTC_OK);
	munit_assert_size(xtc_strlcat(path, "/xtc_ar_XXXXXX", sizeof path),
	    <, sizeof path);
	munit_assert_int(xtc_fs_mkstemp(path, &fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_pwrite(fd, "sync", 4, 0, &done), ==, XTC_OK);
	munit_assert_size(done, ==, 4);
	munit_assert_int(xtc_fs_fdatasync(fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_pread(fd, back, 4, 0, &done), ==, XTC_OK);
	munit_assert_string_equal(back, "sync");
	munit_assert_int(xtc_fs_close(fd), ==, XTC_OK);
	(void)xtc_fs_unlink(path);
	/* A closed descriptor is an error, never XTC_OK. */
	munit_assert_int(xtc_fs_fdatasync(fd), !=, XTC_OK);
	munit_assert_int(xtc_fs_fdatasync(-1), !=, XTC_OK);
	return MUNIT_OK;
}

/* ---- inject: check ------------------------------------------------- */

static void ar_noop_cb(const char *n, void *u) { (void)n; (void)u; }

static MunitResult
test_inject_check(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(xtc_inject_check("ar.point"), ==, 0);
	munit_assert_int(xtc_inject_attach("ar.point", ar_noop_cb, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_inject_check("ar.point"), ==, 1);
	munit_assert_int(xtc_inject_check("ar.other"), ==, 0);
	munit_assert_int(xtc_inject_detach("ar.point"), ==, XTC_OK);
	munit_assert_int(xtc_inject_check("ar.point"), ==, 0);
	return MUNIT_OK;
}

/* ---- io: aio_submit ------------------------------------------------ */

static MunitResult
test_io_aio_submit(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	xtc_aio_t a;
	xtc_io_event_t ev[8];
	char path[256], buf[16] = "aio-submit-data", back[16];
	const char *be = xtc_io_backend_name();
	int fd = -1, rc, n, i, seen = 0, tries;
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(xtc_fs_tmpdir(path, sizeof path), ==, XTC_OK);
	(void)xtc_strlcat(path, "/xtc_ar_aio_XXXXXX", sizeof path);
	munit_assert_int(xtc_fs_mkstemp(path, &fd), ==, XTC_OK);

	memset(&a, 0, sizeof a);
	a.fd = fd; a.op = XTC_AIO_PWRITE; a.buf = buf;
	a.len = (uint32_t)sizeof buf; a.off = 0; a.tag = &a;
	rc = xtc_io_aio_submit(io, &a);
	if (strcmp(be, "uring") == 0) {
		munit_assert_int(xtc_io_aio_submit(NULL, &a), ==, XTC_E_INVAL);
		munit_assert_int(rc, ==, XTC_OK);
	}
	if (rc == XTC_E_NOSYS) {
		/* A readiness-only backend has no native file completion;
		 * the contract is NOSYS so the caller offloads. */
		munit_assert_string_not_equal(be, "uring");
	} else if (rc == XTC_E_AGAIN) {
		/* kqueue/solaris POSIX AIO may decline an fd: offload. */
		munit_assert_true(strcmp(be, "kqueue") == 0 ||
		    strcmp(be, "solaris") == 0);
	} else {
		munit_assert_int(rc, ==, XTC_OK);
		for (tries = 0; tries < 50 && !seen; tries++) {
			n = 0;
			munit_assert_int(xtc_io_poll(io, ev, 8, 100 * MS, &n),
			    ==, XTC_OK);
			for (i = 0; i < n; i++)
				if ((ev[i].flags & XTC_IO_AIO) && ev[i].tag == &a)
					seen = 1;
		}
		munit_assert_int(seen, ==, 1);
		munit_assert_int(a.res, ==, (int)sizeof buf);
		munit_assert_llong((long long)pread(fd, back, sizeof back, 0), ==,
		    (ssize_t)sizeof back);
		munit_assert_memory_equal(sizeof buf, back, buf);
	}
	(void)xtc_fs_close(fd);
	(void)xtc_fs_unlink(path);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- topology: ncpus + numa ---------------------------------------- */

static MunitResult
test_topology(const MunitParameter p[], void *d)
{
	int ncpu = xtc_ncpus(), nn = xtc_numa_nnodes(), c, node;
	long online = sysconf(_SC_NPROCESSORS_ONLN);
#if defined(__linux__)
	cpu_set_t set;
	int first = -1;
#endif
	(void)p; (void)d;
	munit_assert_int(ncpu, >=, 1);
	/* The cgroup quota can only lower the count, never raise it. */
	if (online > 0)
		munit_assert_long((long)ncpu, <=, online);
	munit_assert_int(nn, >=, 1);
	for (c = 0; c < ncpu; c++) {
		node = xtc_numa_node_of_cpu(c);
		munit_assert_int(node, >=, 0);
		munit_assert_int(node, <, 64);
		if (nn == 1)
			munit_assert_int(node, ==, 0);
	}
	node = xtc_numa_current_node();
	munit_assert_int(node, >=, 0);
#if defined(__linux__)
	/* Pinned to one allowed CPU, the current node IS that CPU's node.
	 * munit forks each test, so the pin does not leak. */
	munit_assert_int(sched_getaffinity(0, sizeof set, &set), ==, 0);
	for (c = 0; c < CPU_SETSIZE && first < 0; c++)
		if (CPU_ISSET(c, &set)) first = c;
	munit_assert_int(first, >=, 0);
	CPU_ZERO(&set);
	CPU_SET(first, &set);
	munit_assert_int(sched_setaffinity(0, sizeof set, &set), ==, 0);
	munit_assert_int(xtc_numa_current_node(), ==,
	    xtc_numa_node_of_cpu(first));
#endif
	return MUNIT_OK;
}

/* ---- dump: xtc_panic called directly (not via XTC_PANIC) ----------- */

static MunitResult
test_panic_direct(const MunitParameter p[], void *d)
{
	int pfd[2], st = 0;
	pid_t pid;
	char buf[8192];
	size_t off = 0;
	ssize_t r;
	(void)p; (void)d;
	munit_assert_int(pipe(pfd), ==, 0);
	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDERR_FILENO);
		xtc_panic("ar_file.c", 77, "direct %d", 5);
	}
	close(pfd[1]);
	while (off < sizeof buf - 1 &&
	    (r = read(pfd[0], buf + off, sizeof buf - 1 - off)) > 0)
		off += (size_t)r;
	buf[off] = '\0';
	close(pfd[0]);
	munit_assert_int(waitpid(pid, &st, 0), ==, pid);
	munit_assert_true(WIFSIGNALED(st));
	munit_assert_int(WTERMSIG(st), ==, SIGABRT);
	munit_assert_not_null(strstr(buf, "xtc panic: direct 5"));
	munit_assert_not_null(strstr(buf, "ar_file.c:77"));
	return MUNIT_OK;
}

/* ---- proc: critical_leave, recovery_disarm (forked fault children) -- */

enum { FLT_PLAIN, FLT_DISARM, FLT_CRIT_BALANCED, FLT_CRIT_UNBALANCED };

static void
flt_body(void *arg)
{
	int mode = (int)(intptr_t)arg;
	volatile uintptr_t addr = 0x10;
	switch (mode) {
	case FLT_DISARM:
		xtc_proc_recovery_disarm();
		break;
	case FLT_CRIT_BALANCED:
		xtc_proc_critical_enter(); xtc_proc_critical_enter();
		xtc_proc_critical_leave(); xtc_proc_critical_leave();
		break;
	case FLT_CRIT_UNBALANCED:
		xtc_proc_critical_enter(); xtc_proc_critical_enter();
		xtc_proc_critical_leave();
		break;
	default:
		break;
	}
	*(volatile int *)addr = 1;
}

/* Fork a child whose one proc faults after `mode`'s setup.  Returns the
 * wait status: exit 0 = the fault was contained, killed by SIGSEGV/SIGBUS
 * = it escalated. */
static int
run_fault_child(int mode)
{
	pid_t pid;
	int st = 0;
	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		xtc_loop_t *loop = NULL;
		alarm(20);
		(void)xtc_fault_guard_install();
		if (xtc_loop_init(&loop) != XTC_OK) _exit(97);
		if (xtc_proc_spawn(loop, flt_body, (void *)(intptr_t)mode,
		    NULL, NULL) != XTC_OK) _exit(96);
		(void)xtc_loop_run(loop);
		_exit(0);
	}
	munit_assert_int(waitpid(pid, &st, 0), ==, pid);
	return st;
}

static int
escalated(int st)
{
	return WIFSIGNALED(st) &&
	    (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS);
}

static MunitResult
test_recovery_disarm(const MunitParameter p[], void *d)
{
	int st;
	(void)p; (void)d;
#if defined(SANITIZER_OWNS_SIGSEGV)
	return MUNIT_SKIP;   /* the sanitizer owns SIGSEGV */
#endif
	/* Control: the auto-armed default frame contains the fault. */
	st = run_fault_child(FLT_PLAIN);
	munit_assert_true(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	/* Disarmed: nothing catches it, the process dies. */
	st = run_fault_child(FLT_DISARM);
	munit_assert_true(escalated(st));
	xtc_proc_recovery_disarm();   /* off a proc: a no-op, no crash */
	return MUNIT_OK;
}

static MunitResult
test_critical_leave(const MunitParameter p[], void *d)
{
	int st;
	(void)p; (void)d;
#if defined(SANITIZER_OWNS_SIGSEGV)
	return MUNIT_SKIP;
#endif
	/* enter x2 / leave x2 -> depth 0 -> contained. */
	st = run_fault_child(FLT_CRIT_BALANCED);
	munit_assert_true(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	/* enter x2 / leave x1 -> still inside -> escalates.  Proves leave
	 * decrements by exactly one level (nesting). */
	st = run_fault_child(FLT_CRIT_UNBALANCED);
	munit_assert_true(escalated(st));
	xtc_proc_critical_leave();    /* off a proc: a no-op, no crash */
	return MUNIT_OK;
}

/* ---- proc: recovery_untrack_fd ------------------------------------- */

struct untrack_st {
	int keep_fd, drop_fd;
	int rc_first, rc_again;
};

static void
untrack_body(void *arg)
{
	struct untrack_st *s = arg;
	munit_assert_int(xtc_proc_recovery_track_fd(s->keep_fd), ==, XTC_OK);
	munit_assert_int(xtc_proc_recovery_track_fd(s->drop_fd), ==, XTC_OK);
	s->rc_first = xtc_proc_recovery_untrack_fd(s->keep_fd);
	s->rc_again = xtc_proc_recovery_untrack_fd(s->keep_fd);
	/* normal return: tracked resources are released on exit */
}

static MunitResult
test_recovery_untrack_fd(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct untrack_st s;
	int a[2], b[2];
	(void)p; (void)d;
	munit_assert_int(xtc_proc_recovery_untrack_fd(0), ==, XTC_E_INVAL);
	munit_assert_int(pipe(a), ==, 0);
	munit_assert_int(pipe(b), ==, 0);
	s.keep_fd = a[1]; s.drop_fd = b[1];
	s.rc_first = s.rc_again = 12345;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, untrack_body, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(s.rc_first, ==, XTC_OK);
	munit_assert_int(s.rc_again, ==, XTC_E_NOTFOUND);
	/* The untracked fd survived the proc's exit; the tracked one was
	 * closed by it. */
	munit_assert_int(fcntl(a[1], F_GETFD), !=, -1);
	munit_assert_int(fcntl(b[1], F_GETFD), ==, -1);
	close(a[0]); close(a[1]); close(b[0]);
	return MUNIT_OK;
}

/* ---- proc: set_class ----------------------------------------------- */

struct class_st {
	xtc_exec_class_t mine, foreign;
	int rc_foreign, rc_set, rc_reset;
	uint64_t runs_after_set, runs_after_reset;
};

static void
class_body(void *arg)
{
	struct class_st *s = arg;
	int i;
	s->rc_foreign = xtc_proc_set_class(s->foreign);
	s->rc_set = xtc_proc_set_class(s->mine);
	for (i = 0; i < 5; i++) xtc_yield();
	s->runs_after_set = xtc_exec_class_runs(s->mine);
	s->rc_reset = xtc_proc_set_class(NULL);
	xtc_yield();     /* one more pick from the class queue is possible */
	s->runs_after_set = xtc_exec_class_runs(s->mine);
	for (i = 0; i < 5; i++) xtc_yield();
	s->runs_after_reset = xtc_exec_class_runs(s->mine);
}

static MunitResult
test_proc_set_class(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL, *other = NULL;
	struct class_st s;
	(void)p; (void)d;
	memset(&s, 0, sizeof s);
	munit_assert_int(xtc_proc_set_class(NULL), ==, XTC_E_INVAL); /* off proc */
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&other), ==, XTC_OK);
	munit_assert_int(xtc_exec_class_create(loop, 100, 0, &s.mine),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_class_create(other, 100, 0, &s.foreign),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, class_body, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(s.rc_foreign, ==, XTC_E_INVAL);
	munit_assert_int(s.rc_set, ==, XTC_OK);
	munit_assert_int(s.rc_reset, ==, XTC_OK);
	/* Placed in the class: each yield is a pick from its queue. */
	munit_assert_uint64(s.runs_after_set, >=, 5);
	/* Back on the default lane: the class sees no more picks. */
	munit_assert_uint64(s.runs_after_reset, ==, s.runs_after_set);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(other), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- sim knobs, exercised in the ordinary build (sim dormant) ------ */

static MunitResult
test_sim_knobs(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	xtc_sim_deactivate();
	/* Dormant: every decision point is 0. */
	xtc_sim_buggify_enable(1000);
	xtc_sim_fault_points_enable(1000);
	munit_assert_int(xtc_sim_buggify("ar.bug"), ==, 0);
	munit_assert_int(xtc_sim_buggify_fault(1000), ==, 0);
	munit_assert_int(xtc_sim_fault_point("ar.fp"), ==, 0);

	xtc_sim_activate(0xA11CE);
	/* buggify: a once-per-run coin, cached per name. */
	xtc_sim_buggify_enable(1000);
	munit_assert_int(xtc_sim_buggify("ar.bug"), ==, 1);
	munit_assert_int(xtc_sim_buggify("ar.bug"), ==, 1);
	munit_assert_int(xtc_sim_buggify_reached_count(), ==, 1);
	munit_assert_int(xtc_sim_buggify_active_count(), ==, 1);
	munit_assert_int(xtc_sim_buggify(NULL), ==, 0);
	munit_assert_int(xtc_sim_buggify_fault(1000), ==, 1);
	munit_assert_int(xtc_sim_buggify_fault(0), ==, 0);
	xtc_sim_buggify_enable(0);
	munit_assert_int(xtc_sim_buggify("ar.bug2"), ==, 0);
	xtc_sim_buggify_disable();
	munit_assert_int(xtc_sim_buggify("ar.bug3"), ==, 0);
	munit_assert_int(xtc_sim_buggify_fault(1000), ==, 0);

	/* fault points: a fresh draw each reach, recorded. */
	xtc_sim_fault_points_enable(1000);
	munit_assert_int(xtc_sim_fault_point("ar.fp"), ==, 1);
	munit_assert_int(xtc_sim_fault_point("ar.fp"), ==, 1);
	munit_assert_uint64(xtc_sim_fault_point_fires("ar.fp"), ==, 2);
	munit_assert_int(xtc_sim_fault_point(NULL), ==, 0);
	xtc_sim_fault_points_enable(0);
	munit_assert_int(xtc_sim_fault_point("ar.fp2"), ==, 0);
	xtc_sim_fault_points_disable();

	/* partition_isolate cuts one loop off in both directions only. */
	xtc_sim_partition_clear();
	xtc_sim_partition_isolate(1);
	munit_assert_int(__xtc_sim_partition_blocked(1, 0), ==, 1);
	munit_assert_int(__xtc_sim_partition_blocked(0, 1), ==, 1);
	munit_assert_int(__xtc_sim_partition_blocked(2, 1), ==, 1);
	munit_assert_int(__xtc_sim_partition_blocked(1, 1), ==, 0);
	munit_assert_int(__xtc_sim_partition_blocked(0, 2), ==, 0);
	xtc_sim_partition_isolate(-1);            /* out of range: ignored */
	xtc_sim_partition_clear();
	munit_assert_int(__xtc_sim_partition_blocked(1, 0), ==, 0);

	/* swizzle_disable drops the reorder rate to 0. */
	xtc_sim_swizzle_enable(500);
	munit_assert_int(__xtc_sim_swizzle_pct(), ==, 500);
	xtc_sim_swizzle_disable();
	munit_assert_int(__xtc_sim_swizzle_pct(), ==, 0);

	xtc_sim_deactivate();
	return MUNIT_OK;
}

static MunitResult
test_sim_check(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int saved;
	(void)p; (void)d;
	munit_assert_int(xtc_sim_check(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	munit_assert_int(xtc_sim_check(e), ==, XTC_OK);
	/* Plant one corruption the checker names (negative free-list
	 * count) and require it to be caught, then restore. */
	l = xtc_exec_loop(e, 1);
	saved = l->task_free_n;
	l->task_free_n = -1;
	munit_assert_int(xtc_sim_check(e), ==, XTC_E_INTERNAL);
	l->task_free_n = saved;
	munit_assert_int(xtc_sim_check(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- slab: pressure_listen (fire-and-forget) + reaper_spawn -------- */

static MunitResult
test_slab_pressure_listen(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* No PSI file at that path (or no PSI on this platform): NOSYS,
	 * and no listener thread is left behind. */
	munit_assert_int(xtc_slab_pressure_listen("/nonexistent/xtc/psi",
	    NULL, NULL), ==, XTC_E_NOSYS);
	return MUNIT_OK;
}

struct reap_st {
	xtc_slab_t *slab;
	xtc_pid_t reaper;
	uint64_t before, after;
};

static void
reap_watch(void *arg)
{
	struct reap_st *s = arg;
	xtc_slab_stats_t st;
	munit_assert_int(xtc_slab_stat(s->slab, &st), ==, XTC_OK);
	s->before = st.reaps;
	(void)xtc_proc_sleep(30 * MS);
	munit_assert_int(xtc_slab_stat(s->slab, &st), ==, XTC_OK);
	s->after = st.reaps;
	(void)xtc_exit_pid(s->reaper, 0);   /* it runs forever otherwise */
}

static MunitResult
test_slab_reaper_spawn(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
	struct reap_st s;
	(void)p; (void)d;
	memset(&s, 0, sizeof s);
	o.name = "ar.reap"; o.obj_size = 64;
	munit_assert_int(xtc_slab_create(&o, &s.slab), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_slab_reaper_spawn(NULL, MS, &s.reaper),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_slab_reaper_spawn(loop, 0, &s.reaper),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_slab_reaper_spawn(loop, MS, &s.reaper),
	    ==, XTC_OK);
	munit_assert_false(xtc_pid_is_none(s.reaper));
	munit_assert_int(xtc_proc_spawn(loop, reap_watch, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	/* A 1ms reaper over 30ms reaped the registered cache repeatedly. */
	munit_assert_uint64(s.after, >, s.before);
	xtc_slab_destroy(s.slab);
	return MUNIT_OK;
}

/* ---- tail: from_env + spill_dial9 ---------------------------------- */

static MunitResult
test_tail_from_env(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	xtc_tail_disable();
	munit_assert_int(xtc_env_set("XTC_TAIL_ENABLE", "", 1), ==, XTC_OK);
	munit_assert_uint(xtc_tail_from_env(), ==, 0);
	munit_assert_int(xtc_env_set("XTC_TAIL_ENABLE", "off", 1), ==, XTC_OK);
	munit_assert_uint(xtc_tail_from_env(), ==, 0);
	munit_assert_uint(xtc_tail_enable(0), ==, 0);   /* nothing enabled */
	munit_assert_int(xtc_env_set("XTC_TAIL_ENABLE", "1", 1), ==, XTC_OK);
	munit_assert_uint(xtc_tail_from_env(), ==, XTC_TAIL_SCHED);
	munit_assert_uint(xtc_tail_enable(0), ==, XTC_TAIL_SCHED);
	munit_assert_int(xtc_env_set("XTC_TAIL_ENABLE", "all", 1), ==, XTC_OK);
	munit_assert_uint(xtc_tail_from_env(), ==, XTC_TAIL_ALL);
	munit_assert_uint(xtc_tail_enable(0), ==, XTC_TAIL_ALL);
	(void)xtc_env_set("XTC_TAIL_ENABLE", "", 1);
	return MUNIT_OK;
}

static void tail_worker(void *a) { (void)a; xtc_yield(); }

static MunitResult
test_tail_spill_dial9(const MunitParameter p[], void *d)
{
	char dir[256], file[512];
	xtc_loop_t *loop = NULL;
	DIR *dp;
	struct dirent *de;
	int files = 0, fd;
	unsigned char hdr[5];
	(void)p; (void)d;
	munit_assert_int(xtc_tail_spill_dial9(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_tail_spill_dial9("/nonexistent/xtc/spill"),
	    !=, XTC_OK);

	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	(void)xtc_strlcat(dir, "/xtc_ar_spill_XXXXXX", sizeof dir);
	munit_assert_not_null(mkdtemp(dir));
	(void)xtc_tail_enable(XTC_TAIL_SCHED);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, tail_worker, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(xtc_tail_spill_dial9(dir), ==, XTC_OK);
	xtc_tail_disable();

	/* Exactly one xtc-tail-<pid>-*.d9 segment, in dial9 wire format. */
	dp = opendir(dir);
	munit_assert_not_null(dp);
	while ((de = readdir(dp)) != NULL) {
		size_t n = strlen(de->d_name);
		if (de->d_name[0] == '.') continue;
		files++;
		munit_assert_int(strncmp(de->d_name, "xtc-tail-", 9), ==, 0);
		munit_assert_size(n, >, 3);
		munit_assert_string_equal(de->d_name + n - 3, ".d9");
		snprintf(file, sizeof file, "%s/%s", dir, de->d_name);
		fd = open(file, O_RDONLY);
		munit_assert_int(fd, >=, 0);
		munit_assert_llong((long long)read(fd, hdr, sizeof hdr), ==, 5);
		close(fd);
		munit_assert_memory_equal(4, hdr, "TRC\0");
		munit_assert_uint8(hdr[4], ==, 1);
		(void)unlink(file);
	}
	closedir(dp);
	(void)rmdir(dir);
	munit_assert_int(files, ==, 1);
	return MUNIT_OK;
}

/* ---- tnt: shard_id ------------------------------------------------- */

static atomic_int g_shard_id0 = -1, g_shard_id1 = -1;

static xtc_tnt_transition_t
tnt_boot_init(void *self, const void *args, size_t n)
{
	(void)self; (void)args; (void)n;
	atomic_store(&g_shard_id0, (int)xtc_tnt_shard_id());
	(void)xtc_tnt_spawn_on(1, 1, NULL, 0);   /* the probe, on shard 1 */
	return XTC_TNT_TRANSITION_DONE;
}

static xtc_tnt_transition_t
tnt_probe_init(void *self, const void *args, size_t n)
{
	(void)self; (void)args; (void)n;
	atomic_store(&g_shard_id1, (int)xtc_tnt_shard_id());
	xtc_tnt_stop();
	return XTC_TNT_TRANSITION_DONE;
}

static xtc_tnt_transition_t
tnt_done(void *self, xtc_tnt_message_t *m)
{
	(void)self; (void)m;
	return XTC_TNT_TRANSITION_DONE;
}

static MunitResult
test_tnt_shard_id(const MunitParameter p[], void *d)
{
	static const xtc_tnt_type_t types[2] = {
		{ .id = 0, .name = "boot", .slot_count = 1, .stride = 8,
		  .mailbox_capacity = 1, .budget_weight = 1,
		  .init_fn = tnt_boot_init, .handler_fn = tnt_done },
		{ .id = 1, .name = "probe", .slot_count = 1, .stride = 8,
		  .mailbox_capacity = 1, .budget_weight = 1,
		  .init_fn = tnt_probe_init, .handler_fn = tnt_done },
	};
	xtc_tnt_spec_t spec;
	int rc;
	(void)p; (void)d;
	munit_assert_uint8(xtc_tnt_shard_id(), ==, 0);   /* outside a shard */
	memset(&spec, 0, sizeof spec);
	spec.name = "ar-tnt"; spec.types = types; spec.n_types = 2;
	spec.shard_count = 2; spec.boot_type = 0;
	alarm(20);   /* a missed stop would block xtc_tnt_start forever */
	rc = xtc_tnt_start(&spec);
	alarm(0);
	if (rc == XTC_E_NOSYS)
		return MUNIT_SKIP;
	munit_assert_int(rc, ==, 0);
	munit_assert_int(atomic_load(&g_shard_id0), ==, 0);
	munit_assert_int(atomic_load(&g_shard_id1), ==, 1);
	return MUNIT_OK;
}

/* ---- stats: tuning_check routes advisories to the default log ------ */

#if defined(__linux__)
static char   g_log[8192];
static size_t g_log_n;

static int
cap_sink(void *u, xtc_log_level_t l, const char *b, size_t n)
{
	(void)u; (void)l;
	if (g_log_n + n < sizeof g_log) {
		memcpy(g_log + g_log_n, b, n);
		g_log_n += n;
	}
	return 0;
}

static int
tuning_says_governor(const char *governor)
{
	char path[] = "/tmp/xtc_ar_gov_XXXXXX";
	xtc_log_opts_t o = XTC_LOG_OPTS_DEFAULT;
	xtc_log_t *log = NULL;
	int fd = mkstemp(path);
	munit_assert_int(fd, >=, 0);
	munit_assert_llong((long long)write(fd, governor, strlen(governor)), ==,
	    (ssize_t)strlen(governor));
	close(fd);
	__xtc_os_tuning_governor_path_override(path);
	g_log_n = 0;
	memset(g_log, 0, sizeof g_log);
	o.sink = cap_sink; o.sink_fd = -1;
	munit_assert_int(xtc_log_create(&o, &log), ==, XTC_OK);
	munit_assert_int(xtc_log_set_default(log), ==, XTC_OK);
	xtc_tuning_check();
	munit_assert_int(xtc_log_drain(log), >=, 0);
	(void)xtc_log_set_default(NULL);
	xtc_log_destroy(log);
	__xtc_os_tuning_governor_path_override(NULL);
	(void)unlink(path);
	return strstr(g_log, "cpu governor") != NULL;
}
#endif

static MunitResult
test_tuning_check(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
#if defined(__linux__)
	munit_assert_int(tuning_says_governor("powersave\n"), ==, 1);
	munit_assert_int(tuning_says_governor("performance\n"), ==, 0);
	return MUNIT_OK;
#else
	xtc_tuning_check();   /* every probe is Linux-only: a no-op here */
	return MUNIT_SKIP;
#endif
}

/* ---- xproc: child_main, driven directly over a socketpair ---------- */

static void
xp_root(void *arg)
{
	void *m = NULL;
	size_t n = 0;
	(void)arg;
	if (xtc_recv(&m, &n, 5000 * MS) != XTC_OK)
		xtc_exit_self(9);
	if (n == 4 && memcmp(m, "ping", 4) == 0) {
		xtc_free(m);
		xtc_exit_self(42);
	}
	xtc_free(m);
	xtc_exit_self(7);
}

static MunitResult
test_xproc_child_main(const MunitParameter p[], void *d)
{
	int sv[2], st = 0;
	pid_t pid;
	(void)p; (void)d;
	/* No root function: refused before any runtime is stood up. */
	munit_assert_int(xtc_xproc_child_main(-1, NULL, NULL), !=, 0);

	munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		close(sv[0]);
		alarm(20);
		_exit(xtc_xproc_child_main(sv[1], xp_root, NULL) & 0xff);
	}
	close(sv[1]);
	/* The frame the parent sends reaches the root proc's mailbox; the
	 * root's exit reason becomes child_main's return. */
	munit_assert_int(xtc_net_send_frame(sv[0], "ping", 4), ==, XTC_OK);
	munit_assert_int(waitpid(pid, &st, 0), ==, pid);
	close(sv[0]);
	munit_assert_true(WIFEXITED(st));
	munit_assert_int(WEXITSTATUS(st), ==, 42);
	return MUNIT_OK;
}

#define T(name, fn) { name, fn, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
static MunitTest tests[] = {
	T("/cfg_ref_get_int64",       test_cfg_ref_get_int64),
	T("/cfg_ssn_set_kinds",       test_cfg_ssn_set_kinds),
	T("/fs_fdatasync",            test_fs_fdatasync),
	T("/inject_check",            test_inject_check),
	T("/io_aio_submit",           test_io_aio_submit),
	T("/topology",                test_topology),
	T("/panic_direct",            test_panic_direct),
	T("/recovery_disarm",         test_recovery_disarm),
	T("/critical_leave",          test_critical_leave),
	T("/recovery_untrack_fd",     test_recovery_untrack_fd),
	T("/proc_set_class",          test_proc_set_class),
	T("/sim_knobs",               test_sim_knobs),
	T("/sim_check",               test_sim_check),
	T("/slab_pressure_listen",    test_slab_pressure_listen),
	T("/slab_reaper_spawn",       test_slab_reaper_spawn),
	T("/tail_from_env",           test_tail_from_env),
	T("/tail_spill_dial9",        test_tail_spill_dial9),
	T("/tnt_shard_id",            test_tnt_shard_id),
	T("/tuning_check",            test_tuning_check),
	T("/xproc_child_main",        test_xproc_child_main),
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/coverage/api_reach", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}

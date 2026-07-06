/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/msvc/smoke.c
 *
 *	A dependency-free smoke test for the MSVC build.  The munit
 *	harness uses GCC-isms (VLA array parameters via
 *	MUNIT_ARRAY_PARAM, GCC pragmas) that MSVC rejects, so this
 *	standalone test links xtc.lib and exercises a representative
 *	slice of the library that does not need the harness:
 *
 *	  - version + strerror (pure functions)
 *	  - the monotonic / realtime clocks (the Win32 os_time path)
 *	  - a slab cache alloc/free round-trip (Win32 TLS magazines)
 *	  - an lwlock acquire/release (the lock fast path)
 *
 *	Exit code 0 = pass, nonzero = the first failed check.
 */

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>

#include "xtc.h"
#include "xtc_slab.h"
#include "xtc_lwlock.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_aio.h"
#include "xtc_fs.h"
#include "os_time.h"

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

/* Fault containment: a proc arms a recovery frame, then dereferences a
 * wild pointer.  On Windows the access violation is caught by the
 * Vectored Exception Handler installed by xtc_fault_guard_install,
 * which longjmps back here so xtc_proc_recovery_arm returns nonzero --
 * the proc recovers instead of the process crashing. */
static volatile int s_fault_recovered;
static void
smoke_faulty_proc(void *arg)
{
	volatile int *wild = (volatile int *)0;
	(void)arg;
	if (xtc_proc_recovery_arm() != 0) {
		s_fault_recovered = 1;          /* came back from the fault */
		(void)xtc_exit_self(7);         /* clean contained-fault exit */
		return;
	}
	*wild = 42;                             /* access violation */
}

/* Native IOCP file AIO: a proc writes then reads back a marker through
 * xtc_aio on an OVERLAPPED file handle.  pwrite/pread complete via
 * overlapped ReadFile/WriteFile whose events the loop waits on; fsync
 * has no async form on Windows and is offloaded.  Proves the
 * xtc_io_aio_submit IOCP path end to end. */
static volatile int s_aio_ok;
static int s_aio_fd;
static void
smoke_aio_proc(void *arg)
{
	static const char msg[] = "xtc-iocp-aio-native";
	char buf[64];
	int n;
	(void)arg;
	memset(buf, 0, sizeof buf);
	n = xtc_aio_pwrite(s_aio_fd, msg, (uint32_t)sizeof msg, 0);
	if (n != (int)sizeof msg) { (void)xtc_exit_self(1); return; }
	n = xtc_aio_pread(s_aio_fd, buf, (uint32_t)sizeof msg, 0);
	if (n != (int)sizeof msg) { (void)xtc_exit_self(2); return; }
	if (memcmp(buf, msg, sizeof msg) != 0) { (void)xtc_exit_self(3); return; }
	(void)xtc_aio_fdatasync(s_aio_fd);   /* offloaded on Windows */
	s_aio_ok = 1;
	(void)xtc_exit_self(0);
}

static const int s_sel_sent[5] = { 1, 2, 42, 3, 4 };
static int s_sel_seen[5];
static int s_sel_n;
static int
smoke_match_42(const void *data, size_t size, void *user)
{
	(void)size; (void)user;
	return *(const int *)data == 42;
}
static int
smoke_match_any(const void *data, size_t size, void *user)
{
	(void)data; (void)size; (void)user;
	return 1;
}
static void
smoke_selective_proc(void *arg)
{
	void *msg; size_t sz; int rc;
	(void)arg;
	/* Pick 42 specifically first, even though it is third in the
	 * mailbox, then drain the rest in arrival order. */
	rc = xtc_recv_match(smoke_match_42, NULL, &msg, &sz,
	    1000LL * 1000 * 1000);
	if (rc != XTC_OK) { (void)xtc_exit_self(1); return; }
	s_sel_seen[s_sel_n++] = *(int *)msg;
	xtc_free(msg);
	while (s_sel_n < 5) {
		rc = xtc_recv_match(smoke_match_any, NULL, &msg, &sz,
		    100LL * 1000 * 1000);
		if (rc != XTC_OK) break;
		s_sel_seen[s_sel_n++] = *(int *)msg;
		xtc_free(msg);
	}
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: survive a crash */
	/* --- version --- */
	{
		const char *v = xtc_version_string();
		CHECK(v != NULL && v[0] != '\0');
		printf("  ok   version = %s\n", v);
	}

	/* --- strerror --- */
	{
		const char *e = xtc_strerror(XTC_E_NOMEM);
		CHECK(e != NULL && e[0] != '\0');
		printf("  ok   strerror(XTC_E_NOMEM) = %s\n", e);
	}

	/* --- clocks (Win32 os_time path) --- */
	{
		int64_t a = 0, b = 0, r = 0;
		CHECK(__os_clock_mono(&a) == XTC_OK);
		CHECK(__os_clock_mono(&b) == XTC_OK);
		CHECK(b >= a);                 /* monotonic non-decreasing */
		CHECK(__os_clock_real(&r) == XTC_OK);
		CHECK(r > 0);
		printf("  ok   clocks: mono delta=%lld ns, real>0\n",
		    (long long)(b - a));
	}

	/* --- slab cache (Win32 TLS magazines) --- */
	{
		xtc_slab_t *s = NULL;
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		void *p, *q;
		o.obj_size = 64;
		CHECK(xtc_slab_create(&o, &s) == XTC_OK);
		p = xtc_slab_alloc(s);
		CHECK(p != NULL);
		memset(p, 0xab, 64);
		xtc_slab_free(s, p);
		q = xtc_slab_alloc(s);          /* should recycle */
		CHECK(q != NULL);
		xtc_slab_free(s, q);
		xtc_slab_destroy(s);
		printf("  ok   slab alloc/free round-trip\n");
	}

	/* --- lwlock fast path --- */
	{
		xtc_lwlock_t lw;
		CHECK(xtc_lwlock_init(&lw, 0) == XTC_OK);
		CHECK(xtc_lwlock_acquire(&lw, XTC_LW_EXCLUSIVE) == XTC_OK);
		xtc_lwlock_release(&lw);
		CHECK(xtc_lwlock_acquire(&lw, XTC_LW_SHARED) == XTC_OK);
		xtc_lwlock_release(&lw);
		xtc_lwlock_destroy(&lw);
		printf("  ok   lwlock acquire/release (X + S)\n");
	}

	/* --- fault containment (Windows SEH / VEH) --- */
	{
		xtc_loop_t *loop = NULL;
		xtc_proc_opts_t po = { 0 };
		xtc_pid_t pid;
		s_fault_recovered = 0;
		CHECK(xtc_fault_guard_install() == XTC_OK);
		CHECK(xtc_loop_init(&loop) == XTC_OK);
		po.name = "faulty";
		CHECK(xtc_proc_spawn(loop, smoke_faulty_proc, NULL, &po, &pid)
		    == XTC_OK);
		/* If the VEH did not contain the fault, this process would
		 * crash here instead of returning. */
		CHECK(xtc_loop_run(loop) == XTC_OK);
		CHECK(s_fault_recovered == 1);
		(void)xtc_loop_fini(loop);
		printf("  ok   fault contained (wild write recovered via SEH)\n");
	}

	/* --- native IOCP file AIO (overlapped ReadFile/WriteFile) --- */
	{
		xtc_loop_t *loop = NULL;
		xtc_proc_opts_t po = { 0 };
		xtc_pid_t pid;
		char path[MAX_PATH], dir[MAX_PATH];
		HANDLE fh;
		DWORD dn = GetTempPathA(sizeof dir, dir);
		CHECK(dn > 0 && dn < sizeof dir);
		CHECK(GetTempFileNameA(dir, "xtc", 0, path) != 0);
		/* OVERLAPPED handle so the read/write are truly asynchronous. */
		fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
		    CREATE_ALWAYS,
		    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OVERLAPPED, NULL);
		CHECK(fh != INVALID_HANDLE_VALUE);
		s_aio_fd = _open_osfhandle((intptr_t)fh, _O_BINARY);
		CHECK(s_aio_fd >= 0);
		s_aio_ok = 0;
		CHECK(xtc_loop_init(&loop) == XTC_OK);
		po.name = "aio";
		CHECK(xtc_proc_spawn(loop, smoke_aio_proc, NULL, &po, &pid)
		    == XTC_OK);
		CHECK(xtc_loop_run(loop) == XTC_OK);
		CHECK(s_aio_ok == 1);
		(void)xtc_loop_fini(loop);
		(void)_close(s_aio_fd);            /* closes fh */
		(void)DeleteFileA(path);
		printf("  ok   native IOCP file AIO: overlapped pwrite+pread"
		    " round-trip\n");
	}

	/* --- portable filesystem helpers (xtc_fs): tmpdir + mkstemp +
	 * pwrite/pread round-trip + fsize + stat + unlink on Win32 --- */
	{
		char dir[512], tmpl[600];
		int fd = -1;
		int64_t sz = -1;
		size_t done = 0;
		char buf[32];
		xtc_fs_stat_t st;
		static const char msg[] = "xtc_fs-win";

		CHECK(xtc_fs_tmpdir(dir, sizeof dir) == XTC_OK);
		CHECK(dir[0] != '\0');
		_snprintf(tmpl, sizeof tmpl, "%s\\xtcfs-XXXXXX", dir);
		CHECK(xtc_fs_mkstemp(tmpl, &fd) == XTC_OK && fd >= 0);
		CHECK(xtc_fs_pwrite(fd, msg, sizeof msg, 0, &done) == XTC_OK);
		CHECK(done == sizeof msg);
		CHECK(xtc_fs_fsync(fd) == XTC_OK);
		CHECK(xtc_fs_fsize(fd, &sz) == XTC_OK && sz == (int64_t)sizeof msg);
		memset(buf, 0, sizeof buf);
		done = 0;
		CHECK(xtc_fs_pread(fd, buf, sizeof msg, 0, &done) == XTC_OK);
		CHECK(done == sizeof msg && strcmp(buf, msg) == 0);
		CHECK(xtc_fs_close(fd) == XTC_OK);
		CHECK(xtc_fs_stat(tmpl, &st) == XTC_OK && st.size == (int64_t)sizeof msg);
		CHECK(xtc_fs_exists(tmpl) == 1);
		CHECK(xtc_fs_unlink(tmpl) == XTC_OK);
		CHECK(xtc_fs_exists(tmpl) == 0);
		printf("  ok   xtc_fs: tmpdir + mkstemp + pwrite/pread + stat +"
		    " unlink round-trip\n");
	}

	/* Selective receive on the IOCP wakeup path (KNOWN_ISSUES: this
	 * flaked on Windows historically; confirm the round-2 IOCP rewrite
	 * fixed it).  Send 5 before running the loop; the proc pulls 42
	 * first via xtc_recv_match, then drains 1,2,3,4 in arrival order. */
	{
		xtc_loop_t *loop = NULL;
		xtc_pid_t pid;
		xtc_proc_opts_t po; memset(&po, 0, sizeof po);
		int i;
		s_sel_n = 0;
		memset(s_sel_seen, 0, sizeof s_sel_seen);
		CHECK(xtc_loop_init(&loop) == XTC_OK);
		CHECK(xtc_proc_spawn(loop, smoke_selective_proc, NULL, &po,
		    &pid) == XTC_OK);
		for (i = 0; i < 5; i++)
			CHECK(xtc_send(pid, &s_sel_sent[i], sizeof(int))
			    == XTC_OK);
		CHECK(xtc_loop_run(loop) == XTC_OK);
		(void)xtc_loop_fini(loop);
		CHECK(s_sel_n == 5);
		CHECK(s_sel_seen[0] == 42);
		CHECK(s_sel_seen[1] == 1);
		CHECK(s_sel_seen[2] == 2);
		CHECK(s_sel_seen[3] == 3);
		CHECK(s_sel_seen[4] == 4);
		printf("  ok   selective_receive: 42 first, then 1,2,3,4 in"
		    " order (IOCP wakeup path)\n");
	}

	printf("All MSVC smoke checks passed.\n");
	return 0;
}

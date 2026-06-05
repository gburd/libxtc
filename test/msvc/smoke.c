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

	printf("All MSVC smoke checks passed.\n");
	return 0;
}

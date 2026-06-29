/*-
 * veh_drv.c -- standalone runtime verification of the Windows fault
 * containment path (VEH + RtlCaptureContext) in libxtc, plus the new
 * recovery resource registry.  Builds with MinGW, links libxtc.a.
 *
 * A proc arms a recovery frame, registers an fd + a generic callback,
 * waits for a monitor, then dereferences a wild pointer -> an access
 * violation.  The Vectored Exception Handler must contain it, restore
 * the captured CONTEXT, run the auto-cleanup, and deliver DOWN to the
 * monitor -- the other proc on the loop never notices.  Exit 0 = the
 * fault was contained and all the post-conditions hold.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <io.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define FLT_REASON 42

static int g_saw_down, g_reason, g_cb_ran, g_cleanup_path, g_fd = -1;

static void rec_cb(void *arg) { (void)arg; g_cb_ran = 1; }

static void
faulter(void *arg)
{
	void *m = NULL; size_t n = 0;
	int pfd[2];
	(void)arg;

	/* Arm: on the recovered branch, release tracked resources + exit. */
	int sig = xtc_proc_recovery_arm();
	if (sig != 0) {
		g_cleanup_path = 1;
		xtc_proc_recovery_cleanup();      /* releases fd + callback */
		(void)xtc_exit_self(FLT_REASON);
		return;
	}

	if (_pipe(pfd, 256, 0) == 0) {
		g_fd = pfd[1];
		(void)xtc_proc_recovery_track_fd(pfd[1]);
		(void)_close(pfd[0]);
	}
	(void)xtc_proc_recovery_track(rec_cb, NULL);

	if (xtc_recv(&m, &n, 2LL * 1000 * 1000 * 1000) == XTC_OK && m)
		free(m);

	/* A genuine access violation (wild write). */
	{
		volatile uintptr_t addr = 0x10;
		*(volatile int *)addr = 1;        /* boom */
	}
}

static void
watcher(void *arg)
{
	void *msg = NULL; size_t sz = 0;
	xtc_pid_t target, down_pid;
	uint64_t ref;
	int go = 1, down_reason = 0;
	(void)arg;

	if (xtc_recv(&msg, &sz, 1000LL * 1000 * 1000) != XTC_OK) return;
	memcpy(&target, msg, sizeof target);
	free(msg);
	if (xtc_monitor(target, &ref) != XTC_OK) return;
	(void)xtc_send(target, &go, sizeof go);
	if (xtc_recv(&msg, &sz, 5LL * 1000 * 1000 * 1000) != XTC_OK) return;
	if (xtc_down_decode(msg, sz, &down_pid, &down_reason) == XTC_OK) {
		g_saw_down = 1;
		g_reason = down_reason;
	}
	free(msg);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t f, w;

	if (xtc_fault_guard_install() != XTC_OK) {
		printf("FAIL: fault_guard_install\n"); return 1;
	}
	if (xtc_loop_init(&loop) != XTC_OK) { printf("FAIL: loop_init\n"); return 1; }
	if (xtc_proc_spawn(loop, watcher, NULL, NULL, &w) != XTC_OK) {
		printf("FAIL: spawn watcher\n"); return 1;
	}
	if (xtc_proc_spawn(loop, faulter, NULL, NULL, &f) != XTC_OK) {
		printf("FAIL: spawn faulter\n"); return 1;
	}
	if (xtc_send(w, &f, sizeof f) != XTC_OK) { printf("FAIL: send\n"); return 1; }

	/* If the VEH did NOT contain the fault, the loop thread takes the
	 * access violation here and the process dies (nonzero exit). */
	if (xtc_loop_run(loop) != XTC_OK) { printf("FAIL: loop_run\n"); return 1; }
	xtc_loop_fini(loop);

	if (!g_cleanup_path) { printf("FAIL: recovery branch did not run\n"); return 1; }
	if (!g_cb_ran)       { printf("FAIL: tracked callback not released\n"); return 1; }
	if (g_fd >= 0 && _close(g_fd) != -1) {
		printf("FAIL: tracked fd was not closed (double close succeeded)\n"); return 1;
	}
	if (!g_saw_down)     { printf("FAIL: monitor did not observe DOWN\n"); return 1; }
	if (g_reason != FLT_REASON) { printf("FAIL: DOWN reason=%d\n", g_reason); return 1; }

	printf("OK: Windows VEH contained the access violation; recovery "
	       "registry released fd+callback; monitor saw DOWN(reason=%d)\n",
	       g_reason);
	return 0;
}

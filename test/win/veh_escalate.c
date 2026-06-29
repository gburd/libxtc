/*-
 * veh_escalate.c -- verify the Windows escalation path: a fault inside
 * a critical section must NOT be contained (shared state may be torn),
 * so it crashes the process.  Run by a parent that checks the child
 * died with a nonzero/exception exit code rather than exiting 0.
 *
 * This binary IS the child: it arms recovery, enters a critical
 * section, then faults.  Correct behavior = the VEH escalates
 * (EXCEPTION_CONTINUE_SEARCH) and the process is terminated by the
 * unhandled access violation.  If it ever prints "ESCAPED" or exits 0,
 * containment wrongly fired inside the crit section -- a bug.
 */
#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

static void
faulter(void *arg)
{
	(void)arg;
	if (xtc_proc_recovery_arm() != 0) {
		printf("ESCAPED: fault was contained inside a critical section\n");
		fflush(stdout);
		(void)xtc_exit_self(0);
		return;
	}
	xtc_proc_critical_enter();
	{
		volatile uintptr_t addr = 0x10;
		*(volatile int *)addr = 1;     /* fault in crit -> must escalate */
	}
	printf("ESCAPED: survived the fault\n");
	fflush(stdout);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t f;
	if (xtc_fault_guard_install() != XTC_OK) return 2;
	if (xtc_loop_init(&loop) != XTC_OK) return 2;
	if (xtc_proc_spawn(loop, faulter, NULL, NULL, &f) != XTC_OK) return 2;
	(void)xtc_loop_run(loop);   /* process should die before returning */
	xtc_loop_fini(loop);
	printf("ESCAPED: loop returned normally\n");
	return 0;   /* reaching here at all is wrong */
}

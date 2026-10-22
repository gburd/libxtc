#!/usr/sbin/dtrace -qs
/*
 * xtc_lost_wake.d -- find fibers that parked and never came back, and
 * say which half of the wake chain broke.
 *
 * Usage:  dtrace -qs xtc_lost_wake.d -c ./prog
 *         dtrace -qs xtc_lost_wake.d -p PID       (Ctrl-C to report)
 *
 * fiber-park opens a park for the fiber (arg0 = task) with its reason
 * (arg1) and the object it parked on (arg2); fiber-wake marks that a
 * waker claimed it; fiber-resume closes it.  At the end each fiber is one
 * row whose value is its state, largest first:
 *
 *   2  WOKEN, never resumed: a waker claimed it (fiber-wake `how` 0 local
 *      or 1 remote) but it never ran.  The loss is after the wake, in a
 *      run queue or loop inbox nobody drained.
 *   1  PARKED, never woken: nothing called its waker.  The wake source
 *      never fired (a lost notify, a completion that never came back, a
 *      timer never armed).  Start from the object.
 *   0  closed normally (D prints these too; they sort last).
 *
 * The same chain the xtc_tail ring records (PARK_TASK / WAKE / RUN), live,
 * on a process nobody prepared for tracing.  Reasons: 1 mbox 2 sleep 3 fd
 * 4 aio 5 blocking 6 sync 7 lwlock 8 lockmgr 9 future.
 */

/* Per-fiber state lives in global arrays keyed on the task; size them
 * for a busy server so no state is dropped (dtrace warns if it is). */
#pragma D option dynvarsize=16m

#pragma D option aggsortrev

xtc$target:::fiber-park
{
	st[arg0] = 1;
	why[arg0] = arg1;
	obj[arg0] = arg2;
	@state[arg0, arg1, arg2] = sum(1);
	stranded++;
}

xtc$target:::fiber-wake
/st[arg0] == 1 && arg2 != 2/
{
	st[arg0] = 2;
	@state[arg0, why[arg0], obj[arg0]] = sum(1);
}

xtc$target:::fiber-resume
/st[arg0] != 0/
{
	@state[arg0, why[arg0], obj[arg0]] = sum(-st[arg0]);
	st[arg0] = 0;
	stranded--;
}

END
{
	printf("%d fiber(s) parked and never resumed (state 1 or 2 below)\n",
	    stranded);
	printf("  %-18s %-6s %-18s %s\n", "TASK", "REASON", "OBJECT", "STATE");
	printa("  0x%-16x %-6d 0x%-16x %@d\n", @state);
}

#!/usr/sbin/dtrace -qs
/*
 * xtc_park_latency.d -- how long fibers stay parked, by park reason.
 *
 * Usage:  dtrace -qs xtc_park_latency.d -c ./prog
 *         dtrace -qs xtc_park_latency.d -p PID      (Ctrl-C to print)
 *
 * fiber-park / fiber-resume carry the fiber's task pointer in arg0, so
 * the pair is keyed on it, not on the thread: a migratable fiber can park
 * on one OS thread and resume on another.  arg1 is the reason:
 *   1 mailbox  2 sleep  3 fd  4 aio  5 blocking-pool  6 sync primitive
 *   7 lwlock   8 lock manager  9 future
 * A park with no resume when the program ends is a fiber still parked
 * (or stranded): the END clause counts them.
 */

/* Per-fiber state lives in global arrays keyed on the task; size them
 * for a busy server so no state is dropped (dtrace warns if it is). */
#pragma D option dynvarsize=16m

xtc$target:::fiber-park
{
	park_ts[arg0] = timestamp;
	park_why[arg0] = arg1;
	@parked[arg1] = count();
}

xtc$target:::fiber-resume
/park_ts[arg0]/
{
	@lat_ns[park_why[arg0]] = quantize(timestamp - park_ts[arg0]);
	@max_ns[park_why[arg0]] = max(timestamp - park_ts[arg0]);
	park_ts[arg0] = 0;
	park_why[arg0] = 0;
	@resumed[arg1] = count();
}

END
{
	printf("parks by reason (1 mbox 2 sleep 3 fd 4 aio 5 blocking 6 sync 7 lwlock 8 lockmgr 9 future)\n");
	printa("  reason %d: %@d parks\n", @parked);
	printa("  reason %d: %@d resumes\n", @resumed);
	printf("\npark -> resume latency (ns) by reason\n");
	printa(@lat_ns);
	printa("  reason %d: max %@d ns\n", @max_ns);
}

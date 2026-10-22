#!/usr/sbin/dtrace -qs
/*
 * xtc_lock_contention.d -- which libxtc locks make callers wait, how
 * long, and from where.
 *
 * Usage:  dtrace -qs xtc_lock_contention.d -c ./prog
 *
 * lock-wait-start / lock-wait-done bracket every sleep or park on a
 * contended amutex, arwlock, lwlock or lock-manager object; the
 * uncontended fast paths are not probed.  arg0 is the lock (for the
 * lock manager, the lock object), arg1 the kind (1 amutex, 2 arwlock,
 * 3 lwlock, 4 lockmgr), arg2 the mode, arg3 the waiting fiber's task
 * (0 for an OS-thread waiter).  A waiter is keyed on (task, lock) when it
 * is a fiber and on the thread otherwise, since a fiber can resume on a
 * different thread.  The ustack() aggregation is a flame-graph input:
 *   dtrace -x ustackframes=32 -qs xtc_lock_contention.d -c ./prog > out
 *   stackcollapse.pl out | flamegraph.pl > contention.svg
 */

/* Per-fiber state lives in global arrays keyed on the task; size them
 * for a busy server so no state is dropped (dtrace warns if it is). */
#pragma D option dynvarsize=16m

xtc$target:::lock-wait-start
/arg3 != 0/
{
	fw[arg3, arg0] = timestamp;
	@waits[arg1, arg0] = count();
	@stacks[ustack()] = count();
}

xtc$target:::lock-wait-start
/arg3 == 0/
{
	self->tw[arg0] = timestamp;
	@waits[arg1, arg0] = count();
	@stacks[ustack()] = count();
}

xtc$target:::lock-wait-done
/arg3 != 0 && fw[arg3, arg0]/
{
	@wait_ns[arg1, arg0] = sum(timestamp - fw[arg3, arg0]);
	@dist[arg1] = quantize(timestamp - fw[arg3, arg0]);
	fw[arg3, arg0] = 0;
}

xtc$target:::lock-wait-done
/arg3 == 0 && self->tw[arg0]/
{
	@wait_ns[arg1, arg0] = sum(timestamp - self->tw[arg0]);
	@dist[arg1] = quantize(timestamp - self->tw[arg0]);
	self->tw[arg0] = 0;
}

END
{
	printf("contended waits by lock (kind: 1 amutex 2 arwlock 3 lwlock 4 lockmgr)\n");
	printf("  %-5s %-18s %10s %14s\n", "KIND", "LOCK", "WAITS", "WAITED_NS");
	printa("  %-5d 0x%-16x %@10d %@14d\n", @waits, @wait_ns);
	printf("\nwait time (ns) by kind\n");
	printa(@dist);
	printf("\nwhere the waits come from (most frequent last)\n");
	trunc(@stacks, 10);
	printa(@stacks);
}

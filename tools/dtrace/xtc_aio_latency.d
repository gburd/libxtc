#!/usr/sbin/dtrace -qs
/*
 * xtc_aio_latency.d -- native file AIO latency per operation.
 *
 * Usage:  dtrace -qs xtc_aio_latency.d -c ./prog
 *
 * aio-submit fires when an op reaches the backend (io_uring, kqueue
 * POSIX AIO, IOCP, event ports, the simulator); aio-done when the
 * submitting fiber sees the result.  arg0 is the request (stable for the
 * op's life), arg1 the op: 0 pread 1 pwrite 2 fsync 3 fdatasync
 * 4 preadv 5 pwritev.  An op that fell back to the blocking pool has no
 * aio-submit; it shows up in blocking-enqueue / -start / -done instead
 * (see the second half of the output).
 */

/* Per-fiber state lives in global arrays keyed on the task; size them
 * for a busy server so no state is dropped (dtrace warns if it is). */
#pragma D option dynvarsize=16m

xtc$target:::aio-submit
{
	aio_ts[arg0] = timestamp;
	@bytes[arg1] = sum(arg3);
}

xtc$target:::aio-done
/aio_ts[arg0]/
{
	@lat[arg1] = quantize(timestamp - aio_ts[arg0]);
	@ops[arg1] = count();
	@errs[arg1] = sum(arg2 < 0 ? 1 : 0);
	aio_ts[arg0] = 0;
}

xtc$target:::blocking-enqueue { blk_q[arg0] = timestamp; @pool["enqueued"] = count(); }
xtc$target:::blocking-start
/blk_q[arg0]/
{
	@queue_wait = quantize(timestamp - blk_q[arg0]);
	blk_q[arg0] = 0;
	blk_run[arg0] = timestamp;
}
xtc$target:::blocking-done
/blk_run[arg0]/
{
	@run_time = quantize(timestamp - blk_run[arg0]);
	blk_run[arg0] = 0;
	@pool["completed"] = count();
}

END
{
	printf("native aio by op (0 pread 1 pwrite 2 fsync 3 fdatasync 4 preadv 5 pwritev)\n");
	printa("  op %d: %@d ops, %@d errors, %@d bytes\n", @ops, @errs, @bytes);
	printa(@lat);
	printf("\nblocking pool\n");
	printa("  %-10s %@d\n", @pool);
	printf("queue wait (ns)\n");
	printa(@queue_wait);
	printf("run time (ns)\n");
	printa(@run_time);
}

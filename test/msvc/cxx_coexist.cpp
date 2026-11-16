/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/msvc/cxx_coexist.cpp
 *	A C++ program that includes every installed libxtc header and calls
 *	C functions declared in three of them.  libxtc has no C++ API; this
 *	is the whole of what it promises C++: the headers compile in a C++
 *	translation unit and the functions link with C linkage.
 *
 *	Built and run by test/dist/test_header_cxx.sh (g++, clang++) and by
 *	dist/build_msvc.bat (cl /TP), which also compiles each header named
 *	below alone.  The include list must be exactly LIB_HDRS_PUBLIC in
 *	dist/Makefile.in; test_header_cxx.sh fails when it is not.
 */
#include <xtc.h>
#include <xtc_export.h>
#include <xtc_atomic.h>
#include <xtc_compiler.h>
#include <xtc_runtime.h>
#include <xtc_aio.h>
#include <xtc_alloc_audit.h>
#include <xtc_app.h>
#include <xtc_async.h>
#include <xtc_bdev.h>
#include <xtc_blocking.h>
#include <xtc_accel.h>
#include <xtc_batch.h>
#include <xtc_cfg.h>
#include <xtc_chan.h>
#include <xtc_dio_sched.h>
#include <xtc_dump.h>
#include <xtc_exec.h>
#include <xtc_fs.h>
#include <xtc_inject.h>
#include <xtc_inspect.h>
#include <xtc_io.h>
#include <xtc_iosched.h>
#include <xtc_lockmgr.h>
#include <xtc_log.h>
#include <xtc_loop.h>
#include <xtc_sched.h>
#include <xtc_sim.h>
#include <xtc_lrlock.h>
#include <xtc_lwlock.h>
#include <xtc_mctx.h>
#include <xtc_net.h>
#include <xtc_orc.h>
#include <xtc_osproc.h>
#include <xtc_pdict.h>
#include <xtc_preempt.h>
#include <xtc_preempt_involuntary.h>
#include <xtc_pkey.h>
#include <xtc_proc.h>
#include <xtc_rcu.h>
#include <xtc_chash.h>
#include <xtc_cskip.h>
#include <xtc_prob.h>
#include <xtc_reg.h>
#include <xtc_res.h>
#include <xtc_slab.h>
#include <xtc_stats.h>
#include <xtc_tail.h>
#include <xtc_svr.h>
#include <xtc_fsm.h>
#include <xtc_saga.h>
#include <xtc_pg.h>
#include <xtc_pool.h>
#include <xtc_stream.h>
#include <xtc_credit.h>
#include <xtc_xproc.h>
#include <xtc_launch.h>
#include <xtc_sync.h>
#include <xtc_future.h>
#include <xtc_dispatch.h>
#include <xtc_tailcall.h>
#include <xtc_tls.h>
#include <xtc_tnt.h>
#include <xtc_trace.h>

#include <stdio.h>
#include <string.h>

/*
 * Each failed step returns its own code, so a nonzero exit names the
 * call that failed.  The options are filled in by hand: the
 * XTC_*_DEFAULT initializers use designated initializers, which C++
 * has only from C++20.
 */
int
main()
{
	xtc_log_opts_t lo;
	xtc_log_t *log = NULL;
	xtc_loop_t *loop = NULL;

	if (xtc_version_string() == NULL ||
	    strcmp(xtc_strerror(XTC_OK), "ok") != 0)
		return 1;
	memset(&lo, 0, sizeof lo);
	lo.size = sizeof lo;
	lo.ring_size = 16;
	lo.record_max = 64;
	lo.floor = XTC_LOG_INFO;
	lo.sink_fd = -1;
	if (xtc_log_create(&lo, &log) != XTC_OK)
		return 2;
	xtc_log_write(log, XTC_LOG_INFO, "from C++ %d", 1);
	if (xtc_log_drain(log) != 1)
		return 3;
	xtc_log_destroy(log);
	if (xtc_loop_init(&loop) != XTC_OK)
		return 4;
	if (xtc_loop_fini(loop) != XTC_OK)
		return 5;
	printf("libxtc %s: C++ coexistence OK\n", xtc_version_string());
	return 0;
}

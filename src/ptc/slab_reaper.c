/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/slab_reaper.c
 *	The optional slab-reaper proc: a background xtc_proc that
 *	periodically calls xtc_slab_reap_all().  This is the ONE piece of
 *	the slab subsystem that depends on the L3 process layer
 *	(xtc_proc_spawn / xtc_recv), so it lives here, apart from the
 *	slab core (src/ptc/slab.c), which depends only on the __os_*
 *	primitives and is therefore layer-neutral -- usable by the L2
 *	event loop (the per-loop timer slab) without creating an upward
 *	L2->L3 dependency.  Keeping the reaper separate is what lets the
 *	allocator be a low-level primitive while its convenience wrapper
 *	stays at L3.
 */

#include "xtc_int.h"
#include "xtc_slab.h"
#include "xtc_proc.h"

struct reaper_ctx {
	int64_t interval_ns;
};

static void
__reaper_main(void *arg)
{
	struct reaper_ctx *ctx = arg;
	void *m;
	size_t sz;
	for (;;) {
		/* Block on recv with the interval as timeout; on any
		 * incoming message we reap and continue.  Caller can
		 * stop the reaper via xtc_exit_pid. */
		int rc = xtc_recv(&m, &sz, ctx->interval_ns);
		(void)xtc_slab_reap_all();
		if (rc == XTC_OK && m != NULL) __os_free(m);
	}
}

int
xtc_slab_reaper_spawn(xtc_loop_t *loop, int64_t interval_ns,
                      xtc_pid_t *out_pid)
{
	struct reaper_ctx *ctx;
	int rc;
	xtc_pid_t pid;
	if (loop == NULL || interval_ns <= 0) return XTC_E_INVAL;
	rc = __os_calloc(1, sizeof *ctx, (void **)&ctx);
	if (rc != XTC_OK) return rc;
	ctx->interval_ns = interval_ns;
	rc = xtc_proc_spawn(loop, __reaper_main, ctx, NULL, &pid);
	if (rc != XTC_OK) { __os_free(ctx); return rc; }
	if (out_pid) *out_pid = pid;
	return XTC_OK;
}

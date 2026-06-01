/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/admin.c
 *	Server-side admin commands.  "SHOW PROCESSES" walks the live
 *	xtc proc set via the xtc_inspect_procs introspection API and
 *	renders one Quack result row per proc -- the programmatic form
 *	of the debugger's xtc-procs.  The result-set emission mirrors
 *	db_exec_params in db.c exactly: a cols header, one row per
 *	proc, then a terminating done status.
 */

#include "admin.h"

#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_inspect.h"
#include "xtc_proc.h"

/* Threaded through xtc_inspect_procs as the callback user pointer. */
typedef struct admin_procs_ctx {
	quack_buf_t *out;
	int64_t      count;     /* rows emitted so far */
	int          err;       /* set on the first encode failure */
} admin_procs_ctx_t;

/* Map a run state (with park reason for PARKED) to a stable label. */
static const char *
admin_state_label(const xtc_proc_info_t *info, char *buf, size_t cap)
{
	switch (info->run_state) {
	case XTC_PROC_SCHEDULED:
		return "scheduled";
	case XTC_PROC_RUNNING:
		return "running";
	case XTC_PROC_DONE:
		return "done";
	case XTC_PROC_PARKED:
		break;
	default:
		return "unknown";
	}

	switch (info->park_reason) {
	case XTC_PARK_FD:
		snprintf(buf, cap, "parked:fd");
		break;
	case XTC_PARK_TIMER:
		snprintf(buf, cap, "parked:timer");
		break;
	case XTC_PARK_MAILBOX:
		snprintf(buf, cap, "parked:mailbox");
		break;
	case XTC_PARK_NONE:
	default:
		snprintf(buf, cap, "parked");
		break;
	}
	return buf;
}

/* One live proc -> one Quack data row.  Returns 0 to keep walking,
 * nonzero to stop early (only on an encode failure). */
static int
admin_proc_row(const xtc_proc_info_t *info, void *user)
{
	admin_procs_ctx_t *ctx = (admin_procs_ctx_t *)user;
	quack_buf_t       *out = ctx->out;
	char               pidbuf[48];
	char               statebuf[32];
	const char        *state;
	int                n;

	n = snprintf(pidbuf, sizeof pidbuf, "%u.%u.%u",
	    (unsigned)info->pid.loop_id,
	    (unsigned)info->pid.local_id,
	    (unsigned)info->pid.gen);
	if (n < 0)
		n = 0;

	state = admin_state_label(info, statebuf, sizeof statebuf);

	if (quack_emit_row_begin(out) < 0) goto fail;
	if (quack_emit_row_text(out, 0, pidbuf, strlen(pidbuf)) < 0) goto fail;
	if (quack_emit_row_text(out, 1, state, strlen(state)) < 0) goto fail;
	if (quack_emit_row_int(out, 2, (int64_t)info->mbox_len) < 0) goto fail;
	if (quack_emit_row_int(out, 3, (int64_t)info->mbox_peak) < 0) goto fail;
	if (quack_emit_row_int(out, 4, info->alive ? 1 : 0) < 0) goto fail;
	if (quack_emit_row_end(out) < 0) goto fail;

	ctx->count++;
	return 0;

fail:
	ctx->err = 1;
	return 1;
}

int
admin_show_processes(quack_buf_t *out)
{
	admin_procs_ctx_t ctx;

	ctx.out = out;
	ctx.count = 0;
	ctx.err = 0;

	/* Column header -- emitted unconditionally so the shape is the
	 * same whether or not any procs are live. */
	if (quack_emit_cols_begin(out) < 0) return -1;
	if (quack_emit_cols_name(out, 0, "pid") < 0) return -1;
	if (quack_emit_cols_name(out, 1, "state") < 0) return -1;
	if (quack_emit_cols_name(out, 2, "mbox") < 0) return -1;
	if (quack_emit_cols_name(out, 3, "peak") < 0) return -1;
	if (quack_emit_cols_name(out, 4, "alive") < 0) return -1;
	if (quack_emit_cols_end(out) < 0) return -1;

	(void)xtc_inspect_procs(admin_proc_row, &ctx);
	if (ctx.err) return -1;

	if (quack_emit_done(out, ctx.count) < 0) return -1;
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/xtc_deprecated.c
 *	The 2.x deprecation bridges: every public function renamed in 2.2
 *	keeps its old name here, as an exported out-of-line wrapper that
 *	calls the new one.  A binary built against an older 2.x keeps
 *	linking against this library; a rebuild warns (each declaration is
 *	XTC_DEPRECATED in its header's "Deprecated names" block).
 *
 *	Each wrapper keeps the OLD contract exactly: its arguments in the
 *	old order and its old return values (an old int-returning *_fini
 *	still returns XTC_E_INVAL for NULL and XTC_OK otherwise, though the
 *	new *_destroy is void).  docs/deprecations.md lists every name.
 *	3.0 deletes this file.
 *
 *	The old symbols stay in the XTC_2.0 node of dist/libxtc.map (its
 *	xtc_* glob); their new names are listed in XTC_2.2.
 */
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_io.h"
#include "xtc_prob.h"
#include "xtc_exec.h"
#include "xtc_pg.h"
#include "xtc_chan.h"
#include "xtc_sync.h"
#include "xtc_batch.h"
#include "xtc_proc.h"
#include "xtc_slab.h"
#include "xtc_dispatch.h"
#include "xtc_tnt.h"
#include "xtc_xproc.h"
#include "xtc_runtime.h"
#include "xtc_reg.h"
#include "xtc_async.h"
#include "xtc_cfg.h"

/* Group: loop / io / bloom / hll lifecycle (create / destroy). */

int
xtc_loop_init(xtc_loop_t **out)
{
	return xtc_loop_create(out);
}

int
xtc_loop_fini(xtc_loop_t *loop)
{
	if (loop == NULL)
		return XTC_E_INVAL;
	xtc_loop_destroy(loop);
	return XTC_OK;
}

int
xtc_io_init(xtc_io_t **out)
{
	return xtc_io_create(out);
}

int
xtc_io_fini(xtc_io_t *io)
{
	if (io == NULL)
		return XTC_E_INVAL;
	xtc_io_destroy(io);
	return XTC_OK;
}

int
xtc_bloom_init(xtc_bloom_t **out, size_t n_expected, double fp_rate)
{
	return xtc_bloom_create(n_expected, fp_rate, out);
}

void
xtc_bloom_fini(xtc_bloom_t *b)
{
	xtc_bloom_destroy(b);
}

int
xtc_hll_init(xtc_hll_t **out, int precision)
{
	return xtc_hll_create(precision, out);
}

void
xtc_hll_fini(xtc_hll_t *h)
{
	xtc_hll_destroy(h);
}

/* Group: exec (create takes an opts struct; destroy is void). */

int
xtc_exec_init(xtc_exec_t **out, int n_loops)
{
	xtc_exec_opts_t o = XTC_EXEC_OPTS_INIT;

	o.n_loops = n_loops;
	return xtc_exec_create(&o, out);
}

int
xtc_exec_fini(xtc_exec_t *exec)
{
	if (exec == NULL)
		return XTC_E_INVAL;
	xtc_exec_destroy(exec);
	return XTC_OK;
}

/* Group: pg (join / leave -> subscribe / unsubscribe). */

int
xtc_pg_join(xtc_reg_t *reg, const char *group, xtc_pid_t pid)
{
	return xtc_pg_subscribe(reg, group, pid);
}

int
xtc_pg_leave(xtc_reg_t *reg, const char *group, xtc_pid_t pid)
{
	int rc = xtc_pg_unsubscribe(reg, group, pid);

	/* Before 2.2 a non-member was XTC_E_INVAL. */
	return rc == XTC_E_NOTFOUND ? XTC_E_INVAL : rc;
}

/* Group: close -> shutdown (stop accepting work; queued work drains). */

int
xtc_chan_mpsc_close(xtc_chan_mpsc_t *c)
{
	return xtc_chan_mpsc_shutdown(c);
}

int
xtc_chan_mpmc_close(xtc_chan_mpmc_t *c)
{
	return xtc_chan_mpmc_shutdown(c);
}

int
xtc_chan_demand_close(xtc_chan_demand_t *c)
{
	return xtc_chan_demand_shutdown(c);
}

int
xtc_gate_close(xtc_gate_t *g)
{
	return xtc_gate_shutdown(g);
}

int
xtc_batch_close(xtc_batch_t *b, enum xtc_batch_close_mode mode)
{
	return xtc_batch_shutdown(b, mode);
}

/* Group: scope (a region: enter / leave). */

xtc_scope_t *
xtc_scope_open(void)
{
	return xtc_scope_enter();
}

void
xtc_scope_close(xtc_scope_t *s)
{
	xtc_scope_leave(s);
}

/* Group: slab pressure listener (create / destroy). */

int
xtc_slab_pressure_listen(const char *psi_path, xtc_slab_pressure_fn fn,
    void *user)
{
	xtc_slab_pressure_t *h = NULL;

	/* Fire-and-forget: the handle is dropped and the listener runs until
	 * the process exits, as it always did. */
	return xtc_slab_pressure_create(psi_path, fn, user, &h);
}

int
xtc_slab_pressure_listen_ex(const char *psi_path, xtc_slab_pressure_fn fn,
    void *user, xtc_slab_pressure_t **out)
{
	return xtc_slab_pressure_create(psi_path, fn, user, out);
}

int
xtc_slab_pressure_stop(xtc_slab_pressure_t *handle)
{
	if (handle == NULL)
		return XTC_E_INVAL;
#if !defined(__linux__)
	return XTC_E_NOSYS;
#else
	xtc_slab_pressure_destroy(handle);
	return XTC_OK;
#endif
}

/* Group: detach (give up a handle; the thing continues). */

void
xtc_dispatch_handle_free(xtc_dispatch_handle_t *h)
{
	xtc_dispatch_detach(h);
}

int
xtc_timer_release(xtc_timer_t *timer)
{
	return xtc_timer_detach(timer);
}

/* Group: tnt (the call that drives the system until it stops is run). */

int
xtc_tnt_start(const xtc_tnt_spec_t *spec)
{
	return xtc_tnt_run(spec);
}

/* Group: xproc (the handle's methods take the xtc_xproc_ prefix). */

int
xtc_xspawn(xtc_loop_t *loop, const char *name,
    xtc_xproc_root_fn root_fn, const void *arg, size_t arg_len,
    xtc_xproc_t **out)
{
	return xtc_xproc_spawn(loop, name, root_fn, arg, arg_len, out);
}

int
xtc_xspawn_entry(xtc_loop_t *loop, const char *name,
    const char *entry, const void *arg, size_t arg_len, xtc_xproc_t **out)
{
	return xtc_xproc_spawn_entry(loop, name, entry, arg, arg_len, out);
}

int
xtc_xsend(xtc_xproc_t *p, const void *msg, size_t len)
{
	return xtc_xproc_send(p, msg, len);
}

int
xtc_xmonitor(xtc_xproc_t *p, uint64_t *out_ref)
{
	return xtc_xproc_monitor(p, out_ref);
}

int
xtc_xlink(xtc_xproc_t *p)
{
	return xtc_xproc_link(p);
}

/* Group: process-global knobs (xtc_runtime_set_ / _get_). */

int
xtc_set_stack_size(size_t bytes)
{
	return xtc_runtime_set_stack_size(bytes);
}

size_t
xtc_stack_size(void)
{
	return xtc_runtime_get_stack_size();
}

int
xtc_stack_reclaim_enable(size_t keep_bytes)
{
	/* 0 meant "the default margin, one page"; under the new name 0 is
	 * off, so the bridge passes the page explicitly. */
#if defined(_WIN32)
	(void)keep_bytes;
	return XTC_E_NOSYS; /* no reclaim on the Win32 fiber substrate */
#else
	long pg;

	if (keep_bytes == 0) {
		pg = sysconf(_SC_PAGESIZE);
		keep_bytes = pg > 0 ? (size_t)pg : 4096u;
	}
	return xtc_runtime_set_stack_reclaim(keep_bytes);
#endif
}

void
xtc_stack_reclaim_disable(void)
{
	(void)xtc_runtime_set_stack_reclaim(0);
}

int
xtc_stack_reclaim_enabled(void)
{
	return xtc_runtime_get_stack_reclaim() != 0;
}

int
xtc_blocking_pool_size(int nthreads)
{
	int rc = xtc_runtime_set_blocking_threads(nthreads);

	/* Before 2.2 "too late" was XTC_E_INVAL; the old name keeps it. */
	return rc == XTC_E_STATE ? XTC_E_INVAL : rc;
}

int
xtc_preempt_set_involuntary(int on)
{
	return xtc_runtime_set_preempt_involuntary(on);
}

void
xtc_io_set_iowq_max_workers(unsigned bound, unsigned unbound)
{
	xtc_runtime_set_iowq_workers(bound, unbound);
}

int
xtc_alloc_audit_enable(int on)
{
	return xtc_runtime_set_alloc_audit(on);
}

/* Group: registry (put / get / del).  The old calls reported a taken name
 * and a miss as XTC_E_INVAL; they still do. */

static int
reg_old_rc(int rc)
{
	return (rc == XTC_E_EXIST || rc == XTC_E_NOTFOUND) ? XTC_E_INVAL : rc;
}

int
xtc_reg_register(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	return reg_old_rc(xtc_reg_put(r, name, pid, NULL, 0));
}

int
xtc_reg_register_dup(xtc_reg_t *r, const char *key, xtc_pid_t pid)
{
	return reg_old_rc(xtc_reg_put(r, key, pid, NULL, XTC_REG_DUP));
}

int
xtc_reg_register_mon(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	return reg_old_rc(xtc_reg_put(r, name, pid, NULL, XTC_REG_MONITOR));
}

int
xtc_reg_unregister(xtc_reg_t *r, const char *name)
{
	return reg_old_rc(xtc_reg_del(r, name, XTC_PID_NONE));
}

int
xtc_reg_unregister_pid(xtc_reg_t *r, const char *key, xtc_pid_t pid)
{
	return reg_old_rc(xtc_reg_del(r, key, pid));
}

int
xtc_reg_whereis(xtc_reg_t *r, const char *name, xtc_pid_t *out_pid)
{
	return reg_old_rc(xtc_reg_get(r, name, out_pid));
}

/* Group (tier B): stackful coroutines (async / await -> coro spawn / join). */

int
xtc_async(xtc_loop_t *loop, xtc_coro_fn fn, void *arg,
    xtc_task_t **out_task)
{
	return xtc_coro_spawn(loop, fn, arg, out_task);
}

int
xtc_await(xtc_task_t *t, intptr_t *result)
{
	return xtc_coro_join(t, result);
}

/* Group (tier B): cfg session setters (session, never ssn). */

int
xtc_cfg_ssn_set_bool(xtc_cfg_session_t *s, const char *name, int v,
    xtc_cfg_source_t src)
{
	return xtc_cfg_session_set_bool(s, name, v, src);
}

int
xtc_cfg_ssn_set_int(xtc_cfg_session_t *s, const char *name, int v,
    xtc_cfg_source_t src)
{
	return xtc_cfg_session_set_int(s, name, v, src);
}

int
xtc_cfg_ssn_set_int64(xtc_cfg_session_t *s, const char *name,
    int64_t v, xtc_cfg_source_t src)
{
	return xtc_cfg_session_set_int64(s, name, v, src);
}

int
xtc_cfg_ssn_set_double(xtc_cfg_session_t *s, const char *name,
    double v, xtc_cfg_source_t src)
{
	return xtc_cfg_session_set_double(s, name, v, src);
}

int
xtc_cfg_ssn_set_string(xtc_cfg_session_t *s, const char *name,
    const char *v, xtc_cfg_source_t src)
{
	return xtc_cfg_session_set_string(s, name, v, src);
}

int
xtc_cfg_ssn_set_enum(xtc_cfg_session_t *s, const char *name, int v,
    xtc_cfg_source_t src)
{
	return xtc_cfg_session_set_enum(s, name, v, src);
}

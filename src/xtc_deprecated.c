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
	return xtc_pg_unsubscribe(reg, group, pid);
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

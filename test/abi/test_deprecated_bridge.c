/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
/*
 * test/abi/test_deprecated_bridge.c -- every deprecated 2.x name still
 * works.
 *
 * A name renamed in 2.2 keeps its old spelling until 3.0
 * (docs/deprecations.md): an exported wrapper for a function, an alias
 * for a type, an enumerator or a macro.  This program is the one place in
 * the tree that uses the old spellings: each bridge_* function below
 * calls the old names of one rename group and checks that they do what
 * the new names do.
 *
 * The build compiles it with XTC_NO_DEPRECATION_WARNINGS.
 * test/dist/test_deprecated.sh compiles it again without that define and
 * checks that every old name in the docs/deprecations.md table warns
 * (and that a frozen alias does not).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_alloc_audit.h"
#include "xtc_app.h"
#include "xtc_async.h"
#include "xtc_batch.h"
#include "xtc_cfg.h"
#include "xtc_blocking.h"
#include "xtc_chan.h"
#include "xtc_dispatch.h"
#include "xtc_exec.h"
#include "xtc_future.h"
#include "xtc_inspect.h"
#include "xtc_io.h"
#include "xtc_lockmgr.h"
#include "xtc_log.h"
#include "xtc_loop.h"
#include "xtc_lwlock.h"
#include "xtc_net.h"
#include "xtc_orc.h"
#include "xtc_pg.h"
#include "xtc_preempt_involuntary.h"
#include "xtc_prob.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_res.h"
#include "xtc_runtime.h"
#include "xtc_slab.h"
#include "xtc_stats.h"
#include "xtc_sync.h"
#include "xtc_tls.h"
#include "xtc_tnt.h"
#include "xtc_xproc.h"

#define CHECK(c)                                                               \
	do {                                                                   \
		if (!(c)) {                                                    \
			fprintf(stderr, "  [bridge] FAIL %s:%d: %s\n",         \
			    __FILE__, __LINE__, #c);                           \
			exit(1);                                               \
		}                                                              \
	} while (0)

/* loop / io / bloom / hll lifecycle: *_init / *_fini -> *_create /
 * *_destroy.  The old *_fini keep their int result. */
static void
bridge_lifecycle(void)
{
	xtc_loop_t *loop = NULL;
	xtc_io_t *io = NULL;
	xtc_bloom_t *b = NULL;
	xtc_hll_t *h = NULL;

	CHECK(xtc_loop_init(&loop) == XTC_OK && loop != NULL);
	CHECK(xtc_loop_fini(loop) == XTC_OK);
	CHECK(xtc_loop_fini(NULL) == XTC_E_INVAL);
	CHECK(xtc_io_init(&io) == XTC_OK && io != NULL);
	CHECK(xtc_io_fini(io) == XTC_OK);
	CHECK(xtc_io_fini(NULL) == XTC_E_INVAL);
	CHECK(xtc_bloom_init(&b, 100, 0.01) == XTC_OK && b != NULL);
	xtc_bloom_add(b, "k", 1);
	CHECK(xtc_bloom_maybe_contains(b, "k", 1) == 1);
	CHECK(xtc_bloom_init(&b, 100, 1.0) == XTC_E_INVAL);
	xtc_bloom_fini(b);
	CHECK(xtc_hll_init(&h, 4) == XTC_OK && h != NULL);
	CHECK(xtc_hll_init(&h, 3) == XTC_E_INVAL);
	xtc_hll_fini(h);
}

/* exec: xtc_exec_init(out, n) / xtc_exec_fini -> xtc_exec_create(opts,
 * out) / xtc_exec_destroy. */
static void
bridge_exec(void)
{
	xtc_exec_t *e = NULL;

	CHECK(xtc_exec_init(&e, 3) == XTC_OK && e != NULL);
	CHECK(xtc_exec_n_loops(e) == 3);
	CHECK(xtc_exec_fini(e) == XTC_OK);
	CHECK(xtc_exec_init(NULL, 1) == XTC_E_INVAL);
	CHECK(xtc_exec_fini(NULL) == XTC_E_INVAL);
}

/* pg: join / leave -> subscribe / unsubscribe. */
static void
bridge_pg(void)
{
	xtc_reg_t *r = NULL;
	xtc_pid_t p = { 1, 1, 1 };

	CHECK(xtc_reg_create(&r) == XTC_OK && r != NULL);
	CHECK(xtc_pg_join(r, "g", p) == XTC_OK);
	CHECK(xtc_reg_count(r) == 1);
	CHECK(xtc_pg_leave(r, "g", p) == XTC_OK);
	CHECK(xtc_reg_count(r) == 0);
	CHECK(xtc_pg_leave(r, "g", p) == XTC_E_INVAL);
	xtc_reg_destroy(r);
}

/* close -> shutdown: a shut-down channel refuses a send; a shut-down
 * gate refuses an enter. */
static void
bridge_shutdown(void)
{
	xtc_chan_mpsc_t *a = NULL;
	xtc_chan_mpmc_t *b = NULL;
	xtc_chan_demand_t *c = NULL;
	xtc_gate_t *g = NULL;
	int x = 1, rc;

	CHECK(xtc_chan_mpsc_create(NULL, 4, &a) == XTC_OK);
	CHECK(xtc_chan_mpsc_close(a) == XTC_OK);
	CHECK(xtc_chan_mpsc_try_send(a, &x) != XTC_OK);
	xtc_chan_mpsc_destroy(a);
	CHECK(xtc_chan_mpmc_create(NULL, 4, &b) == XTC_OK);
	CHECK(xtc_chan_mpmc_close(b) == XTC_OK);
	CHECK(xtc_chan_mpmc_try_send(b, &x) != XTC_OK);
	xtc_chan_mpmc_destroy(b);
	CHECK(xtc_chan_demand_create(NULL, 4, &c) == XTC_OK);
	CHECK(xtc_chan_demand_close(c) == XTC_OK);
	CHECK(xtc_chan_demand_send(c, &x) != XTC_OK);
	xtc_chan_demand_destroy(c);
	CHECK(xtc_gate_create(&g) == XTC_OK);
	CHECK(xtc_gate_close(g) == XTC_OK);
	CHECK(xtc_gate_enter(g) != XTC_OK);
	xtc_gate_destroy(g);
	rc = xtc_batch_close(NULL, XTC_BATCH_DRAIN);
	CHECK(rc == XTC_E_INVAL || rc == XTC_E_NOSYS);
}

/* scope: open / close -> enter / leave.  The finalizer runs on the old
 * close.  Off a proc there is no scope, so this runs inside one. */
static int g_scope_fin;

static void
scope_fin(void *arg)
{
	(void)arg;
	g_scope_fin++;
}

static void
scope_proc(void *arg)
{
	xtc_scope_t *s;

	(void)arg;
	s = xtc_scope_open();
	CHECK(s != NULL);
	CHECK(xtc_scope_defer(s, scope_fin, NULL) == XTC_OK);
	xtc_scope_close(s);
}

static void
bridge_scope(void)
{
	xtc_loop_t *loop = NULL;

	CHECK(xtc_scope_open() == NULL);
	CHECK(xtc_loop_create(&loop) == XTC_OK);
	CHECK(xtc_proc_spawn(loop, scope_proc, NULL, NULL, NULL) == XTC_OK);
	CHECK(xtc_loop_run(loop) == XTC_OK);
	xtc_loop_destroy(loop);
	CHECK(g_scope_fin == 1);
}

/* slab pressure: listen / listen_ex / stop -> create / destroy.  Where
 * PSI is missing every form fails the same way, and stop keeps its
 * NULL contract. */
static void
bridge_slab_pressure(void)
{
	xtc_slab_pressure_t *h = NULL;
	int rc;

	CHECK(xtc_slab_pressure_stop(NULL) == XTC_E_INVAL);
	CHECK(xtc_slab_pressure_listen_ex(NULL, NULL, NULL, NULL) ==
	    XTC_E_INVAL);
	rc = xtc_slab_pressure_listen("/nonexistent/xtc/psi", NULL, NULL);
	CHECK(rc == XTC_E_NOSYS);
	rc = xtc_slab_pressure_listen_ex(NULL, NULL, NULL, &h);
	if (rc == XTC_OK) {
		CHECK(h != NULL);
		CHECK(xtc_slab_pressure_stop(h) == XTC_OK);
	} else {
		CHECK(h == NULL);
	}
}

/* detach: dispatch_handle_free / timer_release -> *_detach.  A detached
 * timer still fires. */
static int g_timer_fired;

static void
timer_fn(void *arg)
{
	(void)arg;
	g_timer_fired++;
}

static void
bridge_detach(void)
{
	xtc_loop_t *loop = NULL;
	xtc_timer_t *t = NULL;

	xtc_dispatch_handle_free(NULL);
	CHECK(xtc_timer_release(NULL) == XTC_E_INVAL);
	CHECK(xtc_loop_create(&loop) == XTC_OK);
	CHECK(xtc_timer_set(loop, 1000, timer_fn, NULL, &t) == XTC_OK);
	CHECK(xtc_timer_release(t) == XTC_OK);
	CHECK(xtc_loop_run(loop) == XTC_OK);
	CHECK(g_timer_fired == 1);
	xtc_loop_destroy(loop);
}

/* tnt: start (blocks) -> run.  A spec of size 0 is refused before boot
 * (XTC_E_NOSYS where tnt is not built). */
static void
bridge_tnt(void)
{
	xtc_tnt_spec_t spec;
	int rc;

	memset(&spec, 0, sizeof spec);
	rc = xtc_tnt_start(&spec);
	CHECK(rc == XTC_E_INVAL || rc == XTC_E_NOSYS);
}

/* xproc: xspawn / xspawn_entry / xsend / xmonitor / xlink ->
 * xtc_xproc_*.  Each refuses a NULL handle the same way (a real spawn
 * forks; test_xproc covers that through the new names). */
static void
bridge_xproc(void)
{
	xtc_xproc_t *x = NULL;
	int rc;

	rc = xtc_xspawn(NULL, "b", NULL, NULL, 0, &x);
	CHECK(rc == XTC_E_INVAL || rc == XTC_E_NOSYS);
	rc = xtc_xspawn_entry(NULL, "b", "none", NULL, 0, &x);
	CHECK(rc == XTC_E_INVAL || rc == XTC_E_NOSYS);
	CHECK(xtc_xsend(NULL, "m", 1) == XTC_E_INVAL);
	CHECK(xtc_xmonitor(NULL, NULL) == XTC_E_INVAL);
	CHECK(xtc_xlink(NULL) == XTC_E_INVAL);
}

/* runtime knobs: the per-module setters -> xtc_runtime_set_ / _get_.
 * Each old call is checked against the new getter. */
static void
bridge_runtime(void)
{
	size_t before = xtc_runtime_get_stack_size();
	unsigned b = 0, u = 0;
	int rc;

	CHECK(xtc_set_stack_size(128 * 1024) == XTC_OK);
	CHECK(xtc_stack_size() == 128 * 1024);
	CHECK(xtc_runtime_get_stack_size() == 128 * 1024);
	CHECK(xtc_set_stack_size(1024) == XTC_E_INVAL);
	CHECK(xtc_runtime_set_stack_size(before) == XTC_OK);

	rc = xtc_stack_reclaim_enable(0);
	if (rc == XTC_OK) {
		CHECK(xtc_stack_reclaim_enabled() == 1);
		CHECK(xtc_runtime_get_stack_reclaim() > 0);
	} else {
		CHECK(rc == XTC_E_NOSYS);
	}
	xtc_stack_reclaim_disable();
	CHECK(xtc_stack_reclaim_enabled() == 0);

	/* The pool has not started in this program: a pin succeeds.  Out of
	 * range stays XTC_E_INVAL. */
	CHECK(xtc_blocking_pool_size(0) == XTC_E_INVAL);
	CHECK(xtc_blocking_pool_size(3) == XTC_OK);
	CHECK(xtc_runtime_get_blocking_threads() == 3);

	CHECK(xtc_preempt_set_involuntary(0) == XTC_OK);
	CHECK(xtc_runtime_get_preempt_involuntary() == 0);

	xtc_io_set_iowq_max_workers(2, 7);
	xtc_runtime_get_iowq_workers(&b, &u);
	CHECK(b == 2 && u == 7);
	xtc_runtime_set_iowq_workers(4, 0);

	CHECK(xtc_alloc_audit_enable(1) == XTC_OK);
	CHECK(xtc_runtime_get_alloc_audit() == 1);
	CHECK(xtc_alloc_audit_enable(0) == XTC_OK);
	CHECK(xtc_runtime_get_alloc_audit() == 0);
}

/* registry: register* / unregister* / whereis -> put / del / get.  A
 * taken name and a miss stay XTC_E_INVAL under the old names. */
static void
bridge_reg(void)
{
	xtc_reg_t *r = NULL;
	xtc_pid_t a = { 1, 1, 1 }, b = { 1, 2, 1 }, got;

	CHECK(xtc_reg_create(&r) == XTC_OK && r != NULL);
	CHECK(xtc_reg_register(r, "n", a) == XTC_OK);
	CHECK(xtc_reg_register(r, "n", b) == XTC_E_INVAL);
	CHECK(xtc_reg_whereis(r, "n", &got) == XTC_OK && xtc_pid_eq(got, a));
	CHECK(xtc_reg_whereis(r, "none", &got) == XTC_E_INVAL);
	CHECK(xtc_reg_unregister(r, "n") == XTC_OK);
	CHECK(xtc_reg_unregister(r, "n") == XTC_E_INVAL);
	CHECK(xtc_reg_register_dup(r, "g", a) == XTC_OK);
	CHECK(xtc_reg_register_dup(r, "g", b) == XTC_OK);
	CHECK(xtc_reg_count(r) == 2);
	CHECK(xtc_reg_unregister_pid(r, "g", a) == XTC_OK);
	CHECK(xtc_reg_unregister_pid(r, "g", a) == XTC_E_INVAL);
	CHECK(xtc_reg_register_mon(r, "m", a) == XTC_OK);
	CHECK(xtc_reg_count(r) == 2);
	xtc_reg_destroy(r);
}

/* *_DEFAULT -> *_INIT: each old initializer is the same object. */
static void
bridge_init(void)
{
	xtc_app_opts_t a0 = XTC_APP_OPTS_DEFAULT, a1 = XTC_APP_OPTS_INIT;
	xtc_lockmgr_opts_t l0 = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_lockmgr_opts_t l1 = XTC_LOCKMGR_OPTS_INIT;
	xtc_log_opts_t g0 = XTC_LOG_OPTS_DEFAULT, g1 = XTC_LOG_OPTS_INIT;
	xtc_tcp_opts_t t0 = XTC_TCP_OPTS_DEFAULT, t1 = XTC_TCP_OPTS_INIT;
	xtc_sup_opts_t s0 = XTC_SUP_OPTS_DEFAULT, s1 = XTC_SUP_OPTS_INIT;
	xtc_res_caps_t r0 = XTC_RES_CAPS_DEFAULT, r1 = XTC_RES_CAPS_INIT;
	xtc_slab_opts_t b0 = XTC_SLAB_OPTS_DEFAULT, b1 = XTC_SLAB_OPTS_INIT;

	CHECK(a0.size == a1.size && a0.n_loops == a1.n_loops);
	CHECK(l0.n_partitions == l1.n_partitions);
	CHECK(g0.size == g1.size && g0.ring_size == g1.ring_size);
	CHECK(t0.size == t1.size && t0.nodelay == t1.nodelay);
	CHECK(s0.size == s1.size && s0.max_restarts == s1.max_restarts);
	CHECK(r0.size == r1.size && r0.tasks == r1.tasks);
	CHECK(b0.size == b1.size && b0.align == b1.align);
}

/* coroutines: xtc_async / xtc_await -> xtc_coro_spawn / xtc_coro_join. */
static intptr_t
coro_fn(void *arg)
{
	return (intptr_t)arg + 1;
}

static void
bridge_coro(void)
{
	xtc_loop_t *loop = NULL;
	xtc_task_t *t = NULL;
	intptr_t r = 0;

	CHECK(xtc_loop_create(&loop) == XTC_OK);
	CHECK(xtc_async(loop, coro_fn, (void *)(intptr_t)41, &t) == XTC_OK);
	CHECK(xtc_loop_run(loop) == XTC_OK);
	CHECK(xtc_await(t, &r) == XTC_OK && r == 42);
	xtc_loop_destroy(loop);
}

/* cfg sessions: xtc_cfg_ssn_set_* -> xtc_cfg_session_set_*.  With no
 * session bound and a NULL session, each refuses alike. */
static void
bridge_cfg_session(void)
{
	CHECK(xtc_cfg_ssn_set_bool(NULL, "x", 1, XTC_CFG_SRC_SESSION) ==
	    XTC_E_INVAL);
	CHECK(xtc_cfg_ssn_set_int(NULL, "x", 1, XTC_CFG_SRC_SESSION) ==
	    XTC_E_INVAL);
	CHECK(xtc_cfg_ssn_set_int64(NULL, "x", 1, XTC_CFG_SRC_SESSION) ==
	    XTC_E_INVAL);
	CHECK(xtc_cfg_ssn_set_double(NULL, "x", 1.0, XTC_CFG_SRC_SESSION) ==
	    XTC_E_INVAL);
	CHECK(xtc_cfg_ssn_set_string(NULL, "x", "v", XTC_CFG_SRC_SESSION) ==
	    XTC_E_INVAL);
	CHECK(xtc_cfg_ssn_set_enum(NULL, "x", 1, XTC_CFG_SRC_SESSION) ==
	    XTC_E_INVAL);
}

/* callback types: _cb / _cb_t -> _fn.  The old names are the same
 * types, so they pass where the new ones are expected. */
static void
stall_cb(xtc_loop_t *loop, xtc_task_t *task, int64_t ran, int64_t budget,
    void *user)
{
	(void)loop;
	(void)task;
	(void)ran;
	(void)budget;
	(void)user;
}

static void
bridge_fn_types(void)
{
	xtc_stall_cb s = stall_cb;
	xtc_stall_fn s2 = s;
	xtc_tls_passphrase_cb_t p = NULL;
	xtc_tls_passphrase_fn p2 = p;
	xtc_tls_sni_cb_t n = NULL;
	xtc_tls_sni_fn n2 = n;

	CHECK(s2 == stall_cb && p2 == NULL && n2 == NULL);
}

/* enum constants: XTC_PARK_* -> XTC_PROC_PARK_* (deprecated) and the
 * frozen XTC_LW_* -> XTC_LWLOCK_* (aliases, no warning). */
static void
bridge_enums(void)
{
	xtc_lwlock_mode_t m = XTC_LW_SHARED;
	enum xtc_proc_park pk = XTC_PARK_MAILBOX;

	CHECK(XTC_PARK_NONE == XTC_PROC_PARK_NONE);
	CHECK(XTC_PARK_FD == XTC_PROC_PARK_FD);
	CHECK(XTC_PARK_TIMER == XTC_PROC_PARK_TIMER);
	CHECK(pk == XTC_PROC_PARK_MAILBOX);
	CHECK(m == XTC_LWLOCK_SHARED);
	CHECK(XTC_LW_EXCLUSIVE == XTC_LWLOCK_EXCLUSIVE);
}

/* futures, amutex flags, yield budget, tuning check, and the frozen
 * lwlock alias. */
static void
bridge_misc_b(void)
{
	xtc_promise_t *p = NULL;
	xtc_future_t *f = NULL;
	xtc_amutex_t *m = NULL;
	xtc_loop_t *loop = NULL;
	xtc_lwlock_t lw;

	CHECK(xtc_future_new_pair(&p, &f) == XTC_OK && p != NULL);
	xtc_promise_drop(p);
	CHECK(xtc_future_wait(f, NULL, -1) == XTC_E_ABORTED);
	CHECK(xtc_amutex_create_ex(&m, XTC_AMUTEX_RECURSIVE) == XTC_OK);
	CHECK(xtc_amutex_lock(m, -1) == XTC_OK);
	CHECK(xtc_amutex_lock(m, -1) == XTC_OK);
	CHECK(xtc_amutex_unlock(m) == XTC_OK);
	CHECK(xtc_amutex_unlock(m) == XTC_OK);
	xtc_amutex_destroy(m);
	CHECK(xtc_loop_create(&loop) == XTC_OK);
	xtc_yield_set_budget(loop, 5000);
	CHECK(xtc_loop_get_yield_budget(loop) == 5000);
	xtc_loop_destroy(loop);
	xtc_tuning_check();
	CHECK(xtc_lwlock_init(&lw, 1) == XTC_OK);
	CHECK(xtc_lwlock_acquire_cond(&lw, XTC_LWLOCK_EXCLUSIVE) == XTC_OK);
	xtc_lwlock_release(&lw);
	xtc_lwlock_destroy(&lw);
}

int
main(void)
{
	bridge_lifecycle();
	bridge_exec();
	bridge_pg();
	bridge_shutdown();
	bridge_scope();
	bridge_slab_pressure();
	bridge_detach();
	bridge_tnt();
	bridge_xproc();
	bridge_runtime();
	bridge_reg();
	bridge_init();
	bridge_coro();
	bridge_cfg_session();
	bridge_fn_types();
	bridge_enums();
	bridge_misc_b();
	printf("  [bridge] OK: every deprecated name still works\n");
	return 0;
}

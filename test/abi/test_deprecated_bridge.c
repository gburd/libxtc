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

#include "xtc.h"
#include "xtc_batch.h"
#include "xtc_chan.h"
#include "xtc_exec.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_pg.h"
#include "xtc_prob.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_slab.h"
#include "xtc_sync.h"

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

int
main(void)
{
	bridge_lifecycle();
	bridge_exec();
	bridge_pg();
	bridge_shutdown();
	bridge_scope();
	bridge_slab_pressure();
	printf("  [bridge] OK: every deprecated name still works\n");
	return 0;
}

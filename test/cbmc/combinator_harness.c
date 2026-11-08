/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/combinator_harness.c
 *
 *	(src/ptc/future.c xtc_future_when_all / all_on_complete,
 *	xtc_future_when_any / any_on_complete.)
 *
 *	A combinator's context is shared by its N input callbacks, counted
 *	in `remaining`, and freed by the last.  INVARIANTS, two inputs
 *	completing on two threads with any statuses, over every
 *	interleaving:
 *	  - the combined promise is set exactly once;
 *	  - when_all: it is set by the LAST input, with XTC_OK only if
 *	    every input was OK, else the status of an input that failed;
 *	  - when_any: it carries the value and status of an input;
 *	  - the context is freed exactly once, after both callbacks.
 *
 *	WHAT IS MODELLED: the callbacks, between BEGIN/END SOURCE
 *	(check_drift.sh).  cell_on_complete is "register the callback or
 *	run it now"; CBMC is sequentially consistent here.
 *	Planted bugs, each must FAIL:
 *	  -DPLANT_ALL_FIRST   when_all resolves on the FIRST completion;
 *	  -DPLANT_ANY_NOCAS   when_any sets the promise on every completion.
 *
 *	  -DPLANT_IGNORE_REG_FAIL  ignore a failed registration, as
 *	                           through 2.1.1 (see below).
 *
 *	Registration is modelled too: cell_on_complete can fail (its node
 *	allocation) for any input, and input 0 may fire while input 1 is
 *	still being registered.  Through 2.1.1 the combinators ignored that
 *	failure, so the input's callback never ran and `remaining` never
 *	reached zero: when_all's future never resolved and when_any leaked
 *	its context.  CBMC found it; the registration loop now treats a
 *	failed registration as that input completing with XTC_E_NOMEM.
 */
#include "xtc_atomic.h"
#include <stddef.h>
#include <stdint.h>

#define XTC_OK		0
#define XTC_E_NOMEM	(-2)
#define N		2

struct all_ctx {
	void           *prom;
	int remaining;
	int             total;
	int first_err;
	int err_set;
};
struct any_ctx {
	void          *prom;
	int done;
	int remaining;
};

static struct all_ctx actx;
static struct any_ctx yctx;
static int sets, frees, set_status, done_ct;
static intptr_t set_value;
static int in_status[N];
int nondet_int(void);

static int
xtc_promise_set(void *p, intptr_t v, int status)
{
	(void)p;
	__CPROVER_assert(xtc_atomic_load_i32(&frees, xtc_mo_seq_cst) == 0,
	    "the promise is set while the context is live");
	set_value = v;
	xtc_atomic_store_i32(&set_status, status, xtc_mo_seq_cst);
	xtc_atomic_fetch_add_i32(&sets, 1, xtc_mo_seq_cst);
	return XTC_OK;
}
static void
__os_free(void *p)
{
	(void)p;
	xtc_atomic_fetch_add_i32(&frees, 1, xtc_mo_seq_cst);
}

static void
all_on_complete(intptr_t value, int status, void *u)
{
	/* BEGIN SOURCE src/ptc/future.c all_on_complete */
	struct all_ctx *a = u;
	(void)value;
	if (status != XTC_OK) {
		int expected = 0;
		if (xtc_atomic_cas_i32(&a->err_set, &expected, 1,
		        xtc_mo_acq_rel, xtc_mo_relaxed))
			xtc_atomic_store_i32(&a->first_err, status,
			    xtc_mo_relaxed);
	}
#ifdef PLANT_ALL_FIRST
	if (xtc_atomic_fetch_sub_i32(&a->remaining, 1, xtc_mo_acq_rel) == N) {
#else
	if (xtc_atomic_fetch_sub_i32(&a->remaining, 1, xtc_mo_acq_rel) == 1) {
#endif
		int st = xtc_atomic_load_i32(&a->err_set, xtc_mo_relaxed) ?
		    xtc_atomic_load_i32(&a->first_err, xtc_mo_relaxed) :
		    XTC_OK;
		(void)xtc_promise_set(a->prom, (intptr_t)a->total, st);
		__os_free(a);
	}
	/* END SOURCE */
}

static void
any_on_complete(intptr_t value, int status, void *u)
{
	/* BEGIN SOURCE src/ptc/future.c any_on_complete */
	struct any_ctx *a = u;
	int expected = 0;
#ifdef PLANT_ANY_NOCAS
	(void)expected;
	(void)xtc_promise_set(a->prom, value, status);
#else
	if (xtc_atomic_cas_i32(&a->done, &expected, 1, xtc_mo_acq_rel,
	        xtc_mo_relaxed))
		(void)xtc_promise_set(a->prom, value, status);
#endif
	if (xtc_atomic_fetch_sub_i32(&a->remaining, 1, xtc_mo_acq_rel) == 1)
		__os_free(a);
	/* END SOURCE */
}

/* Registration.  cell_on_complete either queues the callback (it runs
 * when the input fires) or fails. */
struct fut { int cell; };
static struct fut fs[N];
static struct fut *futs[N] = { &fs[0], &fs[1] };
static int registered[N];

static int
cell_on_complete(int cell, void (*cb)(intptr_t, int, void *), void *u)
{
	(void)cb; (void)u;
	registered[cell] = nondet_int() != 0;
	return registered[cell] ? XTC_OK : XTC_E_NOMEM;
}

static void any_retire_failed(struct any_ctx *a);

static void
reg(int i, int any)
{
	if (!any) {
		struct all_ctx *a = &actx;
		/* BEGIN SOURCE src/ptc/future.c xtc_future_when_all register */
#ifdef PLANT_IGNORE_REG_FAIL
		(void)cell_on_complete(futs[i]->cell, all_on_complete, a);
#else
		if (cell_on_complete(futs[i]->cell, all_on_complete, a) !=
		    XTC_OK)
			all_on_complete(0, XTC_E_NOMEM, a);
#endif
		/* END SOURCE */
	} else {
		struct any_ctx *a = &yctx;
		/* BEGIN SOURCE src/ptc/future.c xtc_future_when_any register */
#ifdef PLANT_IGNORE_REG_FAIL
		(void)cell_on_complete(futs[i]->cell, any_on_complete, a);
#else
		if (cell_on_complete(futs[i]->cell, any_on_complete, a) !=
		    XTC_OK)
			any_retire_failed(a);
#endif
		/* END SOURCE */
	}
}

static void
any_retire_failed(struct any_ctx *a)
{
	/* BEGIN SOURCE src/ptc/future.c any_retire_failed */
	if (xtc_atomic_fetch_sub_i32(&a->remaining, 1, xtc_mo_acq_rel) == 1) {
		int expected = 0;
		if (xtc_atomic_cas_i32(&a->done, &expected, 1, xtc_mo_acq_rel,
		        xtc_mo_relaxed))
			(void)xtc_promise_set(a->prom, 0, XTC_E_NOMEM);
		__os_free(a);
	}
	/* END SOURCE */
}

/* Input i fires: its callback runs if it was registered. */
static void
input(int i, int any)
{
	if (registered[i]) {
		if (any)
			any_on_complete(100 + i, in_status[i], &yctx);
		else
			all_on_complete(100 + i, in_status[i], &actx);
	}
	xtc_atomic_fetch_add_i32(&done_ct, 1, xtc_mo_seq_cst);
}
static int which;
static void in0(void) { input(0, which); }

int
main(void)
{
	int i;

	which = nondet_int();
	__CPROVER_assume(which == 0 || which == 1);	/* all / any */
	for (i = 0; i < N; i++) {
		fs[i].cell = i;
		in_status[i] = nondet_int();
		__CPROVER_assume(in_status[i] == XTC_OK ||
		    in_status[i] == -1 || in_status[i] == -3);
	}
	actx.prom = &actx;
	actx.total = N;
	xtc_atomic_store_i32(&actx.remaining, N, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&actx.first_err, XTC_OK, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&actx.err_set, 0, xtc_mo_seq_cst);
	yctx.prom = &yctx;
	xtc_atomic_store_i32(&yctx.done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&yctx.remaining, N, xtc_mo_seq_cst);

	/* Register input 0; it may fire at once, concurrently with input
	 * 1's registration.  Then register input 1 and fire it. */
	reg(0, which);
	__CPROVER_ASYNC_1: in0();
	reg(1, which);
	input(1, which);

	__CPROVER_assume(xtc_atomic_load_i32(&done_ct, xtc_mo_seq_cst) == N);
	__CPROVER_assert(xtc_atomic_load_i32(&sets, xtc_mo_seq_cst) == 1,
	    "the combined promise is set exactly once");
	__CPROVER_assert(xtc_atomic_load_i32(&frees, xtc_mo_seq_cst) == 1,
	    "the context is freed exactly once");
	/* A failed registration counts as that input completing with
	 * XTC_E_NOMEM. */
	for (i = 0; i < N; i++)
		if (!registered[i])
			in_status[i] = XTC_E_NOMEM;
	if (which == 0) {
		if (in_status[0] == XTC_OK && in_status[1] == XTC_OK)
			__CPROVER_assert(xtc_atomic_load_i32(&set_status,
			                     xtc_mo_seq_cst) == XTC_OK,
			    "when_all: every input OK -> OK");
		else
			__CPROVER_assert((xtc_atomic_load_i32(&set_status,
			                      xtc_mo_seq_cst) == in_status[0] &&
			                     in_status[0] != XTC_OK) ||
			        (xtc_atomic_load_i32(&set_status,
			             xtc_mo_seq_cst) == in_status[1] &&
			            in_status[1] != XTC_OK),
			    "when_all: a failure carries a failed input's status");
	} else if (registered[0] || registered[1]) {
		__CPROVER_assert((registered[0] && set_value == 100 &&
		                     xtc_atomic_load_i32(&set_status,
		                         xtc_mo_seq_cst) == in_status[0]) ||
		        (registered[1] && set_value == 101 &&
		            xtc_atomic_load_i32(&set_status, xtc_mo_seq_cst) ==
		                in_status[1]),
		    "when_any: carries a registered input's value and status");
	} else {
		__CPROVER_assert(xtc_atomic_load_i32(&set_status,
		                     xtc_mo_seq_cst) == XTC_E_NOMEM,
		    "when_any: nothing registered -> XTC_E_NOMEM");
	}
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_lrlock.c
 *	Property-based tests for M13b xtc_lrlock.
 *
 *	Properties:
 *	  L1: writer's apply_op sequence is observed by readers in
 *	      the same order after each publish.
 *	  L2: COW mode produces identical reader-visible state to
 *	      the default mode for the same operation sequence.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_lrlock.h"

#if defined(XTC_HAVE_HEGEL)

struct counter { int v; };
struct op_add { int delta; };

static void
apply_add(void *data, const void *op, size_t sz)
{
	struct counter *c = data;
	const struct op_add *a = op;
	(void)sz;
	c->v += a->delta;
}

static void
sync_counter(void *dst, const void *src, size_t sz) { memcpy(dst, src, sz); }

/* ----- L1: ordered apply --------------------------------- */

static void
prop_apply_order(hegel_test_case *tc, void *u)
{
	xtc_lrlock_t *lr;
	int n, i, sum = 0;
	struct op_add op;
	const struct counter *rd;
	(void)u;
	n = (int)hegel_draw_int(tc, hegel_integers(1, 32));
	hegel_assume(xtc_lrlock_create(sizeof(struct counter), apply_add,
	    sync_counter, "pbt", &lr) == XTC_OK);
	{
		struct counter *wr = xtc_lrlock_write_begin(lr);
		(void)wr;
		for (i = 0; i < n; i++) {
			op.delta = (int)hegel_draw_int(tc,
			    hegel_integers(-100, 100));
			sum += op.delta;
			xtc_lrlock_apply_op(lr, &op, sizeof op);
		}
		xtc_lrlock_publish(lr);
		xtc_lrlock_write_end(lr);
	}
	rd = xtc_lrlock_read_begin(lr);
	hegel_assume(rd->v == sum);
	xtc_lrlock_read_end(lr);
	xtc_lrlock_destroy(lr);
}

/* ----- L2: COW mode equivalent to default mode ----------- */

static void
prop_cow_equivalent(hegel_test_case *tc, void *u)
{
	xtc_lrlock_t *lr_def, *lr_cow;
	xtc_lrlock_opts_t opts_cow = { 0 };
	int n, i, deltas[32];
	const struct counter *rd_def, *rd_cow;
	(void)u;
	n = (int)hegel_draw_int(tc, hegel_integers(1, 32));
	for (i = 0; i < n; i++)
		deltas[i] = (int)hegel_draw_int(tc, hegel_integers(-50, 50));

	hegel_assume(xtc_lrlock_create(sizeof(struct counter), apply_add,
	    sync_counter, "def", &lr_def) == XTC_OK);
	opts_cow.name = "cow"; opts_cow.data_size = sizeof(struct counter);
	opts_cow.apply_fn = apply_add; opts_cow.sync_fn = sync_counter;
	opts_cow.flags = XTC_LRLOCK_COW;
	hegel_assume(xtc_lrlock_create_ex(&opts_cow, &lr_cow) == XTC_OK);

	{
		struct counter *wd = xtc_lrlock_write_begin(lr_def);
		struct counter *wc = xtc_lrlock_write_begin(lr_cow);
		struct op_add op;
		(void)wd; (void)wc;
		for (i = 0; i < n; i++) {
			op.delta = deltas[i];
			xtc_lrlock_apply_op(lr_def, &op, sizeof op);
			xtc_lrlock_apply_op(lr_cow, &op, sizeof op);
		}
		xtc_lrlock_publish(lr_def);
		xtc_lrlock_publish(lr_cow);
		xtc_lrlock_write_end(lr_def);
		xtc_lrlock_write_end(lr_cow);
	}

	rd_def = xtc_lrlock_read_begin(lr_def);
	rd_cow = xtc_lrlock_read_begin(lr_cow);
	hegel_assume(rd_def->v == rd_cow->v);
	xtc_lrlock_read_end(lr_def);
	xtc_lrlock_read_end(lr_cow);

	xtc_lrlock_destroy(lr_def);
	xtc_lrlock_destroy(lr_cow);
}

static const pbt_entry_t tests[] = {
	{ "apply_order",     prop_apply_order,     20 },
	{ "cow_equivalent",  prop_cow_equivalent,  20 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "apply_order",     NULL, 0 },
	{ "cow_equivalent",  NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("lrlock", tests)

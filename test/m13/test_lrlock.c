/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_lrlock.c -- verifies M13b left-right lock.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_lrlock.h"
#include "xtc_int.h"

/* Protected data: a tiny dictionary of (key,value) pairs.  The op
 * descriptor is "set key=value". */
struct kv { int key; int value; };
struct dict { struct kv items[64]; int n; };

struct op_set { int key; int value; };

static void
apply_set(void *data, const void *op, size_t op_size)
{
	struct dict *d = data;
	const struct op_set *s = op;
	int i;
	(void)op_size;
	for (i = 0; i < d->n; i++) {
		if (d->items[i].key == s->key) {
			d->items[i].value = s->value;
			return;
		}
	}
	if (d->n < 64) { d->items[d->n].key = s->key; d->items[d->n].value = s->value; d->n++; }
}

static void
sync_dict(void *dst, const void *src, size_t sz)
{
	memcpy(dst, src, sz);
}

static MunitResult
test_lrlock_basic(const MunitParameter p[], void *d)
{
	xtc_lrlock_t *lr;
	const struct dict *rd;
	struct dict *wr;
	struct op_set s;
	(void)p; (void)d;

	munit_assert_int(xtc_lrlock_create(sizeof(struct dict), apply_set,
	    sync_dict, "kv", &lr), ==, XTC_OK);

	/* Reader sees an empty dict. */
	rd = xtc_lrlock_read_begin(lr);
	munit_assert_int(rd->n, ==, 0);
	xtc_lrlock_read_end(lr);

	/* Writer sets key=1,value=100. */
	wr = xtc_lrlock_write_begin(lr);
	munit_assert_not_null(wr);
	s.key = 1; s.value = 100;
	xtc_lrlock_apply_op(lr, &s, sizeof s);
	xtc_lrlock_publish(lr);
	xtc_lrlock_write_end(lr);

	rd = xtc_lrlock_read_begin(lr);
	munit_assert_int(rd->n, ==, 1);
	munit_assert_int(rd->items[0].key, ==, 1);
	munit_assert_int(rd->items[0].value, ==, 100);
	xtc_lrlock_read_end(lr);

	/* Update existing key. */
	wr = xtc_lrlock_write_begin(lr);
	s.key = 1; s.value = 999;
	xtc_lrlock_apply_op(lr, &s, sizeof s);
	xtc_lrlock_publish(lr);
	xtc_lrlock_write_end(lr);

	rd = xtc_lrlock_read_begin(lr);
	munit_assert_int(rd->n, ==, 1);
	munit_assert_int(rd->items[0].value, ==, 999);
	xtc_lrlock_read_end(lr);

	xtc_lrlock_destroy(lr);
	return MUNIT_OK;
}

/* Concurrent reads while writers commit. */
static xtc_lrlock_t *g_lr;
static _Atomic int   g_stop_readers;
static _Atomic int   g_inconsistent;

static void *
reader_thread(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_stop_readers)) {
		const struct dict *r = xtc_lrlock_read_begin(g_lr);
		int i;
		/* Invariant: every item's value should be > 0. */
		for (i = 0; i < r->n; i++)
			if (r->items[i].value <= 0)
				atomic_fetch_add(&g_inconsistent, 1);
		xtc_lrlock_read_end(g_lr);
	}
	return NULL;
}

static MunitResult
test_lrlock_concurrent(const MunitParameter p[], void *d)
{
	pthread_t readers[3];
	int i;
	struct op_set s;
	(void)p; (void)d;

	atomic_store(&g_stop_readers, 0);
	atomic_store(&g_inconsistent, 0);
	munit_assert_int(xtc_lrlock_create(sizeof(struct dict), apply_set,
	    sync_dict, "kv", &g_lr), ==, XTC_OK);

	/* Pre-populate. */
	{
		struct dict *wr = xtc_lrlock_write_begin(g_lr);
		(void)wr;
		s.key = 1; s.value = 1;
		xtc_lrlock_apply_op(g_lr, &s, sizeof s);
		xtc_lrlock_publish(g_lr);
		xtc_lrlock_write_end(g_lr);
	}

	for (i = 0; i < 3; i++) pthread_create(&readers[i], NULL, reader_thread, NULL);

	/* Bang on the lock with N writes. */
	for (i = 0; i < 100; i++) {
		struct dict *wr = xtc_lrlock_write_begin(g_lr);
		(void)wr;
		s.key = (i % 5) + 1; s.value = i + 1;     /* always positive */
		xtc_lrlock_apply_op(g_lr, &s, sizeof s);
		xtc_lrlock_publish(g_lr);
		xtc_lrlock_write_end(g_lr);
	}

	atomic_store(&g_stop_readers, 1);
	for (i = 0; i < 3; i++) pthread_join(readers[i], NULL);

	munit_assert_int(atomic_load(&g_inconsistent), ==, 0);
	xtc_lrlock_destroy(g_lr);
	return MUNIT_OK;
}

/* ---- nested reads ---------------------------------------- */

static MunitResult
test_lrlock_nested_read(const MunitParameter p[], void *d)
{
	xtc_lrlock_t *lr;
	const struct dict *r1, *r2;
	struct op_set s;
	(void)p; (void)d;
	munit_assert_int(xtc_lrlock_create(sizeof(struct dict), apply_set,
	    sync_dict, "nested", &lr), ==, XTC_OK);

	/* Outer read */
	r1 = xtc_lrlock_read_begin(lr);
	munit_assert_not_null(r1);
	/* Nested read returns same idx. */
	r2 = xtc_lrlock_read_begin(lr);
	munit_assert_ptr_equal(r1, r2);

	xtc_lrlock_read_end(lr);   /* still inside outer */
	/* r1 still valid here */
	munit_assert_int(r1->n, ==, 0);
	xtc_lrlock_read_end(lr);   /* truly out */

	/* Now do a write while no readers active. */
	{
		struct dict *wr = xtc_lrlock_write_begin(lr);
		munit_assert_not_null(wr);
		s.key = 7; s.value = 70;
		xtc_lrlock_apply_op(lr, &s, sizeof s);
		xtc_lrlock_publish(lr);
		xtc_lrlock_write_end(lr);
	}

	r1 = xtc_lrlock_read_begin(lr);
	munit_assert_int(r1->n, ==, 1);
	munit_assert_int(r1->items[0].value, ==, 70);
	xtc_lrlock_read_end(lr);

	xtc_lrlock_destroy(lr);
	return MUNIT_OK;
}

/* ---- publish_full_sync (caller mutated state directly) ----- */

static MunitResult
test_lrlock_publish_full_sync(const MunitParameter p[], void *d)
{
	xtc_lrlock_t *lr;
	struct dict *wr;
	const struct dict *rd;
	(void)p; (void)d;
	munit_assert_int(xtc_lrlock_create(sizeof(struct dict), apply_set,
	    sync_dict, "fs", &lr), ==, XTC_OK);

	/* Direct mutation bypassing apply_op. */
	wr = xtc_lrlock_write_begin(lr);
	munit_assert_not_null(wr);
	wr->n = 3;
	wr->items[0].key = 10; wr->items[0].value = 100;
	wr->items[1].key = 20; wr->items[1].value = 200;
	wr->items[2].key = 30; wr->items[2].value = 300;
	xtc_lrlock_publish_full_sync(lr);
	xtc_lrlock_write_end(lr);

	rd = xtc_lrlock_read_begin(lr);
	munit_assert_int(rd->n, ==, 3);
	munit_assert_int(rd->items[1].value, ==, 200);
	xtc_lrlock_read_end(lr);

	/* And the OTHER copy is now also up-to-date so the next
	 * publish() round can use replay safely. */
	wr = xtc_lrlock_write_begin(lr);
	munit_assert_int(wr->n, ==, 3);   /* synced to write copy */
	munit_assert_int(wr->items[2].value, ==, 300);
	xtc_lrlock_write_end(lr);

	xtc_lrlock_destroy(lr);
	return MUNIT_OK;
}

/* ---- COW mode: lazy data[1] + MADV_FREE after publish ------ */

static MunitResult
test_lrlock_cow_basic(const MunitParameter p[], void *d)
{
	xtc_lrlock_t *lr;
	xtc_lrlock_opts_t o = { 0 };
	struct op_set s;
	const struct dict *rd;
	struct dict *wr;
	int i;
	(void)p; (void)d;
	o.name = "cow"; o.data_size = sizeof(struct dict);
	o.apply_fn = apply_set; o.sync_fn = sync_dict;
	o.flags = XTC_LRLOCK_COW;
	munit_assert_int(xtc_lrlock_create_ex(&o, &lr), ==, XTC_OK);

	/* Reader path works on the (single) copy. */
	rd = xtc_lrlock_read_begin(lr);
	munit_assert_int(rd->n, ==, 0);
	xtc_lrlock_read_end(lr);

	/* Several write cycles to exercise MADV_FREE / re-fill. */
	for (i = 0; i < 5; i++) {
		wr = xtc_lrlock_write_begin(lr);
		munit_assert_not_null(wr);
		s.key = i; s.value = i * 100 + 1;
		xtc_lrlock_apply_op(lr, &s, sizeof s);
		xtc_lrlock_publish(lr);
		xtc_lrlock_write_end(lr);
	}

	rd = xtc_lrlock_read_begin(lr);
	munit_assert_int(rd->n, ==, 5);
	munit_assert_int(rd->items[2].value, ==, 201);
	xtc_lrlock_read_end(lr);

	xtc_lrlock_destroy(lr);
	return MUNIT_OK;
}

static xtc_lrlock_t *g_cow_lr;
static _Atomic int   g_cow_stop;
static _Atomic int   g_cow_inconsistent;

static void *
cow_reader_thread(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_cow_stop)) {
		const struct dict *r = xtc_lrlock_read_begin(g_cow_lr);
		int i;
		for (i = 0; i < r->n; i++)
			if (r->items[i].value <= 0)
				atomic_fetch_add(&g_cow_inconsistent, 1);
		xtc_lrlock_read_end(g_cow_lr);
	}
	return NULL;
}

static MunitResult
test_lrlock_cow_concurrent(const MunitParameter p[], void *d)
{
	xtc_lrlock_opts_t o = { 0 };
	pthread_t readers[3];
	int i;
	struct op_set s;
	(void)p; (void)d;
	atomic_store(&g_cow_stop, 0);
	atomic_store(&g_cow_inconsistent, 0);
	o.name = "cowc"; o.data_size = sizeof(struct dict);
	o.apply_fn = apply_set; o.sync_fn = sync_dict;
	o.flags = XTC_LRLOCK_COW;
	munit_assert_int(xtc_lrlock_create_ex(&o, &g_cow_lr), ==, XTC_OK);

	/* Pre-populate. */
	{ struct dict *wr = xtc_lrlock_write_begin(g_cow_lr); (void)wr;
	  s.key = 1; s.value = 1;
	  xtc_lrlock_apply_op(g_cow_lr, &s, sizeof s);
	  xtc_lrlock_publish(g_cow_lr);
	  xtc_lrlock_write_end(g_cow_lr); }

	for (i = 0; i < 3; i++) pthread_create(&readers[i], NULL, cow_reader_thread, NULL);

	for (i = 0; i < 100; i++) {
		struct dict *wr = xtc_lrlock_write_begin(g_cow_lr); (void)wr;
		s.key = (i % 5) + 1; s.value = i + 2;
		xtc_lrlock_apply_op(g_cow_lr, &s, sizeof s);
		xtc_lrlock_publish(g_cow_lr);
		xtc_lrlock_write_end(g_cow_lr);
	}

	atomic_store(&g_cow_stop, 1);
	for (i = 0; i < 3; i++) pthread_join(readers[i], NULL);

	munit_assert_int(atomic_load(&g_cow_inconsistent), ==, 0);
	xtc_lrlock_destroy(g_cow_lr);
	return MUNIT_OK;
}

/* ---- oplog growth ------------------------------------------ */

static MunitResult
test_lrlock_oplog_grow(const MunitParameter p[], void *d)
{
	xtc_lrlock_t *lr;
	xtc_lrlock_opts_t o = { 0 };
	struct op_set s;
	const struct dict *rd;
	int i;
	(void)p; (void)d;
	o.name = "grow"; o.data_size = sizeof(struct dict);
	o.apply_fn = apply_set; o.sync_fn = sync_dict;
	o.oplog_capacity = 64;   /* tiny; will grow */
	munit_assert_int(xtc_lrlock_create_ex(&o, &lr), ==, XTC_OK);

	{
		struct dict *wr = xtc_lrlock_write_begin(lr);
		(void)wr;
		for (i = 0; i < 64; i++) {
			s.key = i; s.value = i + 1;
			xtc_lrlock_apply_op(lr, &s, sizeof s);
		}
		xtc_lrlock_publish(lr);
		xtc_lrlock_write_end(lr);
	}

	rd = xtc_lrlock_read_begin(lr);
	munit_assert_int(rd->n, ==, 64);
	munit_assert_int(rd->items[63].value, ==, 64);
	xtc_lrlock_read_end(lr);

	xtc_lrlock_destroy(lr);
	return MUNIT_OK;
}

/* ---- thread-churn slot reclamation (regression) -------------
 *
 * Before slot reclamation, each new thread consumed one of the
 * 4096 global reader slots permanently.  A process that recycled
 * more than 4096 reader threads exhausted the pool; thereafter
 * read_begin could not announce an epoch and a COW writer could
 * MADV_FREE a buffer out from under a still-reading thread.  This
 * test churns far more than 4096 short-lived reader threads through
 * one lock; with reclamation every read stays consistent and no
 * read_begin returns NULL.
 */

static xtc_lrlock_t  *g_churn_lr;
static _Atomic int    g_churn_null_begin;
static _Atomic int    g_churn_inconsistent;

static void *
churn_reader(void *arg)
{
	(void)arg;
	{
		const struct dict *r = xtc_lrlock_read_begin(g_churn_lr);
		if (r == NULL) {
			atomic_fetch_add(&g_churn_null_begin, 1);
		} else {
			int i;
			for (i = 0; i < r->n; i++)
				if (r->items[i].value <= 0)
					atomic_fetch_add(&g_churn_inconsistent, 1);
			xtc_lrlock_read_end(g_churn_lr);
		}
	}
	return NULL;
}

static MunitResult
test_lrlock_thread_churn(const MunitParameter p[], void *d)
{
	struct op_set s;
	int round;
	(void)p; (void)d;

	atomic_store(&g_churn_null_begin, 0);
	atomic_store(&g_churn_inconsistent, 0);
	munit_assert_int(xtc_lrlock_create(sizeof(struct dict), apply_set,
	    sync_dict, "churn", &g_churn_lr), ==, XTC_OK);

	/* Seed one positive-valued item so the reader invariant
	 * (every value > 0) is meaningful. */
	{
		struct dict *wr = xtc_lrlock_write_begin(g_churn_lr);
		(void)wr;
		s.key = 1; s.value = 42;
		xtc_lrlock_apply_op(g_churn_lr, &s, sizeof s);
		xtc_lrlock_publish(g_churn_lr);
		xtc_lrlock_write_end(g_churn_lr);
	}

	/* 5000 short-lived reader threads, well past the 4096 slot cap,
	 * spawned and joined in small batches.  Without reclamation the
	 * 4097th read_begin would return NULL. */
	for (round = 0; round < 5000 / 8; round++) {
		pthread_t t[8];
		int i;
		for (i = 0; i < 8; i++)
			munit_assert_int(pthread_create(&t[i], NULL,
			    churn_reader, NULL), ==, 0);
		for (i = 0; i < 8; i++)
			pthread_join(t[i], NULL);
	}

	munit_assert_int(atomic_load(&g_churn_null_begin), ==, 0);
	munit_assert_int(atomic_load(&g_churn_inconsistent), ==, 0);

	xtc_lrlock_destroy(g_churn_lr);
	return MUNIT_OK;
}

/* ---- max_readers via opts ---------------------------------- */

static MunitResult
test_lrlock_max_readers(const MunitParameter p[], void *d)
{
	xtc_lrlock_t *lr;
	xtc_lrlock_opts_t o = { 0 };
	const struct dict *rd;
	(void)p; (void)d;
	o.name = "mr"; o.data_size = sizeof(struct dict);
	o.apply_fn = apply_set; o.sync_fn = sync_dict;
	o.max_readers = 16;
	munit_assert_int(xtc_lrlock_create_ex(&o, &lr), ==, XTC_OK);

	rd = xtc_lrlock_read_begin(lr);
	munit_assert_not_null(rd);
	xtc_lrlock_read_end(lr);

	xtc_lrlock_destroy(lr);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/lrlock_basic",      test_lrlock_basic,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lrlock_concurrent", test_lrlock_concurrent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lrlock_nested_read", test_lrlock_nested_read, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lrlock_publish_full_sync", test_lrlock_publish_full_sync, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lrlock_cow_basic", test_lrlock_cow_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lrlock_cow_concurrent", test_lrlock_cow_concurrent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lrlock_oplog_grow", test_lrlock_oplog_grow, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lrlock_max_readers", test_lrlock_max_readers, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lrlock_thread_churn", test_lrlock_thread_churn, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/lrlock", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

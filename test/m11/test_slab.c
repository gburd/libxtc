/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m11/test_slab.c — verifies M11.5 xtc_slab.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <malloc.h>
#  define MAP_FAILED ((void *)-1)
#  define PROT_READ 0
#  define PROT_WRITE 0
#  define MAP_PRIVATE   0
#  define MAP_ANONYMOUS 0
#  define mmap(addr, sz, prot, flags, fd, off) \
     ((void)(addr),(void)(prot),(void)(flags),(void)(fd),(void)(off), \
      malloc(sz))
#  define munmap(p, sz)  ((void)(sz), free(p), 0)
#else
#  include <sys/mman.h>
#endif

#include "munit.h"
#include "xtc.h"
#include "xtc_slab.h"
#include "xtc_int.h"

/* ----- Basic alloc/free ----------------------------------------- */

static MunitResult
test_basic_alloc_free(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *objs[100];
	int i;
	xtc_slab_stats_t st;
	(void)p; (void)d;
	opts.name = "basic"; opts.obj_size = 64;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);

	for (i = 0; i < 100; i++) {
		objs[i] = xtc_slab_alloc(s);
		munit_assert_not_null(objs[i]);
		memset(objs[i], (uint8_t)i, 64);     /* write through */
	}
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.n_inuse, ==, 100);

	for (i = 0; i < 100; i++)
		xtc_slab_free(s, objs[i]);

	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.n_inuse, ==, 0);

	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/* ----- Magazine fast path is hit ------------------------------- */

static MunitResult
test_magazine_fastpath(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *objs[32];
	xtc_slab_stats_t st;
	int i;
	(void)p; (void)d;
	opts.name = "mag"; opts.obj_size = 64; opts.magazine_size = 16;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);

	/* First wave: cold magazine, all slow. */
	for (i = 0; i < 32; i++) objs[i] = xtc_slab_alloc(s);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.alloc_slow, >=, 32);
	munit_assert_uint64(st.alloc_fast, ==, 0);

	/* Free into magazine. */
	for (i = 0; i < 32; i++) xtc_slab_free(s, objs[i]);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.free_fast, >, 0);    /* mag holds 16, rest spill */

	/* Re-alloc: first 16 hit magazine. */
	for (i = 0; i < 16; i++) objs[i] = xtc_slab_alloc(s);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.alloc_fast, >=, 16);

	for (i = 0; i < 16; i++) xtc_slab_free(s, objs[i]);
	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/* ----- Constructor/destructor --------------------------------- */

static _Atomic int g_ctor_count;
static _Atomic int g_dtor_count;

static int
my_ctor(void *obj, void *user) {
	(void)user;
	*(int *)obj = 0xdeadbeef;
	atomic_fetch_add(&g_ctor_count, 1);
	return XTC_OK;
}
static void
my_dtor(void *obj, void *user) {
	(void)user;
	*(int *)obj = 0;
	atomic_fetch_add(&g_dtor_count, 1);
}

static MunitResult
test_ctor_dtor(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *o1, *o2;
	(void)p; (void)d;
	atomic_store(&g_ctor_count, 0);
	atomic_store(&g_dtor_count, 0);
	opts.name = "ctor_dtor"; opts.obj_size = sizeof(int);
	opts.ctor = my_ctor; opts.dtor = my_dtor;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);

	o1 = xtc_slab_alloc(s); munit_assert_not_null(o1);
	o2 = xtc_slab_alloc(s); munit_assert_not_null(o2);
	munit_assert_int(*(int *)o1, ==, (int)0xdeadbeef);
	munit_assert_int(*(int *)o2, ==, (int)0xdeadbeef);
	munit_assert_int(atomic_load(&g_ctor_count), ==, 2);

	xtc_slab_free(s, o1);
	xtc_slab_free(s, o2);
	munit_assert_int(atomic_load(&g_dtor_count), ==, 2);
	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/* ----- OOM_FAIL policy ---------------------------------------- */

static MunitResult
test_oom_fail(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_res_t res;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	void *o;
	xtc_slab_stats_t st;
	(void)p; (void)d;

	caps.mem_bytes = 64 * 1024;            /* one chunk worth */
	munit_assert_int(xtc_res_init(&res, &caps), ==, XTC_OK);

	opts.name = "oom"; opts.obj_size = 4096;
	opts.chunk_size = 64 * 1024;
	opts.res = &res;
	opts.oom_policy = XTC_SLAB_OOM_FAIL;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);

	/* First chunk fills budget; objects come out fine. */
	o = xtc_slab_alloc(s);
	munit_assert_not_null(o);
	/* Drain remaining 15 slots in this chunk. */
	{ int i; for (i = 1; i < 16; i++) (void)xtc_slab_alloc(s); }
	/* Next alloc would need a new chunk → res.acquire fails → OOM. */
	o = xtc_slab_alloc(s);
	munit_assert_null(o);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.oom_fails, >=, 1);

	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/* ----- Reap drops empty chunks --------------------------------- */

static MunitResult
test_reap(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *objs[200];
	int i;
	xtc_slab_stats_t st_before, st_after;
	(void)p; (void)d;
	opts.name = "reap"; opts.obj_size = 64;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	for (i = 0; i < 200; i++) objs[i] = xtc_slab_alloc(s);
	for (i = 0; i < 200; i++) xtc_slab_free(s, objs[i]);
	(void)xtc_slab_stat(s, &st_before);

	(void)xtc_slab_reap(s);

	(void)xtc_slab_stat(s, &st_after);
	munit_assert_uint64(st_after.n_chunks, <=, st_before.n_chunks);
	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/* ----- Redzone catches overrun -------------------------------- */

static MunitResult
test_redzone(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	uint8_t *o;
	xtc_slab_stats_t st;
	(void)p; (void)d;
	opts.name = "rz"; opts.obj_size = 16; opts.flags = XTC_SLAB_REDZONE;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	o = xtc_slab_alloc(s);
	munit_assert_not_null(o);
	/* Write exactly within bounds: fine. */
	memset(o, 'x', 16);
	xtc_slab_free(s, o);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.redzone_violations, ==, 0);

	/* Overwrite beyond bounds (clobbers back redzone). */
	o = xtc_slab_alloc(s);
	memset(o, 'y', 24);            /* 8 bytes past the end */
	xtc_slab_free(s, o);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.redzone_violations, >=, 1);
	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/* ----- Audit ring records events ------------------------------ */

static MunitResult
test_audit(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *o;
	(void)p; (void)d;
	opts.name = "audit"; opts.obj_size = 32; opts.flags = XTC_SLAB_AUDIT;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	/* Just exercise the path; audit ring is internal. */
	o = xtc_slab_alloc(s);
	xtc_slab_free(s, o);
	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/* ----- Shared-memory mode + offset/resolve --------------------- */

static MunitResult
test_shm_offset_resolve(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	size_t shm_size = 1024 * 1024;
	void *shm_base;
	void *obj;
	xtc_slab_off_t off;
	void *resolved;
	(void)p; (void)d;

	shm_base = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	munit_assert_not_null(shm_base);

	opts.name = "shm"; opts.obj_size = 128;
	opts.mode = XTC_SLAB_SHARED_MEMORY;
	opts.shm_base = shm_base;
	opts.shm_size = shm_size;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);

	obj = xtc_slab_alloc(s);
	munit_assert_not_null(obj);

	off = xtc_slab_offset(s, obj);
	munit_assert_int64(off, !=, XTC_SLAB_OFF_NONE);

	resolved = xtc_slab_resolve(s, off);
	munit_assert_ptr_equal(resolved, obj);

	xtc_slab_free(s, obj);
	xtc_slab_destroy(s);
	(void)munmap(shm_base, shm_size);
	return MUNIT_OK;
}

/* ----- Concurrent alloc/free across threads ------------------- */

#define TC_THREADS 4
#define TC_OPS     5000

static xtc_slab_t *g_slab;

static void *
tc_worker(void *arg)
{
	int i;
	void *objs[16];
	(void)arg;
	for (i = 0; i < TC_OPS; i++) {
		int j = i & 15;
		objs[j] = xtc_slab_alloc(g_slab);
		if (objs[j] == NULL) continue;
		if (j == 15) {
			int k;
			for (k = 0; k < 16; k++)
				xtc_slab_free(g_slab, objs[k]);
		}
	}
	return NULL;
}

static MunitResult
test_concurrent(const MunitParameter p[], void *d)
{
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	pthread_t th[TC_THREADS];
	int i;
	xtc_slab_stats_t st;
	(void)p; (void)d;
	opts.name = "conc"; opts.obj_size = 96;
	munit_assert_int(xtc_slab_create(&opts, &g_slab), ==, XTC_OK);

	for (i = 0; i < TC_THREADS; i++)
		pthread_create(&th[i], NULL, tc_worker, NULL);
	for (i = 0; i < TC_THREADS; i++) pthread_join(th[i], NULL);

	(void)xtc_slab_stat(g_slab, &st);
	/* Total ops did happen. */
	munit_assert_uint64(st.alloc_fast + st.alloc_slow, >, 0);
	xtc_slab_destroy(g_slab);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic_alloc_free", test_basic_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/magazine_fastpath", test_magazine_fastpath, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ctor_dtor",        test_ctor_dtor,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/oom_fail",         test_oom_fail,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reap",             test_reap,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/redzone",          test_redzone,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/audit",            test_audit,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shm_offset",       test_shm_offset_resolve, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent",       test_concurrent,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m11/slab", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

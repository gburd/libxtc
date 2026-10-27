/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m11/test_slab.c -- verifies M11.5 xtc_slab.
 */

/*
 * Expose MAP_ANONYMOUS (a BSD/SVID extension) from <sys/mman.h> under
 * strict -std=c11: glibc gates it behind _DEFAULT_SOURCE, which the
 * ISO C dialect does not set on its own.  Without this the test fails
 * to COMPILE on a stock glibc toolchain (e.g. Debian), which is what
 * the container-based CI runners use.  Must precede every include.
 */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
/* No mmap on Windows: back MAP_ANONYMOUS with calloc so the region is
 * ZERO-FILLED, matching mmap(MAP_ANONYMOUS)'s contract.  The slab
 * shm-create path detects a fresh region by its zeroed header, so a
 * plain (uninitialized) malloc here made a reused heap block look like
 * a stale slab and create returned XTC_E_VERSION -- caught by
 * shm_reclaim_single_process on the real Windows host. */
#  include <malloc.h>
#  define MAP_FAILED ((void *)-1)
#  define PROT_READ 0
#  define PROT_WRITE 0
#  define MAP_PRIVATE   0
#  define MAP_ANONYMOUS 0
#  define mmap(addr, sz, prot, flags, fd, off) \
     ((void)(addr),(void)(prot),(void)(flags),(void)(fd),(void)(off), \
      calloc(1, (sz)))
#  define munmap(p, sz)  ((void)(sz), free(p), 0)
#else
#  include <sys/mman.h>
#endif

#include "munit.h"
#include "xtc.h"
#include "xtc_slab.h"
#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_depth (internal) */

#ifndef __has_feature
#  define __has_feature(x) 0     /* gcc: sanitizers via __SANITIZE_* */
#endif

/* ----- Basic alloc/free ----------------------------------------- */

/* Overflow guard: obj_size near SIZE_MAX must be rejected, not wrapped
 * into a tiny slot_size (which would hand out overlapping slots).  Also
 * the documented obj_size==0 rejection.  Fails if the guard in
 * xtc_slab_create() is removed. */
static MunitResult
test_reject_bad_obj_size(const MunitParameter p[], void *d)
{
	xtc_slab_t *s = NULL;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	(void)p; (void)d;

	opts.name = "bad"; opts.obj_size = 0;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_E_INVAL);
	munit_assert_null(s);

	opts.obj_size = SIZE_MAX;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_E_INVAL);
	munit_assert_null(s);

	opts.obj_size = SIZE_MAX - 32;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_E_INVAL);
	munit_assert_null(s);

	opts.obj_size = ((size_t)1 << 30) + 1;   /* just past the ceiling */
	opts.flags = XTC_SLAB_REDZONE;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_E_INVAL);
	munit_assert_null(s);

	return MUNIT_OK;
}

static MunitResult
test_basic_alloc_free(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *objs[100];
	int i;
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
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
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
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
	/*
	 * Destroy must NOT re-run the destructor on objects the caller
	 * already freed.  xtc_slab_free runs the dtor before caching the
	 * slot in the per-thread magazine, and destroy used to drop the
	 * magazine array WITHOUT returning its slots to the chunk free
	 * lists -- so its "not on a free list => still live" scan saw those
	 * slots as live and ran the dtor a SECOND time.  A dtor that closes
	 * an fd or drops a refcount double-released it.  Fails (count 4)
	 * without the magazine drain in xtc_slab_destroy.
	 */
	xtc_slab_destroy(s);
	munit_assert_int(atomic_load(&g_dtor_count), ==, 2);
	return MUNIT_OK;
}

/*
 * A one-WORD slot must not let destroy's live/free marker pass corrupt
 * the chunk free list.  slot_size had a one-pointer floor, but destroy
 * marks each slot by writing its SECOND word -- with obj_size 8 and
 * align 8 that write landed in the NEXT slot's free-list link, so the
 * marker walk mis-classified freed slots as live and ran destructors on
 * garbage: measured 515 dtor calls for 4 objects.  Fails without the
 * two-word slot floor in xtc_slab_create.
 */
static MunitResult
test_word_slot_destroy(const MunitParameter p[], void *d)
{
	static const size_t sizes[] = { 1, 4, 8 };
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *o[4];
	size_t si;
	int i;
	(void)p; (void)d;

	for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
		atomic_store(&g_dtor_count, 0);
		opts = (xtc_slab_opts_t)XTC_SLAB_OPTS_DEFAULT;
		opts.name = "word_slot";
		opts.obj_size = sizes[si];
		opts.align = 8;                /* smallest permitted stride */
		opts.chunk_size = 4096;
		opts.dtor = my_dtor;
		munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
		for (i = 0; i < 4; i++)
			munit_assert_not_null((o[i] = xtc_slab_alloc(s)));
		for (i = 0; i < 4; i++)
			xtc_slab_free(s, o[i]);
		munit_assert_int(atomic_load(&g_dtor_count), ==, 4);
		/* Destroy must add nothing: everything was already freed. */
		xtc_slab_destroy(s);
		munit_assert_int(atomic_load(&g_dtor_count), ==, 4);
	}
	return MUNIT_OK;
}
/*
 * Same double-destructor defect, exercised through the SLOW free path
 * too (NO_MAGAZINE), which must keep behaving: a freed object's dtor
 * runs exactly once, and a STILL-LIVE object's dtor runs exactly once
 * at destroy.
 */
static MunitResult
test_destroy_dtor_once(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *o[4];
	int i;
	(void)p; (void)d;

	/* Magazine path: free everything, destroy, dtor count stays == n. */
	atomic_store(&g_dtor_count, 0);
	opts.name = "dtor_once_mag"; opts.obj_size = sizeof(int);
	opts.dtor = my_dtor;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	for (i = 0; i < 4; i++) {
		o[i] = xtc_slab_alloc(s);
		munit_assert_not_null(o[i]);
	}
	for (i = 0; i < 4; i++)
		xtc_slab_free(s, o[i]);
	munit_assert_int(atomic_load(&g_dtor_count), ==, 4);
	xtc_slab_destroy(s);
	munit_assert_int(atomic_load(&g_dtor_count), ==, 4);

	/* Same with the magazine disabled (slot goes straight back to the
	 * chunk free list): still exactly once each. */
	atomic_store(&g_dtor_count, 0);
	opts.name = "dtor_once_nomag";
	opts.flags = XTC_SLAB_NO_MAGAZINE;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	for (i = 0; i < 4; i++)
		munit_assert_not_null((o[i] = xtc_slab_alloc(s)));
	for (i = 0; i < 4; i++)
		xtc_slab_free(s, o[i]);
	munit_assert_int(atomic_load(&g_dtor_count), ==, 4);
	xtc_slab_destroy(s);
	munit_assert_int(atomic_load(&g_dtor_count), ==, 4);

	/* A genuinely LIVE object at destroy time still gets its one dtor
	 * call -- the drain must not suppress that. */
	atomic_store(&g_dtor_count, 0);
	opts.name = "dtor_live"; opts.flags = 0;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	munit_assert_not_null((o[0] = xtc_slab_alloc(s)));
	munit_assert_not_null((o[1] = xtc_slab_alloc(s)));
	xtc_slab_free(s, o[0]);            /* one dtor here */
	munit_assert_int(atomic_load(&g_dtor_count), ==, 1);
	xtc_slab_destroy(s);               /* one dtor for the live o[1] */
	munit_assert_int(atomic_load(&g_dtor_count), ==, 2);
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
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
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
	/* Next alloc would need a new chunk -> res.acquire fails -> OOM. */
	o = xtc_slab_alloc(s);
	munit_assert_null(o);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.oom_fails, >=, 1);

	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/* ----- The cache lock is a preemption-safe hold -------------------
 *
 * A fiber involuntarily preempted while holding a slab's cache lock,
 * and a loop-mate on the same OS thread then taking the slow path of
 * the same slab, deadlock that thread: the holder can only release by
 * being rescheduled on it (test_wal_kill preempt-nokill hung this way,
 * two fibers in xtc_slab_alloc's slow path, the mutex owned by the
 * blocked thread).  The preemption timer declines while the thread's
 * unsafe depth is > 0, so every hold of the cache lock must raise it.
 * Observed from inside the lock: a res high-water alert fires in
 * __chunk_new, which the alloc slow path runs holding the cache lock;
 * the ctor runs after the lock is dropped (the control: depth 0). */
static int g_depth_in_alert = -1, g_depth_in_ctor = -1;

static void
depth_alert(xtc_res_kind_t k, int64_t used, int64_t cap, void *user)
{
	(void)k; (void)used; (void)cap; (void)user;
	g_depth_in_alert = __xtc_unsafe_depth();
}

static int
depth_ctor(void *obj, void *user)
{
	(void)obj; (void)user;
	g_depth_in_ctor = __xtc_unsafe_depth();
	return XTC_OK;
}

static MunitResult
test_lock_hold_preempt_safe(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_res_t res;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	void *o;
	(void)p; (void)d;

	munit_assert_int(xtc_res_init(&res, &caps), ==, XTC_OK);
	munit_assert_int(xtc_res_set_alert(&res, XTC_RES_MEM_BYTES, 1e-9), ==,
	    XTC_OK);
	munit_assert_int(xtc_res_set_alert_fn(&res, depth_alert, NULL), ==,
	    XTC_OK);
	opts.name = "preempt-safe"; opts.obj_size = 64;
	opts.res = &res;
	opts.flags |= XTC_SLAB_NO_MAGAZINE;   /* every alloc: the slow path */
	opts.ctor = depth_ctor;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	munit_assert_int(__xtc_unsafe_depth(), ==, 0);
	o = xtc_slab_alloc(s);                /* first chunk: alert fires */
	munit_assert_not_null(o);
	munit_assert_int(g_depth_in_alert, >=, 1);   /* lock held: no preempt */
	munit_assert_int(g_depth_in_ctor, ==, 0);    /* lock dropped */
	munit_assert_int(__xtc_unsafe_depth(), ==, 0);
	xtc_slab_free(s, o);
	(void)xtc_slab_reap(s);
	munit_assert_int(__xtc_unsafe_depth(), ==, 0);   /* balanced */
	xtc_slab_destroy(s);
	munit_assert_int(__xtc_unsafe_depth(), ==, 0);
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
	xtc_slab_stats_t st_before = XTC_SLAB_STATS_INIT;
	xtc_slab_stats_t st_after = XTC_SLAB_STATS_INIT;
	(void)p; (void)d;
	opts.name = "reap"; opts.obj_size = 64;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	for (i = 0; i < 200; i++) objs[i] = xtc_slab_alloc(s);
	for (i = 0; i < 200; i++) xtc_slab_free(s, objs[i]);
	(void)xtc_slab_stat(s, &st_before);

	/* Reap every registered slab (this one included); returns the
	 * total chunks reclaimed across the registry (>= 0). */
	munit_assert_int(xtc_slab_reap_all(), >=, 0);

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
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
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

/*
 * REDZONE must not break the requested object alignment.  The slot base
 * is aligned to opts.align, but the returned object pointer was the slot
 * plus a FIXED 16-byte front guard, so a default 64-byte alignment came
 * back 16 mod 64 -- the over-alignment case AGENTS.md calls out (UBSan
 * alignment trap, and a fault on stricter targets).  Enabling a
 * DIAGNOSTIC must not weaken a guarantee.  Fails without the rz_front
 * round-up in xtc_slab_create.
 */
static MunitResult
test_redzone_alignment(const MunitParameter p[], void *d)
{
	static const size_t aligns[] = { 8, 16, 32, 64, 128, 256 };
	static const size_t sizes[] = { 1, 8, 17, 64, 100 };
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
	uint8_t *o[3];
	size_t ai, si;
	int i;
	(void)p; (void)d;

	for (ai = 0; ai < sizeof aligns / sizeof aligns[0]; ai++) {
		for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
			opts = (xtc_slab_opts_t)XTC_SLAB_OPTS_DEFAULT;
			opts.name = "rz_align";
			opts.obj_size = sizes[si];
			opts.align = aligns[ai];
			opts.flags = XTC_SLAB_REDZONE;
			munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
			/* Several objects: every one, not just the first, must be
			 * aligned (the slot stride has to stay a multiple too). */
			for (i = 0; i < 3; i++) {
				o[i] = xtc_slab_alloc(s);
				munit_assert_not_null(o[i]);
				munit_assert_size((size_t)((uintptr_t)o[i] %
				    aligns[ai]), ==, 0);
				/* In-bounds write must stay clean. */
				memset(o[i], 'z', sizes[si]);
			}
			for (i = 0; i < 3; i++)
				xtc_slab_free(s, o[i]);
			(void)xtc_slab_stat(s, &st);
			munit_assert_uint64(st.redzone_violations, ==, 0);
			xtc_slab_destroy(s);
		}
	}

	/* The guards still catch an overrun AND an underrun once the front
	 * guard carries alignment padding -- the magic words must sit
	 * immediately before the object, not at the slot base. */
	opts = (xtc_slab_opts_t)XTC_SLAB_OPTS_DEFAULT;
	opts.name = "rz_align_detect"; opts.obj_size = 16; opts.align = 64;
	opts.flags = XTC_SLAB_REDZONE;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	o[0] = xtc_slab_alloc(s);
	munit_assert_not_null(o[0]);
	munit_assert_size((size_t)((uintptr_t)o[0] % 64), ==, 0);
	memset(o[0] - 8, 'u', 8);          /* clobber the front magic */
	xtc_slab_free(s, o[0]);
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

/* ----- Single-process shm mode + offset/resolve ----------------- */
/*
 * NOTE: This test uses MAP_PRIVATE | MAP_ANONYMOUS, which is NOT actual
 * shared memory.  It only verifies offset/resolve work within a single
 * process.  For real cross-process tests, see test_slab_shm.c.
 */

static MunitResult
test_shm_offset_resolve_single_process(const MunitParameter p[], void *d)
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
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
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

/* ----- Magazine fast path vs involuntary preemption ---------- */

/*
 * The magazine is per THREAD, not per fiber.  An involuntary preemption
 * between the magazine pop and its completion hands the thread to
 * another fiber that pops from the same magazine: one slot, two owners
 * (seen as a corrupted free list, a SEGV in __pop_slot_locked, and an
 * underflowed mailbox counter under test_wal_kill preempt -- every
 * message envelope is a slab object).  The "slab.magazine_window"
 * injection point fires inside the window; its callback raises the REAL
 * preemption signal on the fiber's own thread, so the handler takes the
 * decision a timer tick there would.  Control: the same raise from
 * plain fiber code does preempt (the peer runs before raise returns),
 * so a declined raise inside the window is not vacuous.  Skipped where
 * the involuntary redirect is not implemented and under sanitizers.
 */
#include <signal.h>
#include "xtc_exec.h"
/* No preemption signal on Windows (the preemption tests skip there). */
#if defined(SIGVTALRM)
#define PREEMPT_TICK() ((void)raise(SIGVTALRM))
#else
#define PREEMPT_TICK() ((void)0)
#endif
#include "xtc_inject.h"
#include "xtc_preempt.h"
#include "xtc_proc.h"

static atomic_long g_mp_peer_runs;
static atomic_int  g_mp_arm, g_mp_hits, g_mp_stop;
static atomic_int  g_mp_ctrl_preempted, g_mp_win_preempted;
static void       *g_mp_obj;

static void
mp_raise_in_window(const char *name, void *user)
{
	long before;
	(void)name; (void)user;
	if (!atomic_exchange(&g_mp_arm, 0))
		return;
	atomic_fetch_add(&g_mp_hits, 1);
	before = atomic_load(&g_mp_peer_runs);
	PREEMPT_TICK();              /* a preemption tick, here */
	if (atomic_load(&g_mp_peer_runs) != before)
		atomic_store(&g_mp_win_preempted, 1);
}

static void
mp_peer(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_mp_stop)) {
		atomic_fetch_add(&g_mp_peer_runs, 1);
		xtc_yield();
	}
}

static void
mp_owner(void *arg)
{
	long before;
	void *o;
	(void)arg;
	xtc_yield();                          /* let the peer start */
	before = atomic_load(&g_mp_peer_runs);
	PREEMPT_TICK();               /* control: plain fiber code */
	if (atomic_load(&g_mp_peer_runs) != before)
		atomic_store(&g_mp_ctrl_preempted, 1);
	o = xtc_slab_alloc(g_slab);           /* fill this thread's magazine */
	xtc_slab_free(g_slab, o);
	atomic_store(&g_mp_arm, 1);
	g_mp_obj = xtc_slab_alloc(g_slab);    /* magazine hit: the window */
	xtc_slab_free(g_slab, g_mp_obj);
	atomic_store(&g_mp_stop, 1);
}

static MunitResult
test_magazine_preempt(const MunitParameter p[], void *d)
{
	extern int __xtc_coro_preempt_effective(void);
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	(void)p; (void)d;
	if (!xtc_preempt_supported() || !__xtc_coro_preempt_effective())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test/m14/test_preempt_p2.c */
#endif
	opts.name = "mag-preempt"; opts.obj_size = 64;
	munit_assert_int(xtc_slab_create(&opts, &g_slab), ==, XTC_OK);
	atomic_store(&g_mp_peer_runs, 0);
	atomic_store(&g_mp_arm, 0);
	atomic_store(&g_mp_hits, 0);
	atomic_store(&g_mp_stop, 0);
	atomic_store(&g_mp_ctrl_preempted, 0);
	atomic_store(&g_mp_win_preempted, 0);
	munit_assert_int(xtc_inject_attach("slab.magazine_window",
	    mp_raise_in_window, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);
	/* Arms the handler + unblocks the signal on the worker; the 10 s
	 * CPU-time interval never elapses, so every tick is our raise. */
	munit_assert_int(xtc_exec_set_preempt(e, 10LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	munit_assert_int(xtc_proc_spawn(l, mp_owner, NULL, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(l, mp_peer, NULL, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	(void)xtc_inject_detach("slab.magazine_window");
	xtc_slab_destroy(g_slab);
	if (atomic_load(&g_mp_hits) == 0)
		return MUNIT_SKIP;   /* injection points compiled out */
	munit_assert_int(atomic_load(&g_mp_ctrl_preempted), ==, 1);
	munit_assert_int(atomic_load(&g_mp_win_preempted), ==, 0);
	return MUNIT_OK;
}

/* ----- Striped statistics ------------------------------------ */

/*
 * The hot-path counters are per-thread stripes summed on read, and
 * n_inuse is derived (allocs - ctor refusals - frees).  An object
 * allocated on one thread and freed on another lands its +1 and -1 in
 * DIFFERENT stripes; the sums must still be exact once both threads
 * are quiescent.
 */
#define XS_N 1000
static void *g_xs_objs[XS_N];

static void *
xs_alloc_worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < XS_N; i++)
		g_xs_objs[i] = xtc_slab_alloc(g_slab);
	return NULL;
}

static void *
xs_free_worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < XS_N / 2; i++)
		xtc_slab_free(g_slab, g_xs_objs[i]);
	return NULL;
}

static MunitResult
test_stats_cross_thread(const MunitParameter p[], void *d)
{
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	pthread_t th;
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
	int i;
	(void)p; (void)d;
	opts.name = "xs"; opts.obj_size = 48;
	munit_assert_int(xtc_slab_create(&opts, &g_slab), ==, XTC_OK);
	munit_assert_int(pthread_create(&th, NULL, xs_alloc_worker, NULL), ==, 0);
	munit_assert_int(pthread_join(th, NULL), ==, 0);
	for (i = 0; i < XS_N; i++)
		munit_assert_not_null(g_xs_objs[i]);
	munit_assert_int(pthread_create(&th, NULL, xs_free_worker, NULL), ==, 0);
	munit_assert_int(pthread_join(th, NULL), ==, 0);

	(void)xtc_slab_stat(g_slab, &st);
	munit_assert_uint64(st.alloc_fast + st.alloc_slow, ==, XS_N);
	munit_assert_uint64(st.free_fast + st.free_slow, ==, XS_N / 2);
	munit_assert_uint64(st.n_inuse, ==, XS_N / 2);
	munit_assert_uint64(st.bytes_inuse, ==, (uint64_t)(XS_N / 2) * 48);

	/* This thread frees the rest: a third stripe. */
	for (i = XS_N / 2; i < XS_N; i++)
		xtc_slab_free(g_slab, g_xs_objs[i]);
	(void)xtc_slab_stat(g_slab, &st);
	munit_assert_uint64(st.n_inuse, ==, 0);
	munit_assert_uint64(st.free_fast + st.free_slow, ==, XS_N);
	xtc_slab_destroy(g_slab);
	return MUNIT_OK;
}

/* A refused constructor hands back no object, so it must not count as
 * in use (n_inuse is derived from the alloc counters, which the refusal
 * path backs out). */
static int g_refuse_every_other;

static int
refusing_ctor(void *obj, void *user)
{
	(void)obj; (void)user;
	return (g_refuse_every_other++ & 1) ? XTC_E_INVAL : XTC_OK;
}

static MunitResult
test_stats_ctor_refused(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
	void *got[8];
	int i, n = 0;
	(void)p; (void)d;
	g_refuse_every_other = 0;
	opts.name = "refuse"; opts.obj_size = 32; opts.ctor = refusing_ctor;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	for (i = 0; i < 8; i++) {
		void *o = xtc_slab_alloc(s);
		if (o != NULL)
			got[n++] = o;
	}
	munit_assert_int(n, ==, 4);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.n_inuse, ==, 4);
	for (i = 0; i < n; i++)
		xtc_slab_free(s, got[i]);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.n_inuse, ==, 0);
	xtc_slab_destroy(s);
	return MUNIT_OK;
}

/*
 * Threads allocating and freeing on ONE shared cache must scale.  With a
 * single shared set of stats counters every alloc/free was a locked RMW
 * on the same cache line, so 4 threads together did LESS than one thread
 * alone (measured on a 64-vCPU x86-64 host: aggregate 0.28x of a single
 * thread at 2, 4 and 8 threads); the magazine was per-thread but its
 * statistics were not.  Striped per thread: 3.98x at 4 threads.  The
 * bound (aggregate >= 1.5x single) leaves wide margin both ways.  Needs
 * 4 free CPUs; skipped under sanitizers/valgrind, whose instrumentation
 * of every access swamps the line-bouncing being measured.
 */
#define SC_THREADS 4
#define SC_ROUNDS  200000
static _Atomic int g_sc_go;

static void *
sc_worker(void *arg)
{
	void *o[8];
	long r;
	int j;
	(void)arg;
	while (!atomic_load(&g_sc_go))
		;
	for (r = 0; r < SC_ROUNDS; r++) {
		for (j = 0; j < 8; j++)
			o[j] = xtc_slab_alloc(g_slab);
		for (j = 0; j < 8; j++)
			xtc_slab_free(g_slab, o[j]);
	}
	return NULL;
}

static double
sc_run(int n)
{
	pthread_t th[SC_THREADS];
	int64_t t0, t1;
	int i;
	atomic_store(&g_sc_go, 0);
	for (i = 0; i < n; i++)
		munit_assert_int(pthread_create(&th[i], NULL, sc_worker, NULL),
		    ==, 0);
	t0 = xtc_clock_mono();
	atomic_store(&g_sc_go, 1);
	for (i = 0; i < n; i++)
		(void)pthread_join(th[i], NULL);
	t1 = xtc_clock_mono();
	return (double)n * SC_ROUNDS * 16 / (double)(t1 - t0 > 0 ? t1 - t0 : 1);
}

static MunitResult
test_stats_scale(const MunitParameter p[], void *d)
{
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	double one, all;
	(void)p; (void)d;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;
#endif
	if (getenv("XTC_UNDER_VALGRIND") != NULL || __os_ncpus() < SC_THREADS)
		return MUNIT_SKIP;
	opts.name = "scale"; opts.obj_size = 64;
	munit_assert_int(xtc_slab_create(&opts, &g_slab), ==, XTC_OK);
	(void)sc_run(1);                  /* warm: magazines, chunks */
	one = sc_run(1);
	all = sc_run(SC_THREADS);
	xtc_slab_destroy(g_slab);
	munit_logf(MUNIT_LOG_INFO, "1 thread %.1f M/s, %d threads %.1f M/s "
	    "(%.2fx)", one * 1e3, SC_THREADS, all * 1e3, all / one);
	munit_assert_double(all, >=, 1.5 * one);
	return MUNIT_OK;
}

/* ----- Backtrace + reaper ------------------------------------- */

static MunitResult
test_backtrace(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *o;
	(void)p; (void)d;
	opts.name = "bt"; opts.obj_size = 32;
	opts.flags = XTC_SLAB_AUDIT | XTC_SLAB_BACKTRACE;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	o = xtc_slab_alloc(s);
	munit_assert_not_null(o);
	xtc_slab_free(s, o);
	/* No public accessor for bt frames; we just verify the path
	 * doesn't crash on Linux/glibc and is a no-op elsewhere. */
	xtc_slab_destroy(s);
	return MUNIT_OK;
}

static MunitResult
test_pressure_listen_ex_stop(const MunitParameter p[], void *d)
{
	xtc_slab_pressure_t *h = NULL;
	int rc;
	(void)p; (void)d;

	/* NULL out-pointer is rejected. */
	munit_assert_int(xtc_slab_pressure_listen_ex(NULL, NULL, NULL, NULL),
	    ==, XTC_E_INVAL);
	/* Stopping a NULL handle is rejected, not a crash. */
	munit_assert_int(xtc_slab_pressure_stop(NULL), ==, XTC_E_INVAL);

	rc = xtc_slab_pressure_listen_ex(NULL, NULL, NULL, &h);
	if (rc == XTC_E_NOSYS) {
		/* No PSI on this platform/kernel/container -- nothing to stop. */
		munit_assert_ptr_null(h);
		return MUNIT_SKIP;
	}
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_ptr_not_null(h);
	/* Clean stop: joins the listener thread and frees the handle. */
	munit_assert_int(xtc_slab_pressure_stop(h), ==, XTC_OK);
	return MUNIT_OK;
}

/* ----- SHARED_MEMORY cross-process reclamation ----------------- *
 *
 * The shared region's free list is threaded through region-relative
 * offsets so a slot freed in any attached process is reusable by any
 * other.  These two tests prove (1) in-process reuse pops the freed
 * slot back, and (2) a fork()ed child reuses a slot the parent freed
 * (and vice versa) through a MAP_SHARED segment mapped -- in general
 * -- at a different address in each process.
 */
static MunitResult
test_shm_reclaim_single_process(const MunitParameter pa[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	size_t shm_size = 256 * 1024;
	void *shm_base, *a, *b, *c;
	xtc_slab_stats_t st = XTC_SLAB_STATS_INIT;
	(void)pa; (void)d;

	shm_base = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	munit_assert_not_null(shm_base);
	opts.name = "shm_reuse"; opts.obj_size = 128;
	opts.mode = XTC_SLAB_SHARED_MEMORY;
	opts.shm_base = shm_base; opts.shm_size = shm_size;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);

	a = xtc_slab_alloc(s);
	munit_assert_not_null(a);
	/* Freeing then re-allocating must hand the SAME slot back: the
	 * free list popped what we just pushed (LIFO). */
	xtc_slab_free(s, a);
	b = xtc_slab_alloc(s);
	munit_assert_ptr_equal(a, b);

	/* A second live alloc must be distinct (cursor advanced). */
	c = xtc_slab_alloc(s);
	munit_assert_not_null(c);
	munit_assert_ptr_not_equal(b, c);

	(void)xtc_slab_stat(s, &st);
	/* One slow free recorded (no magazine in shm mode). */
	munit_assert_uint64(st.free_slow, >=, 1);

	xtc_slab_free(s, b);
	xtc_slab_free(s, c);
	xtc_slab_destroy(s);
	(void)munmap(shm_base, shm_size);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic_alloc_free", test_basic_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reject_bad_obj_size", test_reject_bad_obj_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pressure_listen_ex_stop", test_pressure_listen_ex_stop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/magazine_fastpath", test_magazine_fastpath, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ctor_dtor",        test_ctor_dtor,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/destroy_dtor_once", test_destroy_dtor_once, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/word_slot_destroy", test_word_slot_destroy, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/oom_fail",         test_oom_fail,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lock_hold_preempt_safe", test_lock_hold_preempt_safe, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reap",             test_reap,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/redzone",          test_redzone,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/redzone_alignment", test_redzone_alignment, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/audit",            test_audit,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shm_offset_single_process", test_shm_offset_resolve_single_process, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shm_reclaim_single_process", test_shm_reclaim_single_process, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent",       test_concurrent,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stats_cross_thread", test_stats_cross_thread, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stats_ctor_refused", test_stats_ctor_refused, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stats_scale",      test_stats_scale,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/magazine_preempt", test_magazine_preempt, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/backtrace",        test_backtrace,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m11/slab", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

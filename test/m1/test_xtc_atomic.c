/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_xtc_atomic.c
 *	The public atomics (xtc_atomic.h) and compiler directives
 *	(xtc_compiler.h): every operation on every type under every order it
 *	accepts compiles and gives the C11 result; contended RMW and CAS
 *	loops are exact; a release/acquire publication is seen whole; the
 *	pointer operations keep the operand's type; the bit scans agree
 *	with a reference loop.
 *
 *	Built twice: test_xtc_atomic (the compiler's own backend, __atomic
 *	builtins on gcc/clang) and test_xtc_atomic_c11 (-DXTC_ATOMIC_FORCE_C11,
 *	the <stdatomic.h> fallback), so both backends run here.  The MSVC
 *	backend is compiled by test/dist/test_atomic_cxx.sh (clang targeting
 *	*-windows-msvc) and built + run by the windows-msvc CI job.
 */

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"

/*
 * One block per integer type: each op, each order it accepts, against
 * the plain-C expected value.  `one` keeps the arithmetic in T.
 */
/* The (success, failure) pairs C11 allows for a CAS. */
static const xtc_mo_t cas_mo[][2] = {
	{ xtc_mo_relaxed, xtc_mo_relaxed },
	{ xtc_mo_acquire, xtc_mo_relaxed },
	{ xtc_mo_acquire, xtc_mo_acquire },
	{ xtc_mo_release, xtc_mo_relaxed },
	{ xtc_mo_acq_rel, xtc_mo_relaxed },
	{ xtc_mo_acq_rel, xtc_mo_acquire },
	{ xtc_mo_seq_cst, xtc_mo_relaxed },
	{ xtc_mo_seq_cst, xtc_mo_acquire },
	{ xtc_mo_seq_cst, xtc_mo_seq_cst },
};
static const xtc_mo_t ld_mo[] = { xtc_mo_relaxed, xtc_mo_acquire,
	xtc_mo_seq_cst };
static const xtc_mo_t st_mo[] = { xtc_mo_relaxed, xtc_mo_release,
	xtc_mo_seq_cst };
static const xtc_mo_t rmw_mo[] = { xtc_mo_relaxed, xtc_mo_acquire,
	xtc_mo_release, xtc_mo_acq_rel, xtc_mo_seq_cst };

/*
 * One block per integer type: each op under each order it accepts,
 * against the plain-C expected value.
 */
#define CHECK_TYPE(S, T)                                                       \
	do {                                                                   \
		T v = 0, e, one = 1;                                           \
		size_t i;                                                      \
		for (i = 0; i < 3; i++) {                                      \
			xtc_atomic_store_##S(&v, (T)(5 + i), st_mo[i]);        \
			munit_assert_true(v == (T)(5 + i));                    \
			munit_assert_true(                                     \
			    xtc_atomic_load_##S(&v, ld_mo[i]) == (T)(5 + i));  \
		}                                                              \
		for (i = 0; i < 5; i++) {                                      \
			xtc_mo_t mo = rmw_mo[i];                               \
			v = 12;                                                \
			munit_assert_true(                                     \
			    xtc_atomic_exchange_##S(&v, (T)9, mo) == (T)12 &&  \
			    v == (T)9);                                        \
			munit_assert_true(                                     \
			    xtc_atomic_fetch_add_##S(&v, (T)3, mo) == (T)9 &&  \
			    v == (T)12);                                       \
			munit_assert_true(                                     \
			    xtc_atomic_fetch_sub_##S(&v, (T)2, mo) == (T)12 && \
			    v == (T)10);                                       \
			munit_assert_true(                                     \
			    xtc_atomic_fetch_and_##S(&v, (T)6, mo) == (T)10 && \
			    v == (T)2);                                        \
			munit_assert_true(                                     \
			    xtc_atomic_fetch_or_##S(&v, (T)5, mo) == (T)2 &&   \
			    v == (T)7);                                        \
			munit_assert_true(                                     \
			    xtc_atomic_fetch_xor_##S(&v, (T)3, mo) == (T)7 &&  \
			    v == (T)4);                                        \
			v = 0; /* modular, as in C11 */                        \
			(void)xtc_atomic_fetch_sub_##S(&v, one, mo);           \
			munit_assert_true(v == (T)(0 - one));                  \
		}                                                              \
		for (i = 0; i < sizeof cas_mo / sizeof cas_mo[0]; i++) {       \
			xtc_mo_t ok = cas_mo[i][0], bad = cas_mo[i][1];        \
			v = 4;                                                 \
			e = 4;                                                 \
			munit_assert_int(xtc_atomic_cas_##S(&v, &e, (T)8, ok,  \
			                     bad),                             \
			    ==, 1);                                            \
			munit_assert_true(v == (T)8 && e == (T)4);             \
			e = 4;                                                 \
			munit_assert_int(xtc_atomic_cas_##S(&v, &e, (T)1, ok,  \
			                     bad),                             \
			    ==, 0);                                            \
			munit_assert_true(v == (T)8 && e == (T)8);             \
			while (                                                \
			    !xtc_atomic_cas_weak_##S(&v, &e, (T)3, ok, bad))   \
				munit_assert_true(e == (T)8);                  \
			munit_assert_true(v == (T)3);                          \
			e = 8;                                                 \
			munit_assert_int(xtc_atomic_cas_weak_##S(&v, &e, (T)1, \
			                     ok, bad),                         \
			    ==, 0);                                            \
			munit_assert_true(v == (T)3 && e == (T)3);             \
		}                                                              \
	} while (0)

static MunitResult
test_ops(const MunitParameter p[], void *d)
{
	(void)p;
	(void)d;
	CHECK_TYPE(u8, uint8_t);
	CHECK_TYPE(i32, int32_t);
	CHECK_TYPE(u32, uint32_t);
	CHECK_TYPE(i64, int64_t);
	CHECK_TYPE(u64, uint64_t);
	CHECK_TYPE(long, long);
	CHECK_TYPE(ulong, unsigned long);
	CHECK_TYPE(llong, long long);
	CHECK_TYPE(ullong, unsigned long long);
	CHECK_TYPE(size, size_t);
	CHECK_TYPE(iptr, intptr_t);
	CHECK_TYPE(uptr, uintptr_t);
	return MUNIT_OK;
}

/* Full-width values survive a round trip (no truncation to 32 bits). */
static MunitResult
test_wide(const MunitParameter p[], void *d)
{
	uint64_t u = 0;
	int64_t i = 0;
	size_t z = 0;
	(void)p;
	(void)d;
	xtc_atomic_store_u64(&u, 0xfeedfacecafebabeULL, xtc_mo_release);
	munit_assert_uint64(xtc_atomic_load_u64(&u, xtc_mo_acquire), ==,
	    0xfeedfacecafebabeULL);
	munit_assert_uint64(xtc_atomic_fetch_add_u64(&u, 0x100000000ULL,
	                        xtc_mo_relaxed),
	    ==, 0xfeedfacecafebabeULL);
	munit_assert_uint64(u, ==, 0xfeedfacecafebabeULL + 0x100000000ULL);
	xtc_atomic_store_i64(&i, INT64_MIN, xtc_mo_seq_cst);
	munit_assert_int64(xtc_atomic_exchange_i64(&i, INT64_MAX,
	                       xtc_mo_acq_rel),
	    ==, INT64_MIN);
	munit_assert_int64(i, ==, INT64_MAX);
	xtc_atomic_store_size(&z, SIZE_MAX, xtc_mo_relaxed);
	munit_assert_size(xtc_atomic_load_size(&z, xtc_mo_relaxed), ==,
	    SIZE_MAX);
	return MUNIT_OK;
}

struct node {
	int v;
	struct node *next;
};
typedef int (*fn_t)(int);
static int
twice(int x)
{
	return 2 * x;
}
static int
thrice(int x)
{
	return 3 * x;
}

/* The pointer operations take any T ** and give back a T *. */
static MunitResult
test_ptr(const MunitParameter p[], void *d)
{
	struct node a = { 1, NULL }, b = { 2, NULL };
	struct node *head = NULL, *e, *got;
	void *vp = NULL, *ve;
	const char *cs = "x", *cs2;
	fn_t f = NULL, fe;
	(void)p;
	(void)d;

	xtc_atomic_store_ptr(&head, &a, xtc_mo_release);
	got = xtc_atomic_load_ptr(&head, xtc_mo_acquire);
	munit_assert_ptr(got, ==, &a);
	got = xtc_atomic_load_ptr(&head, xtc_mo_relaxed);
	munit_assert_int(got->v, ==, 1);
	e = &a;
	munit_assert_int(xtc_atomic_cas_ptr(&head, &e, &b, xtc_mo_acq_rel,
	                     xtc_mo_acquire),
	    ==, 1);
	munit_assert_ptr(head, ==, &b);
	e = &a;
	munit_assert_int(xtc_atomic_cas_ptr(&head, &e, NULL, xtc_mo_seq_cst,
	                     xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_ptr(e, ==, &b);
	while (!xtc_atomic_cas_weak_ptr(&head, &e, &a, xtc_mo_release,
	    xtc_mo_relaxed))
		munit_assert_ptr(e, ==, &b);
	munit_assert_ptr(xtc_atomic_exchange_ptr(&head, NULL, xtc_mo_acq_rel),
	    ==, &a);
	munit_assert_null(head);

	xtc_atomic_store_ptr(&vp, &a, xtc_mo_seq_cst);
	ve = &a;
	munit_assert_int(xtc_atomic_cas_ptr(&vp, &ve, &b, xtc_mo_relaxed,
	                     xtc_mo_relaxed),
	    ==, 1);
	munit_assert_ptr(xtc_atomic_load_ptr(&vp, xtc_mo_seq_cst), ==, &b);

	cs2 = xtc_atomic_load_ptr(&cs, xtc_mo_acquire);
	munit_assert_char(cs2[0], ==, 'x');

#if (defined(__GNUC__) || defined(__clang__)) && !defined(XTC_ATOMIC_FORCE_C11)
	/* On gcc/clang the result has the operand's own type, so it can be
	 * dereferenced or called directly. */
	munit_assert_int(xtc_atomic_load_ptr(&cs, xtc_mo_relaxed)[0], ==, 'x');
#endif
#if !defined(XTC_ATOMIC_FORCE_C11)
	/* function pointers (not on the C11 fallback: ISO C cannot carry
	 * one through void *); the cast is the portable spelling */
	xtc_atomic_store_ptr(&f, twice, xtc_mo_release);
	fe = (fn_t)xtc_atomic_load_ptr(&f, xtc_mo_acquire);
	munit_assert_int(fe(4), ==, 8);
	munit_assert_int(xtc_atomic_cas_ptr(&f, &fe, thrice, xtc_mo_acq_rel,
	                     xtc_mo_acquire),
	    ==, 1);
	fe = (fn_t)xtc_atomic_exchange_ptr(&f, twice, xtc_mo_seq_cst);
	munit_assert_int(fe(4), ==, 12);
	munit_assert_int(f(5), ==, 10);
#else
	(void)f;
	(void)fe;
	(void)twice;
	(void)thrice;
#endif
	return MUNIT_OK;
}

static MunitResult
test_fences(const MunitParameter p[], void *d)
{
	xtc_mo_t all[] = { xtc_mo_relaxed, xtc_mo_acquire, xtc_mo_release,
		xtc_mo_acq_rel, xtc_mo_seq_cst };
	size_t i;
	(void)p;
	(void)d;
	for (i = 0; i < 5; i++) {
		xtc_atomic_thread_fence(all[i]);
		xtc_atomic_signal_fence(all[i]);
	}
	XTC_CPU_RELAX();
	return MUNIT_OK;
}

/* Contended RMW: relaxed fetch_add, acq_rel fetch_sub and a weak-CAS
 * increment loop from 8 threads all come out exact. */
#define NTHR 8
#define NITER 100000
static uint64_t g_add;
static int64_t g_sub;
static uint32_t g_cas;
static uint8_t g_bits;

static void *
rmw_worker(void *arg)
{
	int i, me = (int)(intptr_t)arg;
	uint32_t c;

	for (i = 0; i < NITER; i++) {
		(void)xtc_atomic_fetch_add_u64(&g_add, 1, xtc_mo_relaxed);
		(void)xtc_atomic_fetch_sub_i64(&g_sub, 2, xtc_mo_acq_rel);
		c = xtc_atomic_load_u32(&g_cas, xtc_mo_relaxed);
		while (!xtc_atomic_cas_weak_u32(&g_cas, &c, c + 1,
		    xtc_mo_acq_rel, xtc_mo_relaxed))
			XTC_CPU_RELAX();
	}
	(void)xtc_atomic_fetch_or_u8(&g_bits, (uint8_t)(1u << me),
	    xtc_mo_release);
	return NULL;
}

static MunitResult
test_contended(const MunitParameter p[], void *d)
{
	pthread_t th[NTHR];
	int i;
	(void)p;
	(void)d;
	g_add = 0;
	g_sub = 0;
	g_cas = 0;
	g_bits = 0;
	for (i = 0; i < NTHR; i++)
		munit_assert_int(pthread_create(&th[i], NULL, rmw_worker,
		                     (void *)(intptr_t)i),
		    ==, 0);
	for (i = 0; i < NTHR; i++)
		munit_assert_int(pthread_join(th[i], NULL), ==, 0);
	munit_assert_uint64(g_add, ==, (uint64_t)NTHR * NITER);
	munit_assert_int64(g_sub, ==, -2 * (int64_t)NTHR * NITER);
	munit_assert_uint32(g_cas, ==, (uint32_t)NTHR * NITER);
	munit_assert_uint8(xtc_atomic_load_u8(&g_bits, xtc_mo_acquire), ==,
	    0xff);
	return MUNIT_OK;
}

/* Release/acquire publication through a plain pointer field: a reader
 * that sees the pointer sees every write made before the release. */
#define NMSG 20000
struct msg {
	uint64_t a, b;
};
static struct msg g_msgs[NMSG];
static struct msg *g_slot;
static int g_torn;

static void *
pub_reader(void *arg)
{
	int seen = 0;
	struct msg *m;
	(void)arg;
	while (seen < NMSG) {
		m = xtc_atomic_load_ptr(&g_slot, xtc_mo_acquire);
		if (m == NULL) {
			XTC_CPU_RELAX();
			continue;
		}
		if (m->a + 1 != m->b)
			g_torn++;
		xtc_atomic_store_ptr(&g_slot, NULL, xtc_mo_release);
		seen++;
	}
	return NULL;
}

static MunitResult
test_publish(const MunitParameter p[], void *d)
{
	pthread_t th;
	int i;
	struct msg *expect;
	(void)p;
	(void)d;
	g_slot = NULL;
	g_torn = 0;
	munit_assert_int(pthread_create(&th, NULL, pub_reader, NULL), ==, 0);
	for (i = 0; i < NMSG; i++) {
		g_msgs[i].a = (uint64_t)i * 7;
		g_msgs[i].b = (uint64_t)i * 7 + 1;
		expect = NULL;
		while (!xtc_atomic_cas_ptr(&g_slot, &expect, &g_msgs[i],
		    xtc_mo_release, xtc_mo_relaxed)) {
			expect = NULL;
			XTC_CPU_RELAX();
		}
	}
	munit_assert_int(pthread_join(th, NULL), ==, 0);
	munit_assert_int(g_torn, ==, 0);
	return MUNIT_OK;
}

/* The superseded out-of-line pair still works (ABI). */
static MunitResult
test_legacy(const MunitParameter p[], void *d)
{
	int64_t v = 40;
	(void)p;
	(void)d;
	munit_assert_int64(xtc_atomic_i64_add(&v, 2), ==, 40);
	munit_assert_int64(xtc_atomic_i64_load(&v), ==, 42);
	return MUNIT_OK;
}

/* ---- xtc_compiler.h ---- */

/* Reference bit counts, BOUNDED: a reference that loops on 0 is how
 * this test once spun forever (ref_ctz(0)). */
static int
ref_ctz(uint64_t v, int bits)
{
	int n = 0;
	while (n < bits && !(v & 1)) {
		v >>= 1;
		n++;
	}
	return n;
}
static int
ref_clz(uint64_t v, int bits)
{
	int n = 0;
	while (n < bits && !(v & ((uint64_t)1 << (bits - 1)))) {
		v <<= 1;
		n++;
	}
	return n;
}
static int
ref_pop(uint64_t v)
{
	int n = 0;
	for (; v != 0; v >>= 1)
		n += (int)(v & 1);
	return n;
}

static MunitResult
test_bits(const MunitParameter p[], void *d)
{
	uint64_t v = 0x9E3779B97F4A7C15ULL, b, lo, hi;
	uint32_t w;
	int i;
	(void)p;
	(void)d;
	for (i = 0; i < 64; i++) {
		b = (uint64_t)1 << i;
		lo = (v | 1) << i; /* lowest set bit is bit i: never 0 */
		hi = (v | ((uint64_t)1 << 63)) >> i; /* highest is 63 - i */
		munit_assert_int(xtc_ctz64(b), ==, i);
		munit_assert_int(xtc_clz64(b), ==, 63 - i);
		munit_assert_int(xtc_ctz64(lo), ==, ref_ctz(lo, 64));
		munit_assert_int(xtc_clz64(hi), ==, ref_clz(hi, 64));
		munit_assert_int(xtc_popcount64(v >> i), ==, ref_pop(v >> i));
		if (i < 32) {
			w = (uint32_t)v | 1u;
			munit_assert_int(xtc_ctz32((uint32_t)b), ==, i);
			munit_assert_int(xtc_clz32((uint32_t)b), ==, 31 - i);
			munit_assert_int(xtc_ctz32(w << i), ==,
			    ref_ctz(w << i, 32));
			munit_assert_int(xtc_clz32(w >> i), ==,
			    ref_clz(w >> i, 32));
			munit_assert_int(xtc_popcount32(w), ==, ref_pop(w));
		}
		v = v * 6364136223846793005ULL + 1442695040888963407ULL;
	}
	munit_assert_int(xtc_popcount64(0), ==, 0);
	munit_assert_int(xtc_popcount64(UINT64_MAX), ==, 64);
	munit_assert_int(xtc_popcount32(UINT32_MAX), ==, 32);
	return MUNIT_OK;
}

struct padded {
	char c;
	XTC_ALIGNED(XTC_CACHE_LINE) uint64_t hot;
};
static XTC_THREAD_LOCAL int t_local = 7;

static void *
tls_worker(void *arg)
{
	int *out = arg;
	*out = t_local; /* the initial value, not main's write */
	t_local = 99;
	return NULL;
}

static MunitResult
test_directives(const MunitParameter p[], void *d)
{
	struct padded pd;
	pthread_t th;
	int seen = 0, n = 0, i;
	(void)p;
	(void)d;
	munit_assert_size(_Alignof(struct padded), ==, XTC_CACHE_LINE);
	munit_assert_size((uintptr_t)&pd.hot % XTC_CACHE_LINE, ==, 0);
	t_local = 1;
	munit_assert_int(pthread_create(&th, NULL, tls_worker, &seen), ==, 0);
	munit_assert_int(pthread_join(th, NULL), ==, 0);
	munit_assert_int(seen, ==, 7);
	munit_assert_int(t_local, ==, 1);
	for (i = 0; i < 10; i++) {
		if (XTC_LIKELY(i != 3))
			n++;
		if (XTC_UNLIKELY(i == 3))
			n += 100;
	}
	munit_assert_int(n, ==, 109);
	return MUNIT_OK;
}

static MunitTest tests[] = { { "/ops", test_ops, NULL, NULL,
	                         MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wide", test_wide, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ptr", test_ptr, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fences", test_fences, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/contended", test_contended, NULL, NULL, MUNIT_TEST_OPTION_NONE,
	    NULL },
	{ "/publish", test_publish, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/legacy", test_legacy, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bits", test_bits, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/directives", test_directives, NULL, NULL, MUNIT_TEST_OPTION_NONE,
	    NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL } };

static const MunitSuite suite = {
#if defined(XTC_ATOMIC_FORCE_C11)
	"/m1/xtc_atomic_c11",
#else
	"/m1/xtc_atomic",
#endif
	tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}

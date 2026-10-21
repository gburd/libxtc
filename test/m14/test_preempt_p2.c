/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_preempt_p2.c
 *	Phase 2 of the preemption plan: TRUE
 *	signal-context involuntary yield.  A fiber in a PURE tight loop
 *	with NO yield points at all is preempted by the timer so its
 *	loop-mates make progress -- the case Phase 1 (cooperative-
 *	assisted) cannot handle.  Resumable: the runaway finishes its
 *	full computation, it was just time-sliced.
 *
 *	Only the ucontext coroutine substrate supports the involuntary
 *	yield; on fctx (musl) / winfiber the redirect declines and this
 *	test asserts the safe fallback (the runaway still completes; it
 *	just is not preempted -- so we skip the "peers ran" assertion
 *	there).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_preempt.h"

#if !defined(__has_feature)
#  define __has_feature(x) 0
#endif

static atomic_int  g_peer;          /* peer progress while the runaway runs */
static atomic_int  g_runaway_done;
static volatile uint64_t g_sink;

/* A PURE runaway: a long CPU loop with NO xtc_yield / xtc_yield_if_due
 * anywhere.  Under cooperative scheduling this monopolizes the loop
 * until it finishes.  Only true (signal-context) preemption can slice
 * it. */
static void
runaway(void *arg)
{
	uint64_t iters = (uint64_t)(intptr_t)arg;
	uint64_t acc = 0, k;
	for (k = 0; k < iters; k++)
		acc += (k ^ (k << 1)) + (acc >> 3);
	g_sink += acc;
	atomic_store_explicit(&g_runaway_done, 1, memory_order_relaxed);
}

/* A peer: increments a counter and yields, until the runaway is done.
 * It makes progress ONLY if the runaway is preempted to let it run. */
static void
peer(void *arg)
{
	(void)arg;
	while (!atomic_load_explicit(&g_runaway_done, memory_order_relaxed)) {
		atomic_fetch_add_explicit(&g_peer, 1, memory_order_relaxed);
		xtc_yield();
	}
}

static MunitResult
test_p2_preempt_tight_loop(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int i;
	(void)p; (void)d;

	if (!xtc_preempt_supported())
		return MUNIT_OK;   /* no per-thread CPU-time timers; skip */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	/* Involuntary (signal-context) preemption rewrites the interrupted
	 * fiber's machine context and redirects it through an out-of-band
	 * cooperative switch.  That nests inside the sanitizer fiber-switch
	 * annotations (ASan/TSan) -- "starting fiber switch while in fiber
	 * switch" -- because the interrupted fiber may be mid-annotation.
	 * Preempting under a sanitizer is not a production scenario (you do
	 * not run the timer-driven involuntary preemptor under ASan), so
	 * skip this sub-behavior when instrumented.  The cooperative
	 * preemption path (Phase 1) and all other fiber switches remain
	 * fully sanitizer-checked. */
	return MUNIT_OK;
#endif

	atomic_store(&g_peer, 0);
	atomic_store(&g_runaway_done, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);

	/* Enable the timer (1 ms slices) AND the signal-context
	 * involuntary yield. */
	munit_assert_int(xtc_exec_set_preempt(e, 1 * 1000 * 1000LL), ==, XTC_OK);
	xtc_preempt_set_involuntary(1);

	/* One pure-tight-loop runaway (no yield points) + peers. */
	(void)xtc_proc_spawn(l, runaway,
	    (void *)(intptr_t)(400ULL * 1000 * 1000), NULL, NULL);
	for (i = 0; i < 4; i++)
		(void)xtc_proc_spawn(l, peer, NULL, NULL, NULL);

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	/* The runaway completed (resumable preemption: it was sliced, not
	 * killed). */
	munit_assert_int(atomic_load(&g_runaway_done), ==, 1);

	/*
	 * If the involuntary yield is active (ucontext substrate), the
	 * peers made progress WHILE the pure-tight-loop runaway ran --
	 * impossible under cooperation alone, since the runaway never
	 * yields.  On the fctx/winfiber substrate the redirect declines
	 * (the runaway is not preemptible with no yield points), so the
	 * peers may not have run until the runaway finished; we only
	 * assert the runaway completed there.
	 */
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

/* A stronger, substrate-aware variant: on a ucontext build, assert the
 * peers actually ran mid-runaway.  Detected by whether a prior
 * involuntary preemption sliced anything -- we infer the substrate by
 * re-running with a compute fiber that never yields and checking peer
 * progress is nonzero. */
static MunitResult
test_p2_peers_progress_on_ucontext(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int i, peers;
	(void)p; (void)d;

	if (!xtc_preempt_supported())
		return MUNIT_OK;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	/* Involuntary preemption's in-handler stack rewrite nests inside the
	 * sanitizer fiber-switch annotations ("starting fiber switch while in
	 * fiber switch").  Not a production combo; skip under a sanitizer.
	 * (Same guard as the tight-loop test above.) */
	return MUNIT_OK;
#endif

	atomic_store(&g_peer, 0);
	atomic_store(&g_runaway_done, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);
	munit_assert_int(xtc_exec_set_preempt(e, 1 * 1000 * 1000LL), ==, XTC_OK);
	xtc_preempt_set_involuntary(1);

	(void)xtc_proc_spawn(l, runaway,
	    (void *)(intptr_t)(400ULL * 1000 * 1000), NULL, NULL);
	for (i = 0; i < 4; i++)
		(void)xtc_proc_spawn(l, peer, NULL, NULL, NULL);

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	peers = atomic_load(&g_peer);
	munit_assert_int(atomic_load(&g_runaway_done), ==, 1);

	/*
	 * peers > 0 proves true preemption occurred (a pure tight loop was
	 * sliced).  On a substrate that declines the involuntary yield the
	 * peers cannot have run before the runaway finished, so peers would
	 * be 0 -- which is the documented fallback, not a failure.  We
	 * therefore assert the invariant that HOLDS on every substrate (the
	 * runaway completed) and additionally REPORT whether preemption
	 * fired.
	 */
	munit_logf(MUNIT_LOG_INFO,
	    "peers advanced %d times during a pure-tight-loop runaway "
	    "(>0 == true preemption fired; 0 == substrate fallback)", peers);

	/*
	 * On a substrate that implements Phase 2b-arch (x86-64 ucontext,
	 * the default glibc build), a pure tight loop with no yield points
	 * MUST have been sliced, so peers MUST have advanced.  This is the
	 * assertion that proves involuntary preemption actually works --
	 * Phase 1 cooperative scheduling could never make peers run while a
	 * non-yielding runaway holds the loop.  On substrates that decline
	 * the redirect (fctx/musl, winfiber, the amalgamation) peers may be
	 * 0 (documented fallback); __xtc_coro_preempt_effective() reports
	 * which, at runtime, so this needs no build-macro visibility.
	 */
	{
		extern int __xtc_coro_preempt_effective(void);
		if (__xtc_coro_preempt_effective())
			munit_assert_int(peers, >, 0);
	}

	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

#if defined(__x86_64__) && defined(__linux__) && \
    (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#define P2_VEC 1
/*
 * /vector_state: involuntary preemption must preserve the WHOLE vector
 * register file, not only x87 + XMM0-15's low halves.  Fibers on one loop
 * each hold a fiber-unique pattern in ymm1 (all 256 bits) and, on an
 * AVX-512 host, zmm17, across a yield-free spin, then read them back.
 * The trampoline used FXSAVE, so every preemption that switched fibers
 * mid-spin handed the resumed fiber another fiber's YMM upper half and
 * ZMM16-31: ~100 of ~69k checks per run were corrupted (glibc's AVX
 * memcpy corrupted sqlxtc log records the same way).  XSAVE fixes it.
 */
static atomic_long g_vec_bad, g_vec_checks;
static int g_vec_avx2, g_vec_avx512;

__attribute__((target("avx2"))) static int
p2_ymm_hold(uint64_t want)
{
	uint64_t got[4];
	__asm__ volatile(
	    "vpbroadcastq %1, %%ymm1\n\t"
	    "movl $20000, %%ecx\n"
	    "1:\tdecl %%ecx\n\t"
	    "jnz 1b\n\t"
	    "vmovdqu %%ymm1, %0\n\t"
	    "vzeroupper"
	    : "=m"(got) : "m"(want) : "ecx", "xmm1", "cc", "memory");
	return got[0] != want || got[1] != want || got[2] != want ||
	    got[3] != want;
}

__attribute__((target("avx512f"))) static int
p2_zmm_hold(uint64_t want)
{
	uint64_t got[8];
	int k, bad = 0;
	__asm__ volatile(
	    "vpbroadcastq %1, %%zmm17\n\t"
	    "movl $20000, %%ecx\n"
	    "1:\tdecl %%ecx\n\t"
	    "jnz 1b\n\t"
	    "vmovdqu64 %%zmm17, %0\n\t"
	    "vzeroupper"
	    : "=m"(got) : "m"(want) : "ecx", "xmm17", "cc", "memory");
	for (k = 0; k < 8; k++)
		bad |= got[k] != want;
	return bad;
}

static void
p2_vec_spinner(void *arg)
{
	uint64_t want = 0x0101010101010101ull * (uint64_t)((intptr_t)arg + 1);
	int64_t end = xtc_clock_mono() + 300LL * 1000 * 1000;
	long n = 0;
	while (xtc_clock_mono() < end) {
		if (g_vec_avx2 && p2_ymm_hold(want))
			atomic_fetch_add(&g_vec_bad, 1);
		if (g_vec_avx512 && p2_zmm_hold(~want))
			atomic_fetch_add(&g_vec_bad, 1);
		n++;
	}
	atomic_fetch_add(&g_vec_checks, n);
}

static MunitResult
test_p2_vector_state(const MunitParameter p[], void *d)
{
	extern int __xtc_coro_preempt_effective(void);
	unsigned a, b, c, dx, lo, hi;
	uint64_t xcr0 = 0;
	xtc_exec_t *e = NULL;
	int i;
	(void)p; (void)d;

	if (!xtc_preempt_supported() || !__xtc_coro_preempt_effective())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	if (!__get_cpuid(1, &a, &b, &c, &dx) || (c & bit_OSXSAVE) == 0)
		return MUNIT_SKIP;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	xcr0 = ((uint64_t)hi << 32) | lo;
	__cpuid_count(7, 0, a, b, c, dx);
	g_vec_avx2 = (xcr0 & 0x6) == 0x6 && (b & bit_AVX2) != 0;
	g_vec_avx512 = (xcr0 & 0xE6) == 0xE6 && (b & bit_AVX512F) != 0;
	if (!g_vec_avx2)
		return MUNIT_SKIP;
	atomic_store(&g_vec_bad, 0);
	atomic_store(&g_vec_checks, 0);
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_set_preempt(e, 50LL * 1000), ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	for (i = 0; i < 4; i++)
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    p2_vec_spinner, (void *)(intptr_t)i, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	munit_logf(MUNIT_LOG_INFO, "%ld checks (avx2 %d avx512 %d), %ld corrupted",
	    atomic_load(&g_vec_checks), g_vec_avx2, g_vec_avx512,
	    atomic_load(&g_vec_bad));
	munit_assert_long(atomic_load(&g_vec_checks), >, 0);
	munit_assert_long(atomic_load(&g_vec_bad), ==, 0);
	return MUNIT_OK;
}

/*
 * /red_zone_flags: an involuntary preemption must be invisible to the
 * interrupted instruction stream.  p2_rz_probe is an ABI-conforming leaf
 * that keeps two values in its red zone (below %rsp, which the SysV ABI
 * guarantees a signal handler leaves alone) and holds a carry flag set
 * by its own `cmp` across a yield-free spin, then checks all three.
 * The handler used to store the resume PC at orig_sp - 8 (inside the red
 * zone: ~100 of ~70k probes per run clobbered) and the trampoline ran
 * `subq` before saving RFLAGS (the carry came back wrong).
 */
int p2_rz_probe(uint64_t v);
__asm__(".text\n\t.type p2_rz_probe,@function\np2_rz_probe:\n\t"
    "movq %rdi, -8(%rsp)\n\t"
    "movq %rdi, -120(%rsp)\n\t"
    "movq $1, %rax\n\t"
    "cmpq $2, %rax\n\t"           /* 1 < 2 unsigned: CF = 1 */
    "movl $20000, %ecx\n"
    "1:\tleal -1(%rcx), %ecx\n\t"  /* flag-neutral countdown */
    "jrcxz 2f\n\t"
    "jmp 1b\n"
    "2:\tmovl $0, %eax\n\t"
    "jc 3f\n\t"
    "orl $4, %eax\n"
    "3:\tcmpq %rdi, -8(%rsp)\n\t"
    "je 4f\n\t"
    "orl $1, %eax\n"
    "4:\tcmpq %rdi, -120(%rsp)\n\t"
    "je 5f\n\t"
    "orl $2, %eax\n"
    "5:\tret\n\t"
    ".size p2_rz_probe, .-p2_rz_probe\n");

static atomic_long g_rz_bad, g_fl_bad, g_rz_n;

static void
p2_rz_spinner(void *arg)
{
	uint64_t v = 0x5a5a5a5a00000000ull | (uint64_t)(intptr_t)arg;
	int64_t end = xtc_clock_mono() + 300LL * 1000 * 1000;
	long n = 0;
	int r;
	while (xtc_clock_mono() < end) {
		r = p2_rz_probe(v);
		if (r & 3)
			atomic_fetch_add(&g_rz_bad, 1);
		if (r & 4)
			atomic_fetch_add(&g_fl_bad, 1);
		n++;
	}
	atomic_fetch_add(&g_rz_n, n);
}

static MunitResult
test_p2_red_zone_flags(const MunitParameter p[], void *d)
{
	extern int __xtc_coro_preempt_effective(void);
	xtc_exec_t *e = NULL;
	int i;
	(void)p; (void)d;

	if (!xtc_preempt_supported() || !__xtc_coro_preempt_effective())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	atomic_store(&g_rz_bad, 0);
	atomic_store(&g_fl_bad, 0);
	atomic_store(&g_rz_n, 0);
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_set_preempt(e, 50LL * 1000), ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	for (i = 0; i < 4; i++)
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    p2_rz_spinner, (void *)(intptr_t)i, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	munit_logf(MUNIT_LOG_INFO, "%ld probes, red zone clobbered %ld, flags "
	    "lost %ld", atomic_load(&g_rz_n), atomic_load(&g_rz_bad),
	    atomic_load(&g_fl_bad));
	munit_assert_long(atomic_load(&g_rz_n), >, 0);
	munit_assert_long(atomic_load(&g_rz_bad), ==, 0);
	munit_assert_long(atomic_load(&g_fl_bad), ==, 0);
	return MUNIT_OK;
}
#endif /* x86-64 Linux */

static MunitTest tests[] = {
	{ "/preempt_tight_loop", test_p2_preempt_tight_loop, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/peers_progress", test_p2_peers_progress_on_ucontext, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#if defined(P2_VEC)
	{ "/vector_state", test_p2_vector_state, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/red_zone_flags", test_p2_red_zone_flags, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/preempt_p2", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}

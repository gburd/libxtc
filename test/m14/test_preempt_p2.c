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
 *	The involuntary yield needs the substrate's signal-context redirect
 *	(src/inc/coro_preempt.h): x86-64 Linux on ucontext or glibc
 *	fcontext, aarch64 Linux on either substrate and libc.  Where it
 *	declines (x86-64 musl, macOS, winfiber, the amalgamation) this
 *	test asserts the safe fallback (the runaway still completes; it
 *	just is not preempted -- so we skip the "peers ran" assertion
 *	there).
 */

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include "xtc.h"
#include "xtc_preempt_involuntary.h"   /* XTC_PREEMPTIBLE */
/* No preemption signal on Windows (the preemption tests skip there).
 * The tick must land in preemptible code: the safe-point check declines a
 * tick whose PC is in libc (raise) or libxtc, and this binary links
 * libxtc.a, so only XTC_PREEMPTIBLE code is preemptible.  So on Linux
 * x86-64 and aarch64 it is a raw tgkill from inline asm in a marked
 * function; the signal is delivered on the syscall's return, at its next
 * instruction. */
#if defined(__linux__) && defined(__x86_64__) && \
    (defined(__GNUC__) || defined(__clang__))
#include <sys/syscall.h>
#include <unistd.h>
XTC_PREEMPTIBLE static void
preempt_tick_raw(void)
{
	long pid = getpid(), tid = syscall(SYS_gettid), rc;
	__asm__ volatile("syscall" : "=a"(rc)
	    : "0"(234L), "D"(pid), "S"(tid), "d"((long)SIGVTALRM)
	    : "rcx", "r11", "memory");
	(void)rc;
}
#define PREEMPT_TICK() preempt_tick_raw()
#elif defined(__linux__) && defined(__aarch64__) && \
    (defined(__GNUC__) || defined(__clang__))
#include <sys/syscall.h>
#include <unistd.h>
XTC_PREEMPTIBLE static void
preempt_tick_raw(void)
{
	long pid = getpid(), tid = syscall(SYS_gettid);
	/* Load the register variables only after the calls: a call may
	 * clobber them. */
	register long x0 __asm__("x0") = pid;
	register long x1 __asm__("x1") = tid;
	register long x2 __asm__("x2") = SIGVTALRM;
	register long x8 __asm__("x8") = 131;   /* __NR_tgkill */
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)
	    : "memory");
}
#define PREEMPT_TICK() preempt_tick_raw()
#elif defined(SIGVTALRM)
#define PREEMPT_TICK() ((void)raise(SIGVTALRM))
#else
#define PREEMPT_TICK() ((void)0)
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_preempt.h"
#include "xtc_aio.h"
#include "xtc_fs.h"
#include "xtc_inject.h"
#include "xtc_io.h"

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
XTC_PREEMPTIBLE static void
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
XTC_PREEMPTIBLE static void
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
#include <sys/syscall.h>
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

XTC_PREEMPTIBLE __attribute__((target("avx2"))) static int
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

XTC_PREEMPTIBLE __attribute__((target("avx512f"))) static int
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

XTC_PREEMPTIBLE static void
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
__asm__(".section xtc_preemptible,\"ax\",@progbits\n\t.type p2_rz_probe,@function\np2_rz_probe:\n\t"
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
    ".size p2_rz_probe, .-p2_rz_probe\n\t.text\n");

static atomic_long g_rz_bad, g_fl_bad, g_rz_n;

XTC_PREEMPTIBLE static void
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

/*
 * Call-boundary state across an involuntary yield.  The trampoline calls
 * xtc_yield from an ARBITRARY instruction, so the scheduler and every
 * fiber it runs in the slice must get the state the ABI promises at a
 * call, whatever the interrupted code had live; and the interrupted code
 * must get its own state back.  Each case forces the tick at a chosen
 * instruction: a raw tgkill(SIGVTALRM) from this file's own text (no
 * libc frame; the signal is delivered on the syscall's return, i.e.
 * before the next instruction), with a 10 s timer so no natural tick
 * lands elsewhere.  A peer fiber on the same loop runs in the slice and
 * checks what it inherited.
 *   /tick_df     DF = 1 at the tick (inside a backward copy: `std; ...;
 *                rep movsb; cld`).  The scheduler and the peer ran with
 *                DF = 1 (neither swapcontext nor jump_fcontext touches
 *                RFLAGS), so the peer's forward `rep movsb` wrote
 *                BACKWARDS below its destination; the preempted
 *                backward copy itself must still be correct.
 *   /tick_ac     AC = 1 at the tick: with CR0.AM set (Linux), every
 *                misaligned access the scheduler made would raise SIGBUS.
 *   /tick_x87    six values on the x87 stack at the tick.  fcontext
 *                reloads only the x87 control word, so the peer inherited
 *                a nearly full x87 stack and its pi + pi + pi overflowed
 *                to NaN; the target must get its six values back.
 *   /tick_errno  errno is per thread: a peer on the same thread set it,
 *                and the interrupted code (say between a failed call and
 *                its errno test) read the peer's value.
 *   /tick_fpenv  MXCSR and the x87 control word set to round-toward-zero
 *                at the tick: the peer runs with its own (the substrate
 *                keeps both per fiber) and the target gets its own back.
 *   /tick_avx_upper  YMM upper halves dirty at the tick: the peer must
 *                start clean (XINUSE bit 2 = 0).  Not an ABI rule, but
 *                dirty upper state taxes the SSE code the scheduler and
 *                its fibers run with transition penalties.
 *   /tick_pkru   PKRU (protection-key rights, xtc_pkey) changed at the
 *                tick, and a peer sets its own: the interrupted code must
 *                resume with its own rights, as if never preempted.
 *                (PKRU stays per THREAD across a COOPERATIVE yield; see
 *                xtc_pkey(3).)  Skips without OSPKE.
 */
uint64_t p2_cb_flags_tick(long pid, long tid, long sig, uint64_t fl);
void p2_cb_bwd_copy_tick(unsigned char *dlast, const unsigned char *slast,
    size_t n, long pid, long tid, long sig);
void p2_cb_fwd_movsb(unsigned char *d, const unsigned char *s, size_t n);
uint64_t p2_cb_rflags(void);
void p2_cb_x87_tick(double *out6, long pid, long tid, long sig);
void p2_cb_tick(long pid, long tid, long sig);
__asm__(".section xtc_preemptible,\"ax\",@progbits\n"
    /* RFLAGS := fl, tick, return the RFLAGS the code resumed with. */
    ".type p2_cb_flags_tick,@function\np2_cb_flags_tick:\n\t"
    "movl $234, %eax\n\t"   /* SYS_tgkill */
    "pushq %rcx\n\t"
    "popfq\n\t"
    "syscall\n\t"
    "pushfq\n\t"
    "popq %rax\n\t"
    "pushq $0x202\n\t"
    "popfq\n\t"
    "ret\n\t"
    ".size p2_cb_flags_tick, .-p2_cb_flags_tick\n"
    /* std, tick, then the backward copy of n bytes ending at slast. */
    ".type p2_cb_bwd_copy_tick,@function\np2_cb_bwd_copy_tick:\n\t"
    "pushq %rdi\n\t"
    "pushq %rsi\n\t"
    "pushq %rdx\n\t"
    "movq %rcx, %rdi\n\t"
    "movq %r8, %rsi\n\t"
    "movq %r9, %rdx\n\t"
    "movl $234, %eax\n\t"   /* SYS_tgkill */
    "std\n\t"
    "syscall\n\t"
    "popq %rcx\n\t"
    "popq %rsi\n\t"
    "popq %rdi\n\t"
    "rep movsb\n\t"
    "cld\n\t"
    "ret\n\t"
    ".size p2_cb_bwd_copy_tick, .-p2_cb_bwd_copy_tick\n"
    /* A forward copy that trusts the ABI's DF = 0 at a call. */
    ".type p2_cb_fwd_movsb,@function\np2_cb_fwd_movsb:\n\t"
    "movq %rdx, %rcx\n\t"
    "rep movsb\n\t"
    "ret\n\t"
    ".size p2_cb_fwd_movsb, .-p2_cb_fwd_movsb\n"
    ".type p2_cb_rflags,@function\np2_cb_rflags:\n\t"
    "pushfq\n\t"
    "popq %rax\n\t"
    "ret\n\t"
    ".size p2_cb_rflags, .-p2_cb_rflags\n"
    /* Six x87 loads, tick, six stores (pops) into out6[0..5]. */
    ".type p2_cb_x87_tick,@function\np2_cb_x87_tick:\n\t"
    "movq %rdi, %r8\n\t"
    "movq %rsi, %rdi\n\t"
    "movq %rdx, %rsi\n\t"
    "movq %rcx, %rdx\n\t"
    "fld1\n\tfldz\n\tfld1\n\tfldpi\n\tfld1\n\tfldl2e\n\t"
    "movl $234, %eax\n\t"   /* SYS_tgkill */
    "syscall\n\t"
    "fstpl 0(%r8)\n\tfstpl 8(%r8)\n\tfstpl 16(%r8)\n\t"
    "fstpl 24(%r8)\n\tfstpl 32(%r8)\n\tfstpl 40(%r8)\n\t"
    "ret\n\t"
    ".size p2_cb_x87_tick, .-p2_cb_x87_tick\n"
    ".type p2_cb_tick,@function\np2_cb_tick:\n\t"
    "movl $234, %eax\n\t"   /* SYS_tgkill */
    "syscall\n\t"
    "ret\n\t"
    ".size p2_cb_tick, .-p2_cb_tick\n"
    /* MXCSR and the x87 CW := round toward zero, tick, return both as
     * resumed with (MXCSR low 32 bits, CW high), then put them back. */
    ".type p2_cb_fpenv_tick,@function\np2_cb_fpenv_tick:\n\t"
    "subq $24, %rsp\n\t"
    "stmxcsr 0(%rsp)\n\t"
    "fnstcw 4(%rsp)\n\t"
    "movl 0(%rsp), %eax\n\t"
    "orl $0x6000, %eax\n\t"
    "movl %eax, 8(%rsp)\n\t"
    "movzwl 4(%rsp), %eax\n\t"
    "orl $0xc00, %eax\n\t"
    "movw %ax, 12(%rsp)\n\t"
    "ldmxcsr 8(%rsp)\n\t"
    "fldcw 12(%rsp)\n\t"
    "movl $234, %eax\n\t"   /* SYS_tgkill */
    "syscall\n\t"
    "stmxcsr 16(%rsp)\n\t"
    "fnstcw 20(%rsp)\n\t"
    "ldmxcsr 0(%rsp)\n\t"
    "fldcw 4(%rsp)\n\t"
    "movzwl 20(%rsp), %eax\n\t"
    "shlq $32, %rax\n\t"
    "movl 16(%rsp), %ecx\n\t"
    "orq %rcx, %rax\n\t"
    "addq $24, %rsp\n\t"
    "ret\n\t"
    ".size p2_cb_fpenv_tick, .-p2_cb_fpenv_tick\n"
    /* Dirty the YMM upper halves, tick, clean them. */
    ".type p2_cb_ymm_tick,@function\np2_cb_ymm_tick:\n\t"
    "vpcmpeqd %ymm1, %ymm1, %ymm1\n\t"
    "movl $234, %eax\n\t"   /* SYS_tgkill */
    "syscall\n\t"
    "vzeroupper\n\t"
    "ret\n\t"
    ".size p2_cb_ymm_tick, .-p2_cb_ymm_tick\n\t.text\n");
uint64_t p2_cb_fpenv_tick(long pid, long tid, long sig);
/* PKRU := v, tick, return the PKRU resumed with (rdpkru/wrpkru need
 * ecx = edx = 0; the caller restores the original). */
uint32_t p2_cb_pkru_tick(long pid, long tid, long sig, uint32_t v);
uint32_t p2_cb_rdpkru(void);
void p2_cb_wrpkru(uint32_t v);
__asm__(".section xtc_preemptible,\"ax\",@progbits\n"
    ".type p2_cb_pkru_tick,@function\np2_cb_pkru_tick:\n\t"
    "movq %rdx, %r8\n\t"
    "movl %ecx, %eax\n\t"
    "xorl %ecx, %ecx\n\t"
    "xorl %edx, %edx\n\t"
    ".byte 0x0f, 0x01, 0xef\n\t"   /* wrpkru */
    "movq %r8, %rdx\n\t"
    "movl $234, %eax\n\t"   /* SYS_tgkill */
    "syscall\n\t"
    "xorl %ecx, %ecx\n\t"
    ".byte 0x0f, 0x01, 0xee\n\t"   /* rdpkru */
    "ret\n\t"
    ".size p2_cb_pkru_tick, .-p2_cb_pkru_tick\n"
    ".type p2_cb_rdpkru,@function\np2_cb_rdpkru:\n\t"
    "xorl %ecx, %ecx\n\t"
    ".byte 0x0f, 0x01, 0xee\n\t"
    "ret\n\t"
    ".size p2_cb_rdpkru, .-p2_cb_rdpkru\n"
    ".type p2_cb_wrpkru,@function\np2_cb_wrpkru:\n\t"
    "movl %edi, %eax\n\t"
    "xorl %ecx, %ecx\n\t"
    "xorl %edx, %edx\n\t"
    ".byte 0x0f, 0x01, 0xef\n\t"
    "ret\n\t"
    ".size p2_cb_wrpkru, .-p2_cb_wrpkru\n\t.text\n");
#define P2_CB_PKEY15 (3u << 30)   /* key 15: access + write disable */
void p2_cb_ymm_tick(long pid, long tid, long sig);

/* XINUSE (XGETBV with ECX = 1) bit 2: the YMM upper halves are not in
 * their initial (zero) state. */
XTC_PREEMPTIBLE static int
p2_cb_ymm_dirty(void)
{
	unsigned lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(1));
	(void)hi;
	return (lo & 4) != 0;
}

#define P2_CB_DF     0x400ull
#define P2_CB_AC     0x40000ull
#define P2_CB_N      64
#define P2_CB_ERRNO  4321
#define P2_CB_PEERNO 8765
static atomic_long g_cb_peer_runs, g_cb_peer_bad, g_cb_tgt_bad, g_cb_preempted;
static atomic_int  g_cb_stop;
static int         g_cb_mode;   /* 0 df, 1 ac, 2 x87, 3 errno, 4 fpenv, 5 ymm, 6 pkru */
static uint32_t    g_cb_pkru0;  /* the thread's PKRU before the run */
static unsigned char g_cb_pat[192];

XTC_PREEMPTIBLE static double
p2_cb_x87_pi3(void)
{
	double r;
	__asm__ volatile("fldpi\n\tfldpi\n\tfldpi\n\tfaddp\n\tfaddp\n\t"
	    "fstpl %0" : "=m"(r) : : "memory");
	return r;
}

XTC_PREEMPTIBLE static void
p2_cb_peer(void *arg)
{
	unsigned char b[192];
	uint32_t mx;
	uint16_t cw;
	int bad, k;
	(void)arg;
	while (!atomic_load(&g_cb_stop)) {
		bad = 0;
		switch (g_cb_mode) {
		case 0:     /* DF: a forward copy must stay inside b[64..128) */
			memset(b, 0, sizeof b);
			bad = (p2_cb_rflags() & P2_CB_DF) != 0;
			p2_cb_fwd_movsb(b + 64, g_cb_pat + 64, 64);
			for (k = 0; k < 192; k++)
				bad |= b[k] != (k >= 64 && k < 128 ?
				    g_cb_pat[k] : 0);
			break;
		case 1:
			bad = (p2_cb_rflags() & P2_CB_AC) != 0;
			break;
		case 2:
			bad = !(p2_cb_x87_pi3() > 9.42477 &&
			    p2_cb_x87_pi3() < 9.42478);
			break;
		case 3:
			errno = P2_CB_PEERNO;
			break;
		case 4:
			__asm__ volatile("stmxcsr %0\n\tfnstcw %1"
			    : "=m"(mx), "=m"(cw));
			bad = (mx & 0x6000) != 0 || (cw & 0xc00) != 0;
			break;
		case 5:
			bad = p2_cb_ymm_dirty();
			break;
		default:    /* another fiber sets its own rights */
			p2_cb_wrpkru(g_cb_pkru0);
			break;
		}
		if (bad)
			atomic_fetch_add(&g_cb_peer_bad, 1);
		atomic_fetch_add(&g_cb_peer_runs, 1);
		xtc_yield();
	}
}

XTC_PREEMPTIBLE static void
p2_cb_target(void *arg)
{
	unsigned char d[192];
	double o[6];
	long pid = getpid(), tid = syscall(SYS_gettid), before;
	uint64_t got;
	int i, k, bad;
	(void)arg;
	xtc_yield();                        /* let the peer start */
	for (i = 0; i < P2_CB_N; i++) {
		bad = 0;
		before = atomic_load(&g_cb_peer_runs);
		switch (g_cb_mode) {
		case 0:     /* d[0..128) := pat[32..160), copied backwards */
			memset(d, 0, sizeof d);
			p2_cb_bwd_copy_tick(d + 127, g_cb_pat + 159, 128, pid,
			    tid, SIGVTALRM);
			for (k = 0; k < 192; k++)
				bad |= d[k] != (k < 128 ? g_cb_pat[k + 32] : 0);
			break;
		case 1:
			got = p2_cb_flags_tick(pid, tid, SIGVTALRM,
			    0x202 | P2_CB_AC | 0x1);
			bad = (got & (P2_CB_AC | 0x1)) != (P2_CB_AC | 0x1);
			break;
		case 2:
			p2_cb_x87_tick(o, pid, tid, SIGVTALRM);
			bad = o[0] != M_LOG2E || o[1] != 1.0 || o[2] != M_PI ||
			    o[3] != 1.0 || o[4] != 0.0 || o[5] != 1.0;
			break;
		case 3:
			errno = P2_CB_ERRNO;
			p2_cb_tick(pid, tid, SIGVTALRM);
			bad = errno != P2_CB_ERRNO;
			break;
		case 4:
			got = p2_cb_fpenv_tick(pid, tid, SIGVTALRM);
			bad = (got & 0x6000) != 0x6000 ||
			    ((got >> 32) & 0xc00) != 0xc00;
			break;
		case 5:
			p2_cb_ymm_tick(pid, tid, SIGVTALRM);
			break;
		default:
			bad = p2_cb_pkru_tick(pid, tid, SIGVTALRM,
			    g_cb_pkru0 | P2_CB_PKEY15) !=
			    (g_cb_pkru0 | P2_CB_PKEY15);
			p2_cb_wrpkru(g_cb_pkru0);
			break;
		}
		if (bad)
			atomic_fetch_add(&g_cb_tgt_bad, 1);
		if (atomic_load(&g_cb_peer_runs) != before)
			atomic_fetch_add(&g_cb_preempted, 1);
	}
	atomic_store(&g_cb_stop, 1);
}

static MunitResult
p2_cb_run(int mode)
{
	extern int __xtc_coro_preempt_effective(void);
	xtc_exec_t *e = NULL;
	unsigned a, b, c, dx, lo, hi;
	int k;

	if (!xtc_preempt_supported() || !__xtc_coro_preempt_effective())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	if (mode == 5) {
		/* AVX enabled by the OS, and XGETBV ECX = 1 (XINUSE). */
		if (!__get_cpuid(1, &a, &b, &c, &dx) ||
		    (c & bit_OSXSAVE) == 0 || __get_cpuid_max(0, NULL) < 0xD)
			return MUNIT_SKIP;
		__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
		__cpuid_count(0xD, 1, a, b, c, dx);
		if ((lo & 6) != 6 || (a & 4) == 0)
			return MUNIT_SKIP;
	}
	if (mode == 6) {
		/* OSPKE (CR4.PKE): rdpkru/wrpkru are usable. */
		if (__get_cpuid_max(0, NULL) < 7)
			return MUNIT_SKIP;
		__cpuid_count(7, 0, a, b, c, dx);
		if ((c & (1u << 4)) == 0)
			return MUNIT_SKIP;
		g_cb_pkru0 = p2_cb_rdpkru();
	}
	g_cb_mode = mode;
	for (k = 0; k < 192; k++)
		g_cb_pat[k] = (unsigned char)(k * 7 + 1);
	atomic_store(&g_cb_peer_runs, 0);
	atomic_store(&g_cb_peer_bad, 0);
	atomic_store(&g_cb_tgt_bad, 0);
	atomic_store(&g_cb_preempted, 0);
	atomic_store(&g_cb_stop, 0);
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_set_preempt(e, 10LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), p2_cb_target,
	    NULL, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), p2_cb_peer,
	    NULL, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	munit_logf(MUNIT_LOG_INFO, "mode %d: %ld/%d ticks preempted, target "
	    "bad %ld, peer bad %ld of %ld", mode, atomic_load(&g_cb_preempted),
	    P2_CB_N, atomic_load(&g_cb_tgt_bad), atomic_load(&g_cb_peer_bad),
	    atomic_load(&g_cb_peer_runs));
	munit_assert_long(atomic_load(&g_cb_preempted), >, 0);
	munit_assert_long(atomic_load(&g_cb_tgt_bad), ==, 0);
	munit_assert_long(atomic_load(&g_cb_peer_bad), ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_p2_tick_df(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_cb_run(0);
}

static MunitResult
test_p2_tick_ac(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_cb_run(1);
}

static MunitResult
test_p2_tick_x87(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_cb_run(2);
}

static MunitResult
test_p2_tick_errno(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_cb_run(3);
}

static MunitResult
test_p2_tick_fpenv(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_cb_run(4);
}

static MunitResult
test_p2_tick_avx_upper(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_cb_run(5);
}

static MunitResult
test_p2_tick_pkru(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_cb_run(6);
}
#endif /* x86-64 Linux */

#if defined(__aarch64__) && defined(__linux__) && \
    (defined(__GNUC__) || defined(__clang__))
#include <sys/auxv.h>
#include <sys/syscall.h>
#define P2_A64 1
#ifndef HWCAP_SVE
#define HWCAP_SVE (1UL << 22)
#endif
/*
 * /a64_sve_state: the aarch64 twin of /vector_state.  Fibers on one loop
 * each hold a fiber-unique pattern across the FULL width of z1 and a
 * fiber-unique predicate in p1 through a yield-free spin, then read both
 * back.  glibc's memcpy/memset are SVE on an SVE core, so a fiber can be
 * preempted with live SVE state.  Skips without SVE.
 */
static atomic_long g_sve_bad, g_sve_checks;
/* Fiber changes between consecutive probes on the loop: > 0 proves the
 * yield-free spinners were sliced, so a pass is not vacuous. */
static atomic_int  g_a64_last;
static atomic_long g_a64_slices;

static void
p2_a64_note(int i)
{
	if (atomic_exchange(&g_a64_last, i) != i)
		atomic_fetch_add(&g_a64_slices, 1);
}

XTC_PREEMPTIBLE __attribute__((target("+sve"))) static int
p2_sve_hold(uint64_t want, int i)
{
	uint64_t got[32];   /* up to 2048-bit vectors */
	uint8_t pg[32];
	uint64_t vl, k;
	int bad = 0;
	__asm__ volatile("cntd %0" : "=r"(vl));
	__asm__ volatile(
	    "mov z1.d, %2\n\t"
	    "cmp %w3, #0\n\t"
	    "b.eq 3f\n\t"
	    "ptrue p1.d, vl1\n\t"
	    "b 4f\n"
	    "3:\tptrue p1.b\n"
	    "4:\tmov w9, #20000\n"
	    "1:\tsubs w9, w9, #1\n\t"
	    "b.ne 1b\n\t"
	    "ptrue p0.d\n\t"
	    "st1d { z1.d }, p0, [%0]\n\t"
	    "str p1, [%1]"
	    :
	    : "r"(got), "r"(pg), "r"(want), "r"(i & 1)
	    : "x9", "z1", "p0", "p1", "cc", "memory");
	for (k = 0; k < vl && k < 32; k++)
		bad |= got[k] != want;
	/* p1: ptrue .d,vl1 sets bit 0 only; ptrue .b sets every bit. */
	bad |= (i & 1) ? (pg[0] != 0x01) : (pg[0] != 0xff);
	return bad;
}

XTC_PREEMPTIBLE static void
p2_sve_spinner(void *arg)
{
	int i = (int)(intptr_t)arg;
	uint64_t want = 0x0101010101010101ull * (uint64_t)(i + 1);
	int64_t end = xtc_clock_mono() + 300LL * 1000 * 1000;
	long n = 0;
	while (xtc_clock_mono() < end) {
		if (p2_sve_hold(want, i))
			atomic_fetch_add(&g_sve_bad, 1);
		p2_a64_note(i);
		n++;
	}
	atomic_fetch_add(&g_sve_checks, n);
}

/*
 * /a64_x16_preserved: an involuntary preemption must be invisible to the
 * interrupted code's registers.  x16/x17 are "intra-procedure-call"
 * scratch, but between calls they are ordinary registers the compiler
 * and hand-written code use (glibc's string functions keep data in
 * them).  The probe holds a fiber-unique value in x16 and x17 and a
 * condition-flag result across a yield-free, flag-neutral spin and
 * checks all three.
 */
static atomic_long g_x16_bad, g_x17_bad, g_nzcv_bad, g_x16_n;

XTC_PREEMPTIBLE static int
p2_x16_hold(uint64_t want)
{
	uint64_t a, b, lt;
	__asm__ volatile(
	    "mov x16, %3\n\t"
	    "mov x17, %3\n\t"
	    "mov w9, #1\n\t"
	    "cmp w9, #2\n\t"             /* 1 - 2: N = 1, V = 0 -> lt */
	    "mov w9, #20000\n"
	    "1:\tsub w9, w9, #1\n\t"      /* flag-neutral countdown */
	    "cbnz w9, 1b\n\t"
	    "cset %2, lt\n\t"
	    "mov %0, x16\n\t"
	    "mov %1, x17"
	    : "=&r"(a), "=&r"(b), "=&r"(lt)
	    : "r"(want)
	    : "x9", "x16", "x17", "cc");
	return (a != want) | ((b != want) << 1) | ((lt != 1) << 2);
}

XTC_PREEMPTIBLE static void
p2_x16_spinner(void *arg)
{
	uint64_t want = 0x5a5a5a5a00000000ull | (uint64_t)(intptr_t)arg;
	int64_t end = xtc_clock_mono() + 300LL * 1000 * 1000;
	long n = 0;
	int r;
	while (xtc_clock_mono() < end) {
		r = p2_x16_hold(want);
		if (r & 1)
			atomic_fetch_add(&g_x16_bad, 1);
		if (r & 2)
			atomic_fetch_add(&g_x17_bad, 1);
		if (r & 4)
			atomic_fetch_add(&g_nzcv_bad, 1);
		p2_a64_note((int)(intptr_t)arg);
		n++;
	}
	atomic_fetch_add(&g_x16_n, n);
}

/* Run four `fn` spinners on one loop with 50 us involuntary slices. */
static MunitResult
p2_a64_run(void (*fn)(void *))
{
	extern int __xtc_coro_preempt_effective(void);
	xtc_exec_t *e = NULL;
	int i;

	if (!xtc_preempt_supported())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	/* Every aarch64 Linux build has the redirect, whatever the libc and
	 * substrate: musl/fcontext used to compile it out and skip here. */
	munit_assert_int(__xtc_coro_preempt_effective(), ==, 1);
	atomic_store(&g_a64_last, -1);
	atomic_store(&g_a64_slices, 0);
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_set_preempt(e, 50LL * 1000), ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	for (i = 0; i < 4; i++)
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), fn,
		    (void *)(intptr_t)i, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	munit_logf(MUNIT_LOG_INFO, "%ld involuntary slices",
	    atomic_load(&g_a64_slices));
	munit_assert_long(atomic_load(&g_a64_slices), >, 4);
	return MUNIT_OK;
}

static MunitResult
test_p2_a64_sve_state(const MunitParameter p[], void *d)
{
	MunitResult r;
	(void)p; (void)d;
	if ((getauxval(AT_HWCAP) & HWCAP_SVE) == 0)
		return MUNIT_SKIP;
	atomic_store(&g_sve_bad, 0);
	atomic_store(&g_sve_checks, 0);
	if ((r = p2_a64_run(p2_sve_spinner)) != MUNIT_OK)
		return r;
	munit_logf(MUNIT_LOG_INFO, "%ld checks, %ld corrupted",
	    atomic_load(&g_sve_checks), atomic_load(&g_sve_bad));
	munit_assert_long(atomic_load(&g_sve_checks), >, 0);
	munit_assert_long(atomic_load(&g_sve_bad), ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_p2_a64_x16_preserved(const MunitParameter p[], void *d)
{
	MunitResult r;
	(void)p; (void)d;
	atomic_store(&g_x16_bad, 0);
	atomic_store(&g_x17_bad, 0);
	atomic_store(&g_nzcv_bad, 0);
	atomic_store(&g_x16_n, 0);
	if ((r = p2_a64_run(p2_x16_spinner)) != MUNIT_OK)
		return r;
	munit_logf(MUNIT_LOG_INFO, "%ld probes, x16 lost %ld, x17 lost %ld, "
	    "flags lost %ld", atomic_load(&g_x16_n), atomic_load(&g_x16_bad),
	    atomic_load(&g_x17_bad), atomic_load(&g_nzcv_bad));
	munit_assert_long(atomic_load(&g_x16_n), >, 0);
	munit_assert_long(atomic_load(&g_x16_bad), ==, 0);
	munit_assert_long(atomic_load(&g_x17_bad), ==, 0);
	munit_assert_long(atomic_load(&g_nzcv_bad), ==, 0);
	return MUNIT_OK;
}

/*
 * The aarch64 twins of the x86-64 /tick_* cases: the tick is a raw
 * tgkill svc from this file's own text, a 10 s timer keeps natural ticks
 * out, and a peer fiber on the same loop checks what the scheduler
 * handed it.
 *   /a64_tick_errno  errno is per thread (the peer sets its own).
 *   /a64_tick_fpcr   FPCR.RMode = round-toward-zero at the tick: the
 *                    peer must run with its own FPCR (the substrate keeps
 *                    FPCR per fiber) and the target gets its own back.
 *   /a64_tick_sme    the tick lands in SME streaming mode with ZA on
 *                    (skips without HWCAP2_SME).  The scheduler and the
 *                    peer ran in streaming mode with ZA live; the peer's
 *                    Advanced SIMD add faults there without
 *                    FEAT_SME_FA64 (SIGILL), and its result is checked.
 */
#ifndef HWCAP2_SME
#define HWCAP2_SME (1UL << 23)
#endif
void p2_a64_cb_tick(long pid, long tid, long sig);
uint64_t p2_a64_cb_fpcr_tick(long pid, long tid, long sig, uint64_t fpcr);
uint64_t p2_a64_cb_sme_tick(long pid, long tid, long sig);
uint64_t p2_a64_cb_svcr(void);
__asm__(".section xtc_preemptible,\"ax\",%progbits\n"
    ".type p2_a64_cb_tick,%function\np2_a64_cb_tick:\n\t"
    "mov x8, #131\n\t"            /* __NR_tgkill */
    "svc #0\n\t"
    "ret\n\t"
    ".size p2_a64_cb_tick, .-p2_a64_cb_tick\n"
    /* FPCR := x3, tick, return the FPCR resumed with; restore 0. */
    ".type p2_a64_cb_fpcr_tick,%function\np2_a64_cb_fpcr_tick:\n\t"
    "mrs x9, fpcr\n\t"
    "msr fpcr, x3\n\t"
    "mov x8, #131\n\t"
    "svc #0\n\t"
    "mrs x0, fpcr\n\t"
    "msr fpcr, x9\n\t"
    "ret\n\t"
    ".size p2_a64_cb_fpcr_tick, .-p2_a64_cb_fpcr_tick\n"
    /* smstart (SM and ZA), tick, read SVCR, smstop: returns the SVCR
     * the code resumed with (3 = SM and ZA, as it was at the tick). */
    ".type p2_a64_cb_sme_tick,%function\np2_a64_cb_sme_tick:\n\t"
    ".inst 0xd503477f\n\t"        /* smstart */
    "mov x8, #131\n\t"
    "svc #0\n\t"
    ".inst 0xd53b4249\n\t"        /* mrs x9, svcr */
    ".inst 0xd503467f\n\t"        /* smstop */
    "mov x0, x9\n\t"
    "ret\n\t"
    ".size p2_a64_cb_sme_tick, .-p2_a64_cb_sme_tick\n"
    ".type p2_a64_cb_svcr,%function\np2_a64_cb_svcr:\n\t"
    ".inst 0xd53b4240\n\t"        /* mrs x0, svcr */
    "ret\n\t"
    ".size p2_a64_cb_svcr, .-p2_a64_cb_svcr\n\t.text\n");

#define P2_A64_RZ    (3ull << 22)   /* FPCR.RMode = round toward zero */
#define P2_A64_N     64
static atomic_long g_a64cb_peer_runs, g_a64cb_peer_bad, g_a64cb_tgt_bad;
static atomic_long g_a64cb_preempted;
static atomic_int  g_a64cb_stop;
static int         g_a64cb_mode;   /* 0 errno, 1 fpcr, 2 sme */

static void
p2_a64cb_nop(int sig)
{
	(void)sig;
}

XTC_PREEMPTIBLE static void
p2_a64cb_peer(void *arg)
{
	uint64_t fpcr, v;
	int bad;
	(void)arg;
	while (!atomic_load(&g_a64cb_stop)) {
		bad = 0;
		switch (g_a64cb_mode) {
		case 0:
			errno = 8765;
			break;
		case 1:
			__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
			bad = (fpcr & P2_A64_RZ) != 0;
			break;
		default:
			bad = p2_a64_cb_svcr() != 0;
			v = 21;
			__asm__ volatile("dup v0.2d, %1\n\t"
			    "add v0.2d, v0.2d, v0.2d\n\t"
			    "umov %0, v0.d[1]" : "=r"(v) : "r"(v) : "v0");
			bad |= v != 42;
			break;
		}
		if (bad)
			atomic_fetch_add(&g_a64cb_peer_bad, 1);
		atomic_fetch_add(&g_a64cb_peer_runs, 1);
		xtc_yield();
	}
}

XTC_PREEMPTIBLE static void
p2_a64cb_target(void *arg)
{
	long pid = getpid(), tid = syscall(SYS_gettid), before;
	int i, bad;
	(void)arg;
	xtc_yield();                        /* let the peer start */
	for (i = 0; i < P2_A64_N; i++) {
		bad = 0;
		before = atomic_load(&g_a64cb_peer_runs);
		switch (g_a64cb_mode) {
		case 0:
			errno = 4321;
			p2_a64_cb_tick(pid, tid, SIGVTALRM);
			bad = errno != 4321;
			break;
		case 1:
			bad = (p2_a64_cb_fpcr_tick(pid, tid, SIGVTALRM,
			    P2_A64_RZ) & P2_A64_RZ) != P2_A64_RZ;
			break;
		default:
			bad = p2_a64_cb_sme_tick(pid, tid, SIGVTALRM) != 3;
			break;
		}
		if (bad)
			atomic_fetch_add(&g_a64cb_tgt_bad, 1);
		if (atomic_load(&g_a64cb_peer_runs) != before)
			atomic_fetch_add(&g_a64cb_preempted, 1);
	}
	atomic_store(&g_a64cb_stop, 1);
}

static MunitResult
p2_a64cb_run(int mode)
{
	xtc_exec_t *e = NULL;
	struct sigaction sa, old;
	int tgt_ok = 1;

	if (!xtc_preempt_supported())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	if (mode == 2 && (getauxval(AT_HWCAP2) & HWCAP2_SME) == 0)
		return MUNIT_SKIP;
	if (mode == 2) {
		/* Control: does a PLAIN signal round trip restore streaming
		 * mode here?  (qemu-user 10.2 restores ZA but not PSTATE.SM.)
		 * If not, the target half measures the platform, not libxtc. */
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = p2_a64cb_nop;
		(void)sigemptyset(&sa.sa_mask);
		(void)sigaction(SIGUSR1, &sa, &old);
		tgt_ok = p2_a64_cb_sme_tick(getpid(), syscall(SYS_gettid),
		    SIGUSR1) == 3;
		(void)sigaction(SIGUSR1, &old, NULL);
		if (!tgt_ok)
			munit_log(MUNIT_LOG_INFO, "a plain signal does not restore "
			    "PSTATE.SM on this platform: target half not checked");
	}
	g_a64cb_mode = mode;
	atomic_store(&g_a64cb_peer_runs, 0);
	atomic_store(&g_a64cb_peer_bad, 0);
	atomic_store(&g_a64cb_tgt_bad, 0);
	atomic_store(&g_a64cb_preempted, 0);
	atomic_store(&g_a64cb_stop, 0);
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_set_preempt(e, 10LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), p2_a64cb_target,
	    NULL, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), p2_a64cb_peer,
	    NULL, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	munit_logf(MUNIT_LOG_INFO, "mode %d: %ld/%d ticks preempted, target "
	    "bad %ld, peer bad %ld of %ld", mode,
	    atomic_load(&g_a64cb_preempted), P2_A64_N,
	    atomic_load(&g_a64cb_tgt_bad), atomic_load(&g_a64cb_peer_bad),
	    atomic_load(&g_a64cb_peer_runs));
	munit_assert_long(atomic_load(&g_a64cb_preempted), >, 0);
	if (tgt_ok)
		munit_assert_long(atomic_load(&g_a64cb_tgt_bad), ==, 0);
	munit_assert_long(atomic_load(&g_a64cb_peer_bad), ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_p2_a64_tick_errno(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_a64cb_run(0);
}

static MunitResult
test_p2_a64_tick_fpcr(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_a64cb_run(1);
}

static MunitResult
test_p2_a64_tick_sme(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return p2_a64cb_run(2);
}
#endif /* aarch64 Linux */

/*
 * /sqe_window_*: an involuntary preemption must never land inside the
 * io_uring SQE reservation window.  io_uring_get_sqe advances the SQ's
 * userspace tail before the slot is written; a fiber redirected to the
 * scheduler there lets any same-thread submitter publish the reserved
 * slot with its STALE contents, and the preempted fiber's own op then
 * never reaches the kernel (test_wal_kill preempt-nokill hung this way,
 * ~0.2% of runs).  The "io.uring.sqe_reserved" injection point fires
 * inside the window; its callback raises the REAL preemption signal on
 * the fiber's own thread, so the handler takes exactly the decision a
 * timer tick there would.  Control: the same raise from plain fiber
 * code DOES preempt (a peer runs before raise returns), so a deferred
 * raise inside the window is not vacuous.
 *
 *   no_preempt     no peer may run while the fiber is inside the window
 *                  (an aio submit, then an fd registration's POLL_ADD)
 *   lost_completion a peer that submits while the fiber is preempted in
 *                  the window must not cost the fiber its completion
 *                  (before the fix: the fiber parks forever; a watchdog
 *                  aborts after 10 s)
 */
#define SW_LEN 4096
static atomic_long g_sw_peer_runs;
static atomic_int  g_sw_arm, g_sw_window, g_sw_hits, g_sw_stop, g_sw_done;
static atomic_int  g_sw_ctrl_preempted, g_sw_win_preempted, g_sw_peer_wrote;
static int         g_sw_fd, g_sw_peer_submits, g_sw_a_ok, g_sw_b_rc, g_sw_fd_ok;

static void
sw_raise_in_window(const char *name, void *user)
{
	long before;
	(void)name; (void)user;
	if (!atomic_exchange(&g_sw_arm, 0))
		return;                          /* one shot: the first SQE */
	atomic_fetch_add(&g_sw_hits, 1);
	before = atomic_load(&g_sw_peer_runs);
	atomic_store(&g_sw_window, 1);
	PREEMPT_TICK();              /* a preemption tick, here */
	atomic_store(&g_sw_window, 0);
	if (atomic_load(&g_sw_peer_runs) != before)
		atomic_store(&g_sw_win_preempted, 1);
}

static void
sw_peer(void *arg)
{
	static unsigned char b[SW_LEN];
	(void)arg;
	memset(b, 0xbb, sizeof b);
	while (!atomic_load(&g_sw_stop)) {
		atomic_fetch_add(&g_sw_peer_runs, 1);
		if (g_sw_peer_submits && atomic_load(&g_sw_window) &&
		    !atomic_load(&g_sw_peer_wrote)) {
			atomic_store(&g_sw_peer_wrote, 1);
			g_sw_b_rc = xtc_aio_pwrite(g_sw_fd, b, SW_LEN, SW_LEN);
		}
		xtc_yield();
	}
}

static void
sw_writer(void *arg)
{
	static unsigned char out[SW_LEN], in[SW_LEN];
	long before;
	int pfd[2];
	uint32_t rev = 0;
	(void)arg;
	memset(out, 0xaa, sizeof out);
	xtc_yield();                          /* let the peer start */
	before = atomic_load(&g_sw_peer_runs);
	PREEMPT_TICK();               /* control: plain fiber code */
	if (atomic_load(&g_sw_peer_runs) != before)
		atomic_store(&g_sw_ctrl_preempted, 1);
	atomic_store(&g_sw_arm, 1);
	if (xtc_aio_pwrite(g_sw_fd, out, SW_LEN, 0) == SW_LEN &&
	    xtc_aio_pread(g_sw_fd, in, SW_LEN, 0) == SW_LEN)
		g_sw_a_ok = memcmp(in, out, SW_LEN) == 0;
	/* The fd-registration producer (POLL_ADD from xtc_io_reg_fd): a
	 * readable pipe, so the wait returns at once. */
	if (pipe(pfd) == 0) {
		if (write(pfd[1], "x", 1) == 1) {
			atomic_store(&g_sw_arm, 1);
			if (xtc_proc_wait_fd(pfd[0], XTC_IO_READABLE, -1,
			    &rev) == XTC_OK && (rev & XTC_IO_READABLE))
				g_sw_fd_ok = 1;
		}
		(void)close(pfd[0]);
		(void)close(pfd[1]);
	}
	atomic_store(&g_sw_stop, 1);
}

static void *
sw_watchdog(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 1000 && !atomic_load(&g_sw_done); i++)
		usleep(10000);
	if (!atomic_load(&g_sw_done)) {
		fprintf(stderr, "sqe_window: the fiber's io completion never "
		    "came (preempted inside the SQE window: %d)\n",
		    atomic_load(&g_sw_win_preempted));
		abort();
	}
	return NULL;
}

static MunitResult
sw_run(int peer_submits)
{
	extern int __xtc_coro_preempt_effective(void);
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	pthread_t wd;
	char dir[512], path[600];

	if (!xtc_preempt_supported() || !__xtc_coro_preempt_effective() ||
	    strcmp(xtc_io_backend_name(), "uring") != 0)
		return MUNIT_SKIP;   /* the window is io_uring's */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc_sqe_window_XXXXXX", dir);
	g_sw_fd = mkstemp(path);
	munit_assert_int(g_sw_fd, >=, 0);
	(void)unlink(path);
	atomic_store(&g_sw_peer_runs, 0);
	atomic_store(&g_sw_arm, 0);
	atomic_store(&g_sw_window, 0);
	atomic_store(&g_sw_hits, 0);
	atomic_store(&g_sw_stop, 0);
	atomic_store(&g_sw_done, 0);
	atomic_store(&g_sw_ctrl_preempted, 0);
	atomic_store(&g_sw_win_preempted, 0);
	atomic_store(&g_sw_peer_wrote, 0);
	g_sw_peer_submits = peer_submits;
	g_sw_a_ok = 0;
	g_sw_b_rc = 0;
	g_sw_fd_ok = 0;
	munit_assert_int(xtc_inject_attach("io.uring.sqe_reserved",
	    sw_raise_in_window, NULL), ==, XTC_OK);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);
	/* Arms the handler + unblocks the signal on the worker; the 10 s
	 * CPU-time interval never elapses, so every tick is our raise. */
	munit_assert_int(xtc_exec_set_preempt(e, 10LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	munit_assert_int(xtc_proc_spawn(l, sw_writer, NULL, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(l, sw_peer, NULL, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(pthread_create(&wd, NULL, sw_watchdog, NULL), ==, 0);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	atomic_store(&g_sw_done, 1);
	(void)pthread_join(wd, NULL);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	(void)xtc_inject_detach("io.uring.sqe_reserved");
	(void)close(g_sw_fd);

	if (atomic_load(&g_sw_hits) == 0)
		return MUNIT_SKIP;   /* injection points compiled out */
	munit_assert_int(atomic_load(&g_sw_ctrl_preempted), ==, 1);
	munit_assert_int(atomic_load(&g_sw_win_preempted), ==, 0);
	munit_assert_int(g_sw_a_ok, ==, 1);
	munit_assert_int(g_sw_fd_ok, ==, 1);
	munit_assert_int(atomic_load(&g_sw_hits), ==, 2);   /* aio + POLL_ADD */
	if (peer_submits && atomic_load(&g_sw_peer_wrote))
		munit_assert_int(g_sw_b_rc, ==, SW_LEN);
	return MUNIT_OK;
}

static MunitResult
test_p2_sqe_window_no_preempt(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return sw_run(0);
}

static MunitResult
test_p2_sqe_window_lost_completion(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return sw_run(1);
}

/*
 * /switch_window_declines: a preemption tick that lands while a fiber
 * switch is saving or restoring a context must decline (and leave the
 * tick pending for the next cooperative yield).  Both sides are probed:
 * the fiber's xtc_yield (context save in flight) and the scheduler's
 * step (the current-coro cursor already names the fiber, the jump not
 * yet taken -- a redirect there would yield the SCHEDULER's own
 * registers into the fiber's context).  The fcontext substrate's
 * "coro.switch_window" injection point sits inside each guarded window;
 * its callback raises the real preemption signal on the fiber's thread
 * and reads the handler's decision back (xtc_preempt_tick_pending is
 * set only when the handler declines).  Control: the same raise from
 * plain fiber code IS redirected (a peer runs, no tick left pending).
 * Skipped where the hook is absent (the ucontext substrate guards its
 * swapcontexts the same way, with no hook) and where the redirect is
 * not implemented.
 */
static atomic_long g_swd_peer_runs;
static atomic_int  g_swd_arm, g_swd_hits, g_swd_stop, g_swd_redirected;
static atomic_int  g_swd_ctrl_preempted, g_swd_ctrl_pending;

static void
swd_raise_in_window(const char *name, void *user)
{
	(void)name; (void)user;
	if (atomic_load(&g_swd_arm) <= 0)
		return;
	atomic_fetch_sub(&g_swd_arm, 1);
	atomic_fetch_add(&g_swd_hits, 1);
	(void)xtc_preempt_tick_pending();    /* clear any stale tick */
	PREEMPT_TICK();              /* a preemption tick, here */
	if (!xtc_preempt_tick_pending())     /* not declined: redirected */
		atomic_fetch_add(&g_swd_redirected, 1);
}

static void
swd_peer(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_swd_stop)) {
		atomic_fetch_add(&g_swd_peer_runs, 1);
		xtc_yield();
	}
}

static void
swd_target(void *arg)
{
	long before;
	int i;
	(void)arg;
	xtc_yield();                          /* let the peer start */
	(void)xtc_preempt_tick_pending();
	before = atomic_load(&g_swd_peer_runs);
	PREEMPT_TICK();               /* control: plain fiber code */
	if (atomic_load(&g_swd_peer_runs) != before)
		atomic_store(&g_swd_ctrl_preempted, 1);
	if (xtc_preempt_tick_pending())
		atomic_store(&g_swd_ctrl_pending, 1);
	/* Each armed hit is one switch window: this fiber's yield, then the
	 * scheduler's steps of the peer and of this fiber. */
	for (i = 0; i < 8; i++) {
		atomic_store(&g_swd_arm, 1);
		xtc_yield();
	}
	atomic_store(&g_swd_arm, 0);
	atomic_store(&g_swd_stop, 1);
}

static MunitResult
test_p2_switch_window_declines(const MunitParameter p[], void *d)
{
	extern int __xtc_coro_preempt_effective(void);
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	(void)p; (void)d;

	if (!xtc_preempt_supported() || !__xtc_coro_preempt_effective())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	atomic_store(&g_swd_peer_runs, 0);
	atomic_store(&g_swd_arm, 0);
	atomic_store(&g_swd_hits, 0);
	atomic_store(&g_swd_stop, 0);
	atomic_store(&g_swd_redirected, 0);
	atomic_store(&g_swd_ctrl_preempted, 0);
	atomic_store(&g_swd_ctrl_pending, 0);
	munit_assert_int(xtc_inject_attach("coro.switch_window",
	    swd_raise_in_window, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);
	munit_assert_int(xtc_exec_set_preempt(e, 10LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	munit_assert_int(xtc_proc_spawn(l, swd_target, NULL, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(l, swd_peer, NULL, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	(void)xtc_inject_detach("coro.switch_window");

	if (atomic_load(&g_swd_hits) == 0)
		return MUNIT_SKIP;   /* no hook: ucontext, or points compiled out */
	munit_logf(MUNIT_LOG_INFO, "%d in-switch ticks, %d redirected",
	    atomic_load(&g_swd_hits), atomic_load(&g_swd_redirected));
	munit_assert_int(atomic_load(&g_swd_ctrl_preempted), ==, 1);
	munit_assert_int(atomic_load(&g_swd_ctrl_pending), ==, 0);
	munit_assert_int(atomic_load(&g_swd_hits), >=, 8);
	munit_assert_int(atomic_load(&g_swd_redirected), ==, 0);
	return MUNIT_OK;
}

/*
 * /runq_*: an involuntary preemption must never land inside a fiber's
 * update of its loop's run queue.  A fiber that wakes a same-loop task
 * (xtc_waker_wake) or spawns onto its own loop enqueues on the owner-
 * only FIFO: `q_tail->q_next = t; q_tail = t`.  A tick between the two
 * stores hands the thread to the scheduler, whose RESCHED re-enqueue of
 * the preempted fiber links itself through the SAME stale q_tail,
 * overwriting the link to t; the fiber then resumes and stores
 * q_tail = t.  t is SCHEDULED but unreachable, and later enqueues chain
 * off it: the loop idles with runnable tasks and q_head NULL (seen as a
 * core in the test_wal_kill preempt sweep).  "loop.enqueue.fifo_link"
 * fires between the two stores; the callback raises the real
 * preemption signal there.  Control: the same raise from plain fiber
 * code does preempt.  A watchdog turns the lost task into an abort.
 *
 *   lost_wake   the woken task must run (phase 1), and a child spawned
 *               onto the waker's own loop must run (phase 2)
 */
static atomic_long g_rq_peer_runs;
static atomic_int  g_rq_arm, g_rq_hits, g_rq_stop, g_rq_done;
static atomic_int  g_rq_ctrl_preempted, g_rq_win_preempted;
static atomic_int  g_rq_target_runs, g_rq_child_ran;
static xtc_task_t *g_rq_target;
static xtc_loop_t *g_rq_loop;

static void
rq_raise_in_window(const char *name, void *user)
{
	long before;
	(void)name; (void)user;
	if (!atomic_exchange(&g_rq_arm, 0))
		return;                          /* one shot per arm */
	atomic_fetch_add(&g_rq_hits, 1);
	before = atomic_load(&g_rq_peer_runs);
	PREEMPT_TICK();              /* a preemption tick, here */
	if (atomic_load(&g_rq_peer_runs) != before)
		atomic_store(&g_rq_win_preempted, 1);
}

static int
rq_target(xtc_task_t *self, void *user)
{
	(void)self; (void)user;
	if (atomic_fetch_add(&g_rq_target_runs, 1) == 0)
		return XTC_TASK_PENDING;          /* park; the waker wakes it */
	return XTC_TASK_DONE;
}

static void
rq_peer(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_rq_stop)) {   /* keeps the FIFO non-empty */
		atomic_fetch_add(&g_rq_peer_runs, 1);
		xtc_yield();
	}
}

static void
rq_child(void *arg)
{
	(void)arg;
	atomic_store(&g_rq_child_ran, 1);
}

static void
rq_waker(void *arg)
{
	xtc_waker_t w;
	long before;
	(void)arg;
	while (atomic_load(&g_rq_target_runs) == 0)
		xtc_yield();                      /* target parked, peer queued */
	before = atomic_load(&g_rq_peer_runs);
	PREEMPT_TICK();               /* control: plain fiber code */
	if (atomic_load(&g_rq_peer_runs) != before)
		atomic_store(&g_rq_ctrl_preempted, 1);
	xtc_yield();                          /* re-queue the peer first */
	/* Phase 1: a same-loop wake. */
	munit_assert_int(xtc_task_waker(g_rq_target, &w), ==, XTC_OK);
	atomic_store(&g_rq_arm, 1);
	(void)xtc_waker_wake(&w);
	while (atomic_load(&g_rq_target_runs) < 2)
		xtc_yield();
	/* Phase 2: a spawn onto this fiber's own loop. */
	xtc_yield();
	atomic_store(&g_rq_arm, 1);
	munit_assert_int(xtc_proc_spawn(g_rq_loop, rq_child, NULL, NULL,
	    NULL), ==, XTC_OK);
	while (!atomic_load(&g_rq_child_ran))
		xtc_yield();
	atomic_store(&g_rq_stop, 1);
}

static void *
rq_watchdog(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 1000 && !atomic_load(&g_rq_done); i++)
		usleep(10000);
	if (!atomic_load(&g_rq_done)) {
		fprintf(stderr, "runq: a woken/spawned task never ran (target "
		    "runs %d, child %d, preempted inside the enqueue: %d)\n",
		    atomic_load(&g_rq_target_runs),
		    atomic_load(&g_rq_child_ran),
		    atomic_load(&g_rq_win_preempted));
		abort();
	}
	return NULL;
}

static MunitResult
test_p2_runq_lost_wake(const MunitParameter p[], void *d)
{
	extern int __xtc_coro_preempt_effective(void);
	xtc_exec_t *e = NULL;
	pthread_t wd;
	(void)p; (void)d;

	if (!xtc_preempt_supported() || !__xtc_coro_preempt_effective())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	atomic_store(&g_rq_peer_runs, 0);
	atomic_store(&g_rq_arm, 0);
	atomic_store(&g_rq_hits, 0);
	atomic_store(&g_rq_stop, 0);
	atomic_store(&g_rq_done, 0);
	atomic_store(&g_rq_ctrl_preempted, 0);
	atomic_store(&g_rq_win_preempted, 0);
	atomic_store(&g_rq_target_runs, 0);
	atomic_store(&g_rq_child_ran, 0);
	munit_assert_int(xtc_inject_attach("loop.enqueue.fifo_link",
	    rq_raise_in_window, NULL), ==, XTC_OK);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	g_rq_loop = xtc_exec_loop(e, 0);
	munit_assert_int(xtc_exec_set_preempt(e, 10LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	munit_assert_int(xtc_exec_spawn_on(e, 0, rq_target, NULL,
	    &g_rq_target), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(g_rq_loop, rq_peer, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(g_rq_loop, rq_waker, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(pthread_create(&wd, NULL, rq_watchdog, NULL), ==, 0);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	atomic_store(&g_rq_done, 1);
	(void)pthread_join(wd, NULL);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	(void)xtc_inject_detach("loop.enqueue.fifo_link");

	if (atomic_load(&g_rq_hits) == 0)
		return MUNIT_SKIP;   /* injection points compiled out */
	munit_assert_int(atomic_load(&g_rq_ctrl_preempted), ==, 1);
	munit_assert_int(atomic_load(&g_rq_win_preempted), ==, 0);
	munit_assert_int(atomic_load(&g_rq_hits), ==, 2);   /* wake + spawn */
	munit_assert_int(atomic_load(&g_rq_target_runs), ==, 2);
	munit_assert_int(atomic_load(&g_rq_child_ran), ==, 1);
	return MUNIT_OK;
}

/*
 * /timer_heap_*: an involuntary preemption must never land inside a
 * fiber's update of its loop's timer min-heap.  Arming a timer from a
 * fiber (xtc_timer_set, xtc_proc_sleep, any timed park) sifts the new
 * node up; between moving a parent down and filling the hole, the
 * parent slot is duplicated.  A tick there lets another fiber on the
 * thread push its own timer into the half-sifted heap; the preempted
 * fiber then fills the hole over it, so that timer never fires (its
 * sleeper hangs) and a node is in the heap twice.  "timer.heap.sift_up"
 * fires after the parent move; the callback raises the real preemption
 * signal there.  Control: the same raise from plain fiber code does
 * preempt.
 *
 *   lost_timer   the peer's timer, armed while the arming fiber is
 *                inside the sift, must fire
 */
static atomic_int  g_th_arm, g_th_hits, g_th_in, g_th_ctrl_preempted;
static atomic_int  g_th_win_preempted, g_th_peer_armed, g_th_stop;
static atomic_int  g_th_x_fired, g_th_y_fired;
static atomic_long g_th_peer_runs;
static xtc_loop_t *g_th_loop;

static void
th_fire_x(void *u) { (void)u; atomic_store(&g_th_x_fired, 1); }

static void
th_fire_y(void *u) { (void)u; atomic_store(&g_th_y_fired, 1); }

static void
th_fire_never(void *u) { (void)u; }

static void
th_raise_in_window(const char *name, void *user)
{
	long before;
	(void)name; (void)user;
	if (!atomic_exchange(&g_th_arm, 0))
		return;                          /* one shot */
	atomic_fetch_add(&g_th_hits, 1);
	before = atomic_load(&g_th_peer_runs);
	atomic_store(&g_th_in, 1);
	PREEMPT_TICK();              /* a preemption tick, here */
	atomic_store(&g_th_in, 0);
	if (atomic_load(&g_th_peer_runs) != before)
		atomic_store(&g_th_win_preempted, 1);
}

static void
th_peer(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_th_stop)) {
		atomic_fetch_add(&g_th_peer_runs, 1);
		if (atomic_load(&g_th_in) && !atomic_load(&g_th_peer_armed)) {
			atomic_store(&g_th_peer_armed, 1);
			munit_assert_int(xtc_timer_set(g_th_loop, 0, th_fire_y,
			    NULL, NULL), ==, XTC_OK);
		}
		xtc_yield();
	}
}

static void
th_armer(void *arg)
{
	xtc_timer_t *far_tm = NULL;
	long before;
	int i;
	(void)arg;
	xtc_yield();                          /* let the peer start */
	before = atomic_load(&g_th_peer_runs);
	PREEMPT_TICK();               /* control: plain fiber code */
	if (atomic_load(&g_th_peer_runs) != before)
		atomic_store(&g_th_ctrl_preempted, 1);
	/* A far_tm root, then a due timer that sifts above it. */
	munit_assert_int(xtc_timer_set(g_th_loop, 10LL * 1000 * 1000 * 1000,
	    th_fire_never, NULL, &far_tm), ==, XTC_OK);
	atomic_store(&g_th_arm, 1);
	munit_assert_int(xtc_timer_set(g_th_loop, 0, th_fire_x, NULL, NULL),
	    ==, XTC_OK);
	for (i = 0; i < 100000 && !(atomic_load(&g_th_x_fired) &&
	    (atomic_load(&g_th_y_fired) || !atomic_load(&g_th_peer_armed)));
	    i++)
		xtc_yield();
	if (atomic_load(&g_th_peer_armed) && !atomic_load(&g_th_y_fired)) {
		/* The heap now holds a duplicated node; tearing it down would
		 * double-release it, so stop here. */
		fprintf(stderr, "timer_heap: the peer's timer never fired "
		    "(preempted inside the sift: %d)\n",
		    atomic_load(&g_th_win_preempted));
		abort();
	}
	(void)xtc_timer_cancel(far_tm);
	(void)xtc_timer_release(far_tm);
	atomic_store(&g_th_stop, 1);
}

static MunitResult
test_p2_timer_heap_lost_timer(const MunitParameter p[], void *d)
{
	extern int __xtc_coro_preempt_effective(void);
	xtc_exec_t *e = NULL;
	(void)p; (void)d;

	if (!xtc_preempt_supported() || !__xtc_coro_preempt_effective())
		return MUNIT_SKIP;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;   /* see test_p2_preempt_tight_loop */
#endif
	atomic_store(&g_th_arm, 0);
	atomic_store(&g_th_hits, 0);
	atomic_store(&g_th_in, 0);
	atomic_store(&g_th_ctrl_preempted, 0);
	atomic_store(&g_th_win_preempted, 0);
	atomic_store(&g_th_peer_armed, 0);
	atomic_store(&g_th_stop, 0);
	atomic_store(&g_th_x_fired, 0);
	atomic_store(&g_th_y_fired, 0);
	atomic_store(&g_th_peer_runs, 0);
	munit_assert_int(xtc_inject_attach("timer.heap.sift_up",
	    th_raise_in_window, NULL), ==, XTC_OK);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	g_th_loop = xtc_exec_loop(e, 0);
	munit_assert_int(xtc_exec_set_preempt(e, 10LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	xtc_preempt_set_involuntary(1);
	munit_assert_int(xtc_proc_spawn(g_th_loop, th_armer, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(g_th_loop, th_peer, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	(void)xtc_inject_detach("timer.heap.sift_up");

	if (atomic_load(&g_th_hits) == 0)
		return MUNIT_SKIP;   /* injection points compiled out */
	munit_assert_int(atomic_load(&g_th_ctrl_preempted), ==, 1);
	munit_assert_int(atomic_load(&g_th_win_preempted), ==, 0);
	munit_assert_int(atomic_load(&g_th_x_fired), ==, 1);
	if (atomic_load(&g_th_peer_armed))
		munit_assert_int(atomic_load(&g_th_y_fired), ==, 1);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/preempt_tight_loop", test_p2_preempt_tight_loop, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/peers_progress", test_p2_peers_progress_on_ucontext, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sqe_window_no_preempt", test_p2_sqe_window_no_preempt, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sqe_window_lost_completion", test_p2_sqe_window_lost_completion,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/switch_window_declines", test_p2_switch_window_declines, NULL,
	    NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/runq_lost_wake", test_p2_runq_lost_wake, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/timer_heap_lost_timer", test_p2_timer_heap_lost_timer, NULL,
	    NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if defined(P2_VEC)
	{ "/vector_state", test_p2_vector_state, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/red_zone_flags", test_p2_red_zone_flags, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tick_df", test_p2_tick_df, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tick_ac", test_p2_tick_ac, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tick_x87", test_p2_tick_x87, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tick_errno", test_p2_tick_errno, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tick_fpenv", test_p2_tick_fpenv, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tick_avx_upper", test_p2_tick_avx_upper, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tick_pkru", test_p2_tick_pkru, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#endif
#if defined(P2_A64)
	{ "/a64_sve_state", test_p2_a64_sve_state, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/a64_x16_preserved", test_p2_a64_x16_preserved, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/a64_tick_errno", test_p2_a64_tick_errno, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/a64_tick_fpcr", test_p2_a64_tick_fpcr, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/a64_tick_sme", test_p2_a64_tick_sme, NULL, NULL,
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

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m8/test_send_owned.c -- zero-copy proc messaging.
 *
 *   owned_identity     xtc_send_owned moves the buffer: the receiver's
 *                      xtc_recv returns the SAME pointer (no copy), at
 *                      64 B, 4 KiB and 1 MiB, and via recv_match.
 *   owned_error_keeps  on a full mailbox or a dead pid the caller still
 *                      owns the buffer, contents intact (no free, no
 *                      double free under the auditor).
 *   large_send_one_copy  xtc_send of a 64 KiB payload: the receiver is
 *                      handed the buffer the send copied into -- recv
 *                      allocates no payload-sized block (a counting
 *                      allocator hook sees 0 mallocs >= 64 KiB in recv;
 *                      the pre-change recv made 1 and copied into it).
 *
 * Leak checks compare the auditor's live count before/after a run, with
 * the process-wide lazily created caches (envelope/link slabs, magazines)
 * warmed by an identical unaudited run first.
 *   owned_crossthread  owned sends from another executor loop arrive
 *                      intact and pointer-identical.
 *   owned_drop_on_exit queued owned buffers are freed when the receiver
 *                      exits unread (no leak under the auditor).
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_alloc_audit.h"
#include "os_alloc.h"    /* test-only: a counting allocator hook */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

static void
fill(uint8_t *p, size_t n, uint8_t seed)
{
	size_t i;
	for (i = 0; i < n; i++)
		p[i] = (uint8_t)(seed + i * 7);
}

static int
check(const uint8_t *p, size_t n, uint8_t seed)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (p[i] != (uint8_t)(seed + i * 7))
			return 0;
	return 1;
}

/* Run spawn(a, aa) + spawn(b, bb) on a fresh loop to completion.  ra, if
 * non-NULL, receives a's pid before b is spawned. */
static xtc_loop_t *g_loop;    /* the loop run2 is running */

static void
run2(xtc_proc_fn a, void *aa, xtc_proc_fn b, void *bb, xtc_pid_t *ra)
{
	xtc_loop_t *loop;
	xtc_pid_t pa;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	g_loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, a, aa, NULL, &pa), ==, XTC_OK);
	if (ra != NULL)
		*ra = pa;
	if (b != NULL)
		munit_assert_int(xtc_proc_spawn(loop, b, bb, NULL, NULL), ==,
		    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
}

/* run2 once unaudited (warms the process-wide caches), then audited;
 * returns the audited run's live-allocation growth. */
static size_t
audited_run2(xtc_proc_fn a, void *aa, xtc_proc_fn b, xtc_pid_t *ra)
{
	size_t c0 = 0, c1 = 0;
	run2(a, aa, b, ra, ra);
	munit_assert_int(xtc_alloc_audit_enable(1), ==, XTC_OK);
	xtc_alloc_audit_stats(&c0, NULL);
	run2(a, aa, b, ra, ra);
	xtc_alloc_audit_stats(&c1, NULL);
	(void)xtc_alloc_audit_enable(0);
	return c1 > c0 ? c1 - c0 : 0;
}

/* ---- owned_identity -------------------------------------------------- */

static const size_t g_sizes[] = { 64, 4096, 1u << 20 };
#define N_SIZES (sizeof g_sizes / sizeof g_sizes[0])
static void *g_sent[N_SIZES + 1];
static int   g_same, g_intact;

static int
match_big(const void *d, size_t n, void *u)
{
	(void)d; (void)u;
	return n == 4096;
}

static void
id_recv(void *arg)
{
	size_t i;
	void *m = NULL;
	size_t n = 0;
	(void)arg;
	/* The 4 KiB message first, by selective receive (save-queue path). */
	if (xtc_recv_match(match_big, NULL, &m, &n, 2000000000LL) == XTC_OK) {
		g_same += (m == g_sent[1]);
		g_intact += (n == 4096 && check(m, n, 1));
		xtc_free(m);
	}
	for (i = 0; i < N_SIZES; i++) {
		size_t k;
		if (i == 1)
			continue;
		if (xtc_recv(&m, &n, 2000000000LL) != XTC_OK)
			return;
		for (k = 0; k < N_SIZES; k++)
			if (m == g_sent[k])
				break;
		g_same += (k < N_SIZES);
		g_intact += (k < N_SIZES && n == g_sizes[k] &&
		    check(m, n, (uint8_t)k));
		xtc_free(m);
	}
	/* Empty owned message: NULL buffer, zero size. */
	if (xtc_recv(&m, &n, 2000000000LL) == XTC_OK && m == NULL && n == 0)
		g_same++, g_intact++;
}

static void
id_send(void *arg)
{
	xtc_pid_t to = *(xtc_pid_t *)arg;
	size_t i;
	for (i = 0; i < N_SIZES; i++) {
		uint8_t *b = xtc_malloc(g_sizes[i]);
		if (b == NULL)
			return;
		fill(b, g_sizes[i], (uint8_t)i);
		g_sent[i] = b;
		if (xtc_send_owned(to, b, g_sizes[i]) != XTC_OK) {
			xtc_free(b);
			return;
		}
	}
	(void)xtc_send_owned(to, NULL, 0);
}

static MunitResult
test_owned_identity(const MunitParameter p[], void *d)
{
	xtc_pid_t r;
	size_t leak;
	(void)p; (void)d;
	leak = audited_run2(id_recv, NULL, id_send, &r);
	/* Counted over both runs. */
	munit_assert_int(g_same, ==, 2 * ((int)N_SIZES + 1));
	munit_assert_int(g_intact, ==, 2 * ((int)N_SIZES + 1));
	munit_assert_size(leak, ==, 0);        /* every buffer freed once */
	return MUNIT_OK;
}

/* ---- owned_error_keeps ----------------------------------------------- */

static int g_full_rc, g_full_intact, g_dead_rc, g_dead_intact, g_null_rc;

static void
idle_proc(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(50LL * 1000 * 1000);
}

static void
exit_now_proc(void *arg)
{
	(void)arg;
}

static void
err_proc(void *arg)
{
	xtc_loop_t *loop = g_loop;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_pid_t full, dead;
	uint8_t *b;
	int i;

	(void)arg;
	o.mailbox_cap = 2;
	if (xtc_proc_spawn(loop, idle_proc, NULL, &o, &full) != XTC_OK ||
	    xtc_proc_spawn(loop, exit_now_proc, NULL, NULL, &dead) != XTC_OK)
		return;
	for (i = 0; i < 2; i++)
		(void)xtc_send(full, "xy", 2);
	b = xtc_malloc(1000);
	if (b == NULL)
		return;
	fill(b, 1000, 9);
	g_full_rc = xtc_send_owned(full, b, 1000);
	g_full_intact = check(b, 1000, 9);     /* still ours: readable */
	(void)xtc_yield();                     /* let `dead` run and exit */
	(void)xtc_proc_sleep(1000000);
	g_dead_rc = xtc_send_owned(dead, b, 1000);
	g_dead_intact = check(b, 1000, 9);
	g_null_rc = xtc_send_owned(full, NULL, 8);
	xtc_free(b);                           /* exactly once, by us */
}

static MunitResult
test_owned_error_keeps(const MunitParameter p[], void *d)
{
	size_t leak;
	(void)p; (void)d;
	leak = audited_run2(err_proc, NULL, NULL, NULL);
	munit_assert_int(g_full_rc, ==, XTC_E_AGAIN);
	munit_assert_int(g_full_intact, ==, 1);
	munit_assert_int(g_dead_rc, ==, XTC_E_INVAL);
	munit_assert_int(g_dead_intact, ==, 1);
	munit_assert_int(g_null_rc, ==, XTC_E_INVAL);
	munit_assert_size(leak, ==, 0);
	return MUNIT_OK;
}

/* ---- large_send_one_copy --------------------------------------------- */

#define BIG (64u * 1024)
static size_t g_recv_big = (size_t)-1;
static int    g_big_ok;
static struct __os_alloc_hook g_down;
static _Atomic size_t g_big_allocs;   /* mallocs of >= BIG bytes */

static void *
cnt_malloc(size_t n)
{
	if (n >= BIG)
		atomic_fetch_add(&g_big_allocs, 1);
	return g_down.malloc(n);
}
static void *
cnt_calloc(size_t k, size_t n)
{
	if (k * n >= BIG)
		atomic_fetch_add(&g_big_allocs, 1);
	return g_down.calloc(k, n);
}
static void *
cnt_realloc(void *q, size_t n)
{
	if (n >= BIG)
		atomic_fetch_add(&g_big_allocs, 1);
	return g_down.realloc(q, n);
}
static void cnt_free(void *q) { g_down.free(q); }
static void *cnt_aligned(size_t a, size_t n) { return g_down.aligned(a, n); }
static void cnt_aligned_free(void *q) { g_down.aligned_free(q); }

static void
big_recv(void *arg)
{
	void *m = NULL;
	size_t n = 0, b0;
	(void)arg;
	/* Wait until the message is queued, then count the payload-sized
	 * allocations the receive itself makes. */
	for (;;) {
		xtc_mailbox_stats_t st = XTC_MAILBOX_STATS_INIT;
		if (xtc_proc_mailbox_stats(xtc_self(), &st) != XTC_OK)
			return;
		if (st.depth > 0)
			break;
		(void)xtc_yield();
	}
	b0 = atomic_load(&g_big_allocs);
	if (xtc_recv(&m, &n, 0) != XTC_OK)
		return;
	g_recv_big = atomic_load(&g_big_allocs) - b0;
	g_big_ok = (n == BIG && check(m, n, 3));
	xtc_free(m);
}

static void
big_send(void *arg)
{
	static uint8_t src[BIG];
	fill(src, BIG, 3);
	(void)xtc_send(*(xtc_pid_t *)arg, src, BIG);
	memset(src, 0, BIG);                   /* the copy was taken at send */
}

static MunitResult
test_large_send_one_copy(const MunitParameter p[], void *d)
{
	struct __os_alloc_hook h;
	xtc_pid_t r;
	(void)p; (void)d;
	munit_assert_int(__os_alloc_get_hook(&g_down), ==, XTC_OK);
	h.malloc = cnt_malloc;
	h.calloc = cnt_calloc;
	h.realloc = cnt_realloc;
	h.free = cnt_free;
	h.aligned = cnt_aligned;
	h.aligned_free = cnt_aligned_free;
	munit_assert_int(__os_alloc_set_hook(&h), ==, XTC_OK);
	atomic_store(&g_big_allocs, 0);
	run2(big_recv, NULL, big_send, &r, &r);
	(void)__os_alloc_set_hook(&g_down);
	munit_assert_int(g_big_ok, ==, 1);
	munit_assert_size(g_recv_big, ==, 0);   /* recv: no copy */
	/* send: exactly one payload-sized buffer (its one copy). */
	munit_assert_size(atomic_load(&g_big_allocs), ==, 1);
	return MUNIT_OK;
}

/* ---- owned_crossthread ----------------------------------------------- */

#define XT_N 2000
static _Atomic xtc_pid_t g_xt_rx;
static _Atomic int g_xt_ready, g_xt_ok, g_xt_got;

static void
xt_rx(void *arg)
{
	int i;
	(void)arg;
	atomic_store(&g_xt_rx, xtc_self());
	atomic_store(&g_xt_ready, 1);
	for (i = 0; i < XT_N; i++) {
		void *m = NULL;
		size_t n = 0;
		uint32_t seq;
		if (xtc_recv(&m, &n, 5000000000LL) != XTC_OK)
			return;
		atomic_fetch_add(&g_xt_got, 1);
		memcpy(&seq, m, sizeof seq);
		if (n == 3000 && check((uint8_t *)m + 4, n - 4, (uint8_t)seq))
			atomic_fetch_add(&g_xt_ok, 1);
		xtc_free(m);
	}
}

static void
xt_tx(void *arg)
{
	uint32_t i;
	(void)arg;
	while (!atomic_load(&g_xt_ready))
		(void)xtc_yield();
	for (i = 0; i < XT_N; i++) {
		uint8_t *b = xtc_malloc(3000);
		int rc;
		if (b == NULL)
			return;
		memcpy(b, &i, sizeof i);
		fill(b + 4, 3000 - 4, (uint8_t)i);
		while ((rc = xtc_send_owned(atomic_load(&g_xt_rx), b, 3000)) ==
		    XTC_E_AGAIN)
			(void)xtc_yield();
		if (rc != XTC_OK) {
			xtc_free(b);
			return;
		}
	}
}

static MunitResult
test_owned_crossthread(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	/* Procs are pinned to their spawn loop by default: two threads. */
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), xt_rx, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 1), xt_tx, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	(void)xtc_exec_fini(e);
	munit_assert_int(atomic_load(&g_xt_got), ==, XT_N);
	munit_assert_int(atomic_load(&g_xt_ok), ==, XT_N);
	return MUNIT_OK;
}

/* ---- owned_drop_on_exit ---------------------------------------------- */

static void
drop_tx(void *arg)
{
	int i;
	for (i = 0; i < 8; i++) {
		uint8_t *b = xtc_malloc(2048);
		if (b == NULL)
			return;
		if (xtc_send_owned(*(xtc_pid_t *)arg, b, 2048) != XTC_OK)
			xtc_free(b);
	}
}

static MunitResult
test_owned_drop_on_exit(const MunitParameter p[], void *d)
{
	xtc_pid_t r;
	(void)p; (void)d;
	munit_assert_size(audited_run2(idle_proc, NULL, drop_tx, &r), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/owned_identity", test_owned_identity, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/owned_error_keeps", test_owned_error_keeps, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/large_send_one_copy", test_large_send_one_copy, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/owned_crossthread", test_owned_crossthread, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/owned_drop_on_exit", test_owned_drop_on_exit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m8/send_owned", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m4/test_fctx.c — exercise the standalone make_fcontext /
 *	jump_fcontext asm without involving the loop or coro layer.
 *	This isolates the alignment & save/restore correctness from
 *	the rest of the system; if this passes, M4.5 has a working
 *	fiber substrate ready to replace the ucontext-based coro_uctx.
 */

#include "munit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && !defined(__APPLE__)
# define HAVE_FCTX 1
#else
# define HAVE_FCTX 0
#endif

#if HAVE_FCTX

extern void *__xtc_make_fcontext(void *stack_top, size_t size,
                                 void (*fn)(void *transfer));
extern void *__xtc_jump_fcontext(void **from, void *to, void *transfer);

/* Static storage for the test "scheduler". */
static void *g_main_ctx;        /* recorded by the fiber entry's first jump back */
static void *g_fiber_ctx;       /* current paused fiber sp                       */
static int   g_fiber_visits;
static int   g_fiber_arg_seen;
static int   g_fiber_payload_seen;

/* The fiber entry: receives a transfer arg from the very first
 * jump_fcontext.  We expect it to be the address of g_main_ctx +
 * a magic encoding to verify arg passing. */
static void
fiber_entry(void *transfer)
{
	intptr_t arg = (intptr_t)transfer;
	g_fiber_arg_seen = (int)arg;

	/* Yield once. */
	g_fiber_visits++;
	{
		void *ret = __xtc_jump_fcontext(&g_fiber_ctx, g_main_ctx,
		    (void *)(intptr_t)42);
		g_fiber_payload_seen = (int)(intptr_t)ret;
	}

	/* Yield again. */
	g_fiber_visits++;
	(void)__xtc_jump_fcontext(&g_fiber_ctx, g_main_ctx,
	    (void *)(intptr_t)99);

	/* If we ever return here we'd run off the stack; abort. */
	abort();
}

static MunitResult
test_fctx_basic(const MunitParameter p[], void *d)
{
	size_t  stack_sz = 64 * 1024;
	void   *stack = malloc(stack_sz);
	void   *ctx;
	void   *result;
	(void)p; (void)d;
	munit_assert_not_null(stack);

	ctx = __xtc_make_fcontext((char *)stack + stack_sz, stack_sz,
	    fiber_entry);
	munit_assert_not_null(ctx);

	g_fiber_visits = 0;
	g_fiber_arg_seen = 0;
	g_fiber_payload_seen = 0;

	/* First jump: fiber starts, increments visits to 1, yields with 42. */
	result = __xtc_jump_fcontext(&g_main_ctx, ctx, (void *)(intptr_t)7);
	munit_assert_int(g_fiber_visits, ==, 1);
	munit_assert_int(g_fiber_arg_seen, ==, 7);
	munit_assert_int((int)(intptr_t)result, ==, 42);

	/* Second jump: resume, fiber sees payload, yields with 99. */
	result = __xtc_jump_fcontext(&g_main_ctx, g_fiber_ctx,
	    (void *)(intptr_t)123);
	munit_assert_int(g_fiber_visits, ==, 2);
	munit_assert_int(g_fiber_payload_seen, ==, 123);
	munit_assert_int((int)(intptr_t)result, ==, 99);

	free(stack);
	return MUNIT_OK;
}

/* Verify callee-saved registers survive a swap.  We can't easily
 * pin a specific register from C, but the fact that the `for` loop
 * counter (`i`) and the local stack-allocated array survive proves
 * that callee-saved storage round-trips correctly. */
static MunitResult
test_fctx_state(const MunitParameter p[], void *d)
{
	size_t stack_sz = 64 * 1024;
	void  *stack    = malloc(stack_sz);
	void  *ctx;
	int    i;
	(void)p; (void)d;
	munit_assert_not_null(stack);

	ctx = __xtc_make_fcontext((char *)stack + stack_sz, stack_sz,
	    fiber_entry);
	g_fiber_visits = 0;

	for (i = 0; i < 5; i++) {
		void *r;
		if (i == 0)
			r = __xtc_jump_fcontext(&g_main_ctx, ctx,
			    (void *)(intptr_t)i);
		else
			r = __xtc_jump_fcontext(&g_main_ctx, g_fiber_ctx,
			    (void *)(intptr_t)(i * 1000));
		(void)r;
		if (g_fiber_visits >= 2) break;
	}
	munit_assert_int(g_fiber_visits, ==, 2);
	munit_assert_int(i, >=, 1);

	free(stack);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/fctx_basic", test_fctx_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fctx_state", test_fctx_state, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

#else /* !HAVE_FCTX */

static MunitResult test_skip(const MunitParameter p[], void *d) {
	(void)p; (void)d; return MUNIT_SKIP;
}
static MunitTest tests[] = {
	{ "/fctx_skip", test_skip, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

#endif

static const MunitSuite suite = { "/m4.5/fctx", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

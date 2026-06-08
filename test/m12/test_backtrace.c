/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m12/test_backtrace.c -- the __os_backtrace seam: capture, emit,
 * and (where the platform symbolizes) function-name resolution.
 *
 * The capture functions below have EXTERNAL linkage on purpose: with
 * the binary linked -rdynamic (see the Makefile rule), their names land
 * in the dynamic symbol table so both the execinfo and the libunwind+
 * dladdr backends can resolve them.  static helpers would symbolize to
 * an address+offset only.
 *
 * Backend-portability of the assertions:
 *   - capture always returns >= 2 frames from a 3-deep chain (the chain
 *     itself plus the harness above it) on any real backend;
 *   - emit produces one line per frame;
 *   - if the emitted text shows ANY resolved name (detected by probing
 *     for `main`, which both symbolizing backends resolve under
 *     -rdynamic), we additionally require at least one of our own chain
 *     frames to be named.  On an addresses-only platform no names appear
 *     and we assert the frame count alone, exactly as documented in
 *     docs/M_PORT.md.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "os_backtrace.h"

/* Three-deep call chain with external linkage (see file comment).
 *
 * noinline (and noclone) keep the optimizer from collapsing the chain
 * into the caller at -O2, which would otherwise leave only `main` in the
 * trace.  Each level does a tiny bit of post-call work on a volatile so
 * the call is not turned into a tail call (a tail call replaces the
 * frame and would also hide the intermediate function).  Compilers
 * without these attributes still build -- the names just may collapse,
 * and the test falls back to asserting the frame count. */
#if defined(__GNUC__)
#define BT_NOINLINE __attribute__((noinline))
#else
#define BT_NOINLINE
#endif

int  test_bt_capture_leaf(void **frames, int max) BT_NOINLINE;
int  test_bt_capture_mid(void **frames, int max) BT_NOINLINE;
int  test_bt_capture_root(void **frames, int max) BT_NOINLINE;

static volatile int bt_sink;

int
test_bt_capture_leaf(void **frames, int max)
{
	int n = __os_backtrace(frames, max);
	bt_sink = n;
	return n;
}

int
test_bt_capture_mid(void **frames, int max)
{
	int n = test_bt_capture_leaf(frames, max);
	bt_sink = n + 1;
	return n;
}

int
test_bt_capture_root(void **frames, int max)
{
	int n = test_bt_capture_mid(frames, max);
	bt_sink = n + 2;
	return n;
}

/* Drain __os_backtrace_emit into a heap buffer via a pipe. */
static char *
emit_to_string(void *const *frames, int n)
{
	int p[2];
	char *out;
	size_t cap = 16384, off = 0;

	out = malloc(cap);
	munit_assert_not_null(out);
	if (pipe(p) != 0) {
		free(out);
		return NULL;
	}
	__os_backtrace_emit(p[1], frames, n);
	(void)close(p[1]);
	for (;;) {
		ssize_t r = read(p[0], out + off, cap - 1 - off);
		if (r <= 0)
			break;
		off += (size_t)r;
		if (off >= cap - 1)
			break;
	}
	(void)close(p[0]);
	out[off] = '\0';
	return out;
}

/* ---------- /backtrace/capture: a real backend yields >= 2 frames ----- */

static MunitResult
test_capture(const MunitParameter p[], void *d)
{
	void *frames[64];
	int n;
	(void)p; (void)d;

	if (!__os_backtrace_supported()) {
		/* Honest stub platform: capture returns 0, emit is silent. */
		n = test_bt_capture_root(frames, 64);
		munit_assert_int(n, ==, 0);
		return MUNIT_OK;
	}

	n = test_bt_capture_root(frames, 64);
	/* The 3-deep chain plus the munit harness above it guarantees at
	 * least two frames on any backend that actually walks the stack. */
	munit_assert_int(n, >=, 2);
	munit_assert_int(n, <=, 64);
	return MUNIT_OK;
}

/* ---------- /backtrace/emit: one line per frame, names where able ----- */

static MunitResult
test_emit(const MunitParameter p[], void *d)
{
	void *frames[64];
	int n, i, lines;
	char *text;
	(void)p; (void)d;

	if (!__os_backtrace_supported())
		return MUNIT_OK;   /* nothing to emit on the stub */

	n = test_bt_capture_root(frames, 64);
	munit_assert_int(n, >=, 2);

	text = emit_to_string(frames, n);
	munit_assert_not_null(text);
	munit_assert_int((int)strlen(text), >, 0);

	/* One newline-terminated line per frame (best-effort: at least as
	 * many newlines as frames, since each emitted frame ends in \n). */
	lines = 0;
	for (i = 0; text[i] != '\0'; i++)
		if (text[i] == '\n')
			lines++;
	munit_assert_int(lines, >=, n);

	/*
	 * Symbolization probe.  Both symbolizing backends (execinfo;
	 * libunwind+dladdr) resolve `main` under -rdynamic.  If we see it,
	 * the platform symbolizes, so at least one of our own chain frames
	 * must also be named.  Otherwise we are addresses-only and the
	 * frame count above is the whole contract.
	 */
	if (strstr(text, "main") != NULL) {
		munit_assert_true(
		    strstr(text, "test_bt_capture_leaf") != NULL ||
		    strstr(text, "test_bt_capture_mid") != NULL ||
		    strstr(text, "test_bt_capture_root") != NULL);
	}

	free(text);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/capture", test_capture, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/emit",    test_emit,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/backtrace", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char **argv)
{
	return munit_suite_main(&suite, NULL, argc, argv);
}

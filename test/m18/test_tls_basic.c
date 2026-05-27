/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m18/test_tls_basic.c
 *	Smoke tests for the xtc_tls API surface (TLS-1 task).
 *
 *	These tests exercise the public API symbols -- create/destroy
 *	lifecycle and the NOSYS stub contract -- without requiring an
 *	actual TLS handshake.  When TLS support is not compiled in
 *	(--with-tls=none) every test skips cleanly.
 *
 *	TLS-2 will extend this file with real handshake tests once the
 *	OpenSSL backend is implemented.
 */

#include "munit.h"
#include "xtc_int.h"
#include "xtc_tls.h"

/* -------------------------------------------------------------------------
 * Helpers.
 * ----------------------------------------------------------------------- */

/*
 * When TLS is not compiled in, every API call returns XTC_E_NOSYS.
 * When it IS compiled in but the TLS-2 implementation is not yet
 * landed, the skeleton also returns XTC_E_NOSYS.  Both outcomes are
 * acceptable for TLS-1.
 */
#define TLS_ACCEPTABLE(rc) \
	((rc) == XTC_OK || (rc) == XTC_E_NOSYS)

/* -------------------------------------------------------------------------
 * Test: ctx create + destroy with NULL cert files (server role).
 *
 * Verifies:
 *   - xtc_tls_ctx_create compiles and links.
 *   - With NULL cert_file / key_file, returns XTC_OK or XTC_E_NOSYS.
 *   - xtc_tls_ctx_destroy handles the result correctly (no crash).
 * ----------------------------------------------------------------------- */
static MunitResult
test_ctx_create_destroy(const MunitParameter params[], void *data)
{
	xtc_tls_opts_t opts;
	xtc_tls_ctx_t *ctx = NULL;
	int rc;

	(void)params;
	(void)data;

	/* Zero out opts so all fields are NULL / 0. */
	__builtin_memset(&opts, 0, sizeof(opts));
	opts.verify_peer  = 0;
	opts.min_version  = XTC_TLS_VER_12;
	opts.max_version  = XTC_TLS_VER_13;

	rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
	munit_assert_true(TLS_ACCEPTABLE(rc));

	if (rc == XTC_E_NOSYS) {
		/* ctx must remain NULL when NOSYS is returned. */
		munit_assert_ptr_null(ctx);
	} else {
		/* ctx must be non-NULL on success. */
		munit_assert_ptr_not_null(ctx);
	}

	/* destroy must be safe regardless (NULL is a no-op). */
	xtc_tls_ctx_destroy(ctx);
	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Test: ctx create with NULL opts (defaults).
 * ----------------------------------------------------------------------- */
static MunitResult
test_ctx_null_opts(const MunitParameter params[], void *data)
{
	xtc_tls_ctx_t *ctx = NULL;
	int rc;

	(void)params;
	(void)data;

	rc = xtc_tls_ctx_create(XTC_TLS_CLIENT, NULL, &ctx);
	munit_assert_true(TLS_ACCEPTABLE(rc));
	xtc_tls_ctx_destroy(ctx);
	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Test: NULL out-pointer returns XTC_E_INVAL.
 * ----------------------------------------------------------------------- */
static MunitResult
test_ctx_null_out(const MunitParameter params[], void *data)
{
	int rc;

	(void)params;
	(void)data;

	rc = xtc_tls_ctx_create(XTC_TLS_SERVER, NULL, NULL);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Test: ctx_destroy(NULL) is a no-op (must not crash).
 * ----------------------------------------------------------------------- */
static MunitResult
test_ctx_destroy_null(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	xtc_tls_ctx_destroy(NULL);
	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Test: xtc_tls_create with a fake fd (3) returns XTC_OK or XTC_E_NOSYS
 *       and xtc_tls_destroy handles the result.
 * ----------------------------------------------------------------------- */
static MunitResult
test_tls_create_destroy(const MunitParameter params[], void *data)
{
	xtc_tls_opts_t opts;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t     *tls = NULL;
	int ctx_rc, tls_rc;

	(void)params;
	(void)data;

	__builtin_memset(&opts, 0, sizeof(opts));
	ctx_rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
	munit_assert_true(TLS_ACCEPTABLE(ctx_rc));

	if (ctx_rc == XTC_OK) {
		/* Context was created: test per-connection create. */
		tls_rc = xtc_tls_create(ctx, 3 /* fake fd */, &tls);
		munit_assert_true(TLS_ACCEPTABLE(tls_rc));
		xtc_tls_destroy(tls);
		xtc_tls_ctx_destroy(ctx);
	}
	/* else: ctx is NULL (NOSYS); nothing more to test in TLS-1. */

	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Test: xtc_tls_create bad-argument guards.
 * ----------------------------------------------------------------------- */
static MunitResult
test_tls_create_inval(const MunitParameter params[], void *data)
{
	xtc_tls_t *tls = NULL;
	int rc;

	(void)params;
	(void)data;

	/* NULL ctx */
	rc = xtc_tls_create(NULL, 3, &tls);
	munit_assert_int(rc, ==, XTC_E_INVAL);

	/* negative fd */
	rc = xtc_tls_create((xtc_tls_ctx_t *)(uintptr_t)1, -1, &tls);
	munit_assert_int(rc, ==, XTC_E_INVAL);

	/* NULL out */
	rc = xtc_tls_create((xtc_tls_ctx_t *)(uintptr_t)1, 3, NULL);
	munit_assert_int(rc, ==, XTC_E_INVAL);

	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Test: xtc_tls_destroy(NULL) is a no-op.
 * ----------------------------------------------------------------------- */
static MunitResult
test_tls_destroy_null(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	xtc_tls_destroy(NULL);
	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Test: handshake / read / write / shutdown all return INVAL on NULL.
 * ----------------------------------------------------------------------- */
static MunitResult
test_null_tls_guards(const MunitParameter params[], void *data)
{
	size_t n = 0;
	char   buf[4] = {0};
	int    rc;

	(void)params;
	(void)data;

	rc = xtc_tls_handshake(NULL);
	munit_assert_int(rc, ==, XTC_E_INVAL);

	rc = xtc_tls_read(NULL, buf, sizeof(buf), &n);
	munit_assert_int(rc, ==, XTC_E_INVAL);

	rc = xtc_tls_write(NULL, buf, sizeof(buf), &n);
	munit_assert_int(rc, ==, XTC_E_INVAL);

	rc = xtc_tls_shutdown(NULL);
	munit_assert_int(rc, ==, XTC_E_INVAL);

	/* wants_read / wants_write return 0 on NULL, not INVAL. */
	munit_assert_int(xtc_tls_wants_read(NULL),  ==, 0);
	munit_assert_int(xtc_tls_wants_write(NULL), ==, 0);

	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Test: version constants are defined and have expected values.
 * ----------------------------------------------------------------------- */
static MunitResult
test_version_constants(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	munit_assert_int(XTC_TLS_VER_12, ==, 0x0303);
	munit_assert_int(XTC_TLS_VER_13, ==, 0x0304);
	munit_assert_int(XTC_TLS_SERVER,  ==, 0);
	munit_assert_int(XTC_TLS_CLIENT,  ==, 1);
	return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Suite.
 * ----------------------------------------------------------------------- */
static MunitTest tests[] = {
	{ "/ctx_create_destroy",   test_ctx_create_destroy, NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ctx_null_opts",        test_ctx_null_opts,      NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ctx_null_out",         test_ctx_null_out,       NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ctx_destroy_null",     test_ctx_destroy_null,   NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tls_create_destroy",   test_tls_create_destroy, NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tls_create_inval",     test_tls_create_inval,   NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tls_destroy_null",     test_tls_destroy_null,   NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ "/null_guards",          test_null_tls_guards,    NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ "/version_constants",    test_version_constants,  NULL, NULL,
	  MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m18/tls_basic", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}

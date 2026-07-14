/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_crypto.c -- __os_crypto_* / __os_csprng_* (PLAN.md 19.2 v1).
 *
 *	When the build was NOT configured with the OpenSSL TLS backend
 *	(XTC_TLS_BACKEND_OPENSSL undefined), every function under test
 *	returns XTC_E_NOSYS at runtime -- the tests below assert exactly
 *	that instead of skipping, so a --with-tls=none build still
 *	proves the "always linkable, NOSYS when unsupported" contract.
 *
 *	No PBT test here: known-answer crypto vectors are exact values
 *	from NIST/RFC, not properties over random input, so a
 *	property-based test would not add coverage a fixed vector does
 *	not already give more directly.  No DST test either: this file
 *	is not scheduler-visible behavior -- see the CSPRNG test below
 *	for the one determinism-adjacent note (RAND_bytes vs. the sim
 *	guard), which stays a plain host-mode assertion, not a sim test.
 */

#include <stdio.h>
#include <string.h>

#include "munit.h"
#include "xtc_int.h"
#include "xtc_sim.h"
#include "os_crypto.h"

#if defined(XTC_TLS_BACKEND_OPENSSL)
# define HAVE_CRYPTO 1
#else
# define HAVE_CRYPTO 0
#endif

/* Hex-decode helper for test vectors. */
static void
hex2bin(const char *hex, uint8_t *out, size_t outlen)
{
	size_t i;
	for (i = 0; i < outlen; i++) {
		unsigned int b;
		munit_assert_int(sscanf(hex + 2 * i, "%2x", &b), ==, 1);
		out[i] = (uint8_t)b;
	}
}

/* [SHA256-1] SHA-256("") == e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
static MunitResult
test_sha256_empty(const MunitParameter p[], void *d)
{
	uint8_t out[XTC_CRYPTO_SHA256_LEN];
	uint8_t expect[XTC_CRYPTO_SHA256_LEN];
	int rc;
	(void)p; (void)d;

	hex2bin("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	    expect, sizeof(expect));

	rc = __os_crypto_sha256("", 0, out);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
#endif
	return MUNIT_OK;
}

/* [SHA256-2] SHA-256("abc") == ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
static MunitResult
test_sha256_abc(const MunitParameter p[], void *d)
{
	uint8_t out[XTC_CRYPTO_SHA256_LEN];
	uint8_t expect[XTC_CRYPTO_SHA256_LEN];
	int rc;
	(void)p; (void)d;

	hex2bin("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	    expect, sizeof(expect));

	rc = __os_crypto_sha256("abc", 3, out);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
#endif
	return MUNIT_OK;
}

/* [SHA256-3] NULL out -> XTC_E_INVAL, regardless of backend. */
static MunitResult
test_sha256_inval(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__os_crypto_sha256("abc", 3, NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/*
 * [HMAC-1] RFC 4231 test case 1:
 *   key  = 0x0b (repeated 20 times)
 *   data = "Hi There"
 *   HMAC-SHA256 = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
 */
static MunitResult
test_hmac_rfc4231_case1(const MunitParameter p[], void *d)
{
	uint8_t key[20];
	uint8_t out[XTC_CRYPTO_SHA256_LEN];
	uint8_t expect[XTC_CRYPTO_SHA256_LEN];
	int rc;
	(void)p; (void)d;

	memset(key, 0x0b, sizeof(key));
	hex2bin("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
	    expect, sizeof(expect));

	rc = __os_crypto_hmac_sha256(key, sizeof(key), "Hi There", 8, out);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
#endif
	return MUNIT_OK;
}

static MunitResult
test_hmac_inval(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__os_crypto_hmac_sha256("k", 1, "d", 1, NULL),
	    ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [AESGCM-1] round trip: encrypt then decrypt recovers the plaintext. */
static MunitResult
test_aesgcm_roundtrip(const MunitParameter p[], void *d)
{
	uint8_t key[XTC_CRYPTO_AES_KEY_LEN];
	uint8_t iv[XTC_CRYPTO_AES_IV_LEN];
	uint8_t tag[XTC_CRYPTO_AES_TAG_LEN];
	const char plaintext[] = "the quick brown fox jumps over the lazy dog";
	uint8_t ciphertext[sizeof(plaintext)];
	uint8_t recovered[sizeof(plaintext)];
	const char aad[] = "wal-record-42";
	int rc;
	(void)p; (void)d;

	memset(key, 0x42, sizeof(key));
	memset(iv, 0x24, sizeof(iv));

	rc = __os_crypto_aes_gcm_encrypt(key, iv, aad, sizeof(aad),
	    plaintext, sizeof(plaintext), ciphertext, tag);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_not_equal(sizeof(plaintext), plaintext, ciphertext);

	memset(recovered, 0, sizeof(recovered));
	rc = __os_crypto_aes_gcm_decrypt(key, iv, aad, sizeof(aad),
	    ciphertext, sizeof(ciphertext), tag, recovered);
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(plaintext), recovered, plaintext);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
	(void)recovered;
#endif
	return MUNIT_OK;
}

/* [AESGCM-2] tamper: flip one ciphertext byte -> decrypt fails, and the
 * output buffer never ends up holding the original plaintext. */
static MunitResult
test_aesgcm_tamper_ciphertext(const MunitParameter p[], void *d)
{
#if HAVE_CRYPTO
	uint8_t key[XTC_CRYPTO_AES_KEY_LEN];
	uint8_t iv[XTC_CRYPTO_AES_IV_LEN];
	uint8_t tag[XTC_CRYPTO_AES_TAG_LEN];
	const char plaintext[] = "top secret wal payload";
	uint8_t ciphertext[sizeof(plaintext)];
	uint8_t recovered[sizeof(plaintext)];
	int rc;
	(void)p; (void)d;

	memset(key, 0x77, sizeof(key));
	memset(iv, 0x11, sizeof(iv));

	rc = __os_crypto_aes_gcm_encrypt(key, iv, NULL, 0,
	    plaintext, sizeof(plaintext), ciphertext, tag);
	munit_assert_int(rc, ==, XTC_OK);

	ciphertext[0] ^= 0x01;   /* flip one bit */

	memset(recovered, 0xee, sizeof(recovered));   /* poison canary */
	rc = __os_crypto_aes_gcm_decrypt(key, iv, NULL, 0,
	    ciphertext, sizeof(ciphertext), tag, recovered);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	munit_assert_memory_not_equal(sizeof(plaintext), recovered, plaintext);
#else
	(void)p; (void)d;
#endif
	return MUNIT_OK;
}

/* [AESGCM-3] tamper: flip one tag byte -> decrypt fails, same guarantee. */
static MunitResult
test_aesgcm_tamper_tag(const MunitParameter p[], void *d)
{
#if HAVE_CRYPTO
	uint8_t key[XTC_CRYPTO_AES_KEY_LEN];
	uint8_t iv[XTC_CRYPTO_AES_IV_LEN];
	uint8_t tag[XTC_CRYPTO_AES_TAG_LEN];
	const char plaintext[] = "another wal record";
	uint8_t ciphertext[sizeof(plaintext)];
	uint8_t recovered[sizeof(plaintext)];
	int rc;
	(void)p; (void)d;

	memset(key, 0x99, sizeof(key));
	memset(iv, 0x33, sizeof(iv));

	rc = __os_crypto_aes_gcm_encrypt(key, iv, NULL, 0,
	    plaintext, sizeof(plaintext), ciphertext, tag);
	munit_assert_int(rc, ==, XTC_OK);

	tag[0] ^= 0x01;

	memset(recovered, 0xee, sizeof(recovered));
	rc = __os_crypto_aes_gcm_decrypt(key, iv, NULL, 0,
	    ciphertext, sizeof(ciphertext), tag, recovered);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	munit_assert_memory_not_equal(sizeof(plaintext), recovered, plaintext);
#else
	(void)p; (void)d;
#endif
	return MUNIT_OK;
}

static MunitResult
test_aesgcm_inval(const MunitParameter p[], void *d)
{
	uint8_t key[XTC_CRYPTO_AES_KEY_LEN] = {0};
	uint8_t iv[XTC_CRYPTO_AES_IV_LEN] = {0};
	uint8_t tag[XTC_CRYPTO_AES_TAG_LEN] = {0};
	(void)p; (void)d;

	munit_assert_int(__os_crypto_aes_gcm_encrypt(NULL, iv, NULL, 0,
	    "x", 1, tag, tag), ==, XTC_E_INVAL);
	munit_assert_int(__os_crypto_aes_gcm_decrypt(key, iv, NULL, 0,
	    "x", 1, NULL, tag), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [CSPRNG-1] init/bytes/destroy basic sanity: two consecutive draws
 * produce different output (a real CSPRNG, not deterministic -- the
 * task explicitly does not want/expect a fixed-seed replay check
 * here). */
static MunitResult
test_csprng_basic(const MunitParameter p[], void *d)
{
	__os_csprng_t *rng = NULL;
	uint8_t a[32], b[32];
	int rc;
	(void)p; (void)d;

	rc = __os_csprng_init(&rng);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_not_null(rng);

	munit_assert_int(__os_csprng_bytes(rng, a, sizeof(a)), ==, XTC_OK);
	munit_assert_int(__os_csprng_bytes(rng, b, sizeof(b)), ==, XTC_OK);
	munit_assert_memory_not_equal(sizeof(a), a, b);

	__os_csprng_destroy(rng);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
	munit_assert_null(rng);
	(void)a; (void)b;
#endif
	return MUNIT_OK;
}

static MunitResult
test_csprng_inval(const MunitParameter p[], void *d)
{
	uint8_t buf[8];
	(void)p; (void)d;
	munit_assert_int(__os_csprng_init(NULL), ==, XTC_E_INVAL);
	munit_assert_int(__os_csprng_bytes(NULL, buf, sizeof(buf)),
	    ==, XTC_E_INVAL);
	__os_csprng_destroy(NULL);   /* must not crash */
	return MUNIT_OK;
}

/*
 * [CSPRNG-2] sim-determinism interaction check.  __os_csprng_bytes
 * calls into OpenSSL's RAND_bytes, which is a REAL entropy source and
 * would break DST replay if reached on a sim-controlled path -- but
 * this v1 CSPRNG is explicitly NOT wired into __xtc_sim_active() (see
 * the header/implementation comments: it is a real CSPRNG, not the
 * seeded xtc_sim_* PRNG tree used for scheduling decisions).  This
 * test just documents/proves that outside a sim run (the only mode
 * this primitive is meant for) there is no determinism-guard
 * interaction: __xtc_sim_active() is 0 here, so nothing in this file
 * calls __xtc_sim_nondeterminism, and none is expected.  A future
 * task that wants the CSPRNG reachable FROM inside sim-controlled
 * code must add that guard call at the point of use, mirroring
 * os_time.c's real-clock pattern; that is out of scope for v1, whose
 * only consumer is per-loop nonce/jitter generation outside the
 * deterministic scheduler's replay-sensitive path.
 */
static MunitResult
test_csprng_not_sim_reachable(const MunitParameter p[], void *d)
{
	__os_csprng_t *rng = NULL;
	uint8_t buf[16];
	(void)p; (void)d;

	munit_assert_int(__xtc_sim_active(), ==, 0);
	munit_assert_int(xtc_sim_nondeterminism_count(), ==, 0);

#if HAVE_CRYPTO
	munit_assert_int(__os_csprng_init(&rng), ==, XTC_OK);
	munit_assert_int(__os_csprng_bytes(rng, buf, sizeof(buf)), ==, XTC_OK);
	__os_csprng_destroy(rng);
#else
	(void)rng; (void)buf;
#endif
	/* Confirm the guard count is still zero: this primitive did not
	 * (and, while sim is inactive, cannot) trip the determinism
	 * guard. */
	munit_assert_int(xtc_sim_nondeterminism_count(), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/sha256_empty",       test_sha256_empty,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sha256_abc",         test_sha256_abc,               NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sha256_inval",       test_sha256_inval,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hmac_rfc4231_case1", test_hmac_rfc4231_case1,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hmac_inval",         test_hmac_inval,               NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aesgcm_roundtrip",   test_aesgcm_roundtrip,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aesgcm_tamper_ct",   test_aesgcm_tamper_ciphertext, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aesgcm_tamper_tag",  test_aesgcm_tamper_tag,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aesgcm_inval",       test_aesgcm_inval,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/csprng_basic",       test_csprng_basic,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/csprng_inval",       test_csprng_inval,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/csprng_not_sim",     test_csprng_not_sim_reachable, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/crypto", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

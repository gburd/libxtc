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

/* [SHA3-1] NIST FIPS 202 SHA3-256("") ==
 * a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a */
static MunitResult
test_sha3_256_empty(const MunitParameter p[], void *d)
{
	uint8_t out[XTC_CRYPTO_SHA3_256_LEN];
	uint8_t expect[XTC_CRYPTO_SHA3_256_LEN];
	int rc;
	(void)p; (void)d;

	hex2bin("a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
	    expect, sizeof(expect));
	rc = __os_crypto_sha3_256("", 0, out);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
#endif
	return MUNIT_OK;
}

/* [SHA3-2] NIST FIPS 202 SHA3-256("abc") ==
 * 3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532 */
static MunitResult
test_sha3_256_abc(const MunitParameter p[], void *d)
{
	uint8_t out[XTC_CRYPTO_SHA3_256_LEN];
	uint8_t expect[XTC_CRYPTO_SHA3_256_LEN];
	int rc;
	(void)p; (void)d;

	hex2bin("3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
	    expect, sizeof(expect));
	rc = __os_crypto_sha3_256("abc", 3, out);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
#endif
	return MUNIT_OK;
}

/* [SHA3-3] NIST FIPS 202 SHA3-512("abc") == b751850b1a57168a5693cd924b6b096e
 * 08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934057340b4c
 * f408d5a56592f8274eec53f0 */
static MunitResult
test_sha3_512_abc(const MunitParameter p[], void *d)
{
	uint8_t out[XTC_CRYPTO_SHA3_512_LEN];
	uint8_t expect[XTC_CRYPTO_SHA3_512_LEN];
	int rc;
	(void)p; (void)d;

	hex2bin("b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
	    "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0",
	    expect, sizeof(expect));
	rc = __os_crypto_sha3_512("abc", 3, out);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
#endif
	return MUNIT_OK;
}

static MunitResult
test_sha3_inval(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__os_crypto_sha3_256("abc", 3, NULL), ==, XTC_E_INVAL);
	munit_assert_int(__os_crypto_sha3_512("abc", 3, NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* BLAKE3 is portable (no OpenSSL), so it works on EVERY build: these
 * assert XTC_OK and the exact digest regardless of HAVE_CRYPTO.  The
 * three vectors are from the official BLAKE3 test_vectors.json, using
 * the spec's repeating input pattern 0,1,...,250,0,1,... */
static void
blake3_fill(uint8_t *buf, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		buf[i] = (uint8_t)(i % 251);
}

/* [BLAKE3-1] len 0 ==
 * af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262 */
static MunitResult
test_blake3_empty(const MunitParameter p[], void *d)
{
	uint8_t out[XTC_CRYPTO_BLAKE3_LEN];
	uint8_t expect[XTC_CRYPTO_BLAKE3_LEN];
	(void)p; (void)d;

	hex2bin("af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
	    expect, sizeof(expect));
	munit_assert_int(__os_crypto_blake3("", 0, out), ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
	return MUNIT_OK;
}

/* [BLAKE3-2] len 1024 (single full chunk) ==
 * 42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7 */
static MunitResult
test_blake3_1024(const MunitParameter p[], void *d)
{
	uint8_t in[1024];
	uint8_t out[XTC_CRYPTO_BLAKE3_LEN];
	uint8_t expect[XTC_CRYPTO_BLAKE3_LEN];
	(void)p; (void)d;

	blake3_fill(in, sizeof(in));
	hex2bin("42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7",
	    expect, sizeof(expect));
	munit_assert_int(__os_crypto_blake3(in, sizeof(in), out), ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
	return MUNIT_OK;
}

/* [BLAKE3-3] len 3072 (multi-chunk tree) ==
 * b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2 */
static MunitResult
test_blake3_3072(const MunitParameter p[], void *d)
{
	uint8_t in[3072];
	uint8_t out[XTC_CRYPTO_BLAKE3_LEN];
	uint8_t expect[XTC_CRYPTO_BLAKE3_LEN];
	(void)p; (void)d;

	blake3_fill(in, sizeof(in));
	hex2bin("b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2",
	    expect, sizeof(expect));
	munit_assert_int(__os_crypto_blake3(in, sizeof(in), out), ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
	return MUNIT_OK;
}

/* [BLAKE3-4] len 5000 (ragged multi-chunk tree, last chunk partial) ==
 * ee78d92070de3df1c57c37002abf0a6b1a6589acdeef4d8ffac7cf3d9e8f2836
 * (cross-checked with b3sum). */
static MunitResult
test_blake3_5000(const MunitParameter p[], void *d)
{
	uint8_t in[5000];
	uint8_t out[XTC_CRYPTO_BLAKE3_LEN];
	uint8_t expect[XTC_CRYPTO_BLAKE3_LEN];
	(void)p; (void)d;

	blake3_fill(in, sizeof(in));
	hex2bin("ee78d92070de3df1c57c37002abf0a6b1a6589acdeef4d8ffac7cf3d9e8f2836",
	    expect, sizeof(expect));
	munit_assert_int(__os_crypto_blake3(in, sizeof(in), out), ==, XTC_OK);
	munit_assert_memory_equal(sizeof(expect), out, expect);
	return MUNIT_OK;
}

static MunitResult
test_blake3_inval(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__os_crypto_blake3("abc", 3, NULL), ==, XTC_E_INVAL);
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

/* [CHACHA-1] RFC 8439 section 2.8.2 known-answer test. */
static MunitResult
test_chacha_rfc8439(const MunitParameter p[], void *d)
{
	uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN];
	uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN];
	uint8_t aad[12];
	const char pt[] =
	    "Ladies and Gentlemen of the class of '99: If I could offer you "
	    "only one tip for the future, sunscreen would be it.";
	size_t ptlen = sizeof(pt) - 1;   /* exclude the NUL terminator */
	uint8_t ct[128], tag[XTC_CRYPTO_CHACHA_TAG_LEN];
	uint8_t expect_ct[128], expect_tag[XTC_CRYPTO_CHACHA_TAG_LEN];
	size_t i;
	int rc;
	(void)p; (void)d;

	munit_assert_size(ptlen, ==, 114);
	for (i = 0; i < sizeof(key); i++)
		key[i] = (uint8_t)(0x80 + i);
	{
		static const uint8_t n[12] =
		    { 0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43,
		      0x44, 0x45, 0x46, 0x47 };
		static const uint8_t a[12] =
		    { 0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3,
		      0xc4, 0xc5, 0xc6, 0xc7 };
		memcpy(nonce, n, sizeof(nonce));
		memcpy(aad, a, sizeof(aad));
	}
	hex2bin(
	    "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
	    "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
	    "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
	    "3ff4def08e4b7a9de576d26586cec64b6116", expect_ct, ptlen);
	hex2bin("1ae10b594f09e26a7e902ecbd0600691", expect_tag,
	    sizeof(expect_tag));

	rc = __os_crypto_chacha20_poly1305_encrypt(key, nonce, aad, sizeof(aad),
	    pt, ptlen, ct, tag);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(ptlen, ct, expect_ct);
	munit_assert_memory_equal(sizeof(tag), tag, expect_tag);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
	(void)expect_ct; (void)expect_tag;
#endif
	return MUNIT_OK;
}

/* [CHACHA-2] round trip. */
static MunitResult
test_chacha_roundtrip(const MunitParameter p[], void *d)
{
	uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN];
	uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN];
	uint8_t tag[XTC_CRYPTO_CHACHA_TAG_LEN];
	const char plaintext[] = "the quick brown fox jumps over the lazy dog";
	uint8_t ciphertext[sizeof(plaintext)];
	uint8_t recovered[sizeof(plaintext)];
	const char aad[] = "wal-record-42";
	int rc;
	(void)p; (void)d;

	memset(key, 0x42, sizeof(key));
	memset(nonce, 0x24, sizeof(nonce));

	rc = __os_crypto_chacha20_poly1305_encrypt(key, nonce, aad, sizeof(aad),
	    plaintext, sizeof(plaintext), ciphertext, tag);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_not_equal(sizeof(plaintext), plaintext, ciphertext);

	memset(recovered, 0, sizeof(recovered));
	rc = __os_crypto_chacha20_poly1305_decrypt(key, nonce, aad, sizeof(aad),
	    ciphertext, sizeof(ciphertext), tag, recovered);
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(plaintext), recovered, plaintext);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
	(void)recovered;
#endif
	return MUNIT_OK;
}

/* [CHACHA-3] tamper: flip one ciphertext byte -> decrypt rejects. */
static MunitResult
test_chacha_tamper(const MunitParameter p[], void *d)
{
#if HAVE_CRYPTO
	uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN];
	uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN];
	uint8_t tag[XTC_CRYPTO_CHACHA_TAG_LEN];
	const char plaintext[] = "top secret wal payload";
	uint8_t ciphertext[sizeof(plaintext)];
	uint8_t recovered[sizeof(plaintext)];
	int rc;
	(void)p; (void)d;

	memset(key, 0x77, sizeof(key));
	memset(nonce, 0x11, sizeof(nonce));

	rc = __os_crypto_chacha20_poly1305_encrypt(key, nonce, NULL, 0,
	    plaintext, sizeof(plaintext), ciphertext, tag);
	munit_assert_int(rc, ==, XTC_OK);

	ciphertext[0] ^= 0x01;
	memset(recovered, 0xee, sizeof(recovered));
	rc = __os_crypto_chacha20_poly1305_decrypt(key, nonce, NULL, 0,
	    ciphertext, sizeof(ciphertext), tag, recovered);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	munit_assert_memory_not_equal(sizeof(plaintext), recovered, plaintext);

	/* Also flip a tag byte on a fresh ciphertext. */
	rc = __os_crypto_chacha20_poly1305_encrypt(key, nonce, NULL, 0,
	    plaintext, sizeof(plaintext), ciphertext, tag);
	munit_assert_int(rc, ==, XTC_OK);
	tag[0] ^= 0x01;
	memset(recovered, 0xee, sizeof(recovered));
	rc = __os_crypto_chacha20_poly1305_decrypt(key, nonce, NULL, 0,
	    ciphertext, sizeof(ciphertext), tag, recovered);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	munit_assert_memory_not_equal(sizeof(plaintext), recovered, plaintext);
#else
	(void)p; (void)d;
#endif
	return MUNIT_OK;
}

static MunitResult
test_chacha_inval(const MunitParameter p[], void *d)
{
	uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN] = {0};
	uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN] = {0};
	uint8_t tag[XTC_CRYPTO_CHACHA_TAG_LEN] = {0};
	(void)p; (void)d;

	munit_assert_int(__os_crypto_chacha20_poly1305_encrypt(NULL, nonce,
	    NULL, 0, "x", 1, tag, tag), ==, XTC_E_INVAL);
	munit_assert_int(__os_crypto_chacha20_poly1305_decrypt(key, nonce,
	    NULL, 0, "x", 1, NULL, tag), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [HKDF-1] RFC 5869 Test Case 1 (HKDF-SHA256).  Both the intermediate
 * PRK and the final 42-byte OKM are checked. */
static MunitResult
test_hkdf_rfc5869_case1(const MunitParameter p[], void *d)
{
	uint8_t ikm[22], salt[13], info[10];
	uint8_t prk[XTC_CRYPTO_SHA256_LEN];
	uint8_t okm[42], okm2[42];
	uint8_t expect_prk[XTC_CRYPTO_SHA256_LEN];
	uint8_t expect_okm[42];
	size_t i;
	int rc;
	(void)p; (void)d;

	memset(ikm, 0x0b, sizeof(ikm));
	for (i = 0; i < sizeof(salt); i++)
		salt[i] = (uint8_t)i;
	for (i = 0; i < sizeof(info); i++)
		info[i] = (uint8_t)(0xf0 + i);
	hex2bin("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
	    expect_prk, sizeof(expect_prk));
	hex2bin("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
	    "34007208d5b887185865", expect_okm, sizeof(expect_okm));

	rc = __os_crypto_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
#if HAVE_CRYPTO
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(prk), prk, expect_prk);

	rc = __os_crypto_hkdf_expand(expect_prk, info, sizeof(info),
	    okm, sizeof(okm));
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(okm), okm, expect_okm);

	/* Combined extract+expand must equal the two-step result. */
	rc = __os_crypto_hkdf(ikm, sizeof(ikm), salt, sizeof(salt),
	    info, sizeof(info), okm2, sizeof(okm2));
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_memory_equal(sizeof(okm2), okm2, expect_okm);
#else
	munit_assert_int(rc, ==, XTC_E_NOSYS);
	(void)expect_prk; (void)expect_okm; (void)okm; (void)okm2; (void)info;
#endif
	return MUNIT_OK;
}

static MunitResult
test_hkdf_inval(const MunitParameter p[], void *d)
{
	uint8_t prk[XTC_CRYPTO_SHA256_LEN] = {0};
	uint8_t out[32];
	(void)p; (void)d;

	munit_assert_int(__os_crypto_hkdf_extract(NULL, 0, "x", 1, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(__os_crypto_hkdf_expand(prk, NULL, 0, NULL, 32),
	    ==, XTC_E_INVAL);
	/* outlen 0 and outlen > 255*32 are invalid per RFC 5869. */
	munit_assert_int(__os_crypto_hkdf_expand(prk, NULL, 0, out, 0),
	    ==, XTC_E_INVAL);
	munit_assert_int(__os_crypto_hkdf_expand(prk, NULL, 0, out,
	    255 * 32 + 1), ==, XTC_E_INVAL);
	munit_assert_int(__os_crypto_hkdf(NULL, 0, NULL, 0, NULL, 0, NULL, 32),
	    ==, XTC_E_INVAL);
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
	{ "/sha3_256_empty",     test_sha3_256_empty,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sha3_256_abc",       test_sha3_256_abc,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sha3_512_abc",       test_sha3_512_abc,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sha3_inval",         test_sha3_inval,               NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/blake3_empty",       test_blake3_empty,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/blake3_1024",        test_blake3_1024,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/blake3_3072",        test_blake3_3072,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/blake3_5000",        test_blake3_5000,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/blake3_inval",       test_blake3_inval,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aesgcm_roundtrip",   test_aesgcm_roundtrip,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aesgcm_tamper_ct",   test_aesgcm_tamper_ciphertext, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aesgcm_tamper_tag",  test_aesgcm_tamper_tag,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aesgcm_inval",       test_aesgcm_inval,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/chacha_rfc8439",     test_chacha_rfc8439,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/chacha_roundtrip",   test_chacha_roundtrip,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/chacha_tamper",      test_chacha_tamper,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/chacha_inval",       test_chacha_inval,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hkdf_rfc5869_case1", test_hkdf_rfc5869_case1,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hkdf_inval",         test_hkdf_inval,               NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/csprng_basic",       test_csprng_basic,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/csprng_inval",       test_csprng_inval,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/csprng_not_sim",     test_csprng_not_sim_reachable, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/crypto", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

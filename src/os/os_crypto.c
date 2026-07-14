/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/os/os_crypto.c
 *	L0 cryptography building blocks (PLAN.md 19.2), v1 scope.  See
 *	src/inc/os_crypto.h for the full contract and the scoping
 *	rationale; short version: this ships ONLY what the two real v1
 *	use cases need (SHA-256/HMAC-SHA256 for WAL/checksum integrity,
 *	AES-256-GCM for authenticated encryption, and a per-loop CSPRNG),
 *	backed ONLY by OpenSSL.
 *
 *	This file is ALWAYS compiled into libxtc.a, exactly like
 *	src/os/os_rand.c.  It is guarded internally by
 *	XTC_TLS_BACKEND_OPENSSL -- the SAME macro src/io/tls_openssl.c
 *	uses to detect "OpenSSL is the selected TLS backend" -- so it
 *	needs no new configure flag and no conditional Makefile/meson
 *	wiring: adding this one file to the existing source list is the
 *	entire build change required.  When OpenSSL is not the selected
 *	backend, the #else branch below provides the identical public
 *	symbols as NOSYS stubs, following the exact convention
 *	src/io/tls_none.c uses for xtc_tls_*: every function stays
 *	linkable, argument validation still happens, and the runtime
 *	result is XTC_E_NOSYS rather than a link error.
 *
 *	AES-GCM nonce contract: __os_crypto_aes_gcm_encrypt/_decrypt use
 *	the standard 96-bit (12-byte) GCM nonce, which is also OpenSSL's
 *	default IV length for EVP_aes_256_gcm() -- no EVP_CTRL_AEAD_SET_
 *	IVLEN call is needed.  Reusing an (key, iv) pair for two
 *	different plaintexts is a catastrophic confidentiality break;
 *	the CSPRNG below is the intended nonce source.
 *
 *	Tag verification: EVP_DecryptFinal_ex is where OpenSSL actually
 *	checks the GCM tag; bytes fed to EVP_DecryptUpdate before that
 *	point are provisional.  On a Final failure this code zeroes the
 *	caller's plaintext buffer (defense in depth: no unauthenticated
 *	byte survives in memory the caller might read) AND returns
 *	XTC_E_INVAL, a distinct code from XTC_E_INTERNAL (an OpenSSL
 *	invariant violation) and from XTC_OK -- the caller can never
 *	mistake a tag-verification failure for success.
 *
 *	CSPRNG design choice (documented per the task's "your choice,
 *	document it"): rather than hand-roll a second DRBG (either via
 *	the EVP_RAND CTR-DRBG plumbing or a custom AES-GCM counter-mode
 *	construction), __os_csprng_t delegates generation directly to
 *	OpenSSL's own default RAND_bytes.  OpenSSL 3.x's default
 *	generator IS already a properly-seeded, continuously-reseeded,
 *	thread-safe CSPRNG (seeded from the OS entropy source via
 *	RAND_poll on first use); duplicating that machinery here would
 *	add real complexity (parameter plumbing, reseed policy, provider
 *	fetch/free lifetime) for no security benefit in v1's stated use
 *	case (a per-loop CSPRNG for nonces/keys/jitter, not FIPS-mode
 *	determinism -- see the man page and the task's explicit note that
 *	CSPRNG determinism is NOT expected here).  __os_csprng_init mixes
 *	__os_rand_u64 draws into OpenSSL's pool via RAND_add as
 *	additional entropy, per "reuse the existing primitive as the
 *	seed source, do not invent a new one"; it does not replace
 *	OpenSSL's own seeding, only supplements it.  The opaque handle
 *	exists so callers have a per-loop object to own/destroy and so a
 *	future task can swap in a real per-handle DRBG without an ABI
 *	change.
 */

#include "xtc_int.h"
#include "os_crypto.h"
#include "os_sharp.h"   /* __os_rand_u64: seed source for the CSPRNG */

#if defined(XTC_TLS_BACKEND_OPENSSL)

#include <limits.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

struct __os_csprng {
	int unused;   /* opaque; state lives in OpenSSL's own default RNG */
};

/*
 * PUBLIC: int __os_crypto_sha256 __P((const void *, size_t, uint8_t *));
 */
int
__os_crypto_sha256(const void *data, size_t len,
    uint8_t out[XTC_CRYPTO_SHA256_LEN])
{
	unsigned int outlen;

	if ((data == NULL && len != 0) || out == NULL)
		return XTC_E_INVAL;

	if (EVP_Digest(data, len, out, &outlen, EVP_sha256(), NULL) != 1)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_crypto_hmac_sha256 __P((const void *, size_t, const void *, size_t, uint8_t *));
 */
int
__os_crypto_hmac_sha256(const void *key, size_t keylen,
    const void *data, size_t len, uint8_t out[XTC_CRYPTO_SHA256_LEN])
{
	unsigned int outlen;

	if ((key == NULL && keylen != 0) || (data == NULL && len != 0) ||
	    out == NULL)
		return XTC_E_INVAL;
	if (keylen > (size_t)INT_MAX)
		return XTC_E_RANGE;

	if (HMAC(EVP_sha256(), key, (int)keylen, data, len, out, &outlen) ==
	    NULL)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_crypto_aes_gcm_encrypt __P((const uint8_t *, const uint8_t *, const void *, size_t, const void *, size_t, void *, uint8_t *));
 */
int
__os_crypto_aes_gcm_encrypt(
    const uint8_t key[XTC_CRYPTO_AES_KEY_LEN],
    const uint8_t iv[XTC_CRYPTO_AES_IV_LEN],
    const void *aad, size_t aadlen,
    const void *plaintext, size_t len,
    void *ciphertext_out, uint8_t tag_out[XTC_CRYPTO_AES_TAG_LEN])
{
	EVP_CIPHER_CTX *ctx;
	unsigned char final_scratch[EVP_MAX_BLOCK_LENGTH];
	unsigned char *cout = ciphertext_out;
	int outl, tmplen;

	if (key == NULL || iv == NULL || tag_out == NULL)
		return XTC_E_INVAL;
	if ((plaintext == NULL || ciphertext_out == NULL) && len != 0)
		return XTC_E_INVAL;
	if (aad == NULL && aadlen != 0)
		return XTC_E_INVAL;
	if (len > (size_t)INT_MAX || aadlen > (size_t)INT_MAX)
		return XTC_E_RANGE;

	if ((ctx = EVP_CIPHER_CTX_new()) == NULL)
		return XTC_E_INTERNAL;

	if (EVP_EncryptInit_ex2(ctx, EVP_aes_256_gcm(), key, iv, NULL) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	if (aadlen > 0 &&
	    EVP_EncryptUpdate(ctx, NULL, &outl, aad, (int)aadlen) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	outl = 0;
	if (len > 0 && EVP_EncryptUpdate(ctx, cout, &outl,
	    plaintext, (int)len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	/* GCM is a stream mode: EncryptFinal always emits zero bytes here.
	 * When len==0 (and cout may legitimately be NULL), point at a
	 * local scratch buffer instead of doing NULL+outl pointer
	 * arithmetic, which is undefined behavior in C even at offset 0. */
	if (EVP_EncryptFinal_ex(ctx,
	    (cout != NULL) ? cout + outl : final_scratch, &tmplen) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
	    XTC_CRYPTO_AES_TAG_LEN, tag_out) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	EVP_CIPHER_CTX_free(ctx);
	return XTC_OK;
}

/*
 * PUBLIC: int __os_crypto_aes_gcm_decrypt __P((const uint8_t *, const uint8_t *, const void *, size_t, const void *, size_t, const uint8_t *, void *));
 */
int
__os_crypto_aes_gcm_decrypt(
    const uint8_t key[XTC_CRYPTO_AES_KEY_LEN],
    const uint8_t iv[XTC_CRYPTO_AES_IV_LEN],
    const void *aad, size_t aadlen,
    const void *ciphertext, size_t len,
    const uint8_t tag[XTC_CRYPTO_AES_TAG_LEN],
    void *plaintext_out)
{
	EVP_CIPHER_CTX *ctx;
	unsigned char final_scratch[EVP_MAX_BLOCK_LENGTH];
	unsigned char *pout = plaintext_out;
	int outl, tmplen, rc;

	if (key == NULL || iv == NULL || tag == NULL)
		return XTC_E_INVAL;
	if ((ciphertext == NULL || plaintext_out == NULL) && len != 0)
		return XTC_E_INVAL;
	if (aad == NULL && aadlen != 0)
		return XTC_E_INVAL;
	if (len > (size_t)INT_MAX || aadlen > (size_t)INT_MAX)
		return XTC_E_RANGE;

	if ((ctx = EVP_CIPHER_CTX_new()) == NULL)
		return XTC_E_INTERNAL;

	if (EVP_DecryptInit_ex2(ctx, EVP_aes_256_gcm(), key, iv, NULL) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	if (aadlen > 0 &&
	    EVP_DecryptUpdate(ctx, NULL, &outl, aad, (int)aadlen) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	outl = 0;
	if (len > 0 && EVP_DecryptUpdate(ctx, pout, &outl,
	    ciphertext, (int)len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		if (len > 0)
			memset(pout, 0, len);
		return XTC_E_INTERNAL;
	}

	/* EVP_CTRL_AEAD_SET_TAG's third argument is void * in the OpenSSL
	 * prototype even though it only reads the tag; the cast away from
	 * const is required and safe (no -Wcast-qual in this project's
	 * CFLAGS). */
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
	    XTC_CRYPTO_AES_TAG_LEN, (void *)tag) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		if (len > 0)
			memset(pout, 0, len);
		return XTC_E_INTERNAL;
	}

	/* The tag is actually checked HERE.  A mismatch (rc != 1) means
	 * the bytes EVP_DecryptUpdate already wrote are unauthenticated;
	 * wipe them before returning so no caller can accidentally use
	 * them, and return a code distinct from both XTC_OK and
	 * XTC_E_INTERNAL.  As in the encrypt path, avoid NULL+outl pointer
	 * arithmetic when pout is NULL (len==0). */
	rc = EVP_DecryptFinal_ex(ctx,
	    (pout != NULL) ? pout + outl : final_scratch, &tmplen);
	EVP_CIPHER_CTX_free(ctx);
	if (rc != 1) {
		if (len > 0)
			memset(pout, 0, len);
		return XTC_E_INVAL;
	}
	return XTC_OK;
}

/*
 * PUBLIC: int __os_csprng_init __P((__os_csprng_t **));
 */
int
__os_csprng_init(__os_csprng_t **out)
{
	__os_csprng_t *r;
	uint64_t       seed[4];
	int            rc, i;

	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	if ((rc = __os_malloc(sizeof(*r), (void **)&r)) != XTC_OK)
		return rc;
	r->unused = 0;

	/* Additional per-handle entropy on top of OpenSSL's own seeding
	 * (see the file header comment); NOT a replacement entropy
	 * source. */
	for (i = 0; i < 4; i++)
		seed[i] = __os_rand_u64();
	RAND_add(seed, (int)sizeof(seed), (double)sizeof(seed));

	*out = r;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_csprng_bytes __P((__os_csprng_t *, void *, size_t));
 */
int
__os_csprng_bytes(__os_csprng_t *rng, void *buf, size_t len)
{
	unsigned char *p = buf;

	if (rng == NULL || (buf == NULL && len != 0))
		return XTC_E_INVAL;

	while (len > 0) {
		int chunk = (len > (size_t)INT_MAX) ? INT_MAX : (int)len;
		if (RAND_bytes(p, chunk) != 1)
			return XTC_E_INTERNAL;
		p   += chunk;
		len -= (size_t)chunk;
	}
	return XTC_OK;
}

/*
 * PUBLIC: void __os_csprng_destroy __P((__os_csprng_t *));
 */
void
__os_csprng_destroy(__os_csprng_t *rng)
{
	__os_free(rng);
}

#else /* !XTC_TLS_BACKEND_OPENSSL */

/*
 * NOSYS stubs.  Same argument validation as the real backend so a
 * caller gets XTC_E_INVAL on a bad argument even without OpenSSL;
 * every otherwise-valid call returns XTC_E_NOSYS.  Mirrors
 * src/io/tls_none.c exactly.
 */

int
__os_crypto_sha256(const void *data, size_t len,
    uint8_t out[XTC_CRYPTO_SHA256_LEN])
{
	if ((data == NULL && len != 0) || out == NULL)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_crypto_hmac_sha256(const void *key, size_t keylen,
    const void *data, size_t len, uint8_t out[XTC_CRYPTO_SHA256_LEN])
{
	if ((key == NULL && keylen != 0) || (data == NULL && len != 0) ||
	    out == NULL)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_crypto_aes_gcm_encrypt(
    const uint8_t key[XTC_CRYPTO_AES_KEY_LEN],
    const uint8_t iv[XTC_CRYPTO_AES_IV_LEN],
    const void *aad, size_t aadlen,
    const void *plaintext, size_t len,
    void *ciphertext_out, uint8_t tag_out[XTC_CRYPTO_AES_TAG_LEN])
{
	if (key == NULL || iv == NULL || tag_out == NULL)
		return XTC_E_INVAL;
	if ((plaintext == NULL || ciphertext_out == NULL) && len != 0)
		return XTC_E_INVAL;
	if (aad == NULL && aadlen != 0)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_crypto_aes_gcm_decrypt(
    const uint8_t key[XTC_CRYPTO_AES_KEY_LEN],
    const uint8_t iv[XTC_CRYPTO_AES_IV_LEN],
    const void *aad, size_t aadlen,
    const void *ciphertext, size_t len,
    const uint8_t tag[XTC_CRYPTO_AES_TAG_LEN],
    void *plaintext_out)
{
	if (key == NULL || iv == NULL || tag == NULL)
		return XTC_E_INVAL;
	if ((ciphertext == NULL || plaintext_out == NULL) && len != 0)
		return XTC_E_INVAL;
	if (aad == NULL && aadlen != 0)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_csprng_init(__os_csprng_t **out)
{
	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;
	return XTC_E_NOSYS;
}

int
__os_csprng_bytes(__os_csprng_t *rng, void *buf, size_t len)
{
	if (rng == NULL || (buf == NULL && len != 0))
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

void
__os_csprng_destroy(__os_csprng_t *rng)
{
	(void)rng;
}

#endif /* XTC_TLS_BACKEND_OPENSSL */

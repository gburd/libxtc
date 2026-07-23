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
 *	Extended past the original v1 scope for future TDE/WAL-encryption
 *	work: ChaCha20-Poly1305 (a second AEAD), SHA-3 256/512, BLAKE3,
 *	and HKDF-SHA256.  ChaCha20-Poly1305, SHA-3, and HKDF are
 *	OpenSSL-backed (NOSYS otherwise); BLAKE3 is a portable
 *	self-contained reimplementation (below, ahead of the backend
 *	guard) and is available on every build.
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

#include <string.h>

/*
 * ===================================================================
 * BLAKE3 (portable reimplementation)
 * ===================================================================
 *
 * OpenSSL does not provide BLAKE3, so this is a small self-contained
 * PORTABLE reimplementation of the BLAKE3 hash, following the
 * public-domain BLAKE3 specification and reference implementation
 * (the reference is dual-licensed CC0-1.0 / Apache-2.0; this is an
 * independent from-spec rewrite in BSD KNF, not a copy).  It is
 * one-shot only (no keyed/derive-key modes, no incremental update)
 * and emits the default 32-byte digest -- exactly what
 * __os_crypto_blake3 needs.  Because it depends on nothing but the C
 * standard library it is compiled into EVERY build regardless of the
 * selected TLS backend, and therefore never returns XTC_E_NOSYS.
 *
 * Structure mirrors the spec: a 7-round compression function over a
 * 16-word state; inputs are split into 1024-byte chunks, each chunk
 * hashed block-by-block (64-byte blocks) with CHUNK_START/CHUNK_END
 * domain flags; chunk chaining values are combined by a binary tree
 * of PARENT nodes; the final (root) node is compressed with the ROOT
 * flag and squeezed for the output.
 */

#define B3_BLOCK_LEN  64
#define B3_CHUNK_LEN  1024

#define B3_CHUNK_START (1u << 0)
#define B3_CHUNK_END   (1u << 1)
#define B3_PARENT      (1u << 2)
#define B3_ROOT        (1u << 3)

static const uint32_t b3_iv[8] = {
	0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
	0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

/* Per-round message-word permutation schedule. */
static const uint8_t b3_perm[7][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8 },
	{ 3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1 },
	{ 10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6 },
	{ 12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4 },
	{ 9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7 },
	{ 11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13 },
};

static uint32_t
b3_rotr(uint32_t w, int c)
{
	return (w >> c) | (w << (32 - c));
}

static uint32_t
b3_load32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
b3_store32(uint8_t *p, uint32_t w)
{
	p[0] = (uint8_t)w;
	p[1] = (uint8_t)(w >> 8);
	p[2] = (uint8_t)(w >> 16);
	p[3] = (uint8_t)(w >> 24);
}

static void
b3_g(uint32_t *s, int a, int b, int c, int d, uint32_t x, uint32_t y)
{
	s[a] = s[a] + s[b] + x;
	s[d] = b3_rotr(s[d] ^ s[a], 16);
	s[c] = s[c] + s[d];
	s[b] = b3_rotr(s[b] ^ s[c], 12);
	s[a] = s[a] + s[b] + y;
	s[d] = b3_rotr(s[d] ^ s[a], 8);
	s[c] = s[c] + s[d];
	s[b] = b3_rotr(s[b] ^ s[c], 7);
}

static void
b3_round(uint32_t st[16], const uint32_t m[16], int r)
{
	const uint8_t *s = b3_perm[r];

	b3_g(st, 0, 4, 8, 12, m[s[0]], m[s[1]]);
	b3_g(st, 1, 5, 9, 13, m[s[2]], m[s[3]]);
	b3_g(st, 2, 6, 10, 14, m[s[4]], m[s[5]]);
	b3_g(st, 3, 7, 11, 15, m[s[6]], m[s[7]]);
	b3_g(st, 0, 5, 10, 15, m[s[8]], m[s[9]]);
	b3_g(st, 1, 6, 11, 12, m[s[10]], m[s[11]]);
	b3_g(st, 2, 7, 8, 13, m[s[12]], m[s[13]]);
	b3_g(st, 3, 4, 9, 14, m[s[14]], m[s[15]]);
}

static void
b3_compress(const uint32_t cv[8], const uint8_t block[B3_BLOCK_LEN],
    uint8_t block_len, uint64_t counter, uint32_t flags, uint32_t out[16])
{
	uint32_t st[16];
	uint32_t m[16];
	int      i;

	for (i = 0; i < 16; i++)
		m[i] = b3_load32(block + 4 * i);

	for (i = 0; i < 8; i++)
		st[i] = cv[i];
	st[8] = b3_iv[0]; st[9] = b3_iv[1]; st[10] = b3_iv[2];
	st[11] = b3_iv[3];
	st[12] = (uint32_t)counter;
	st[13] = (uint32_t)(counter >> 32);
	st[14] = (uint32_t)block_len;
	st[15] = flags;

	for (i = 0; i < 7; i++)
		b3_round(st, m, i);

	for (i = 0; i < 8; i++) {
		out[i] = st[i] ^ st[i + 8];
		out[i + 8] = st[i + 8] ^ cv[i];
	}
}

/*
 * An "output node": the inputs to a final compression, deferred so the
 * ROOT flag and arbitrary-length squeeze can be applied to the
 * eventual top-of-tree node.
 */
struct b3_output {
	uint32_t cv[8];
	uint8_t  block[B3_BLOCK_LEN];
	uint8_t  block_len;
	uint64_t counter;
	uint32_t flags;
};

static void
b3_output_root(const struct b3_output *o, uint8_t *out, size_t outlen)
{
	uint64_t counter = 0;

	while (outlen > 0) {
		uint32_t words[16];
		uint8_t  wide[B3_BLOCK_LEN];
		size_t   take;
		int      i;

		b3_compress(o->cv, o->block, o->block_len, counter,
		    o->flags | B3_ROOT, words);
		for (i = 0; i < 16; i++)
			b3_store32(wide + 4 * i, words[i]);

		take = (outlen < B3_BLOCK_LEN) ? outlen : B3_BLOCK_LEN;
		memcpy(out, wide, take);
		out += take;
		outlen -= take;
		counter++;
	}
}

/* Compress one chunk (<= 1024 bytes) into its (deferred) output node. */
static struct b3_output
b3_chunk_output(const uint8_t *chunk, size_t len, uint64_t chunk_counter)
{
	struct b3_output o;
	uint32_t cv[8];
	size_t   pos = 0;
	int      i;

	for (i = 0; i < 8; i++)
		cv[i] = b3_iv[i];

	while (len - pos > B3_BLOCK_LEN) {
		uint32_t words[16];
		uint32_t f = (pos == 0) ? B3_CHUNK_START : 0;
		b3_compress(cv, chunk + pos, B3_BLOCK_LEN, chunk_counter,
		    f, words);
		for (i = 0; i < 8; i++)
			cv[i] = words[i];
		pos += B3_BLOCK_LEN;
	}

	for (i = 0; i < 8; i++)
		o.cv[i] = cv[i];
	memset(o.block, 0, sizeof(o.block));
	o.block_len = (uint8_t)(len - pos);
	memcpy(o.block, chunk + pos, o.block_len);
	o.counter = chunk_counter;
	o.flags = B3_CHUNK_END | ((pos == 0) ? B3_CHUNK_START : 0);
	return o;
}

static void
b3_output_cv(const struct b3_output *o, uint32_t out_cv[8])
{
	uint32_t words[16];
	int      i;

	b3_compress(o->cv, o->block, o->block_len, o->counter, o->flags,
	    words);
	for (i = 0; i < 8; i++)
		out_cv[i] = words[i];
}

static struct b3_output
b3_parent_output(const uint32_t left[8], const uint32_t right[8])
{
	struct b3_output o;
	int i;

	for (i = 0; i < 8; i++)
		o.cv[i] = b3_iv[i];
	for (i = 0; i < 8; i++)
		b3_store32(o.block + 4 * i, left[i]);
	for (i = 0; i < 8; i++)
		b3_store32(o.block + 32 + 4 * i, right[i]);
	o.block_len = B3_BLOCK_LEN;
	o.counter = 0;
	o.flags = B3_PARENT;
	return o;
}

static void
b3_parent_cv(const uint32_t left[8], const uint32_t right[8], uint32_t out[8])
{
	struct b3_output o = b3_parent_output(left, right);
	b3_output_cv(&o, out);
}

static void
b3_hash(const uint8_t *data, size_t len, uint8_t *out, size_t outlen)
{
	/* 54 levels covers 2^54 chunks (~2^64 bytes), the BLAKE3 max. */
	uint32_t cv_stack[54][8];
	uint64_t cv_stack_len = 0;
	uint64_t chunk_counter = 0;
	size_t   pos = 0;
	struct b3_output final;

	if (len <= B3_CHUNK_LEN) {
		final = b3_chunk_output(data, len, 0);
		b3_output_root(&final, out, outlen);
		return;
	}

	/* Absorb every chunk EXCEPT the last, pushing each chunk's CV and
	 * merging complete equal-height subtrees per the trailing-zero
	 * rule.  The last chunk stays an output node (not CV'd) so
	 * finalize can apply ROOT to whichever node ends up on top. */
	while (len - pos > B3_CHUNK_LEN) {
		struct b3_output co;
		uint32_t new_cv[8];
		uint64_t total;

		co = b3_chunk_output(data + pos, B3_CHUNK_LEN, chunk_counter);
		b3_output_cv(&co, new_cv);
		pos += B3_CHUNK_LEN;
		chunk_counter++;

		total = chunk_counter;
		while ((total & 1) == 0) {
			uint32_t merged[8];
			cv_stack_len--;
			b3_parent_cv(cv_stack[cv_stack_len], new_cv, merged);
			memcpy(new_cv, merged, sizeof(merged));
			total >>= 1;
		}
		memcpy(cv_stack[cv_stack_len], new_cv, sizeof(new_cv));
		cv_stack_len++;
	}

	/* Finalize: the last chunk is the current output node.  Fold the
	 * stack into it right-to-left; each popped entry is the left
	 * child, the running node's CV the right child.  The topmost
	 * parent stays an output node so ROOT is applied there. */
	final = b3_chunk_output(data + pos, len - pos, chunk_counter);
	while (cv_stack_len > 0) {
		uint32_t rcv[8];
		cv_stack_len--;
		b3_output_cv(&final, rcv);
		final = b3_parent_output(cv_stack[cv_stack_len], rcv);
	}
	b3_output_root(&final, out, outlen);
}

/*
 * PUBLIC: int __os_crypto_blake3 __P((const void *, size_t, uint8_t *));
 */
int
__os_crypto_blake3(const void *data, size_t len,
    uint8_t out[XTC_CRYPTO_BLAKE3_LEN])
{
	if ((data == NULL && len != 0) || out == NULL)
		return XTC_E_INVAL;

	b3_hash((const uint8_t *)data, len, out, XTC_CRYPTO_BLAKE3_LEN);
	return XTC_OK;
}

#if defined(XTC_TLS_BACKEND_OPENSSL)

#include <limits.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#if defined(OPENSSL_IS_BORINGSSL)
/*
 * BoringSSL does not implement OpenSSL 3.x's EVP_KDF / OSSL_PARAM
 * provider API (no <openssl/kdf.h> EVP_KDF, no core_names.h); it
 * exposes HKDF through the older, simpler <openssl/hkdf.h> one-shot
 * functions instead.  The HKDF implementation below branches on
 * OPENSSL_IS_BORINGSSL to use whichever the backend actually provides.
 */
#include <openssl/hkdf.h>
#include <openssl/digest.h>
#include <openssl/aead.h>   /* EVP_aead_chacha20_poly1305 (BoringSSL) */
#else
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#endif

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
 * PUBLIC: int __os_crypto_sha3_256 __P((const void *, size_t, uint8_t *));
 */
int
__os_crypto_sha3_256(const void *data, size_t len,
    uint8_t out[XTC_CRYPTO_SHA3_256_LEN])
{
	unsigned int outlen;

	if ((data == NULL && len != 0) || out == NULL)
		return XTC_E_INVAL;

#if defined(OPENSSL_IS_BORINGSSL)
	/* BoringSSL does not implement SHA-3 at all (no EVP_sha3_*, no
	 * Keccak in its EVP surface).  Rather than carry a portable Keccak
	 * reimplementation for a deferred-until-needed primitive on one
	 * backend, follow the module's "always linkable, NOSYS when the
	 * backend cannot provide it" convention. */
	(void)outlen;
	return XTC_E_NOSYS;
#else
	if (EVP_Digest(data, len, out, &outlen, EVP_sha3_256(), NULL) != 1)
		return XTC_E_INTERNAL;
	return XTC_OK;
#endif
}

/*
 * PUBLIC: int __os_crypto_sha3_512 __P((const void *, size_t, uint8_t *));
 */
int
__os_crypto_sha3_512(const void *data, size_t len,
    uint8_t out[XTC_CRYPTO_SHA3_512_LEN])
{
	unsigned int outlen;

	if ((data == NULL && len != 0) || out == NULL)
		return XTC_E_INVAL;

#if defined(OPENSSL_IS_BORINGSSL)
	(void)outlen;
	return XTC_E_NOSYS;   /* no SHA-3 in BoringSSL (see sha3_256) */
#else
	if (EVP_Digest(data, len, out, &outlen, EVP_sha3_512(), NULL) != 1)
		return XTC_E_INTERNAL;
	return XTC_OK;
#endif
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

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) != 1) {
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

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) != 1) {
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
 * PUBLIC: int __os_crypto_chacha20_poly1305_encrypt __P((const uint8_t *, const uint8_t *, const void *, size_t, const void *, size_t, void *, uint8_t *));
 */
int
__os_crypto_chacha20_poly1305_encrypt(
    const uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN],
    const uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN],
    const void *aad, size_t aadlen,
    const void *plaintext, size_t len,
    void *ciphertext_out, uint8_t tag_out[XTC_CRYPTO_CHACHA_TAG_LEN])
{
	if (key == NULL || nonce == NULL || tag_out == NULL)
		return XTC_E_INVAL;
	if ((plaintext == NULL || ciphertext_out == NULL) && len != 0)
		return XTC_E_INVAL;
	if (aad == NULL && aadlen != 0)
		return XTC_E_INVAL;
	if (len > (size_t)INT_MAX || aadlen > (size_t)INT_MAX)
		return XTC_E_RANGE;

#if defined(OPENSSL_IS_BORINGSSL)
	{
		/* BoringSSL exposes ChaCha20-Poly1305 only via EVP_AEAD, not
		 * the EVP_CIPHER interface (no EVP_chacha20_poly1305 /
		 * EVP_EncryptInit_ex2).  The AEAD seal writes ciphertext||tag
		 * contiguously, so seal into a scratch layout and split. */
		const EVP_AEAD *aead = EVP_aead_chacha20_poly1305();
		EVP_AEAD_CTX *actx;
		unsigned char stackbuf[512];
		unsigned char *sealbuf = stackbuf;
		size_t sealed = 0, need = len + XTC_CRYPTO_CHACHA_TAG_LEN;
		int ok = 0;

		if (need > sizeof stackbuf &&
		    __os_malloc(need, (void **)&sealbuf) != XTC_OK)
			return XTC_E_NOMEM;
		actx = EVP_AEAD_CTX_new(aead, key, XTC_CRYPTO_CHACHA_KEY_LEN,
		    XTC_CRYPTO_CHACHA_TAG_LEN);
		if (actx != NULL &&
		    EVP_AEAD_CTX_seal(actx, sealbuf, &sealed, need,
		        nonce, XTC_CRYPTO_CHACHA_NONCE_LEN,
		        plaintext, len, aad, aadlen) == 1 &&
		    sealed == need) {
			if (len > 0 && ciphertext_out != NULL)
				memcpy(ciphertext_out, sealbuf, len);
			memcpy(tag_out, sealbuf + len,
			    XTC_CRYPTO_CHACHA_TAG_LEN);
			ok = 1;
		}
		if (actx != NULL)
			EVP_AEAD_CTX_free(actx);
		if (sealbuf != stackbuf)
			__os_free(sealbuf);
		return ok ? XTC_OK : XTC_E_INTERNAL;
	}
#else
	{
	EVP_CIPHER_CTX *ctx;
	unsigned char final_scratch[EVP_MAX_BLOCK_LENGTH];
	unsigned char *cout = ciphertext_out;
	int outl, tmplen;

	if ((ctx = EVP_CIPHER_CTX_new()) == NULL)
		return XTC_E_INTERNAL;

	/* The RFC 8439 12-byte nonce is EVP_chacha20_poly1305's default IV
	 * length, so no EVP_CTRL_AEAD_SET_IVLEN is needed (same as the
	 * AES-GCM path above). */
	if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL,
	    key, nonce) != 1) {
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

	if (EVP_EncryptFinal_ex(ctx,
	    (cout != NULL) ? cout + outl : final_scratch, &tmplen) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
	    XTC_CRYPTO_CHACHA_TAG_LEN, tag_out) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return XTC_E_INTERNAL;
	}

	EVP_CIPHER_CTX_free(ctx);
	return XTC_OK;
	}
#endif
}

/*
 * PUBLIC: int __os_crypto_chacha20_poly1305_decrypt __P((const uint8_t *, const uint8_t *, const void *, size_t, const void *, size_t, const uint8_t *, void *));
 */
int
__os_crypto_chacha20_poly1305_decrypt(
    const uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN],
    const uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN],
    const void *aad, size_t aadlen,
    const void *ciphertext, size_t len,
    const uint8_t tag[XTC_CRYPTO_CHACHA_TAG_LEN],
    void *plaintext_out)
{
	unsigned char *pout = plaintext_out;

	if (key == NULL || nonce == NULL || tag == NULL)
		return XTC_E_INVAL;
	if ((ciphertext == NULL || plaintext_out == NULL) && len != 0)
		return XTC_E_INVAL;
	if (aad == NULL && aadlen != 0)
		return XTC_E_INVAL;
	if (len > (size_t)INT_MAX || aadlen > (size_t)INT_MAX)
		return XTC_E_RANGE;

#if defined(OPENSSL_IS_BORINGSSL)
	{
		/* BoringSSL EVP_AEAD open: the sealed input is
		 * ciphertext||tag contiguous, so stitch them into one buffer,
		 * open (which verifies the tag), and on failure wipe any
		 * provisional output and return XTC_E_INVAL. */
		const EVP_AEAD *aead = EVP_aead_chacha20_poly1305();
		EVP_AEAD_CTX *actx;
		unsigned char stackbuf[512];
		unsigned char *inbuf = stackbuf;
		size_t got = 0, need = len + XTC_CRYPTO_CHACHA_TAG_LEN;
		int rc = XTC_E_INVAL;

		if (need > sizeof stackbuf &&
		    __os_malloc(need, (void **)&inbuf) != XTC_OK)
			return XTC_E_NOMEM;
		if (len > 0 && ciphertext != NULL)
			memcpy(inbuf, ciphertext, len);
		memcpy(inbuf + len, tag, XTC_CRYPTO_CHACHA_TAG_LEN);
		actx = EVP_AEAD_CTX_new(aead, key, XTC_CRYPTO_CHACHA_KEY_LEN,
		    XTC_CRYPTO_CHACHA_TAG_LEN);
		if (actx != NULL &&
		    EVP_AEAD_CTX_open(actx, pout, &got, len,
		        nonce, XTC_CRYPTO_CHACHA_NONCE_LEN,
		        inbuf, need, aad, aadlen) == 1 && got == len) {
			rc = XTC_OK;
		} else if (len > 0 && pout != NULL) {
			memset(pout, 0, len);
		}
		if (actx != NULL)
			EVP_AEAD_CTX_free(actx);
		if (inbuf != stackbuf)
			__os_free(inbuf);
		return rc;
	}
#else
	{
	EVP_CIPHER_CTX *ctx;
	unsigned char final_scratch[EVP_MAX_BLOCK_LENGTH];
	int outl, tmplen, rc;

	if ((ctx = EVP_CIPHER_CTX_new()) == NULL)
		return XTC_E_INTERNAL;

	if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL,
	    key, nonce) != 1) {
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

	/* Cast away const on the tag: EVP_CTRL_AEAD_SET_TAG's third arg is
	 * void * even though it only reads (same as the AES-GCM path). */
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
	    XTC_CRYPTO_CHACHA_TAG_LEN, (void *)tag) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		if (len > 0)
			memset(pout, 0, len);
		return XTC_E_INTERNAL;
	}

	/* Poly1305 tag is verified HERE.  On mismatch, wipe any
	 * provisional plaintext and return XTC_E_INVAL (distinct from
	 * XTC_OK and XTC_E_INTERNAL), exactly like the AES-GCM path. */
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
#endif
}

/*
 * HKDF-SHA256 (RFC 5869) via OpenSSL 3.x's EVP_KDF "HKDF" provider.
 * hkdf_run drives one EVP_KDF_derive in the requested mode; the three
 * public entry points below (extract, expand, combined) are thin
 * wrappers over it.
 */

/* Backend-neutral HKDF mode selector (the OpenSSL-3.x EVP_KDF_HKDF_MODE_*
 * and BoringSSL paths both key off these, so callers never name a
 * backend-specific constant). */
#define XTC_HKDF_MODE_EXTRACT_AND_EXPAND  0
#define XTC_HKDF_MODE_EXTRACT_ONLY        1
#define XTC_HKDF_MODE_EXPAND_ONLY         2

static int
hkdf_run(int mode, const void *ikm, size_t ikmlen,
    const void *salt, size_t saltlen, const void *info, size_t infolen,
    unsigned char *out, size_t outlen)
{
#if defined(OPENSSL_IS_BORINGSSL)
	/*
	 * BoringSSL: use the one-shot <openssl/hkdf.h> functions.
	 * EXTRACT_ONLY -> HKDF_extract (out is the PRK, outlen == HashLen);
	 * EXPAND_ONLY  -> HKDF_expand (ikm is the PRK);
	 * EXTRACT_AND_EXPAND -> HKDF (the full one-shot).
	 */
	const EVP_MD *md = EVP_sha256();

	if (ikmlen > (size_t)INT_MAX || saltlen > (size_t)INT_MAX ||
	    infolen > (size_t)INT_MAX)
		return XTC_E_RANGE;

	switch (mode) {
	case XTC_HKDF_MODE_EXTRACT_ONLY: {
		size_t prklen = 0;
		if (HKDF_extract(out, &prklen, md, ikm, ikmlen,
		    salt, saltlen) != 1)
			return XTC_E_INTERNAL;
		return XTC_OK;
	}
	case XTC_HKDF_MODE_EXPAND_ONLY:
		if (HKDF_expand(out, outlen, md, ikm, ikmlen,
		    info, infolen) != 1)
			return XTC_E_INTERNAL;
		return XTC_OK;
	case XTC_HKDF_MODE_EXTRACT_AND_EXPAND:
	default:
		if (HKDF(out, outlen, md, ikm, ikmlen, salt, saltlen,
		    info, infolen) != 1)
			return XTC_E_INTERNAL;
		return XTC_OK;
	}
#else
	/* OpenSSL 3.x: the EVP_KDF / OSSL_PARAM provider API. */
	static const int evp_mode[] = {
		[XTC_HKDF_MODE_EXTRACT_AND_EXPAND] =
		    EVP_KDF_HKDF_MODE_EXTRACT_AND_EXPAND,
		[XTC_HKDF_MODE_EXTRACT_ONLY] = EVP_KDF_HKDF_MODE_EXTRACT_ONLY,
		[XTC_HKDF_MODE_EXPAND_ONLY]  = EVP_KDF_HKDF_MODE_EXPAND_ONLY,
	};
	EVP_KDF     *kdf;
	EVP_KDF_CTX *kctx;
	OSSL_PARAM   params[6], *p = params;
	char         digest[] = "SHA256";
	int          m = evp_mode[mode];
	int          rc = XTC_E_INTERNAL;

	if (ikmlen > (size_t)INT_MAX || saltlen > (size_t)INT_MAX ||
	    infolen > (size_t)INT_MAX)
		return XTC_E_RANGE;

	if ((kdf = EVP_KDF_fetch(NULL, "HKDF", NULL)) == NULL)
		return XTC_E_INTERNAL;
	if ((kctx = EVP_KDF_CTX_new(kdf)) == NULL) {
		EVP_KDF_free(kdf);
		return XTC_E_INTERNAL;
	}

	*p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
	    digest, 0);
	*p++ = OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &m);
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
	    (void *)ikm, ikmlen);
	if (salt != NULL)
		*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
		    (void *)salt, saltlen);
	if (info != NULL)
		*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
		    (void *)info, infolen);
	*p = OSSL_PARAM_construct_end();

	if (EVP_KDF_derive(kctx, out, outlen, params) == 1)
		rc = XTC_OK;

	EVP_KDF_CTX_free(kctx);
	EVP_KDF_free(kdf);
	return rc;
#endif
}

/*
 * PUBLIC: int __os_crypto_hkdf_extract __P((const void *, size_t, const void *, size_t, uint8_t *));
 */
int
__os_crypto_hkdf_extract(const void *salt, size_t saltlen,
    const void *ikm, size_t ikmlen, uint8_t prk_out[XTC_CRYPTO_SHA256_LEN])
{
	if (prk_out == NULL || (ikm == NULL && ikmlen != 0) ||
	    (salt == NULL && saltlen != 0))
		return XTC_E_INVAL;
	return hkdf_run(XTC_HKDF_MODE_EXTRACT_ONLY, ikm, ikmlen,
	    salt, saltlen, NULL, 0, prk_out, XTC_CRYPTO_SHA256_LEN);
}

/*
 * PUBLIC: int __os_crypto_hkdf_expand __P((const uint8_t *, const void *, size_t, void *, size_t));
 */
int
__os_crypto_hkdf_expand(const uint8_t prk[XTC_CRYPTO_SHA256_LEN],
    const void *info, size_t infolen, void *out, size_t outlen)
{
	if (prk == NULL || out == NULL || (info == NULL && infolen != 0))
		return XTC_E_INVAL;
	/* RFC 5869: expand output is at most 255*HashLen. */
	if (outlen == 0 || outlen > 255 * (size_t)XTC_CRYPTO_SHA256_LEN)
		return XTC_E_INVAL;
	/* Expand-only takes the PRK via the "key" param. */
	return hkdf_run(XTC_HKDF_MODE_EXPAND_ONLY, prk,
	    XTC_CRYPTO_SHA256_LEN, NULL, 0, info, infolen, out, outlen);
}

/*
 * PUBLIC: int __os_crypto_hkdf __P((const void *, size_t, const void *, size_t, const void *, size_t, void *, size_t));
 */
int
__os_crypto_hkdf(const void *ikm, size_t ikmlen,
    const void *salt, size_t saltlen, const void *info, size_t infolen,
    void *out, size_t outlen)
{
	if (out == NULL || (ikm == NULL && ikmlen != 0) ||
	    (salt == NULL && saltlen != 0) || (info == NULL && infolen != 0))
		return XTC_E_INVAL;
	if (outlen == 0 || outlen > 255 * (size_t)XTC_CRYPTO_SHA256_LEN)
		return XTC_E_INVAL;
	return hkdf_run(XTC_HKDF_MODE_EXTRACT_AND_EXPAND, ikm, ikmlen,
	    salt, saltlen, info, infolen, out, outlen);
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
__os_crypto_sha3_256(const void *data, size_t len,
    uint8_t out[XTC_CRYPTO_SHA3_256_LEN])
{
	if ((data == NULL && len != 0) || out == NULL)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_crypto_sha3_512(const void *data, size_t len,
    uint8_t out[XTC_CRYPTO_SHA3_512_LEN])
{
	if ((data == NULL && len != 0) || out == NULL)
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
__os_crypto_chacha20_poly1305_encrypt(
    const uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN],
    const uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN],
    const void *aad, size_t aadlen,
    const void *plaintext, size_t len,
    void *ciphertext_out, uint8_t tag_out[XTC_CRYPTO_CHACHA_TAG_LEN])
{
	if (key == NULL || nonce == NULL || tag_out == NULL)
		return XTC_E_INVAL;
	if ((plaintext == NULL || ciphertext_out == NULL) && len != 0)
		return XTC_E_INVAL;
	if (aad == NULL && aadlen != 0)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_crypto_chacha20_poly1305_decrypt(
    const uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN],
    const uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN],
    const void *aad, size_t aadlen,
    const void *ciphertext, size_t len,
    const uint8_t tag[XTC_CRYPTO_CHACHA_TAG_LEN],
    void *plaintext_out)
{
	if (key == NULL || nonce == NULL || tag == NULL)
		return XTC_E_INVAL;
	if ((ciphertext == NULL || plaintext_out == NULL) && len != 0)
		return XTC_E_INVAL;
	if (aad == NULL && aadlen != 0)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_crypto_hkdf_extract(const void *salt, size_t saltlen,
    const void *ikm, size_t ikmlen, uint8_t prk_out[XTC_CRYPTO_SHA256_LEN])
{
	if (prk_out == NULL || (ikm == NULL && ikmlen != 0) ||
	    (salt == NULL && saltlen != 0))
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_crypto_hkdf_expand(const uint8_t prk[XTC_CRYPTO_SHA256_LEN],
    const void *info, size_t infolen, void *out, size_t outlen)
{
	if (prk == NULL || out == NULL || (info == NULL && infolen != 0))
		return XTC_E_INVAL;
	if (outlen == 0 || outlen > 255 * (size_t)XTC_CRYPTO_SHA256_LEN)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
__os_crypto_hkdf(const void *ikm, size_t ikmlen,
    const void *salt, size_t saltlen, const void *info, size_t infolen,
    void *out, size_t outlen)
{
	if (out == NULL || (ikm == NULL && ikmlen != 0) ||
	    (salt == NULL && saltlen != 0) || (info == NULL && infolen != 0))
		return XTC_E_INVAL;
	if (outlen == 0 || outlen > 255 * (size_t)XTC_CRYPTO_SHA256_LEN)
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

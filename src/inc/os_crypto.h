/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/os_crypto.h
 *	L0 cryptography building blocks (PLAN.md 19.2), v1 scope: only
 *	the two real, stated use cases -- checksums/hashing for WAL
 *	integrity, and a per-loop CSPRNG.  Backed ONLY by OpenSSL (already
 *	a build dependency for TLS); the guard is the SAME macro
 *	src/io/tls_openssl.c uses to detect the selected TLS backend
 *	(XTC_TLS_BACKEND_OPENSSL), so this header/implementation need no
 *	new configure flag.
 *
 *	When OpenSSL is NOT the selected backend (XTC_TLS_BACKEND_OPENSSL
 *	undefined -- e.g. --with-tls=none, mbedtls, gnutls, ...) every
 *	function below is still linkable; each returns XTC_E_NOSYS at
 *	runtime, following the same "always linkable, NOSYS when
 *	unsupported" convention as src/io/tls_none.c.
 *
 *	Extended past the original v1 scope for future TDE/WAL-encryption
 *	work (built ahead of a concrete consumer, per PLAN.md 19.2):
 *	ChaCha20-Poly1305 AEAD, SHA-3 (256/512), BLAKE3, and HKDF-SHA256.
 *	ChaCha20-Poly1305, SHA-3, and HKDF are OpenSSL-backed (NOSYS when
 *	OpenSSL is not the selected backend); BLAKE3 is a small
 *	self-contained portable C reimplementation and is therefore
 *	available on EVERY build regardless of TLS backend.
 *
 *	Still DEFERRED (see man/man3/__os_crypto.3): any backend other
 *	than OpenSSL (libsodium, BoringSSL, SChannel, CommonCrypto).
 *	This is a library-internal L0 primitive; there is no public
 *	xtc_* wrapper in this pass because no consumer needs direct
 *	access yet.
 */

#ifndef XTC_OS_CRYPTO_H
#define XTC_OS_CRYPTO_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#define XTC_CRYPTO_SHA256_LEN     32
#define XTC_CRYPTO_SHA3_256_LEN   32
#define XTC_CRYPTO_SHA3_512_LEN   64
#define XTC_CRYPTO_BLAKE3_LEN     32   /* BLAKE3 default output length */
#define XTC_CRYPTO_AES_KEY_LEN    32   /* AES-256 */
#define XTC_CRYPTO_AES_IV_LEN     12   /* GCM standard nonce length */
#define XTC_CRYPTO_AES_TAG_LEN    16   /* GCM standard tag length */
#define XTC_CRYPTO_CHACHA_KEY_LEN 32   /* ChaCha20-Poly1305 key length */
#define XTC_CRYPTO_CHACHA_NONCE_LEN 12 /* ChaCha20-Poly1305 nonce length */
#define XTC_CRYPTO_CHACHA_TAG_LEN 16   /* Poly1305 tag length */

/*
 * --- One-shot hashing (WAL/checksum use) ---
 *
 * __os_crypto_sha256 writes the 32-byte SHA-256 digest of data[0..len)
 * to out.  __os_crypto_hmac_sha256 writes the 32-byte HMAC-SHA256 of
 * data[0..len) under key[0..keylen) to out.
 *
 * Both return XTC_OK, XTC_E_INVAL (NULL data/out with nonzero len, or
 * NULL out), or XTC_E_INTERNAL (the underlying OpenSSL call failed --
 * an invariant violation, not an expected runtime condition).  When
 * OpenSSL is not the selected TLS backend, both return XTC_E_NOSYS.
 *
 * PUBLIC: int __os_crypto_sha256 __P((const void *, size_t, uint8_t *));
 * PUBLIC: int __os_crypto_hmac_sha256 __P((const void *, size_t, const void *, size_t, uint8_t *));
 */
XTC_API int __os_crypto_sha256(const void *data, size_t len,
            uint8_t out[XTC_CRYPTO_SHA256_LEN]);
XTC_API int __os_crypto_hmac_sha256(const void *key, size_t keylen,
            const void *data, size_t len, uint8_t out[XTC_CRYPTO_SHA256_LEN]);

/*
 * --- SHA-3 one-shot digests ---
 *
 * __os_crypto_sha3_256 / _sha3_512 write the 32- resp. 64-byte SHA-3
 * (FIPS 202, Keccak) digest of data[0..len) to out.  Same return
 * codes and NULL-argument contract as __os_crypto_sha256.  Return
 * XTC_E_NOSYS when OpenSSL is not the selected TLS backend AND on
 * BoringSSL, which -- despite defining XTC_TLS_BACKEND_OPENSSL --
 * ships no SHA-3 (no EVP_sha3_*, no Keccak in its EVP surface).  A
 * portable Keccak reimplementation was judged not worth carrying for
 * a deferred-until-needed primitive on one backend; BLAKE3 (which
 * OpenSSL lacks too) IS carried portably because it was the primary
 * hashing target.
 *
 * PUBLIC: int __os_crypto_sha3_256 __P((const void *, size_t, uint8_t *));
 * PUBLIC: int __os_crypto_sha3_512 __P((const void *, size_t, uint8_t *));
 */
XTC_API int __os_crypto_sha3_256(const void *data, size_t len,
            uint8_t out[XTC_CRYPTO_SHA3_256_LEN]);
XTC_API int __os_crypto_sha3_512(const void *data, size_t len,
            uint8_t out[XTC_CRYPTO_SHA3_512_LEN]);

/*
 * --- BLAKE3 one-shot digest ---
 *
 * __os_crypto_blake3 writes the 32-byte (default-length) BLAKE3
 * digest of data[0..len) to out.  UNLIKE the other primitives here,
 * BLAKE3 is a small self-contained PORTABLE reimplementation compiled
 * directly into os_crypto.c (OpenSSL does not provide BLAKE3), so it
 * is available on EVERY build and NEVER returns XTC_E_NOSYS.  Returns
 * XTC_OK or XTC_E_INVAL (NULL out, or NULL data with nonzero len).
 *
 * PUBLIC: int __os_crypto_blake3 __P((const void *, size_t, uint8_t *));
 */
XTC_API int __os_crypto_blake3(const void *data, size_t len,
            uint8_t out[XTC_CRYPTO_BLAKE3_LEN]);

/*
 * --- AES-256-GCM one-shot authenticated encryption ---
 *
 * key is XTC_CRYPTO_AES_KEY_LEN (32) bytes, iv is XTC_CRYPTO_AES_IV_LEN
 * (12) bytes (the standard GCM nonce length -- reuse of an (key, iv)
 * pair is a catastrophic confidentiality break; callers must supply a
 * fresh iv per encryption, e.g. from __os_csprng_bytes).  aad may be
 * NULL/0 (no additional authenticated data).  ciphertext and
 * plaintext are exactly len bytes; tag is exactly
 * XTC_CRYPTO_AES_TAG_LEN (16) bytes.
 *
 * __os_crypto_aes_gcm_encrypt writes len bytes of ciphertext and the
 * 16-byte tag.  Returns XTC_OK, XTC_E_INVAL (bad argument), or
 * XTC_E_INTERNAL.
 *
 * __os_crypto_aes_gcm_decrypt verifies the tag BEFORE trusting the
 * recovered bytes.  On a tag mismatch it returns XTC_E_INVAL and does
 * NOT write plaintext (the output buffer is left untouched -- the
 * caller must not read it).  On success it writes len bytes of
 * plaintext and returns XTC_OK.  Returns XTC_E_INTERNAL on an
 * unexpected OpenSSL failure unrelated to tag verification.
 *
 * When OpenSSL is not the selected TLS backend both return
 * XTC_E_NOSYS.
 *
 * PUBLIC: int __os_crypto_aes_gcm_encrypt __P((const uint8_t *, const uint8_t *, const void *, size_t, const void *, size_t, void *, uint8_t *));
 * PUBLIC: int __os_crypto_aes_gcm_decrypt __P((const uint8_t *, const uint8_t *, const void *, size_t, const void *, size_t, const uint8_t *, void *));
 */
XTC_API int __os_crypto_aes_gcm_encrypt(
            const uint8_t key[XTC_CRYPTO_AES_KEY_LEN],
            const uint8_t iv[XTC_CRYPTO_AES_IV_LEN],
            const void *aad, size_t aadlen,
            const void *plaintext, size_t len,
            void *ciphertext_out, uint8_t tag_out[XTC_CRYPTO_AES_TAG_LEN]);
XTC_API int __os_crypto_aes_gcm_decrypt(
            const uint8_t key[XTC_CRYPTO_AES_KEY_LEN],
            const uint8_t iv[XTC_CRYPTO_AES_IV_LEN],
            const void *aad, size_t aadlen,
            const void *ciphertext, size_t len,
            const uint8_t tag[XTC_CRYPTO_AES_TAG_LEN],
            void *plaintext_out);

/*
 * --- ChaCha20-Poly1305 one-shot authenticated encryption ---
 *
 * A second AEAD, same shape as the AES-256-GCM pair above: key is
 * XTC_CRYPTO_CHACHA_KEY_LEN (32) bytes, nonce is
 * XTC_CRYPTO_CHACHA_NONCE_LEN (12) bytes (the RFC 8439 96-bit nonce;
 * reuse of a (key, nonce) pair is a catastrophic confidentiality
 * break -- supply a fresh nonce per encryption, e.g. from
 * __os_csprng_bytes).  aad may be NULL/0.  ciphertext and plaintext
 * are exactly len bytes; tag is XTC_CRYPTO_CHACHA_TAG_LEN (16) bytes.
 *
 * _decrypt verifies the Poly1305 tag BEFORE trusting the recovered
 * bytes; on a tag mismatch it returns XTC_E_INVAL, zeroes any
 * provisional plaintext, and does NOT leave the plaintext in
 * plaintext_out (identical guarantee to __os_crypto_aes_gcm_decrypt).
 *
 * OpenSSL is the only backend, so both return XTC_E_NOSYS when
 * OpenSSL is not the selected TLS backend.
 *
 * PUBLIC: int __os_crypto_chacha20_poly1305_encrypt __P((const uint8_t *, const uint8_t *, const void *, size_t, const void *, size_t, void *, uint8_t *));
 * PUBLIC: int __os_crypto_chacha20_poly1305_decrypt __P((const uint8_t *, const uint8_t *, const void *, size_t, const void *, size_t, const uint8_t *, void *));
 */
XTC_API int __os_crypto_chacha20_poly1305_encrypt(
            const uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN],
            const uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN],
            const void *aad, size_t aadlen,
            const void *plaintext, size_t len,
            void *ciphertext_out,
            uint8_t tag_out[XTC_CRYPTO_CHACHA_TAG_LEN]);
XTC_API int __os_crypto_chacha20_poly1305_decrypt(
            const uint8_t key[XTC_CRYPTO_CHACHA_KEY_LEN],
            const uint8_t nonce[XTC_CRYPTO_CHACHA_NONCE_LEN],
            const void *aad, size_t aadlen,
            const void *ciphertext, size_t len,
            const uint8_t tag[XTC_CRYPTO_CHACHA_TAG_LEN],
            void *plaintext_out);

/*
 * --- HKDF-SHA256 (RFC 5869) key derivation ---
 *
 * __os_crypto_hkdf_extract runs the HKDF-Extract step: it computes
 * PRK = HMAC-SHA256(salt, ikm) and writes the 32-byte PRK to
 * prk_out.  salt may be NULL/0 (RFC 5869 then substitutes a string of
 * HashLen zero bytes).
 *
 * __os_crypto_hkdf_expand runs the HKDF-Expand step: it derives
 * outlen bytes of output key material from prk (32 bytes) and the
 * optional info (may be NULL/0) into out.  outlen must be in
 * [1, 255*32] per RFC 5869; a larger request returns XTC_E_INVAL.
 *
 * __os_crypto_hkdf combines both (Extract then Expand) in one call --
 * the common case -- deriving outlen bytes from (ikm, salt, info).
 *
 * OpenSSL is the only backend, so all three return XTC_E_NOSYS when
 * OpenSSL is not the selected TLS backend.  Otherwise XTC_OK,
 * XTC_E_INVAL (bad argument), XTC_E_RANGE (a length exceeds INT_MAX),
 * or XTC_E_INTERNAL.
 *
 * PUBLIC: int __os_crypto_hkdf_extract __P((const void *, size_t, const void *, size_t, uint8_t *));
 * PUBLIC: int __os_crypto_hkdf_expand __P((const uint8_t *, const void *, size_t, void *, size_t));
 * PUBLIC: int __os_crypto_hkdf __P((const void *, size_t, const void *, size_t, const void *, size_t, void *, size_t));
 */
XTC_API int __os_crypto_hkdf_extract(const void *salt, size_t saltlen,
            const void *ikm, size_t ikmlen,
            uint8_t prk_out[XTC_CRYPTO_SHA256_LEN]);
XTC_API int __os_crypto_hkdf_expand(
            const uint8_t prk[XTC_CRYPTO_SHA256_LEN],
            const void *info, size_t infolen, void *out, size_t outlen);
XTC_API int __os_crypto_hkdf(const void *ikm, size_t ikmlen,
            const void *salt, size_t saltlen,
            const void *info, size_t infolen, void *out, size_t outlen);

/*
 * --- Per-loop CSPRNG ---
 *
 * An opaque handle around an OpenSSL DRBG (see os_crypto.c for which
 * one and why).  __os_csprng_init seeds it from __os_rand_u64 (the
 * existing per-thread entropy primitive in src/os/os_rand.c) mixed
 * with OpenSSL's own default RAND_bytes seed material -- no new
 * entropy source is invented.  __os_csprng_bytes streams len
 * cryptographically-strong pseudorandom bytes into buf.
 * __os_csprng_destroy releases the handle; NULL is a no-op.
 *
 * Returns XTC_OK, XTC_E_INVAL, XTC_E_NOMEM (init only), or
 * XTC_E_INTERNAL.  When OpenSSL is not the selected TLS backend,
 * init/bytes return XTC_E_NOSYS and destroy is a no-op.
 *
 * PUBLIC: int __os_csprng_init __P((__os_csprng_t **));
 * PUBLIC: int __os_csprng_bytes __P((__os_csprng_t *, void *, size_t));
 * PUBLIC: void __os_csprng_destroy __P((__os_csprng_t *));
 */
typedef struct __os_csprng __os_csprng_t;

XTC_API int  __os_csprng_init(__os_csprng_t **out);
XTC_API int  __os_csprng_bytes(__os_csprng_t *rng, void *buf, size_t len);
XTC_API void __os_csprng_destroy(__os_csprng_t *rng);

#endif /* XTC_OS_CRYPTO_H */

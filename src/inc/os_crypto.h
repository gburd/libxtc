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
 *	Deliberately DEFERRED past v1 (see man/man3/__os_crypto.3 and the
 *	implementer's report): ChaCha20-Poly1305, SHA-3, BLAKE3, HKDF,
 *	and any backend other than OpenSSL (libsodium, BoringSSL,
 *	SChannel, CommonCrypto).  This is a library-internal L0
 *	primitive; there is no public xtc_* wrapper in this pass because
 *	no consumer needs direct access yet.
 */

#ifndef XTC_OS_CRYPTO_H
#define XTC_OS_CRYPTO_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#define XTC_CRYPTO_SHA256_LEN   32
#define XTC_CRYPTO_AES_KEY_LEN  32   /* AES-256 */
#define XTC_CRYPTO_AES_IV_LEN   12   /* GCM standard nonce length */
#define XTC_CRYPTO_AES_TAG_LEN  16   /* GCM standard tag length */

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

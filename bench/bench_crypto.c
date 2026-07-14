/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_crypto.c
 *	Throughput of the L0 crypto building blocks (PLAN.md 19.2 v1):
 *	SHA-256, AES-256-GCM (encrypt), and CSPRNG byte generation, at a
 *	few buffer sizes.  Reports MB/s for the hash/cipher and bytes/sec
 *	for the CSPRNG.
 *
 *	Requires a build configured with the OpenSSL TLS backend
 *	(--with-tls=openssl); under any other backend every primitive
 *	returns XTC_E_NOSYS and this benchmark says so and exits
 *	cleanly rather than reporting a bogus zero throughput.
 *
 *	Usage: bench_crypto [iterations_per_size]   (default 200)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc_int.h"
#include "os_crypto.h"
#include "os_time.h"

static const size_t sizes[] = { 64, 4096, 65536, 1048576 };

static double
elapsed_seconds(int64_t t0, int64_t t1)
{
	return (double)(t1 - t0) / 1e9;
}

static void
bench_sha256(int iters)
{
	size_t i;
	uint8_t out[XTC_CRYPTO_SHA256_LEN];

	printf("-- SHA-256 --\n");
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t sz = sizes[i];
		uint8_t *buf = malloc(sz);
		int64_t t0, t1;
		int n;

		memset(buf, 0xab, sz);
		(void)__os_clock_mono(&t0);
		for (n = 0; n < iters; n++)
			(void)__os_crypto_sha256(buf, sz, out);
		(void)__os_clock_mono(&t1);

		{
			double secs = elapsed_seconds(t0, t1);
			double mb = (double)sz * (double)iters / (1024.0 * 1024.0);
			printf("  %8zu bytes: %8.1f MB/s\n", sz, mb / secs);
		}
		free(buf);
	}
}

static void
bench_aesgcm(int iters)
{
	size_t i;
	uint8_t key[XTC_CRYPTO_AES_KEY_LEN];
	uint8_t iv[XTC_CRYPTO_AES_IV_LEN];
	uint8_t tag[XTC_CRYPTO_AES_TAG_LEN];

	memset(key, 0x5a, sizeof(key));
	memset(iv, 0x24, sizeof(iv));

	printf("-- AES-256-GCM encrypt --\n");
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t sz = sizes[i];
		uint8_t *plain = malloc(sz);
		uint8_t *cipher = malloc(sz);
		int64_t t0, t1;
		int n;

		memset(plain, 0xcd, sz);
		(void)__os_clock_mono(&t0);
		for (n = 0; n < iters; n++)
			(void)__os_crypto_aes_gcm_encrypt(key, iv, NULL, 0,
			    plain, sz, cipher, tag);
		(void)__os_clock_mono(&t1);

		{
			double secs = elapsed_seconds(t0, t1);
			double mb = (double)sz * (double)iters / (1024.0 * 1024.0);
			printf("  %8zu bytes: %8.1f MB/s\n", sz, mb / secs);
		}
		free(plain);
		free(cipher);
	}
}

static void
bench_csprng(int iters)
{
	__os_csprng_t *rng = NULL;
	size_t i;

	if (__os_csprng_init(&rng) != XTC_OK) {
		printf("-- CSPRNG -- init failed, skipping\n");
		return;
	}

	printf("-- CSPRNG bytes --\n");
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t sz = sizes[i];
		uint8_t *buf = malloc(sz);
		int64_t t0, t1;
		int n;

		(void)__os_clock_mono(&t0);
		for (n = 0; n < iters; n++)
			(void)__os_csprng_bytes(rng, buf, sz);
		(void)__os_clock_mono(&t1);

		{
			double secs = elapsed_seconds(t0, t1);
			double mb = (double)sz * (double)iters / (1024.0 * 1024.0);
			printf("  %8zu bytes: %8.1f MB/s\n", sz, mb / secs);
		}
		free(buf);
	}
	__os_csprng_destroy(rng);
}

int
main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 200;
	uint8_t probe[XTC_CRYPTO_SHA256_LEN];

	if (iters <= 0)
		iters = 200;

	if (__os_crypto_sha256("", 0, probe) == XTC_E_NOSYS) {
		printf("bench_crypto: OpenSSL is not the selected TLS backend "
		    "(XTC_TLS_BACKEND_OPENSSL undefined); every primitive "
		    "returns XTC_E_NOSYS.  Rebuild with --with-tls=openssl "
		    "to measure throughput.\n");
		return 0;
	}

	printf("xtc bench_crypto: iterations_per_size=%d\n", iters);
	bench_sha256(iters);
	bench_aesgcm(iters);
	bench_csprng(iters);
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sx_siphash.h
 *	SipHash-1-3 (Aumasson and Bernstein; one compression round, three
 *	finalization rounds, the variant Rust's HashMap uses), keyed by two
 *	64-bit words.  The index key encoding hashes a long key tuple with
 *	it (xstore.c, xs_idx_rowkey), so the function and its output are
 *	on-disk format: changing either bumps XS_IDX_FORMAT.
 *
 *	sx_siphash13(k0, k1, p, n) equals Rust's
 *	SipHasher13::new_with_keys(k0, k1) fed p[0..n) with Hasher::write,
 *	then finish(); test_uniq_long checks it against values from there.
 */

#ifndef SQLXTC_SX_SIPHASH_H
#define SQLXTC_SX_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

#define SX_SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

static inline void
sx_sip_round(uint64_t v[4])
{
	v[0] += v[1];
	v[1] = SX_SIP_ROTL(v[1], 13);
	v[1] ^= v[0];
	v[0] = SX_SIP_ROTL(v[0], 32);
	v[2] += v[3];
	v[3] = SX_SIP_ROTL(v[3], 16);
	v[3] ^= v[2];
	v[0] += v[3];
	v[3] = SX_SIP_ROTL(v[3], 21);
	v[3] ^= v[0];
	v[2] += v[1];
	v[1] = SX_SIP_ROTL(v[1], 17);
	v[1] ^= v[2];
	v[2] = SX_SIP_ROTL(v[2], 32);
}

/* SipHash-1-3 of p[0..n) under the key (k0, k1).  The message is read
 * as little-endian 64-bit words, as the reference does. */
static inline uint64_t
sx_siphash13(uint64_t k0, uint64_t k1, const void *src, size_t n)
{
	const uint8_t *p = src;
	uint64_t v[4], m, last = (uint64_t)n << 56;
	size_t i, j, tail = n & 7u;

	v[0] = 0x736f6d6570736575ull ^ k0;
	v[1] = 0x646f72616e646f6dull ^ k1;
	v[2] = 0x6c7967656e657261ull ^ k0;
	v[3] = 0x7465646279746573ull ^ k1;
	for (i = 0; i + 8 <= n; i += 8) {
		m = 0;
		for (j = 0; j < 8; j++)
			m |= (uint64_t)p[i + j] << (8 * j);
		v[3] ^= m;
		sx_sip_round(v);
		v[0] ^= m;
	}
	for (j = 0; j < tail; j++)
		last |= (uint64_t)p[i + j] << (8 * j);
	v[3] ^= last;
	sx_sip_round(v);
	v[0] ^= last;
	v[2] ^= 0xff;
	sx_sip_round(v);
	sx_sip_round(v);
	sx_sip_round(v);
	return v[0] ^ v[1] ^ v[2] ^ v[3];
}

#endif /* SQLXTC_SX_SIPHASH_H */

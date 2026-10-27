/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/fuzz/fuzz_kaka.c -- libFuzzer harness for kaka's request
 * decoding (examples/07_kaka/frame.c), driven the way broker.c drives
 * it: kaka_frame_parse over the connection's receive buffer, and for
 * each complete frame the decoder its type selects (PRODUCE then every
 * record, FETCH, COMMIT, OFFSETS).
 *
 *	The input is the receive buffer, in an exact-size heap copy.
 *	Invariants: a parsed frame lies inside the buffer and its body
 *	length is what the prefix said; every decoded topic, group, key and
 *	value lies inside the frame body; the record walk terminates.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"

static const uint8_t *g_lo, *g_hi;

static void
fz_check(int ok)
{
	if (!ok)
		abort();
}

static void
fz_in(const void *p, size_t n)
{
	const uint8_t *b = p;
	fz_check(n == 0 || (b >= g_lo && b + n <= g_hi));
}

static void
fz_frame(uint8_t type, const uint8_t *body, size_t blen)
{
	kaka_produce_t pr;
	kaka_record_t r;
	kaka_fetch_t f;
	kaka_commit_t c;
	size_t guard;
	int got = 0;

	g_lo = body;
	g_hi = body + blen;
	switch (type) {
	case KAKA_PRODUCE:
		if (kaka_decode_produce(body, blen, &pr) != 0)
			break;
		fz_in(pr.topic, pr.topic_len);
		for (guard = 0; guard <= blen; guard++) {
			got = kaka_produce_next_record(&pr, &r);
			if (got != 1)
				break;
			fz_in(r.key, r.key_len);
			fz_in(r.value, r.value_len);
		}
		fz_check(got != 1);
		break;
	case KAKA_FETCH:
		if (kaka_decode_fetch(body, blen, &f) == 0)
			fz_in(f.topic, f.topic_len);
		break;
	case KAKA_COMMIT:
	case KAKA_OFFSETS:
		if ((type == KAKA_COMMIT ?
		            kaka_decode_commit(body, blen, &c) :
		            kaka_decode_offsets(body, blen, &c)) == 0) {
			fz_in(c.group, c.group_len);
			fz_in(c.topic, c.topic_len);
		}
		break;
	default:
		break;
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int
LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	const uint8_t *body;
	uint8_t *buf, type;
	size_t off = 0, blen;
	long fn;

	if (n > 1 << 20)
		return 0;
	buf = malloc(n ? n : 1);
	fz_check(buf != NULL);
	memcpy(buf, d, n);
	while (off < n) {
		fn = kaka_frame_parse(buf + off, n - off, &type, &body, &blen);
		if (fn <= 0)
			break;
		fz_check((size_t)fn <= n - off && (size_t)fn == 5 + blen);
		fz_check(body == buf + off + 5);
		fz_frame(type, body, blen);
		off += (size_t)fn;
	}
	free(buf);
	return 0;
}

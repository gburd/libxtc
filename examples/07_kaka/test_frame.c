/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_frame.c -- standalone round-trip tests for the kaka codec.
 *	No broker, no sockets: encode into a buffer, parse it back,
 *	assert equality.  Runs in make's test target without a daemon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"

#define ASSERT(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	exit(1); } } while (0)

static void
test_int_roundtrip(void)
{
	uint8_t b[8];
	kaka_put_u16(b, 0xbeef);  ASSERT(kaka_get_u16(b) == 0xbeef);
	kaka_put_u32(b, 0xdeadbeefu); ASSERT(kaka_get_u32(b) == 0xdeadbeefu);
	kaka_put_u64(b, 0x0123456789abcdefULL);
	ASSERT(kaka_get_u64(b) == 0x0123456789abcdefULL);
	/* Big-endian on the wire regardless of host. */
	kaka_put_u32(b, 1);
	ASSERT(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1);
	printf("  ok   int_roundtrip\n");
}

static void
test_frame_envelope(void)
{
	uint8_t buf[64];
	uint8_t type;
	const uint8_t *body;
	size_t body_len;
	long n;

	/* header for a 10-byte body */
	ASSERT(kaka_frame_header(buf, sizeof buf, KAKA_PRODUCE, 10) == 0);
	memset(buf + 5, 0x55, 10);

	/* partial: only 3 bytes -> need more */
	ASSERT(kaka_frame_parse(buf, 3, &type, &body, &body_len) == 0);
	/* partial: header present but body short */
	ASSERT(kaka_frame_parse(buf, 6, &type, &body, &body_len) == 0);
	/* complete */
	n = kaka_frame_parse(buf, 5 + 10, &type, &body, &body_len);
	ASSERT(n == 5 + 10);
	ASSERT(type == KAKA_PRODUCE);
	ASSERT(body_len == 10);
	ASSERT(body == buf + 5);
	printf("  ok   frame_envelope\n");
}

static void
test_produce_roundtrip(void)
{
	uint8_t buf[256];
	uint8_t *p = buf;
	kaka_produce_t pr;
	kaka_record_t rec;
	const char *topic = "events";
	uint16_t tlen = (uint16_t)strlen(topic);
	int got;

	/* Hand-encode a PRODUCE body with 2 records. */
	kaka_put_u16(p, tlen); p += 2;
	memcpy(p, topic, tlen); p += tlen;
	kaka_put_u32(p, 3); p += 4;          /* partition 3 */
	kaka_put_u32(p, 2); p += 4;          /* 2 records */
	/* record 0: key "k0" value "v0" */
	kaka_put_u32(p, 2); p += 4; kaka_put_u32(p, 2); p += 4;
	memcpy(p, "k0", 2); p += 2; memcpy(p, "v0", 2); p += 2;
	/* record 1: no key, value "hello" */
	kaka_put_u32(p, 0); p += 4; kaka_put_u32(p, 5); p += 4;
	memcpy(p, "hello", 5); p += 5;

	ASSERT(kaka_decode_produce(buf, (size_t)(p - buf), &pr) == 0);
	ASSERT(pr.topic_len == tlen);
	ASSERT(memcmp(pr.topic, "events", tlen) == 0);
	ASSERT(pr.partition == 3);
	ASSERT(pr.n_records == 2);

	got = kaka_produce_next_record(&pr, &rec);
	ASSERT(got == 1);
	ASSERT(rec.key_len == 2 && memcmp(rec.key, "k0", 2) == 0);
	ASSERT(rec.value_len == 2 && memcmp(rec.value, "v0", 2) == 0);

	got = kaka_produce_next_record(&pr, &rec);
	ASSERT(got == 1);
	ASSERT(rec.key_len == 0);
	ASSERT(rec.value_len == 5 && memcmp(rec.value, "hello", 5) == 0);

	got = kaka_produce_next_record(&pr, &rec);
	ASSERT(got == 0);                    /* exhausted */
	printf("  ok   produce_roundtrip\n");
}

static void
test_fetch_roundtrip(void)
{
	uint8_t buf[64];
	uint8_t *p = buf;
	kaka_fetch_t f;
	const char *topic = "log";
	uint16_t tlen = (uint16_t)strlen(topic);

	kaka_put_u16(p, tlen); p += 2;
	memcpy(p, topic, tlen); p += tlen;
	kaka_put_u32(p, 7); p += 4;           /* partition */
	kaka_put_u64(p, 0x1000); p += 8;      /* offset */
	kaka_put_u32(p, 65536); p += 4;       /* max_bytes */

	ASSERT(kaka_decode_fetch(buf, (size_t)(p - buf), &f) == 0);
	ASSERT(f.partition == 7);
	ASSERT(f.offset == 0x1000);
	ASSERT(f.max_bytes == 65536);
	ASSERT(memcmp(f.topic, "log", tlen) == 0);
	printf("  ok   fetch_roundtrip\n");
}

static void
test_response_encoders(void)
{
	uint8_t buf[128];
	uint8_t type;
	const uint8_t *body;
	size_t body_len;
	long n;

	/* PRODUCE_ACK */
	n = kaka_encode_produce_ack(buf, sizeof buf, 0xcafe);
	ASSERT(n == 13);
	ASSERT(kaka_frame_parse(buf, (size_t)n, &type, &body, &body_len) == n);
	ASSERT(type == KAKA_PRODUCE_ACK);
	ASSERT(kaka_get_u64(body) == 0xcafe);

	/* ERROR */
	n = kaka_encode_error(buf, sizeof buf, 42, "boom");
	ASSERT(n == 5 + 2 + 4);
	ASSERT(kaka_frame_parse(buf, (size_t)n, &type, &body, &body_len) == n);
	ASSERT(type == KAKA_ERROR);
	ASSERT(kaka_get_u16(body) == 42);
	ASSERT(memcmp(body + 2, "boom", 4) == 0);
	printf("  ok   response_encoders\n");
}

static void
test_malformed(void)
{
	uint8_t buf[16];
	uint8_t type;
	const uint8_t *body;
	size_t body_len;
	kaka_produce_t pr;

	/* zero-length frame is malformed (len must be >= 1 for type) */
	kaka_put_u32(buf, 0);
	ASSERT(kaka_frame_parse(buf, 4, &type, &body, &body_len) == -1);

	/* truncated PRODUCE body */
	ASSERT(kaka_decode_produce((const uint8_t *)"\x00", 1, &pr) == -1);
	printf("  ok   malformed_rejected\n");
}

int
main(void)
{
	test_int_roundtrip();
	test_frame_envelope();
	test_produce_roundtrip();
	test_fetch_roundtrip();
	test_response_encoders();
	test_malformed();
	printf("All kaka frame tests passed.\n");
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/frame.h
 *
 *	The kaka wire framing: a length-prefixed binary protocol.
 *	See PROTOCOL.md.  All integers are big-endian (network order),
 *	matching the Bitcask record framing the partition logs reuse.
 *
 *	A frame is:  u32 len | u8 type | body[len-1]
 *	(len counts the type byte plus the body.)
 *
 *	This module is pure codec: encode/decode against caller-owned
 *	buffers, no I/O, no allocation beyond optional growable buffers.
 *	It is unit-tested standalone (test_frame.c) with no broker
 *	running, so the test never needs the network or a daemon.
 */

#ifndef KAKA_FRAME_H
#define KAKA_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Request and response type bytes. */
enum {
	KAKA_PRODUCE      = 0x01,
	KAKA_FETCH        = 0x02,
	KAKA_COMMIT       = 0x03,
	KAKA_OFFSETS      = 0x04,
	KAKA_METADATA     = 0x05,

	KAKA_PRODUCE_ACK  = 0x81,
	KAKA_RECORDS      = 0x82,
	KAKA_COMMIT_ACK   = 0x83,
	KAKA_OFFSET       = 0x84,
	KAKA_METADATA_RSP = 0x85,
	KAKA_ERROR        = 0xff
};

/* A decoded record (borrowed pointers into the source buffer). */
typedef struct kaka_record {
	const uint8_t *key;
	uint32_t       key_len;
	const uint8_t *value;
	uint32_t       value_len;
} kaka_record_t;

/* A PRODUCE request body, decoded. */
typedef struct kaka_produce {
	const char    *topic;
	uint16_t       topic_len;
	uint32_t       partition;
	uint32_t       n_records;
	/* records are decoded on demand by kaka_produce_next_record */
	const uint8_t *records_cur;
	const uint8_t *records_end;
} kaka_produce_t;

/* A FETCH request body, decoded. */
typedef struct kaka_fetch {
	const char *topic;
	uint16_t    topic_len;
	uint32_t    partition;
	uint64_t    offset;
	uint32_t    max_bytes;
} kaka_fetch_t;

/* ---- big-endian primitives (exposed for the partition log) ---- */

void     kaka_put_u16(uint8_t *p, uint16_t v);
void     kaka_put_u32(uint8_t *p, uint32_t v);
void     kaka_put_u64(uint8_t *p, uint64_t v);
uint16_t kaka_get_u16(const uint8_t *p);
uint32_t kaka_get_u32(const uint8_t *p);
uint64_t kaka_get_u64(const uint8_t *p);

/* ---- frame envelope ---- */

/* Returns the total frame length (4 + 1 + body_len) for a body of
 * body_len bytes, or 0 if it would overflow. */
size_t kaka_frame_size(size_t body_len);

/* Write a frame header (length + type) at dst, which must have room
 * for 5 bytes.  body_len is the number of body bytes that follow. */
int kaka_frame_header(uint8_t *dst, size_t dst_cap,
                      uint8_t type, size_t body_len);

/* Given a buffer that may hold a partial or whole frame, determine
 * whether a complete frame is present.  On success sets *type, *body,
 * *body_len (borrowed into buf) and returns the total bytes consumed.
 * Returns 0 if more bytes are needed, -1 on a malformed/oversized
 * frame. */
long kaka_frame_parse(const uint8_t *buf, size_t len,
                      uint8_t *type, const uint8_t **body, size_t *body_len);

/* ---- request decoders (body -> struct) ---- */

int kaka_decode_produce(const uint8_t *body, size_t body_len,
                        kaka_produce_t *out);
/* Pull the next record from a decoded PRODUCE body.  Returns 1 on a
 * record, 0 when exhausted, -1 on malformed input. */
int kaka_produce_next_record(kaka_produce_t *p, kaka_record_t *rec);

int kaka_decode_fetch(const uint8_t *body, size_t body_len,
                      kaka_fetch_t *out);

/* ---- response encoders (into a caller buffer) ---- */

/* PRODUCE_ACK: the base offset assigned to the produced batch. */
long kaka_encode_produce_ack(uint8_t *dst, size_t cap, uint64_t base_offset);

/* ERROR: u16 code + message. */
long kaka_encode_error(uint8_t *dst, size_t cap,
                       uint16_t code, const char *msg);

/* RECORDS frame header: caller then appends each record via
 * kaka_encode_record.  Returns header bytes written, or -1. */
long kaka_encode_records_header(uint8_t *dst, size_t cap,
                                uint64_t base_offset, uint32_t n_records);
/* One record (offset + key + value) appended to dst. */
long kaka_encode_record(uint8_t *dst, size_t cap,
                        uint64_t offset, const kaka_record_t *rec);

#endif /* KAKA_FRAME_H */

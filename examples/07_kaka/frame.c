/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/frame.c -- kaka wire-framing codec.  See frame.h.
 */

#include "frame.h"

#include <string.h>

#define KAKA_MAX_FRAME  (64u * 1024u * 1024u)   /* 64 MiB hard cap */
#define KAKA_MAX_RECORD (16u * 1024u * 1024u)

/* ---- big-endian primitives ---- */

void kaka_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void kaka_put_u32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
void kaka_put_u64(uint8_t *p, uint64_t v)
{ kaka_put_u32(p, (uint32_t)(v >> 32)); kaka_put_u32(p + 4, (uint32_t)v); }

uint16_t kaka_get_u16(const uint8_t *p)
{ return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
uint32_t kaka_get_u32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
       | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
uint64_t kaka_get_u64(const uint8_t *p)
{ return ((uint64_t)kaka_get_u32(p) << 32) | kaka_get_u32(p + 4); }

/* ---- frame envelope ---- */

size_t
kaka_frame_size(size_t body_len)
{
	if (body_len > KAKA_MAX_FRAME - 5) return 0;
	return body_len + 5;            /* 4 len + 1 type */
}

int
kaka_frame_header(uint8_t *dst, size_t dst_cap, uint8_t type, size_t body_len)
{
	if (dst_cap < 5 || body_len + 1 > KAKA_MAX_FRAME) return -1;
	kaka_put_u32(dst, (uint32_t)(body_len + 1));    /* len = type + body */
	dst[4] = type;
	return 0;
}

long
kaka_frame_parse(const uint8_t *buf, size_t len,
                 uint8_t *type, const uint8_t **body, size_t *body_len)
{
	uint32_t flen;
	if (len < 4) return 0;                   /* need the length prefix */
	flen = kaka_get_u32(buf);
	if (flen < 1 || flen > KAKA_MAX_FRAME) return -1;
	if (len < (size_t)4 + flen) return 0;    /* incomplete */
	*type = buf[4];
	*body = buf + 5;
	*body_len = flen - 1;                    /* minus the type byte */
	return (long)(4 + flen);
}

/* ---- PRODUCE decode ----
 *
 * body: u16 topic_len | topic | u32 partition | u32 n_records |
 *       [ u32 key_len | u32 value_len | key | value ] * n_records
 */
int
kaka_decode_produce(const uint8_t *body, size_t body_len, kaka_produce_t *out)
{
	const uint8_t *p = body, *end = body + body_len;
	if (body_len < 2) return -1;
	out->topic_len = kaka_get_u16(p); p += 2;
	if ((size_t)(end - p) < (size_t)out->topic_len + 8) return -1;
	out->topic = (const char *)p; p += out->topic_len;
	out->partition = kaka_get_u32(p); p += 4;
	out->n_records = kaka_get_u32(p); p += 4;
	out->records_cur = p;
	out->records_end = end;
	return 0;
}

int
kaka_produce_next_record(kaka_produce_t *pr, kaka_record_t *rec)
{
	const uint8_t *p = pr->records_cur, *end = pr->records_end;
	uint32_t kl, vl;
	if (p >= end) return 0;
	if ((size_t)(end - p) < 8) return -1;
	kl = kaka_get_u32(p); p += 4;
	vl = kaka_get_u32(p); p += 4;
	if (kl > KAKA_MAX_RECORD || vl > KAKA_MAX_RECORD) return -1;
	if ((size_t)(end - p) < (size_t)kl + vl) return -1;
	rec->key = p; rec->key_len = kl; p += kl;
	rec->value = p; rec->value_len = vl; p += vl;
	pr->records_cur = p;
	return 1;
}

/* ---- FETCH decode ----
 *
 * body: u16 topic_len | topic | u32 partition | u64 offset | u32 max_bytes
 */
int
kaka_decode_fetch(const uint8_t *body, size_t body_len, kaka_fetch_t *out)
{
	const uint8_t *p = body, *end = body + body_len;
	if (body_len < 2) return -1;
	out->topic_len = kaka_get_u16(p); p += 2;
	if ((size_t)(end - p) < (size_t)out->topic_len + 16) return -1;
	out->topic = (const char *)p; p += out->topic_len;
	out->partition = kaka_get_u32(p); p += 4;
	out->offset = kaka_get_u64(p); p += 8;
	out->max_bytes = kaka_get_u32(p); p += 4;
	return 0;
}

/* ---- response encoders ---- */

long
kaka_encode_produce_ack(uint8_t *dst, size_t cap, uint64_t base_offset)
{
	if (cap < 5 + 8) return -1;
	kaka_frame_header(dst, cap, KAKA_PRODUCE_ACK, 8);
	kaka_put_u64(dst + 5, base_offset);
	return 5 + 8;
}

long
kaka_encode_error(uint8_t *dst, size_t cap, uint16_t code, const char *msg)
{
	size_t mlen = msg ? strlen(msg) : 0;
	size_t body = 2 + mlen;
	if (cap < 5 + body) return -1;
	kaka_frame_header(dst, cap, KAKA_ERROR, body);
	kaka_put_u16(dst + 5, code);
	if (mlen > 0) memcpy(dst + 7, msg, mlen);
	return (long)(5 + body);
}

long
kaka_encode_records_header(uint8_t *dst, size_t cap,
                           uint64_t base_offset, uint32_t n_records)
{
	/* This is a streaming header; the body length is not known yet,
	 * so we write a RECORDS type with a placeholder length the caller
	 * patches via kaka_frame_header once the total is known.  For the
	 * in-memory Phase 1 path the caller sizes the buffer first, so we
	 * emit base_offset + n_records as the fixed prefix after a header
	 * the caller will rewrite.  Returns the prefix length. */
	if (cap < 5 + 12) return -1;
	dst[4] = KAKA_RECORDS;          /* type; length patched later */
	kaka_put_u64(dst + 5, base_offset);
	kaka_put_u32(dst + 13, n_records);
	return 5 + 12;
}

long
kaka_encode_record(uint8_t *dst, size_t cap, uint64_t offset,
                   const kaka_record_t *rec)
{
	size_t need = 8 + 4 + 4 + rec->key_len + rec->value_len;
	uint8_t *p = dst;
	if (cap < need) return -1;
	kaka_put_u64(p, offset); p += 8;
	kaka_put_u32(p, rec->key_len); p += 4;
	kaka_put_u32(p, rec->value_len); p += 4;
	if (rec->key_len) { memcpy(p, rec->key, rec->key_len); p += rec->key_len; }
	if (rec->value_len) { memcpy(p, rec->value, rec->value_len); p += rec->value_len; }
	return (long)need;
}

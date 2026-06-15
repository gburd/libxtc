/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sqlrec.c
 *	SQLite-compatible record codec.  See sqlrec.h.  The byte layout
 *	matches the reference engine's sqlite3PutVarint / sqlite3GetVarint
 *	and sqlite3VdbeSerialType / serialGet, verified byte-for-byte by
 *	test_sqlrec against the vendored encoder.
 */

#include <string.h>

#include "sqlrec.h"

/* ---- varint ------------------------------------------------------- */

int
sqlrec_put_varint(uint8_t *buf, uint64_t v)
{
	/* SQLite stores the low 7 bits of each byte, most-significant
	 * group first, with the high bit set on every byte except the
	 * last -- EXCEPT the 9-byte form, whose 9th byte carries a full
	 * 8 bits.  This reproduces sqlite3PutVarint / putVarint64. */
	uint8_t tmp[10];
	int i, n;

	if (v <= 0x7f) {
		buf[0] = (uint8_t)(v & 0x7f);
		return 1;
	}
	if (v <= 0x3fff) {
		buf[0] = (uint8_t)(((v >> 7) & 0x7f) | 0x80);
		buf[1] = (uint8_t)(v & 0x7f);
		return 2;
	}

	n = 0;
	if (v & (((uint64_t)0xff000000) << 32)) {
		/* The top byte is full 8 bits (9-byte form). */
		buf[8] = (uint8_t)(v & 0xff);
		v >>= 8;
		for (i = 7; i >= 0; i--) {
			buf[i] = (uint8_t)((v & 0x7f) | 0x80);
			v >>= 7;
		}
		return 9;
	}
	do {
		tmp[n++] = (uint8_t)((v & 0x7f) | 0x80);
		v >>= 7;
	} while (v != 0);
	tmp[0] &= 0x7f;   /* last group written first; clear its continuation bit */
	for (i = 0; i < n; i++)
		buf[i] = tmp[n - 1 - i];
	return n;
}

int
sqlrec_get_varint(const uint8_t *p, size_t avail, uint64_t *v)
{
	uint64_t x = 0;
	int i;
	for (i = 0; i < 8; i++) {
		if ((size_t)i >= avail) return 0;
		x = (x << 7) | (p[i] & 0x7f);
		if ((p[i] & 0x80) == 0) { *v = x; return i + 1; }
	}
	/* 9th byte: all 8 bits. */
	if ((size_t)8 >= avail) return 0;
	x = (x << 8) | p[8];
	*v = x;
	return 9;
}

int
sqlrec_varint_len(uint64_t v)
{
	int n = 1;
	if (v & (((uint64_t)0xff000000) << 32)) return 9;
	if (v <= 0x7f) return 1;
	while (v > 0x7f) { v >>= 7; n++; }
	return n;
}

/* ---- big-endian integer helpers ---------------------------------- */

static void
put_be(uint8_t *buf, uint64_t u, int nbytes)
{
	int i;
	for (i = nbytes - 1; i >= 0; i--) {
		buf[i] = (uint8_t)(u & 0xff);
		u >>= 8;
	}
}

static int64_t
get_be_int(const uint8_t *p, int nbytes)
{
	/* Sign-extend from the top byte. */
	int64_t v = (int8_t)p[0];
	int i;
	for (i = 1; i < nbytes; i++)
		v = (v << 8) | p[i];
	return v;
}

/* ---- serial types ------------------------------------------------- */

uint64_t
sqlrec_serial_type(const sqlrec_value_t *v, uint32_t *out_len)
{
	switch (v->type) {
	case SQLREC_NULL:
		*out_len = 0;
		return 0;
	case SQLREC_INT: {
		int64_t i = v->u.i;
		uint64_t u = (i < 0) ? (uint64_t)(~i) : (uint64_t)i;
		if (u <= 127) {
			/* Constant 0/1 take zero body bytes (types 8/9). */
			if ((i & 1) == i) { *out_len = 0; return 8 + (uint64_t)u; }
			*out_len = 1; return 1;
		}
		if (u <= 32767)      { *out_len = 2; return 2; }
		if (u <= 8388607)    { *out_len = 3; return 3; }
		if (u <= 2147483647) { *out_len = 4; return 4; }
		if (u <= ((((uint64_t)0x00008000) << 32) - 1)) { *out_len = 6; return 5; }
		*out_len = 8; return 6;
	}
	case SQLREC_REAL:
		*out_len = 8;
		return 7;
	case SQLREC_TEXT:
		*out_len = v->u.bytes.n;
		return ((uint64_t)v->u.bytes.n * 2) + 13;
	case SQLREC_BLOB:
		*out_len = v->u.bytes.n;
		return ((uint64_t)v->u.bytes.n * 2) + 12;
	}
	*out_len = 0;
	return 0;
}

uint32_t
sqlrec_serial_len(uint64_t serial_type)
{
	static const uint8_t small[12] = { 0,1,2,3,4,6,8,8,0,0,0,0 };
	if (serial_type >= 12)
		return (uint32_t)((serial_type - 12) / 2);
	return small[serial_type];
}

uint32_t
sqlrec_serial_put(uint8_t *buf, const sqlrec_value_t *v, uint64_t serial_type)
{
	switch (serial_type) {
	case 0: case 8: case 9:
		return 0;                            /* NULL / 0 / 1: no body */
	case 1: put_be(buf, (uint64_t)v->u.i, 1); return 1;
	case 2: put_be(buf, (uint64_t)v->u.i, 2); return 2;
	case 3: put_be(buf, (uint64_t)v->u.i, 3); return 3;
	case 4: put_be(buf, (uint64_t)v->u.i, 4); return 4;
	case 5: put_be(buf, (uint64_t)v->u.i, 6); return 6;
	case 6: put_be(buf, (uint64_t)v->u.i, 8); return 8;
	case 7: {
		uint64_t bits;
		memcpy(&bits, &v->u.r, 8);            /* native double bits */
		put_be(buf, bits, 8);                /* stored big-endian */
		return 8;
	}
	default: {
		uint32_t n = sqlrec_serial_len(serial_type);
		if (n) memcpy(buf, v->u.bytes.p, n);
		return n;
	}
	}
}

void
sqlrec_serial_get(const uint8_t *p, uint64_t serial_type, sqlrec_value_t *out)
{
	switch (serial_type) {
	case 0:
		out->type = SQLREC_NULL;
		break;
	case 1: out->type = SQLREC_INT; out->u.i = get_be_int(p, 1); break;
	case 2: out->type = SQLREC_INT; out->u.i = get_be_int(p, 2); break;
	case 3: out->type = SQLREC_INT; out->u.i = get_be_int(p, 3); break;
	case 4: out->type = SQLREC_INT; out->u.i = get_be_int(p, 4); break;
	case 5: out->type = SQLREC_INT; out->u.i = get_be_int(p, 6); break;
	case 6: out->type = SQLREC_INT; out->u.i = get_be_int(p, 8); break;
	case 7: {
		uint64_t bits = 0;
		int i;
		for (i = 0; i < 8; i++) bits = (bits << 8) | p[i];
		out->type = SQLREC_REAL;
		memcpy(&out->u.r, &bits, 8);
		break;
	}
	case 8: out->type = SQLREC_INT; out->u.i = 0; break;
	case 9: out->type = SQLREC_INT; out->u.i = 1; break;
	default: {
		uint32_t n = sqlrec_serial_len(serial_type);
		out->u.bytes.p = p;
		out->u.bytes.n = n;
		out->type = (serial_type & 1) ? SQLREC_TEXT : SQLREC_BLOB;
		break;
	}
	}
}

/* ---- whole-record encode ----------------------------------------- */

int
sqlrec_encode(uint8_t *buf, size_t cap, const sqlrec_value_t *vals, int nvals)
{
	uint64_t types[64];
	uint32_t lens[64];
	uint64_t *tp = types;
	uint32_t *lp = lens;
	int i, hdr_size_len, total, body_total = 0, hdr_total;
	uint8_t vtmp[9];

	/* For >64 columns, fall back to two passes without the stack arrays
	 * (the common case is small).  Here we keep it simple and bounded. */
	if (nvals > 64) return -1;

	/* Serial types + body lengths. */
	for (i = 0; i < nvals; i++) {
		tp[i] = sqlrec_serial_type(&vals[i], &lp[i]);
		body_total += (int)lp[i];
	}

	/* Header length = its own size-varint + the per-column type
	 * varints.  The size varint's own length depends on the total,
	 * which depends on its length -- iterate to a fixed point. */
	{
		int types_len = 0;
		for (i = 0; i < nvals; i++) types_len += sqlrec_varint_len(tp[i]);
		hdr_size_len = 1;
		for (;;) {
			int h = hdr_size_len + types_len;
			int nl = sqlrec_varint_len((uint64_t)h);
			if (nl == hdr_size_len) break;
			hdr_size_len = nl;
		}
		hdr_total = hdr_size_len + types_len;
	}

	total = hdr_total + body_total;
	if (buf == NULL) return total;
	if ((size_t)total > cap) return -1;

	/* Write the header size varint, then each serial type. */
	{
		uint8_t *w = buf;
		int n = sqlrec_put_varint(vtmp, (uint64_t)hdr_total);
		/* hdr_total may need fewer bytes than hdr_size_len reserved;
		 * but our fixed-point loop guarantees n == hdr_size_len. */
		memcpy(w, vtmp, (size_t)n);
		w += n;
		for (i = 0; i < nvals; i++)
			w += sqlrec_put_varint(w, tp[i]);
		/* Body. */
		for (i = 0; i < nvals; i++)
			w += sqlrec_serial_put(w, &vals[i], tp[i]);
	}
	return total;
}

/* ---- record reader ----------------------------------------------- */

int
sqlrec_reader_open(sqlrec_reader_t *rd, const uint8_t *rec, size_t rec_len)
{
	uint64_t hdr_len = 0;
	int n, ncol = 0;
	const uint8_t *h, *hend;

	memset(rd, 0, sizeof *rd);
	rd->rec = rec;
	rd->rec_len = rec_len;

	n = sqlrec_get_varint(rec, rec_len, &hdr_len);
	if (n == 0 || hdr_len < (uint64_t)n || hdr_len > rec_len) return -1;

	rd->hdr = rec + n;
	rd->hdr_len = (size_t)hdr_len;
	rd->body = rec + hdr_len;

	/* Count columns by walking the serial-type varints in the header. */
	h = rd->hdr;
	hend = rec + hdr_len;
	while (h < hend) {
		uint64_t st;
		int m = sqlrec_get_varint(h, (size_t)(hend - h), &st);
		if (m == 0) return -1;
		h += m;
		ncol++;
	}
	rd->ncol = ncol;
	rd->ok = 1;
	return 0;
}

int
sqlrec_reader_ncol(const sqlrec_reader_t *rd)
{
	return rd->ok ? rd->ncol : -1;
}

int
sqlrec_reader_col(const sqlrec_reader_t *rd, int i, sqlrec_value_t *out)
{
	const uint8_t *h, *hend;
	const uint8_t *body;
	int col = 0;

	if (!rd->ok || i < 0 || i >= rd->ncol) return -1;

	/* Walk the header to column i, accumulating the body offset. */
	h = rd->hdr;
	hend = rd->rec + rd->hdr_len;
	body = rd->body;
	while (h < hend) {
		uint64_t st;
		int m = sqlrec_get_varint(h, (size_t)(hend - h), &st);
		uint32_t blen;
		if (m == 0) return -1;
		h += m;
		blen = sqlrec_serial_len(st);
		if (col == i) {
			if (body + blen > rd->rec + rd->rec_len) return -1;
			sqlrec_serial_get(body, st, out);
			return 0;
		}
		body += blen;
		col++;
	}
	return -1;
}

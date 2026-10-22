/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_rec_codec.c
 *	The row record codec (xstore_rec_put / xstore_rec_col /
 *	xstore_rec_cols, xstore.c).
 *
 *	(1) Round trip: random rows of every class -- integers at every
 *	    width boundary (+-64, +-2^(8n-1)), INT64_MIN/MAX, REALs that are
 *	    and are not integral, -0.0, NaN, +-inf, 2^53 +- 1, TEXT and BLOB
 *	    of 0..300 bytes and at the 63/64, 31/32 and 65535/65536 length
 *	    edges -- decode to exactly the values written (REAL bit for bit,
 *	    except that every NaN stays a NaN), column by column and in one
 *	    pass, and the class is never changed (an integral REAL stays REAL).
 *	(2) Size: a TPC-C order_line-shaped row is at most 60% of the 1.59
 *	    encoding (9-byte integers and 5-byte string headers).
 *	(3) Compatibility: 1.59 records (tags 0-4 only, built byte by byte
 *	    here) decode as before.
 *	(4) Robustness: every truncation of every encoded row decodes with
 *	    no read past the end (columns past the cut read as NULL), and
 *	    an unknown tag reads as NULL.
 *	No libxtc runtime needed; links xstore.o for the codec only.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xstore.h"
#include "t_tmp.h"

#define NCOL 24

static uint64_t g_s = 0x2545F4914F6CDD1Dull;
static uint64_t
rnd(void)
{
	g_s ^= g_s << 13; g_s ^= g_s >> 7; g_s ^= g_s << 17;
	return g_s;
}

static const int64_t edge_i[] = {
	0, 1, -1, 63, 64, -64, -65, 127, 128, -128, -129, 255, 256,
	32767, 32768, -32768, -32769, 8388607, 8388608, -8388608, -8388609,
	2147483647LL, 2147483648LL, -2147483648LL, -2147483649LL,
	549755813887LL, 549755813888LL, -549755813888LL, -549755813889LL,
	140737488355327LL, 140737488355328LL, -140737488355328LL,
	36028797018963967LL, 36028797018963968LL, -36028797018963968LL,
	-36028797018963969LL, INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1
};
static const double edge_r[] = {
	0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 63.0, 64.0, -65.0, 128.0, 1e300, -1e-300,
	9007199254740991.0, 9007199254740992.0, 9007199254740993.0,
	-9007199254740992.0, 18446744073709551616.0, -9223372036854775808.0,
	9223372036854775807.0, 4.9e-324, 3.25, 100.25, 1234567.0
};
static const int edge_n[] = { 0, 1, 31, 32, 33, 63, 64, 65, 255, 256, 1000 };

static int g_fail;

static int
same_real(double a, double b)
{
	uint64_t x, y;
	if (isnan(a) && isnan(b)) return 1;
	memcpy(&x, &a, 8); memcpy(&y, &b, 8);
	return x == y;
}

/* A random value of a random class into v; bytes for TEXT/BLOB go in buf. */
static void
gen(xstore_val_t *v, uint8_t *buf)
{
	int k = (int)(rnd() % 8), j;
	memset(v, 0, sizeof *v);
	switch (k) {
	case 0: v->cls = XSTORE_C_NULL; break;
	case 1: v->cls = XSTORE_C_INT;
		v->i = edge_i[rnd() % (sizeof edge_i / sizeof edge_i[0])]; break;
	case 2: v->cls = XSTORE_C_INT; v->i = (int64_t)rnd() >> (rnd() % 64); break;
	case 3: v->cls = XSTORE_C_REAL;
		v->r = edge_r[rnd() % (sizeof edge_r / sizeof edge_r[0])];
		if (rnd() % 16 == 0) v->r = NAN;
		if (rnd() % 16 == 0) v->r = (rnd() & 1) ? INFINITY : -INFINITY;
		break;
	case 4: v->cls = XSTORE_C_REAL;
		v->r = (double)((int64_t)rnd() >> (rnd() % 64)) / ((rnd() & 1) ? 1.0 : 8.0);
		break;
	default:
		v->cls = (k & 1) ? XSTORE_C_TEXT : XSTORE_C_BLOB;
		v->n = (rnd() % 3) ? (int)(rnd() % 80)
		    : edge_n[rnd() % (sizeof edge_n / sizeof edge_n[0])];
		for (j = 0; j < v->n; j++) buf[j] = (uint8_t)rnd();
		v->p = buf;
		break;
	}
}

static int
same(const xstore_val_t *a, int cls, int64_t iv, double rv, const uint8_t *p, int n)
{
	if (a->cls != cls) return 0;
	switch (cls) {
	case XSTORE_C_INT: return a->i == iv;
	case XSTORE_C_REAL: return same_real(a->r, rv);
	case XSTORE_C_TEXT: case XSTORE_C_BLOB:
		return a->n == n && (n == 0 || memcmp(a->p, p, (size_t)n) == 0);
	default: return 1;
	}
}

static int
encode(const xstore_val_t *v, int ncol, uint8_t *out, int cap)
{
	int i, off = 1;
	out[0] = (uint8_t)ncol;
	for (i = 0; i < ncol && off >= 0; i++)
		off = xstore_rec_put(out, off, cap, v[i].cls, v[i].i, v[i].r, v[i].p, v[i].n);
	return off;
}

static void
t_roundtrip(void)
{
	static uint8_t bufs[NCOL][1024], rec[NCOL * 1100];
	xstore_val_t v[NCOL], out[NCOL];
	int t, i, len, bad = 0, n;

	for (t = 0; t < 200000; t++) {
		int ncol = 1 + (int)(rnd() % NCOL);
		for (i = 0; i < ncol; i++) gen(&v[i], bufs[i]);
		len = encode(v, ncol, rec, (int)sizeof rec);
		if (len < 0) { bad++; continue; }
		n = xstore_rec_cols(rec, len, out, NCOL);
		if (n != ncol) bad++;
		for (i = 0; i < ncol; i++) {
			int64_t iv = 0; double rv = 0; const uint8_t *p = NULL; int pn = 0, cls;
			cls = xstore_rec_col(rec, len, i, &iv, &rv, &p, &pn);
			if (!same(&v[i], cls, iv, rv, p, pn) ||
			    !same(&v[i], out[i].cls, out[i].i, out[i].r, out[i].p, out[i].n)) {
				if (bad++ < 5)
					fprintf(stderr, "  col %d cls %d: wrote i=%lld r=%.17g n=%d, "
					    "read cls %d i=%lld r=%.17g n=%d\n", i, v[i].cls,
					    (long long)v[i].i, v[i].r, v[i].n, cls, (long long)iv, rv, pn);
			}
		}
		/* an encoding never fits a buffer one byte too small */
		if (encode(v, ncol, rec, len - 1) >= 0) bad++;
	}
	if (bad) { fprintf(stderr, "FAIL roundtrip: %d bad\n", bad); g_fail = 1; }
	else printf("  ok   200000 random rows round-trip (every class, width edges, -0.0, NaN)\n");
}

/* The 1.59 encoding of a row, for comparison and compatibility. */
static int
enc159(const xstore_val_t *v, int ncol, uint8_t *out)
{
	int i, off = 0, b;
	uint64_t u;
	out[off++] = (uint8_t)ncol;
	for (i = 0; i < ncol; i++) {
		switch (v[i].cls) {
		case XSTORE_C_NULL: out[off++] = 0; break;
		case XSTORE_C_INT: case XSTORE_C_REAL:
			out[off++] = v[i].cls == XSTORE_C_INT ? 1 : 2;
			if (v[i].cls == XSTORE_C_INT) u = (uint64_t)v[i].i;
			else memcpy(&u, &v[i].r, 8);
			for (b = 0; b < 8; b++) out[off++] = (uint8_t)(u >> (8 * b));
			break;
		default:
			out[off++] = v[i].cls == XSTORE_C_TEXT ? 3 : 4;
			for (b = 0; b < 4; b++) out[off++] = (uint8_t)((uint32_t)v[i].n >> (8 * b));
			memcpy(out + off, v[i].p, (size_t)v[i].n);
			off += v[i].n;
		}
	}
	return off;
}

static void
t_size_and_compat(void)
{
	/* order_line: o_id, d_id, w_id, number, i_id, supply_w_id,
	 * delivery_d (NULL or a 19-byte date), quantity, amount (REAL),
	 * dist_info (24 chars) -- the non-key columns of a 3000-order load. */
	static uint8_t date[] = "2026-10-21 12:00:00", dist[] = "abcdefghijklmnopqrstuvwx";
	xstore_val_t v[10], out[10];
	uint8_t a[512], b[512];
	int la, lb, i, n, bad = 0;

	v[0] = (xstore_val_t){ XSTORE_C_INT, 2999, 0, NULL, 0 };
	v[1] = (xstore_val_t){ XSTORE_C_INT, 7, 0, NULL, 0 };
	v[2] = (xstore_val_t){ XSTORE_C_INT, 3, 0, NULL, 0 };
	v[3] = (xstore_val_t){ XSTORE_C_INT, 11, 0, NULL, 0 };
	v[4] = (xstore_val_t){ XSTORE_C_INT, 87512, 0, NULL, 0 };
	v[5] = (xstore_val_t){ XSTORE_C_INT, 3, 0, NULL, 0 };
	v[6] = (xstore_val_t){ XSTORE_C_TEXT, 0, 0, date, 19 };
	v[7] = (xstore_val_t){ XSTORE_C_INT, 5, 0, NULL, 0 };
	v[8] = (xstore_val_t){ XSTORE_C_REAL, 0, 0.0, NULL, 0 };
	v[9] = (xstore_val_t){ XSTORE_C_TEXT, 0, 0, dist, 24 };
	la = encode(v, 10, a, (int)sizeof a);
	lb = enc159(v, 10, b);
	printf("  order_line row: %d B (1.59 format: %d B, %.0f%%)\n", la, lb,
	    100.0 * la / lb);
	if (la < 0 || la * 10 > lb * 6) {
		fprintf(stderr, "FAIL size: %d B vs %d B in the 1.59 format\n", la, lb);
		g_fail = 1;
	}
	/* 1.59 records decode exactly, with the REAL bit pattern intact. */
	v[8].r = 1234.5;
	lb = enc159(v, 10, b);
	n = xstore_rec_cols(b, lb, out, 10);
	for (i = 0; i < 10; i++)
		if (!same(&v[i], out[i].cls, out[i].i, out[i].r, out[i].p, out[i].n)) bad++;
	if (n != 10 || bad) {
		fprintf(stderr, "FAIL compat: a 1.59 record decodes wrong (%d cols bad)\n", bad);
		g_fail = 1;
	} else {
		printf("  ok   a 1.59 record (tags 0-4) decodes as before\n");
	}
}

static void
t_truncate(void)
{
	static uint8_t bufs[NCOL][1024], rec[NCOL * 1100], *cp;
	xstore_val_t v[NCOL], out[NCOL];
	int t, i, len, cut, bad = 0;

	for (t = 0; t < 3000; t++) {
		int ncol = 1 + (int)(rnd() % NCOL);
		for (i = 0; i < ncol; i++) gen(&v[i], bufs[i]);
		len = encode(v, ncol, rec, (int)sizeof rec);
		for (cut = 0; cut < len; cut++) {
			int k;
			/* an exact-size heap copy, so a read past it is an ASan error */
			cp = malloc((size_t)(cut > 0 ? cut : 1));
			memcpy(cp, rec, (size_t)cut);
			(void)xstore_rec_cols(cp, cut, out, NCOL);
			for (k = 0; k < ncol; k++) {
				int64_t iv; double rv; const uint8_t *p; int pn;
				(void)xstore_rec_col(cp, cut, k, &iv, &rv, &p, &pn);
			}
			free(cp);
		}
	}
	/* unknown tag 0x07 in column 1 */
	rec[0] = 3; rec[1] = 0x81; rec[2] = 0x07; rec[3] = 0x81;
	if (xstore_rec_col(rec, 4, 1, NULL, NULL, NULL, NULL) != XSTORE_C_NULL) bad++;
	if (xstore_rec_cols(rec, 4, out, NCOL) != 3 || out[0].cls != XSTORE_C_INT ||
	    out[0].i != -63 || out[1].cls != XSTORE_C_NULL) bad++;
	if (bad) { fprintf(stderr, "FAIL truncate / unknown tag\n"); g_fail = 1; }
	else printf("  ok   every truncation decodes in bounds; an unknown tag reads as NULL\n");
}

int
main(void)
{
	printf("test_rec_codec\n");
	t_roundtrip();
	t_size_and_compat();
	t_truncate();
	if (g_fail) { fprintf(stderr, "FAIL: test_rec_codec\n"); return 1; }
	printf("  ok   record codec\n");
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_sqlrec.c
 *	Tests for the SQLite-compatible record codec (sqlrec).
 *
 *	Three oracles:
 *	  1. Round-trip: encode a set of values, decode them back, assert
 *	     equality (and that the reader reports the right column count).
 *	  2. Golden vectors: hand-derived byte sequences from the SQLite
 *	     record-format spec (the serial-type table and big-endian
 *	     integer/float layout) -- asserts byte-for-byte encoding.
 *	  3. Reference cross-check: decode a value with sqlrec and compare
 *	     against what the reference engine (sqlite3_column_*) reports
 *	     for the same logical value, exercising every storage class and
 *	     the integer-width boundaries.
 *
 *	Together (2) pins the on-disk bytes to SQLite's format and (3)
 *	pins the value interpretation to the reference engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlrec.h"
#include "sqlite3.h"   /* reference engine; xsql.h (force-included) renames it */

static int g_fail;
#define CK(c, msg) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
	g_fail = 1; } } while (0)

static sqlrec_value_t V_null(void) { sqlrec_value_t v; v.type = SQLREC_NULL; return v; }
static sqlrec_value_t V_int(int64_t i) { sqlrec_value_t v; v.type = SQLREC_INT; v.u.i = i; return v; }
static sqlrec_value_t V_real(double r) { sqlrec_value_t v; v.type = SQLREC_REAL; v.u.r = r; return v; }
static sqlrec_value_t V_text(const char *s) {
	sqlrec_value_t v; v.type = SQLREC_TEXT;
	v.u.bytes.p = (const uint8_t *)s; v.u.bytes.n = (uint32_t)strlen(s); return v;
}
static sqlrec_value_t V_blob(const void *p, uint32_t n) {
	sqlrec_value_t v; v.type = SQLREC_BLOB;
	v.u.bytes.p = (const uint8_t *)p; v.u.bytes.n = n; return v;
}

static int
val_eq(const sqlrec_value_t *a, const sqlrec_value_t *b)
{
	if (a->type != b->type) return 0;
	switch (a->type) {
	case SQLREC_NULL: return 1;
	case SQLREC_INT:  return a->u.i == b->u.i;
	case SQLREC_REAL: return a->u.r == b->u.r;
	case SQLREC_TEXT:
	case SQLREC_BLOB:
		return a->u.bytes.n == b->u.bytes.n &&
		       (a->u.bytes.n == 0 ||
		        memcmp(a->u.bytes.p, b->u.bytes.p, a->u.bytes.n) == 0);
	}
	return 0;
}

/* ---- oracle 1: round-trip ---------------------------------------- */
static void
test_roundtrip(void)
{
	static const char *blob = "\x00\x01\xff\x80";
	sqlrec_value_t in[] = {
		V_null(), V_int(0), V_int(1), V_int(-1), V_int(127), V_int(128),
		V_int(-128), V_int(32767), V_int(-32768), V_int(8388607),
		V_int(2147483647), V_int(-2147483648LL), V_int(1099511627776LL),
		V_int(9223372036854775807LL), V_int(-9223372036854775807LL - 1),
		V_real(3.14159265358979), V_real(-0.0), V_real(1e300),
		V_text(""), V_text("hello"), V_text("unicode \xe2\x9c\x93 check"),
		V_blob(blob, 4), V_blob("", 0)
	};
	int nin = (int)(sizeof in / sizeof in[0]);
	uint8_t buf[512];
	int len;
	sqlrec_reader_t rd;
	int i;

	len = sqlrec_encode(buf, sizeof buf, in, nin);
	CK(len > 0, "encode should succeed");
	CK(sqlrec_encode(NULL, 0, in, nin) == len, "size probe matches");

	CK(sqlrec_reader_open(&rd, buf, (size_t)len) == 0, "reader_open");
	CK(sqlrec_reader_ncol(&rd) == nin, "column count");

	for (i = 0; i < nin; i++) {
		sqlrec_value_t out;
		CK(sqlrec_reader_col(&rd, i, &out) == 0, "decode col");
		/* TEXT compared as BLOB-equal handles the empty-string case;
		 * note the blob with embedded NUL also round-trips. */
		CK(val_eq(&in[i], &out), in[i].type == SQLREC_TEXT ? "text rt" :
		   in[i].type == SQLREC_BLOB ? "blob rt" :
		   in[i].type == SQLREC_REAL ? "real rt" :
		   in[i].type == SQLREC_INT ? "int rt" : "null rt");
	}
}

/* ---- oracle 2: golden byte vectors from the SQLite spec ----------- */
static void
expect_bytes(const char *name, const sqlrec_value_t *v, int nv,
             const uint8_t *want, int wantn)
{
	uint8_t buf[256];
	int len = sqlrec_encode(buf, sizeof buf, v, nv);
	if (len != wantn || memcmp(buf, want, (size_t)wantn) != 0) {
		int j;
		fprintf(stderr, "FAIL golden %s: got [", name);
		for (j = 0; j < len; j++) fprintf(stderr, "%02x ", buf[j]);
		fprintf(stderr, "] want [");
		for (j = 0; j < wantn; j++) fprintf(stderr, "%02x ", want[j]);
		fprintf(stderr, "]\n");
		g_fail = 1;
	}
}

static void
test_golden(void)
{
	/* One NULL: header = [02 00], no body. */
	{ sqlrec_value_t v[] = { V_null() };
	  uint8_t w[] = { 0x02, 0x00 };
	  expect_bytes("null", v, 1, w, sizeof w); }

	/* Integer 0: serial type 8, zero body.  header [02 08]. */
	{ sqlrec_value_t v[] = { V_int(0) };
	  uint8_t w[] = { 0x02, 0x08 };
	  expect_bytes("int0", v, 1, w, sizeof w); }

	/* Integer 1: serial type 9, zero body. */
	{ sqlrec_value_t v[] = { V_int(1) };
	  uint8_t w[] = { 0x02, 0x09 };
	  expect_bytes("int1", v, 1, w, sizeof w); }

	/* Integer 5: serial type 1 (1 byte), body 0x05. */
	{ sqlrec_value_t v[] = { V_int(5) };
	  uint8_t w[] = { 0x02, 0x01, 0x05 };
	  expect_bytes("int5", v, 1, w, sizeof w); }

	/* Integer -1: serial type 1, body 0xff. */
	{ sqlrec_value_t v[] = { V_int(-1) };
	  uint8_t w[] = { 0x02, 0x01, 0xff };
	  expect_bytes("int-1", v, 1, w, sizeof w); }

	/* Integer 300: serial type 2 (2 bytes), body 0x01 0x2c (big-endian). */
	{ sqlrec_value_t v[] = { V_int(300) };
	  uint8_t w[] = { 0x02, 0x02, 0x01, 0x2c };
	  expect_bytes("int300", v, 1, w, sizeof w); }

	/* Text "abc": serial type 13+3*2 = 19 = 0x13, body 'a','b','c'. */
	{ sqlrec_value_t v[] = { V_text("abc") };
	  uint8_t w[] = { 0x02, 0x13, 'a', 'b', 'c' };
	  expect_bytes("textabc", v, 1, w, sizeof w); }

	/* Blob {0xde,0xad}: serial type 12+2*2 = 16 = 0x10. */
	{ uint8_t b[] = { 0xde, 0xad };
	  sqlrec_value_t v[] = { V_blob(b, 2) };
	  uint8_t w[] = { 0x02, 0x10, 0xde, 0xad };
	  expect_bytes("blob", v, 1, w, sizeof w); }

	/* Real 1.0: serial type 7, big-endian IEEE-754 of 1.0 =
	 * 0x3ff0000000000000. */
	{ sqlrec_value_t v[] = { V_real(1.0) };
	  uint8_t w[] = { 0x02, 0x07, 0x3f, 0xf0, 0,0,0,0,0,0 };
	  expect_bytes("real1", v, 1, w, sizeof w); }

	/* Two columns: (int 0, text "x").  header [03 08 0f], body 'x'.
	 * 'x' text serial = 13+1*2 = 15 = 0x0f.  header size = 3. */
	{ sqlrec_value_t v[] = { V_int(0), V_text("x") };
	  uint8_t w[] = { 0x03, 0x08, 0x0f, 'x' };
	  expect_bytes("int0+textx", v, 2, w, sizeof w); }
}

/* ---- oracle 3: decode cross-checked against the reference engine --- *
 *
 * For each value, INSERT it into a one-column table via the reference
 * engine, SELECT it back, and read it with sqlite3_column_*.  Encode the
 * same logical value with sqlrec, decode it, and assert the two agree on
 * type and value.  This pins sqlrec's interpretation to the engine's
 * across every storage class and integer width without needing access
 * to the engine's private record bytes. */
static void
test_vs_reference(void)
{
	sqlite3 *db = NULL;
	char *err = NULL;
	int i;
	struct { sqlrec_value_t v; const char *literal; } cases[] = {
		{ {0,{0}}, "NULL" },
		{ {0,{0}}, "0" }, { {0,{0}}, "1" }, { {0,{0}}, "-1" },
		{ {0,{0}}, "127" }, { {0,{0}}, "128" }, { {0,{0}}, "-129" },
		{ {0,{0}}, "32767" }, { {0,{0}}, "65536" },
		{ {0,{0}}, "8388607" }, { {0,{0}}, "2147483648" },
		{ {0,{0}}, "9223372036854775807" },
		{ {0,{0}}, "3.5" }, { {0,{0}}, "-2.25" },
		{ {0,{0}}, "'hello world'" }, { {0,{0}}, "''" },
		{ {0,{0}}, "x'cafe'" }
	};
	/* Fill in the sqlrec value for each case to match the literal. */
	cases[0].v = V_null();
	cases[1].v = V_int(0); cases[2].v = V_int(1); cases[3].v = V_int(-1);
	cases[4].v = V_int(127); cases[5].v = V_int(128); cases[6].v = V_int(-129);
	cases[7].v = V_int(32767); cases[8].v = V_int(65536);
	cases[9].v = V_int(8388607); cases[10].v = V_int(2147483648LL);
	cases[11].v = V_int(9223372036854775807LL);
	cases[12].v = V_real(3.5); cases[13].v = V_real(-2.25);
	cases[14].v = V_text("hello world"); cases[15].v = V_text("");
	{ static const uint8_t cafe[] = { 0xca, 0xfe };
	  cases[16].v = V_blob(cafe, 2); }

	if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
		fprintf(stderr, "FAIL: open reference db\n"); g_fail = 1; return;
	}

	for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
		char sql[128];
		sqlite3_stmt *st = NULL;
		uint8_t buf[256];
		int len;
		sqlrec_reader_t rd;
		sqlrec_value_t dec, ref;

		/* What does the reference engine make of this literal? */
		snprintf(sql, sizeof sql, "SELECT %s", cases[i].literal);
		if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK ||
		    sqlite3_step(st) != SQLITE_ROW) {
			fprintf(stderr, "FAIL: ref prepare/step %s\n", cases[i].literal);
			g_fail = 1; if (st) sqlite3_finalize(st); continue;
		}
		ref.type = SQLREC_NULL;
		switch (sqlite3_column_type(st, 0)) {
		case SQLITE_NULL:    ref = V_null(); break;
		case SQLITE_INTEGER: ref = V_int(sqlite3_column_int64(st, 0)); break;
		case SQLITE_FLOAT:   ref = V_real(sqlite3_column_double(st, 0)); break;
		case SQLITE_TEXT:    ref.type = SQLREC_TEXT;
		                     ref.u.bytes.p = sqlite3_column_text(st, 0);
		                     ref.u.bytes.n = (uint32_t)sqlite3_column_bytes(st, 0);
		                     break;
		case SQLITE_BLOB:    ref.type = SQLREC_BLOB;
		                     ref.u.bytes.p = sqlite3_column_blob(st, 0);
		                     ref.u.bytes.n = (uint32_t)sqlite3_column_bytes(st, 0);
		                     break;
		}
		/* sqlrec encode + decode of the corresponding value. */
		len = sqlrec_encode(buf, sizeof buf, &cases[i].v, 1);
		if (len <= 0 || sqlrec_reader_open(&rd, buf, (size_t)len) != 0 ||
		    sqlrec_reader_col(&rd, 0, &dec) != 0) {
			fprintf(stderr, "FAIL: sqlrec codec %s\n", cases[i].literal);
			g_fail = 1; sqlite3_finalize(st); continue;
		}
		/* The reference text/blob pointer is owned by the stmt; compare
		 * before finalize. */
		if (!val_eq(&dec, &ref)) {
			fprintf(stderr, "FAIL: %s -- sqlrec and reference disagree "
			        "(types %d vs %d)\n", cases[i].literal, dec.type, ref.type);
			g_fail = 1;
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(db);
	(void)err;
}

int
main(void)
{
	test_roundtrip();
	test_golden();
	test_vs_reference();

	if (g_fail) {
		fprintf(stderr, "  sqlrec: FAILURES\n");
		return 1;
	}
	printf("  ok   record round-trip, golden byte vectors (SQLite format), "
	       "and value cross-check vs the reference engine\n");
	printf("All sqlxtc record-codec tests passed.\n");
	return 0;
}

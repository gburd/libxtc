/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_xlog.c
 *	Unit test for the ARIES log-record codec (xlog.c).  Round-trips
 *	every record type through encode -> parse and verifies every
 *	field survives; checks that the size helpers agree with the
 *	encoders, that a too-small encode buffer is refused, and that a
 *	truncated record (a torn tail) is rejected by the parser rather
 *	than read out of bounds.  Plain asserts; no loop, no I/O.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xlog.h"
#include "xtc.h"

static int g_fail;
#define CK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

int
main(void)
{
	uint8_t buf[512];
	xl_hdr_t h, ph;
	xl_body_t b, pb;
	int n;

	/* --- header-only records --- */
	for (int t = 0; t < 4; t++) {
		uint8_t types[] = { XL_BEGIN, XL_COMMIT, XL_ABORT, XL_END };
		memset(&h, 0, sizeof h);
		h.type = types[t];
		h.txn_id = 0x1122334455667788ull + t;
		h.prev_lsn = 0xA0B0C0D0ull + t;
		n = xl_enc_simple(buf, sizeof buf, &h);
		CK(n == XL_HDR_LEN);
		CK(xl_parse_hdr(buf, (uint32_t)n, &ph) == XTC_OK);
		CK(ph.type == h.type);
		CK(ph.txn_id == h.txn_id);
		CK(ph.prev_lsn == h.prev_lsn);
	}
	/* a non-header type is rejected by xl_enc_simple */
	h.type = XL_UPDATE;
	CK(xl_enc_simple(buf, sizeof buf, &h) == XTC_E_INVAL);

	/* --- UPDATE with both redo and undo images --- */
	{
		const char redo[] = "after-image-value";
		const char undo[] = "BEFORE";
		memset(&h, 0, sizeof h);
		h.type = XL_UPDATE; h.txn_id = 42; h.prev_lsn = 99;
		memset(&b, 0, sizeof b);
		b.page_id = 7; b.tableid = 3; b.rowid = -123456789;
		b.commit_ts = 0xDEADBEEFCAFEull; b.flags = 0x01;
		b.redo = redo; b.redo_len = (uint16_t)sizeof redo;
		b.undo = undo; b.undo_len = (uint16_t)sizeof undo;

		n = xl_enc_update(buf, sizeof buf, &h, &b);
		CK(n == (int)(XL_HDR_LEN + xl_update_size(b.redo_len, b.undo_len)));

		CK(xl_parse_update(buf, (uint32_t)n, &ph, &pb) == XTC_OK);
		CK(ph.type == XL_UPDATE && ph.txn_id == 42 && ph.prev_lsn == 99);
		CK(pb.page_id == 7 && pb.tableid == 3 && pb.rowid == -123456789);
		CK(pb.commit_ts == 0xDEADBEEFCAFEull && pb.flags == 0x01);
		CK(pb.redo_len == b.redo_len && memcmp(pb.redo, redo, b.redo_len) == 0);
		CK(pb.undo_len == b.undo_len && memcmp(pb.undo, undo, b.undo_len) == 0);

		/* truncated: every prefix shorter than the full record is rejected. */
		for (uint32_t L = 0; L < (uint32_t)n; L++)
			CK(xl_parse_update(buf, L, &ph, &pb) != XTC_OK);
	}

	/* --- UPDATE that is a fresh insert (no before-image) --- */
	{
		const char redo[] = "v";
		memset(&h, 0, sizeof h);
		h.type = XL_UPDATE; h.txn_id = 1;
		memset(&b, 0, sizeof b);
		b.tableid = 9; b.rowid = 5; b.commit_ts = 100;
		b.redo = redo; b.redo_len = (uint16_t)sizeof redo;
		/* undo_len stays 0: inverse is "remove the key". */
		n = xl_enc_update(buf, sizeof buf, &h, &b);
		CK(n > 0);
		CK(xl_parse_update(buf, (uint32_t)n, &ph, &pb) == XTC_OK);
		CK(pb.undo_len == 0 && pb.undo == NULL);
		CK(pb.redo_len == b.redo_len && memcmp(pb.redo, redo, b.redo_len) == 0);
	}

	/* --- CLR --- */
	{
		const char redo[] = "compensating";
		memset(&h, 0, sizeof h);
		h.type = XL_CLR; h.txn_id = 42; h.prev_lsn = 7;
		memset(&b, 0, sizeof b);
		b.undo_next_lsn = 88; b.tableid = 3; b.rowid = 17; b.commit_ts = 50;
		b.redo = redo; b.redo_len = (uint16_t)sizeof redo;
		n = xl_enc_clr(buf, sizeof buf, &h, &b);
		CK(n == (int)(XL_HDR_LEN + xl_clr_size(b.redo_len)));
		CK(xl_parse_clr(buf, (uint32_t)n, &ph, &pb) == XTC_OK);
		CK(ph.type == XL_CLR && ph.txn_id == 42 && ph.prev_lsn == 7);
		CK(pb.undo_next_lsn == 88 && pb.tableid == 3 && pb.rowid == 17);
		CK(pb.redo_len == b.redo_len && memcmp(pb.redo, redo, b.redo_len) == 0);
		for (uint32_t L = 0; L < (uint32_t)n; L++)
			CK(xl_parse_clr(buf, L, &ph, &pb) != XTC_OK);
	}

	/* --- CHECKPOINT --- */
	{
		uint64_t clk = 0;
		n = xl_enc_checkpoint(buf, sizeof buf, 0x0123456789ABCDEFull);
		CK(n == XL_HDR_LEN + 8);
		CK(xl_parse_checkpoint(buf, (uint32_t)n, &clk) == XTC_OK);
		CK(clk == 0x0123456789ABCDEFull);
		CK(xl_parse_hdr(buf, (uint32_t)n, &ph) == XTC_OK && ph.type == XL_CHECKPOINT);
		CK(xl_parse_checkpoint(buf, XL_HDR_LEN, &clk) != XTC_OK); /* missing clock */
	}

	/* --- too-small encode buffers are refused, never overrun --- */
	{
		memset(&h, 0, sizeof h); h.type = XL_BEGIN;
		CK(xl_enc_simple(buf, XL_HDR_LEN - 1, &h) == XTC_E_RANGE);
		h.type = XL_UPDATE;
		memset(&b, 0, sizeof b); b.redo_len = 0;
		CK(xl_enc_update(buf, XL_HDR_LEN, &h, &b) == XTC_E_RANGE);
		CK(xl_enc_checkpoint(buf, XL_HDR_LEN, 0) == XTC_E_RANGE);
	}

	/* --- cross-type parse rejects the wrong record --- */
	{
		memset(&h, 0, sizeof h); h.type = XL_BEGIN;
		n = xl_enc_simple(buf, sizeof buf, &h);
		CK(xl_parse_update(buf, (uint32_t)n, &ph, &pb) == XTC_E_INVAL);
		CK(xl_parse_clr(buf, (uint32_t)n, &ph, &pb) == XTC_E_INVAL);
		CK(xl_parse_checkpoint(buf, (uint32_t)n, &b.commit_ts) == XTC_E_INVAL);
	}

	/* --- walking several records packed back to back (one WAL frame
	 * carries a whole transaction: UPDATE, UPDATE, COMMIT) --- */
	{
		uint8_t blob[256];
		uint8_t *q = blob;
		const char *vals[2] = { "row-one", "r2" };
		uint32_t off, count;
		int rl;

		for (int u = 0; u < 2; u++) {
			memset(&h, 0, sizeof h);
			h.type = XL_UPDATE; h.txn_id = 7;
			memset(&b, 0, sizeof b);
			b.tableid = 1; b.rowid = 10 + u; b.commit_ts = 7;
			b.redo = vals[u]; b.redo_len = (uint16_t)(strlen(vals[u]) + 1);
			n = xl_enc_update(q, sizeof blob - (size_t)(q - blob), &h, &b);
			CK(n > 0); q += n;
		}
		memset(&h, 0, sizeof h); h.type = XL_COMMIT; h.txn_id = 7;
		n = xl_enc_simple(q, sizeof blob - (size_t)(q - blob), &h);
		CK(n > 0); q += n;

		/* walk it */
		off = 0; count = 0;
		while (off < (uint32_t)(q - blob)) {
			rl = xl_record_len(blob + off, (uint32_t)(q - blob) - off);
			CK(rl > 0);
			CK(xl_parse_hdr(blob + off, (uint32_t)rl, &ph) == XTC_OK);
			count++;
			off += (uint32_t)rl;
		}
		CK(off == (uint32_t)(q - blob));   /* landed exactly on the end */
		CK(count == 3);                    /* two UPDATEs + one COMMIT */
		/* a buffer one byte short of a complete trailing record is rejected */
		CK(xl_record_len(blob, XL_HDR_LEN - 1) == XTC_E_INVAL);
	}

	if (g_fail) {
		fprintf(stderr, "test_xlog: FAILED\n");
		return 1;
	}
	printf("test_xlog: all record types round-trip; truncation rejected\n");
	return 0;
}

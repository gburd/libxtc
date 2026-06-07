/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xlog.c
 *	ARIES log-record codec (see xlog.h).  Pure byte (de)serialization
 *	with bounds checks; no I/O and no engine state.  Record layouts,
 *	all fields host-endian and written/read through memcpy:
 *
 *	  header   [type:1][txn_id:8][prev_lsn:8]                      (17)
 *	  UPDATE   header [page_id:4][tableid:4][rowid:8][commit_ts:8]
 *	                  [flags:1][redo_len:2][redo][undo_len:2][undo]
 *	  CLR      header [undo_next_lsn:8][page_id:4][tableid:4]
 *	                  [rowid:8][commit_ts:8][flags:1][redo_len:2][redo]
 *	  CHECKPT  header [commit_clock:8]
 *	  BEGIN/COMMIT/ABORT/END  header only
 */

#include <string.h>

#include "xlog.h"
#include "xtc.h"

/* Fixed-size body prefixes (the bytes before the variable redo/undo). */
#define XL_UPD_FIXED 27   /* page4 + tableid4 + rowid8 + commit_ts8 + flags1 + redo_len2 */
#define XL_CLR_FIXED 35   /* undo_next8 + page4 + tableid4 + rowid8 + commit_ts8 + flags1 + redo_len2 */

/* Little-helper put/get: advance the cursor, host-endian via memcpy. */
static inline uint8_t *
put(uint8_t *p, const void *src, size_t n)
{
	memcpy(p, src, n);
	return p + n;
}

static inline const uint8_t *
get(const uint8_t *p, void *dst, size_t n)
{
	memcpy(dst, p, n);
	return p + n;
}

size_t
xl_update_size(uint16_t redo_len, uint16_t undo_len)
{
	return (size_t)XL_UPD_FIXED + redo_len + 2 + undo_len;
}

size_t
xl_clr_size(uint16_t redo_len)
{
	return (size_t)XL_CLR_FIXED + redo_len;
}

static uint8_t *
enc_hdr(uint8_t *p, const xl_hdr_t *h)
{
	*p++ = h->type;
	p = put(p, &h->txn_id, 8);
	p = put(p, &h->prev_lsn, 8);
	return p;
}

int
xl_enc_simple(uint8_t *buf, size_t cap, const xl_hdr_t *h)
{
	if (buf == NULL || h == NULL)
		return XTC_E_INVAL;
	if (h->type != XL_BEGIN && h->type != XL_COMMIT &&
	    h->type != XL_ABORT && h->type != XL_END)
		return XTC_E_INVAL;
	if (cap < XL_HDR_LEN)
		return XTC_E_RANGE;
	(void)enc_hdr(buf, h);
	return XL_HDR_LEN;
}

int
xl_enc_update(uint8_t *buf, size_t cap, const xl_hdr_t *h, const xl_body_t *b)
{
	uint8_t *p = buf;
	size_t need;

	if (buf == NULL || h == NULL || b == NULL || h->type != XL_UPDATE)
		return XTC_E_INVAL;
	if ((b->redo_len && b->redo == NULL) || (b->undo_len && b->undo == NULL))
		return XTC_E_INVAL;
	need = (size_t)XL_HDR_LEN + xl_update_size(b->redo_len, b->undo_len);
	if (cap < need)
		return XTC_E_RANGE;

	p = enc_hdr(p, h);
	p = put(p, &b->page_id, 4);
	p = put(p, &b->tableid, 4);
	p = put(p, &b->rowid, 8);
	p = put(p, &b->commit_ts, 8);
	*p++ = b->flags;
	p = put(p, &b->redo_len, 2);
	if (b->redo_len) p = put(p, b->redo, b->redo_len);
	p = put(p, &b->undo_len, 2);
	if (b->undo_len) p = put(p, b->undo, b->undo_len);
	return (int)(p - buf);
}

int
xl_enc_clr(uint8_t *buf, size_t cap, const xl_hdr_t *h, const xl_body_t *b)
{
	uint8_t *p = buf;
	size_t need;

	if (buf == NULL || h == NULL || b == NULL || h->type != XL_CLR)
		return XTC_E_INVAL;
	if (b->redo_len && b->redo == NULL)
		return XTC_E_INVAL;
	need = (size_t)XL_HDR_LEN + xl_clr_size(b->redo_len);
	if (cap < need)
		return XTC_E_RANGE;

	p = enc_hdr(p, h);
	p = put(p, &b->undo_next_lsn, 8);
	p = put(p, &b->page_id, 4);
	p = put(p, &b->tableid, 4);
	p = put(p, &b->rowid, 8);
	p = put(p, &b->commit_ts, 8);
	*p++ = b->flags;
	p = put(p, &b->redo_len, 2);
	if (b->redo_len) p = put(p, b->redo, b->redo_len);
	return (int)(p - buf);
}

int
xl_enc_checkpoint(uint8_t *buf, size_t cap, uint64_t commit_clock)
{
	xl_hdr_t h;
	uint8_t *p = buf;

	if (buf == NULL)
		return XTC_E_INVAL;
	if (cap < (size_t)XL_HDR_LEN + 8)
		return XTC_E_RANGE;
	h.type = XL_CHECKPOINT;
	h.txn_id = 0;
	h.prev_lsn = 0;
	p = enc_hdr(p, &h);
	p = put(p, &commit_clock, 8);
	return (int)(p - buf);
}

int
xl_parse_hdr(const void *rec, uint32_t len, xl_hdr_t *h)
{
	const uint8_t *p = rec;

	if (rec == NULL || h == NULL || len < XL_HDR_LEN)
		return XTC_E_INVAL;
	h->type = *p++;
	p = get(p, &h->txn_id, 8);
	(void)get(p, &h->prev_lsn, 8);
	if (h->type < XL_BEGIN || h->type > XL_END)
		return XTC_E_INVAL;
	return XTC_OK;
}

int
xl_parse_update(const void *rec, uint32_t len, xl_hdr_t *h, xl_body_t *b)
{
	const uint8_t *p = rec;
	int rc;

	if (b == NULL)
		return XTC_E_INVAL;
	if ((rc = xl_parse_hdr(rec, len, h)) != XTC_OK)
		return rc;
	if (h->type != XL_UPDATE || len < (uint32_t)XL_HDR_LEN + XL_UPD_FIXED)
		return XTC_E_INVAL;
	memset(b, 0, sizeof *b);
	p += XL_HDR_LEN;
	p = get(p, &b->page_id, 4);
	p = get(p, &b->tableid, 4);
	p = get(p, &b->rowid, 8);
	p = get(p, &b->commit_ts, 8);
	b->flags = *p++;
	p = get(p, &b->redo_len, 2);
	/* header + fixed + redo + undo_len(2) + undo must fit. */
	if ((uint32_t)XL_HDR_LEN + xl_update_size(b->redo_len, 0) > len)
		return XTC_E_INVAL;
	b->redo = b->redo_len ? p : NULL;
	p += b->redo_len;
	p = get(p, &b->undo_len, 2);
	if ((uint32_t)XL_HDR_LEN + xl_update_size(b->redo_len, b->undo_len) > len)
		return XTC_E_INVAL;
	b->undo = b->undo_len ? p : NULL;
	return XTC_OK;
}

int
xl_parse_clr(const void *rec, uint32_t len, xl_hdr_t *h, xl_body_t *b)
{
	const uint8_t *p = rec;
	int rc;

	if (b == NULL)
		return XTC_E_INVAL;
	if ((rc = xl_parse_hdr(rec, len, h)) != XTC_OK)
		return rc;
	if (h->type != XL_CLR || len < (uint32_t)XL_HDR_LEN + XL_CLR_FIXED)
		return XTC_E_INVAL;
	memset(b, 0, sizeof *b);
	p += XL_HDR_LEN;
	p = get(p, &b->undo_next_lsn, 8);
	p = get(p, &b->page_id, 4);
	p = get(p, &b->tableid, 4);
	p = get(p, &b->rowid, 8);
	p = get(p, &b->commit_ts, 8);
	b->flags = *p++;
	p = get(p, &b->redo_len, 2);
	if ((uint32_t)XL_HDR_LEN + xl_clr_size(b->redo_len) > len)
		return XTC_E_INVAL;
	b->redo = b->redo_len ? p : NULL;
	return XTC_OK;
}

int
xl_parse_checkpoint(const void *rec, uint32_t len, uint64_t *commit_clock)
{
	xl_hdr_t h;
	int rc;

	if (commit_clock == NULL)
		return XTC_E_INVAL;
	if ((rc = xl_parse_hdr(rec, len, &h)) != XTC_OK)
		return rc;
	if (h.type != XL_CHECKPOINT || len < (uint32_t)XL_HDR_LEN + 8)
		return XTC_E_INVAL;
	(void)get((const uint8_t *)rec + XL_HDR_LEN, commit_clock, 8);
	return XTC_OK;
}

int
xl_record_len(const void *rec, uint32_t avail)
{
	const uint8_t *p = rec;
	uint16_t redo_len, undo_len;
	uint32_t total;

	if (rec == NULL || avail < XL_HDR_LEN)
		return XTC_E_INVAL;
	switch (p[0]) {
	case XL_BEGIN: case XL_COMMIT: case XL_ABORT: case XL_END:
		total = XL_HDR_LEN;
		break;
	case XL_CHECKPOINT:
		total = XL_HDR_LEN + 8;
		break;
	case XL_UPDATE:
		if (avail < (uint32_t)XL_HDR_LEN + XL_UPD_FIXED)
			return XTC_E_INVAL;
		memcpy(&redo_len, p + XL_HDR_LEN + XL_UPD_FIXED - 2, 2);
		/* must reach the undo_len field to know the full size */
		if (avail < (uint32_t)XL_HDR_LEN + XL_UPD_FIXED + redo_len + 2)
			return XTC_E_INVAL;
		memcpy(&undo_len, p + XL_HDR_LEN + XL_UPD_FIXED + redo_len, 2);
		total = (uint32_t)XL_HDR_LEN + (uint32_t)xl_update_size(redo_len, undo_len);
		break;
	case XL_CLR:
		if (avail < (uint32_t)XL_HDR_LEN + XL_CLR_FIXED)
			return XTC_E_INVAL;
		memcpy(&redo_len, p + XL_HDR_LEN + XL_CLR_FIXED - 2, 2);
		total = (uint32_t)XL_HDR_LEN + (uint32_t)xl_clr_size(redo_len);
		break;
	default:
		return XTC_E_INVAL;
	}
	if (avail < total)
		return XTC_E_INVAL;
	return (int)total;
}

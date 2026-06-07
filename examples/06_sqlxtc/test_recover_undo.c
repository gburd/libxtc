/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_recover_undo.c
 *	Exercise the recovery undo pass and CLR writing in isolation.
 *
 *	The NO-STEAL commit path never leaves a loser in the log (a
 *	transaction is logged atomically at commit), so the undo pass is
 *	dormant there.  This test synthesizes the log a STEAL engine
 *	would leave: a committed transaction (a winner) and a second
 *	transaction whose updates were logged -- as they would be once
 *	uncommitted versions can reach the tree -- but that never wrote
 *	an XL_COMMIT (a loser).  Recovery must redo the winner, then undo
 *	the loser (deleting the versions it inserted) and write one XL_CLR
 *	per reversed update plus a closing XL_END.
 *
 *	Plain asserts; no loop; the WAL is driven with wal_commit_sync.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "wal.h"
#include "xlog.h"
#include "xtc.h"
#include "t_tmp.h"

#define PAGE_SZ 4096

static int g_fail;
#define CK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* Replicates xstore.c's enc_vkey: (tableid:4 BE)(rowid^signbit:8 BE)(~ts:8 BE). */
static void
vkey(uint32_t tableid, int64_t rowid, uint64_t ts, uint8_t out[20])
{
	uint64_t r = (uint64_t)rowid ^ 0x8000000000000000ull;
	uint64_t t = ~ts;
	int i;
	for (i = 3; i >= 0; i--) { out[i] = (uint8_t)(tableid & 0xFF); tableid >>= 8; }
	for (i = 7; i >= 0; i--) { out[4 + i] = (uint8_t)(r & 0xFF); r >>= 8; }
	for (i = 7; i >= 0; i--) { out[12 + i] = (uint8_t)(t & 0xFF); t >>= 8; }
}

/* Append one XL_UPDATE for (tableid,rowid,ts,val) to p; returns new cursor. */
static uint8_t *
put_update(uint8_t *p, size_t cap, uint64_t txn, uint32_t tableid,
    int64_t rowid, uint64_t ts, const char *val)
{
	xl_hdr_t h;
	xl_body_t b;
	int n;
	h.type = XL_UPDATE; h.txn_id = txn; h.prev_lsn = 0;
	memset(&b, 0, sizeof b);
	b.tableid = tableid; b.rowid = rowid; b.commit_ts = ts;
	b.redo = val; b.redo_len = (uint16_t)(strlen(val) + 1);
	n = xl_enc_update(p, cap, &h, &b);
	return n < 0 ? NULL : p + n;
}

static int g_clr, g_end, g_clr_done;
static int
count_cb(uint64_t lsn, const void *rec, uint32_t len, void *u)
{
	uint32_t off = 0;
	(void)lsn; (void)u;
	while (off < len) {
		xl_hdr_t h;
		int rl = xl_record_len((const uint8_t *)rec + off, len - off);
		if (rl <= 0) break;
		if (xl_parse_hdr((const uint8_t *)rec + off, (uint32_t)rl, &h) != XTC_OK)
			break;
		if (h.type == XL_CLR && h.txn_id == 20) {
			xl_hdr_t ch; xl_body_t cb;
			g_clr++;
			if (xl_parse_clr((const uint8_t *)rec + off, (uint32_t)rl, &ch, &cb) == XTC_OK
			    && cb.undo_next_lsn == 0)
				g_clr_done++;     /* the CLR that completed the undo */
		}
		if (h.type == XL_END && h.txn_id == 20) g_end++;
		off += (uint32_t)rl;
	}
	return 0;
}

int
main(void)
{
	char logp[256], btp[256];
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *wal = NULL;
	wal_opts_t wo;
	uint64_t lsn;
	uint8_t fr[256], *p;
	uint8_t k[20], buf[64];
	uint16_t vl;
	int fd;

	t_tmpl(logp, sizeof logp, "recundo-wal");
	t_tmpl(btp, sizeof btp, "recundo-bt");
	if ((fd = mkstemp(logp)) >= 0) close(fd);
	if ((fd = mkstemp(btp)) >= 0) close(fd);

	/* ---- build the log a crash would leave ---- */
	memset(&wo, 0, sizeof wo);
	wo.path = logp;                         /* append=0: fresh log */
	CK(wal_open(&wo, &wal) == XTC_OK);

	/* winner: one atomic frame [XL_UPDATE][XL_COMMIT], txn 10 */
	p = put_update(fr, sizeof fr, 10, 1, 1, 10, "win");
	CK(p != NULL);
	{ xl_hdr_t h; int n; h.type = XL_COMMIT; h.txn_id = 10; h.prev_lsn = 0;
	  n = xl_enc_simple(p, sizeof fr - (size_t)(p - fr), &h); CK(n > 0); p += n; }
	CK(wal_commit_sync(wal, fr, (uint32_t)(p - fr), &lsn) == XTC_OK);

	/* loser: two separate frames, txn 20, NO commit (as STEAL would log) */
	p = put_update(fr, sizeof fr, 20, 1, 2, 20, "los2"); CK(p != NULL);
	CK(wal_commit_sync(wal, fr, (uint32_t)(p - fr), &lsn) == XTC_OK);
	p = put_update(fr, sizeof fr, 20, 1, 3, 20, "los3"); CK(p != NULL);
	CK(wal_commit_sync(wal, fr, (uint32_t)(p - fr), &lsn) == XTC_OK);
	wal_close(wal);

	/* ---- recover into a fresh tree ---- */
	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = 64;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(xstore_recover(bt, logp) == XTC_OK);

	/* winner survives */
	vkey(1, 1, 10, k);
	CK(bt_lookup(bt, k, 20, buf, sizeof buf, &vl) == XTC_OK);
	CK(vl == 5 && buf[0] == 0 && memcmp(buf + 1, "win", 4) == 0);

	/* loser's versions are gone (redone, then undone) */
	vkey(1, 2, 20, k);
	CK(bt_lookup(bt, k, 20, buf, sizeof buf, &vl) == XTC_E_NOTFOUND);
	vkey(1, 3, 20, k);
	CK(bt_lookup(bt, k, 20, buf, sizeof buf, &vl) == XTC_E_NOTFOUND);

	bt_close(bt);
	bm_destroy(bm);

	/* ---- the undo pass left CLRs + an XL_END in the log ---- */
	CK(wal_scan(logp, count_cb, NULL) == XTC_OK);
	CK(g_clr == 2);          /* one compensation record per reversed update */
	CK(g_clr_done == 1);     /* the last (oldest) reversal has undo_next_lsn 0 */
	CK(g_end == 1);          /* loser fully undone */

	unlink(logp); unlink(btp);
	{ char s[280]; snprintf(s, sizeof s, "%s.dwb", btp); unlink(s); }

	if (g_fail) { fprintf(stderr, "test_recover_undo: FAILED\n"); return 1; }
	printf("test_recover_undo: winner redone, loser undone, CLRs + END written\n");
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_inplace_redo.c
 *	End-to-end ARIES physiological in-place crash recovery: the
 *	XL_PAGE full-page after-image path, driven through the LIVE
 *	structure-modification emission hook, repairs a torn base in place
 *	(page-LSN gated, idempotent) rather than discarding it.
 *
 *	test_redo_page already unit-tests bm_apply_page_image in isolation
 *	(a hand-built image, manual LSN gate).  This test closes the loop:
 *	it drives REAL B-tree splits through SQL with physiological SMO
 *	logging on (xstore_register_smo), so the engine emits an XL_PAGE
 *	for every page a split writes inside a nested-top-action bracket,
 *	then proves two end-to-end properties of xstore_recover_inplace:
 *
 *	  A. Idempotent replay over a CURRENT base.  Recover in place
 *	     against the already-correct (clean-checkpointed) base:
 *	     forward image replay reproduces each page's final state
 *	     (replaying a page's image sequence in LSN order is idempotent
 *	     on the end result) and logical XL_UPDATE redo is an idempotent
 *	     upsert, so the tree is left correct -- every row present and
 *	     ordered.  This proves the recovery driver handles XL_PAGE and
 *	     composes with logical redo without corrupting a current base.
 *
 *	  B. Repair of a TORN structure modification.  Take the same
 *	     correct base, find the pages a split wrote (the page ids in
 *	     the XL_PAGE records) and TEAR them on disk (zero them, LSN ->
 *	     0, as a lost mid-SMO flush would).  Recover in place: the
 *	     XL_PAGE images are now newer than the zeroed pages, so
 *	     bm_apply_page_image rewrites each torn page from its image
 *	     (pages applied > 0).  Every row reappears and a full ordered
 *	     scan returns exactly the inserted keys -- the torn SMO was
 *	     repaired physiologically, in place, with no rebuild.
 *
 *	Scope note (docs/M_SQLXTC_BDB.md): a fully in-place crash restart
 *	from an ARBITRARILY torn base -- where NON-split row writes are
 *	also lost -- additionally needs physiological logging of those row
 *	writes, so logical XL_UPDATE redo never has to navigate torn
 *	structure.  That is why the live sx_storage_open crash path keeps
 *	the proven logical rebuild; this test exercises the physiological
 *	repair MECHANISM end to end on the cases page images cover.
 *
 *	Plain asserts; no loop; the WAL is driven with wal_commit_sync.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "wal.h"
#include "xlog.h"
#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_ROWS   300
#define PAGE_SZ  512          /* tiny -> shallow fan-out -> deep tree, many splits */
#define POOL     64           /* big enough to hold the build; we tear by hand */
#define MAX_PG   4096


static int
eval_int(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int v = -1;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -1;
	if (sx_step(st) == SX_ROW)
		v = (int)sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static int
sel_v(sx_db *db, int k, char *out, size_t cap)
{
	sx_stmt *st = NULL;
	int got = 0;
	if (sx_prepare(db, "SELECT v FROM t WHERE k=?", -1, &st, NULL) != SX_OK)
		return -1;
	sx_bind_int64(st, 1, k);
	if (sx_step(st) == SX_ROW) {
		const unsigned char *t = sx_column_text(st, 0);
		size_t n = (size_t)sx_column_bytes(st, 0);
		if (n >= cap) n = cap - 1;
		if (t) memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	sx_finalize(st);
	return got;
}

/* Collect the distinct page ids named in the log's XL_PAGE records --
 * the pages real splits wrote.  These are the SMO pages a torn flush
 * could have lost. */
/* Collect the distinct page ids named in the log's XL_PAGE records --
 * the pages real splits wrote.  These are the SMO pages a torn flush
 * could have lost.  Also tallies the XL_PAGE records and the dummy CLRs
 * that close each nested top action (txn_id 0), to prove the NTA bracket
 * fired. */
struct pidset { uint32_t pid[1024]; int n; int n_page; int n_nta_clr; };
static void
pidset_add(struct pidset *s, uint32_t pid)
{
	int i;
	for (i = 0; i < s->n; i++)
		if (s->pid[i] == pid)
			return;
	if (s->n < (int)(sizeof s->pid / sizeof s->pid[0]))
		s->pid[s->n++] = pid;
}
static int
collect_cb(uint64_t lsn, const void *rec, uint32_t len, void *u)
{
	struct pidset *s = u;
	uint32_t off = 0;
	(void)lsn;
	while (off < len) {
		xl_hdr_t h;
		int rl = xl_record_len((const uint8_t *)rec + off, len - off);
		if (rl <= 0) break;
		if (xl_parse_hdr((const uint8_t *)rec + off, (uint32_t)rl, &h) != XTC_OK)
			break;
		if (h.type == XL_PAGE) {
			xl_hdr_t ph; uint32_t pid = 0; const void *img = NULL; uint16_t il = 0;
			if (xl_parse_page((const uint8_t *)rec + off, (uint32_t)rl,
			    &ph, &pid, &img, &il) == XTC_OK) {
				pidset_add(s, pid);
				s->n_page++;
			}
		} else if (h.type == XL_CLR && h.txn_id == 0) {
			s->n_nta_clr++;       /* dummy CLR closing a nested top action */
		}
		off += (uint32_t)rl;
	}
	return 0;
}

/* Verify the recovered tree: count, per-key value, ordered full scan. */
static void
verify(const char *btp, const char *logp, int reopen, uint64_t *out_pages)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	char want[32], got[32];
	int i, miss = 0, scanned = 0, prev = -1, ordered = 1;
	uint64_t pages = 0;

	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = POOL;
	bo.reopen = (uint8_t)reopen; bo.lsn_off = 0;  /* no double_write: tearing is by hand */
	CK(bm_create(&bo, &bm) == XTC_OK);
	if (reopen)
		CK(bt_reopen(bm, &bt) == XTC_OK);
	else
		CK(bt_open(bm, &bt) == XTC_OK);
	CK(xstore_recover_inplace(bt, bm, logp, &pages) == XTC_OK);
	CK(bm_checkpoint(bm) == XTC_OK);

	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL)
	    == SX_OK);

	CK(eval_int(db, "SELECT count(*) FROM t") == N_ROWS);
	for (i = 0; i < N_ROWS; i++) {
		snprintf(want, sizeof want, "val-%d", i);
		if (sel_v(db, i, got, sizeof got) == 1 && strcmp(got, want) == 0)
			continue;
		miss++;
	}
	CK(miss == 0);
	if (sx_prepare(db, "SELECT k FROM t ORDER BY k", -1, &st, NULL)
	    == SX_OK) {
		while (sx_step(st) == SX_ROW) {
			int k = (int)sx_column_int64(st, 0);
			if (k <= prev) ordered = 0;
			prev = k;
			scanned++;
		}
		sx_finalize(st);
	}
	CK(ordered == 1);
	CK(scanned == N_ROWS);
	CK(prev == N_ROWS - 1);
	if (miss || !ordered || scanned != N_ROWS)
		fprintf(stderr, "FAIL: verify miss=%d scanned=%d ordered=%d pages=%llu\n",
		    miss, scanned, ordered, (unsigned long long)pages);

	sx_close(db); bt_close(bt); bm_destroy(bm);
	if (out_pages) *out_pages = pages;
}

int
main(void)
{
	wal_t *wal = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	wal_opts_t wo = { 0 };
	char btp[256]; t_tmpl(btp, sizeof btp, "sqlxtc-iplace");
	char logp[256]; t_tmpl(logp, sizeof logp, "sqlxtc-iplace-log");
	char dwp[288];
	struct pidset smo = { {0}, 0, 0, 0 };
	uint64_t pages_a = 0, pages_b = 0;
	int fd, i, torn, bfd;

	fd = mkstemp(btp);  if (fd < 0) return 1; close(fd);
	fd = mkstemp(logp); if (fd < 0) return 1; close(fd);

	/* ---- build the tree with physiological SMO logging on ---- */
	wo.path = logp; wo.window_ns = 0; wo.max_batch = 1;
	if (wal_open(&wo, &wal) != XTC_OK) return 1;
	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = POOL;
	bo.lsn_off = 0;            /* ARIES page LSN; double_write off (hand tearing) */
	if (bm_create(&bo, &bm) != XTC_OK) return 1;
	if (bt_open(bm, &bt) != XTC_OK) return 1;
	xstore_set_wal((struct wal *)wal);
	xstore_register_smo(1);    /* emit XL_PAGE per split page, NTA-bracketed */
	if (sx_open_bt(bt, &db) != SX_OK) return 1;
	if (sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL)
	    != SX_OK) return 1;
	for (i = 0; i < N_ROWS; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'val-%d');", i, i);
		if (sx_exec(db, sql, NULL) != SX_OK) {
			fprintf(stderr, "FAIL: insert %d\n", i); return 1;
		}
	}
	/* Clean-checkpoint the base durable: now every page is at its final
	 * LSN, the correct answer.  Close the engine cleanly. */
	if (bm_checkpoint(bm) != XTC_OK) return 1;
	sx_close(db);
	xstore_register_smo(0);
	xstore_set_wal(NULL);
	bt_close(bt);
	bm_destroy(bm);
	wal_close(wal);

	/* The split pages, from the XL_PAGE records the build emitted. */
	if (wal_scan(logp, collect_cb, &smo) != XTC_OK) return 1;
	CK(smo.n > 0);               /* real splits logged page images */
	CK(smo.n_page > 0);          /* XL_PAGE after-images were emitted */
	CK(smo.n_nta_clr > 0);       /* each split closed its nested top action */
	printf("  ok   SMO logging: %d splits emitted %d XL_PAGE after-images over "
	    "%d distinct pages, each bracketed by a nested-top-action dummy CLR\n",
	    smo.n_nta_clr, smo.n_page, smo.n);

	/* ---- A. idempotent replay over the current (correct) base ----
	 * Recover in place against the already-correct base.  Forward image
	 * replay reproduces each page's final state (replaying a page's
	 * image sequence in LSN order is idempotent on the end result), and
	 * logical XL_UPDATE redo is an idempotent upsert, so every row is
	 * still present and the scan is ordered -- the recovery driver
	 * handles XL_PAGE and composes with logical redo without corrupting
	 * a current base. */
	verify(btp, logp, 1, &pages_a);
	printf("  ok   in-place replay over a current base: %d rows intact, "
	    "scan ordered (XL_PAGE + logical redo idempotent over the final "
	    "state; %llu images re-applied through the LSN sequence)\n",
	    N_ROWS, (unsigned long long)pages_a);

	/* ---- B. tear the SMO pages on disk, then repair in place ---- */
	bfd = open(btp, O_RDWR);
	CK(bfd >= 0);
	torn = 0;
	if (bfd >= 0) {
		uint8_t zero[MAX_PG];
		memset(zero, 0, sizeof zero);
		for (i = 0; i < smo.n; i++) {
			off_t at = (off_t)smo.pid[i] * (off_t)PAGE_SZ;
			if (pwrite(bfd, zero, PAGE_SZ, at) == (ssize_t)PAGE_SZ)
				torn++;
		}
		fsync(bfd);
		close(bfd);
	}
	CK(torn == smo.n);           /* every split page zeroed (LSN -> 0) */

	verify(btp, logp, 1, &pages_b);
	CK(pages_b > 0);             /* physiological redo repaired torn pages */
	printf("  ok   in-place redo: %d split pages torn (zeroed) on disk, then "
	    "repaired from XL_PAGE images (%llu applied); %d rows reappeared and "
	    "scan ordered -- torn SMO repaired in place, no rebuild\n",
	    torn, (unsigned long long)pages_b, N_ROWS);

	unlink(btp); unlink(logp);
	snprintf(dwp, sizeof dwp, "%s.dwb", btp); unlink(dwp);

	if (g_fail) { fprintf(stderr, "test_inplace_redo: FAILED\n"); return 1; }
	printf("All sqlxtc in-place physiological-redo recovery tests passed.\n");
	return 0;
}

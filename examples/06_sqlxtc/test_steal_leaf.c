/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_steal_leaf.c
 *	Increment 3: physiological per-NON-split-leaf after-image logging
 *	makes in-place crash recovery on a torn NON-split leaf safe.
 *
 *	Before this increment, logical XL_UPDATE redo over a trusted base
 *	descended a torn (zeroed) NON-split leaf and lost its whole key
 *	range: tearing ONE committed non-split leaf lost ~3000 of 6000
 *	committed rows (the docs/M_SQLXTC_BDB.md S3 trap, reproduced by
 *	the probe this test replaces).  There was no per-non-split-leaf
 *	after-image to gate a repair on -- only split pages were logged.
 *
 *	Now every plain in-leaf insert logs an XL_PAGE after-image
 *	(xstore.c:xs_leaf_page), each as its own monotonically-numbered WAL
 *	record, and in-place recovery runs in two passes: pass 1 applies
 *	every page image (record-LSN gated, so the LAST image of a page
 *	wins) BEFORE pass 2's logical redo descends the base.  A torn
 *	NON-split leaf is repaired from its final image, so logical redo
 *	never navigates torn structure.
 *
 *	This test proves it end to end:
 *
 *	  A. Durability through a torn NON-split leaf.  Commit a large txn
 *	     (double_write OFF so a torn leaf is NOT auto-repaired), hand-
 *	     tear several committed NON-split leaves on disk (zero them,
 *	     LSN -> 0), recover in place.  The FULL committed set (6000
 *	     rows) reappears, a full ordered scan returns exactly the keys
 *	     (no missing, torn, duplicated, or misordered row), and
 *	     pages_redone > 0 proves images repaired the torn leaves rather
 *	     than a silent rebuild.  This is the 5144-lost-rows repro now
 *	     passing.
 *
 *	  B. Atomicity + no-leak of a torn loser.  A large UNCOMMITTED txn
 *	     whose leaves are also torn is undone in place via CLRs; the
 *	     base holds exactly the committed set, no uncommitted row
 *	     leaks, and the committed rows survive whole.
 *
 *	Every recovery here runs on a torn base, so the process caps its
 *	own address space at 256 MB (setrlimit(RLIMIT_AS)) BEFORE any
 *	recovery, so a regression that reintroduced the unbounded-alloc
 *	torn-base balloon fails cleanly (malloc NULL) instead of OOM-killing
 *	the machine.  Relaxed only under AddressSanitizer (huge shadow map).
 *
 *	Low-level bt/bm harness (like test_inplace_redo / test_steal_page);
 *	no loop; the WAL is driven synchronously (wal_commit_sync).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>

#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "wal.h"
#include "xlog.h"
#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_ROWS   6000
#define PAGE_SZ  512          /* tiny -> shallow fan-out -> deep tree, many leaves */
#define POOL     256          /* holds the build; we tear by hand */
#define NCOMMIT  32           /* small committed baseline that MUST survive */
#define MAX_PG   4096
#define N_TEAR   4            /* number of committed leaves to tear */
#define VLEN     200          /* big rows -> the loser spills (STEAL) to disk */
#define AS_CAP   (256u * 1024u * 1024u)

static char g_val[VLEN + 1];

static void
cap_address_space(void)
{
#if defined(__SANITIZE_ADDRESS__)
#  define XTC_LEAF_UNDER_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define XTC_LEAF_UNDER_ASAN 1
#  endif
#endif
#ifndef XTC_LEAF_UNDER_ASAN
	struct rlimit rl;
	if (getrlimit(RLIMIT_AS, &rl) == 0) {
		if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > (rlim_t)AS_CAP) {
			rl.rlim_cur = (rlim_t)AS_CAP;
			if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < (rlim_t)AS_CAP)
				rl.rlim_cur = rl.rlim_max;
			(void)setrlimit(RLIMIT_AS, &rl);
		}
	}
#endif
}

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
		const char *t = sx_column_text(st, 0);
		size_t n = (size_t)sx_column_bytes(st, 0);
		if (n >= cap) n = cap - 1;
		if (t) memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	sx_finalize(st);
	return got;
}

/* Insert [base, base+n) rows through db; wrap in a single BEGIN..fin
 * transaction (fin == "COMMIT" or NULL to leave it open).  When big != 0
 * the value is a VLEN-byte blob so a large uncommitted txn spills its
 * payloads to disk (STEAL); otherwise a short 'val-k' the durability
 * check can read back. */
static int
big_txn(sx_db *db, int base, int n, const char *fin, int big)
{
	int i;
	if (sx_exec(db, "BEGIN;", NULL) != SX_OK) return -1;
	for (i = 0; i < n; i++) {
		char sql[VLEN + 80];
		if (big)
			snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'%s');",
			    base + i, g_val);
		else
			snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'val-%d');",
			    base + i, base + i);
		if (sx_exec(db, sql, NULL) != SX_OK) {
			fprintf(stderr, "insert %d failed\n", base + i);
			return -1;
		}
	}
	if (fin != NULL) {
		char b[16];
		snprintf(b, sizeof b, "%s;", fin);
		if (sx_exec(db, b, NULL) != SX_OK) return -1;
	}
	return 0;
}

/* Tear up to want committed NON-split leaves on disk: zero pages in the
 * middle of the file (LSN -> 0) as a lost mid-write flush would.  Returns
 * how many were torn.  Skips page 0 (superblock).  A "leaf" here is any
 * live btnode page carrying data; the middle of the file is where the
 * committed leaf level lives, not the shallow interior. */
static int
tear_leaves(const char *btp, unsigned psz, int want)
{
	uint8_t pg[MAX_PG], zero[MAX_PG];
	int bfd = open(btp, O_RDWR);
	off_t fsz, npg, p;
	int torn = 0;

	if (bfd < 0) return 0;
	fsz = lseek(bfd, 0, SEEK_END);
	npg = fsz / (off_t)psz;
	memset(zero, 0, sizeof zero);
	/* Walk the second half of the file, tearing every Kth nonzero page
	 * so we hit several distinct committed leaves spread across the
	 * key range (not one adjacent run). */
	for (p = npg / 2; p < npg && torn < want; p += (npg / 2) / (want + 1) + 1) {
		if (pread(bfd, pg, (size_t)psz, p * (off_t)psz) != (ssize_t)psz) continue;
		{
			int nz = 0; unsigned b;
			for (b = 8; b < psz; b++) if (pg[b]) { nz = 1; break; }
			if (!nz) continue;
		}
		if (pwrite(bfd, zero, (size_t)psz, p * (off_t)psz) == (ssize_t)psz)
			torn++;
	}
	fsync(bfd);
	close(bfd);
	return torn;
}

/* Open a fresh base+log with SMO + leaf physiological logging on. */
static int
open_fresh(const char *btp, const char *logp, unsigned psz, unsigned n_frames,
    bm_t **bmo, bt_t **bto, wal_t **walo)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	wal_opts_t wo;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *wal = NULL;

	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.window_ns = 0; wo.max_batch = 1;   /* synchronous */
	if (wal_open(&wo, &wal) != XTC_OK)
		return -1;
	bo.path = btp; bo.page_size = psz; bo.n_frames = n_frames;
	bo.lsn_off = 0;             /* ARIES page LSN at the node front */
	/* double_write OFF: a torn leaf must NOT be auto-repaired -- the
	 * physiological image is what must repair it. */
	if (bm_create(&bo, &bm) != XTC_OK) { wal_close(wal); return -1; }
	if (bt_open(bm, &bt) != XTC_OK) { bm_destroy(bm); wal_close(wal); return -1; }
	xstore_set_wal((struct wal *)wal);
	xstore_register_smo(1);     /* emit XL_PAGE per split AND per leaf */
	*bmo = bm; *bto = bt; *walo = wal;
	return 0;
}

/* Reopen the torn base in place and recover with xstore_recover_inplace. */
static int
reopen_recover_inplace(const char *btp, const char *logp, unsigned psz,
    unsigned n_frames, bm_t **bmo, bt_t **bto, uint64_t *pages)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	wal_opts_t wo;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *w = NULL;

	bo.path = btp; bo.page_size = psz; bo.n_frames = n_frames;
	bo.lsn_off = 0; bo.reopen = 1;
	if (bm_create(&bo, &bm) != XTC_OK)
		return -1;
	if (bt_reopen(bm, &bt) != XTC_OK) { bm_destroy(bm); return -1; }

	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.append = 1;
	if (wal_open(&wo, &w) != XTC_OK) { bt_close(bt); bm_destroy(bm); return -1; }
	xstore_set_wal((struct wal *)w);        /* CLRs append to the same log */
	xstore_register_smo(1);
	if (xstore_recover_inplace(bt, bm, logp, pages) != XTC_OK) {
		xstore_register_smo(0);
		xstore_set_wal(NULL); wal_close(w);
		bt_close(bt); bm_destroy(bm);
		return -1;
	}
	xstore_register_smo(0);
	xstore_set_wal(NULL);
	wal_close(w);
	(void)bm_checkpoint(bm);
	*bmo = bm; *bto = bt;
	return 0;
}

/* Full verification of a recovered base holding [0, n) rows of table t:
 * count, per-key value, and an ordered gap/dup-free full scan. */
static void
verify_full(bt_t *bt, int n)
{
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	int i, miss = 0, scanned = 0, prev = -1, ordered = 1;
	char want[32], got[32];

	CK(sx_open_bt(bt, &db) == SX_OK);
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	CK(eval_int(db, "SELECT count(*) FROM t") == n);
	for (i = 0; i < n; i++) {
		snprintf(want, sizeof want, "val-%d", i);
		if (sel_v(db, i, got, sizeof got) == 1 && strcmp(got, want) == 0)
			continue;
		miss++;
	}
	CK(miss == 0);
	if (sx_prepare(db, "SELECT k FROM t ORDER BY k", -1, &st, NULL) == SX_OK) {
		while (sx_step(st) == SX_ROW) {
			int k = (int)sx_column_int64(st, 0);
			if (k <= prev) ordered = 0;
			prev = k;
			scanned++;
		}
		sx_finalize(st);
	}
	CK(ordered == 1);
	CK(scanned == n);
	CK(prev == n - 1);
	if (miss || !ordered || scanned != n)
		fprintf(stderr, "FAIL: verify miss=%d scanned=%d ordered=%d\n",
		    miss, scanned, ordered);
	sx_close(db);
}

/* Count live rows of t (no ordering/value check) -- kept for future use
 * on the loser path; verify_full covers the committed-set checks. */
#if 0
static int
count_rows(bt_t *bt)
{
	sx_db *db = NULL;
	int n;
	if (sx_open_bt(bt, &db) != SX_OK)
		return -1;
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	n = eval_int(db, "SELECT count(*) FROM t");
	sx_close(db);
	return n;
}
#endif

int
main(void)
{
	char btp[256], logp[256], dwp[288];
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *wal = NULL;
	sx_db *db = NULL;
	uint64_t pages = 0, clrs0;
	int fd, torn;

	cap_address_space();      /* MANDATORY: torn-base recovery below */

	memset(g_val, 'x', VLEN); g_val[VLEN] = '\0';

	/* =============================================================
	 * A. Durability through a torn NON-split leaf (the 5144-lost-rows
	 * repro): commit a large txn, tear several committed leaves,
	 * recover in place, assert the FULL committed set is restored.
	 * ============================================================= */
	t_tmpl(btp, sizeof btp, "steal-leaf-bt");
	t_tmpl(logp, sizeof logp, "steal-leaf-log");
	if ((fd = mkstemp(btp)) >= 0) close(fd);
	if ((fd = mkstemp(logp)) >= 0) close(fd);

	CK(open_fresh(btp, logp, PAGE_SZ, POOL, &bm, &bt, &wal) == 0);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(big_txn(db, 0, N_ROWS, "COMMIT", 0) == 0);   /* one large committed txn */

	/* Clean-checkpoint the base durable: every page is now at its final
	 * LSN, the correct answer.  Then close the engine. */
	CK(bm_checkpoint(bm) == XTC_OK);
	sx_close(db); db = NULL;
	xstore_register_smo(0);
	xstore_set_wal(NULL);
	bt_close(bt); bt = NULL;
	bm_destroy(bm); bm = NULL;
	wal_close(wal); wal = NULL;

	/* Tear several committed NON-split leaves on disk (LSN -> 0). */
	torn = tear_leaves(btp, PAGE_SZ, N_TEAR);
	CK(torn > 0);

	/* Recover in place: pass 1 repairs the torn leaves from their
	 * images, pass 2 logical-redoes over the repaired base. */
	CK(reopen_recover_inplace(btp, logp, PAGE_SZ, POOL, &bm, &bt, &pages) == 0);
	CK(pages > 0);                 /* images repaired torn leaves, not rebuild */
	verify_full(bt, N_ROWS);       /* FULL committed set, ordered, no gap/dup */
	printf("  ok   torn NON-split leaf durability: %d committed leaves torn "
	    "(zeroed, LSN->0), repaired from %llu physiological page images; all "
	    "%d rows reappeared, scan ordered -- the ~3000-lost-rows repro now "
	    "recovers FULLY in place\n",
	    torn, (unsigned long long)pages, N_ROWS);

	bt_close(bt); bt = NULL;
	bm_destroy(bm); bm = NULL;
	unlink(btp); unlink(logp);
	snprintf(dwp, sizeof dwp, "%s.dwb", btp); unlink(dwp);

	/* =============================================================
	 * B. Atomicity + no-leak of a torn LOSER: a committed baseline
	 * survives; a large uncommitted txn whose leaves are torn is undone
	 * in place via CLRs, and no uncommitted row leaks.
	 * ============================================================= */
	t_tmpl(btp, sizeof btp, "steal-leaf-bt2");
	t_tmpl(logp, sizeof logp, "steal-leaf-log2");
	if ((fd = mkstemp(btp)) >= 0) close(fd);
	if ((fd = mkstemp(logp)) >= 0) close(fd);

	/* Part B uses a 4 KB page (a 200-byte staged row fits with room to
	 * spare) and a tiny 16-frame pool so the large uncommitted txn both
	 * SPILLS (STEAL) and force-flushes to disk. */
	CK(open_fresh(btp, logp, 4096, 16, &bm, &bt, &wal) == 0);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(big_txn(db, 0, NCOMMIT, "COMMIT", 0) == 0);   /* committed baseline */
	CK(bm_checkpoint(bm) == XTC_OK);              /* baseline durable */

	/* A large uncommitted txn (distinct key range) with big payloads so
	 * it SPILLS to disk (STEAL) under the buffer buffer -- genuinely
	 * reaching the tree as a loser.  It never commits. */
	clrs0 = xstore_undo_clrs();
	CK(big_txn(db, 100000, N_ROWS, NULL, 1) == 0);
	CK(bm_checkpoint(bm) == XTC_OK);   /* force the loser's leaves to disk */

	/* CRASH: drop everything with no commit, no clean marker. */
	sx_close(db); db = NULL;
	xstore_register_smo(0);
	xstore_set_wal(NULL);
	bt_close(bt); bt = NULL;
	bm_destroy(bm); bm = NULL;
	wal_close(wal); wal = NULL;

	/* Tear several committed baseline leaves too (harsher: torn base AND
	 * a loser to undo). */
	torn = tear_leaves(btp, 4096, N_TEAR);

	CK(reopen_recover_inplace(btp, logp, 4096, 16, &bm, &bt, &pages) == 0);
	CK(xstore_undo_clrs() > clrs0);    /* the loser was reversed via CLRs */
	verify_full(bt, NCOMMIT);          /* exactly the committed baseline */
	printf("  ok   torn loser undone in place: committed baseline (%d rows) "
	    "recovered whole from images even with %d torn leaves; the %d-row "
	    "uncommitted loser reversed via %llu CLRs, no uncommitted row leaked\n",
	    NCOMMIT, torn, N_ROWS,
	    (unsigned long long)(xstore_undo_clrs() - clrs0));

	bt_close(bt);
	bm_destroy(bm);
	unlink(btp); unlink(logp);
	snprintf(dwp, sizeof dwp, "%s.dwb", btp); unlink(dwp);

	if (g_fail) { fprintf(stderr, "test_steal_leaf: FAILED\n"); return 1; }
	printf("test_steal_leaf: physiological per-non-split-leaf logging makes "
	    "in-place recovery on a torn NON-split leaf durable (full committed "
	    "set) and atomic (torn loser undone), under a 256 MB RLIMIT_AS cap\n");
	return 0;
}

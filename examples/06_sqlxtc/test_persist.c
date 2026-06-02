/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_persist.c
 *	Persistence primitives: a checkpointed B-tree survives a clean
 *	restart (superblock + non-truncating reopen), and the WAL can be
 *	truncated once its effects are durable.  Off a loop -- demand
 *	eviction with synchronous I/O.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "wal.h"
#include "xtc.h"

#define PAGE_SZ  4096
#define N_KEYS   3000      /* > pool: forces paging, multi-level tree, splits */

static int g_fail;
#define CK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#c); g_fail=1; } } while (0)

/* ---- (1) checkpoint + clean reopen ---- */
static int
scenario_reopen(void)
{
	char path[] = "/tmp/sqlxtc-persist-XXXXXX";
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	int fd, k, ok = 1;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);

	/* Build a tree larger than the pool, then checkpoint it durable. */
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 32; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	for (k = 0; ok && k < N_KEYS; k++) {
		char key[24], val[24];
		snprintf(key, sizeof key, "k-%06d", k);
		snprintf(val, sizeof val, "v-%06d", k);
		if (bt_insert(bt, key, (uint16_t)strlen(key), val,
		    (uint16_t)strlen(val)) != XTC_OK) { ok = 0; break; }
	}
	CK(ok);
	CK(bm_checkpoint(bm) == XTC_OK);     /* flush all + fdatasync */
	bt_close(bt);
	bm_destroy(bm);                      /* closes the fd; file persists */

	/* Reopen the SAME file WITHOUT truncating; find the root via the
	 * superblock; every key must still be there. */
	{
		bm_opts_t b2 = BM_OPTS_DEFAULT;
		bm_t *bm2 = NULL;
		bt_t *bt2 = NULL;
		int miss = 0;
		b2.path = path; b2.page_size = PAGE_SZ; b2.n_frames = 32;
		b2.cool_pct = 25; b2.reopen = 1;
		CK(bm_create(&b2, &bm2) == XTC_OK);
		CK(bt_reopen(bm2, &bt2) == XTC_OK);
		for (k = 0; k < N_KEYS; k++) {
			char key[24], want[24], got[24];
			uint16_t vlen = 0;
			snprintf(key, sizeof key, "k-%06d", k);
			snprintf(want, sizeof want, "v-%06d", k);
			if (bt_lookup(bt2, key, (uint16_t)strlen(key), got, sizeof got,
			    &vlen) != XTC_OK || vlen != strlen(want) ||
			    memcmp(got, want, vlen) != 0)
				miss++;
		}
		CK(miss == 0);
		bt_close(bt2);
		bm_destroy(bm2);
	}
	unlink(path);
	if (g_fail) return 1;
	printf("  ok   %d-key B-tree checkpointed, reopened from its superblock "
	    "after a clean restart, all keys intact\n", N_KEYS);
	return 0;
}

/* ---- (2) wal_truncate ---- */
static int
g_rec_count;
static int
count_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	(void)lsn; (void)rec; (void)len; (void)user;
	g_rec_count++;
	return 0;
}
static int
scenario_wal_truncate(void)
{
	char path[] = "/tmp/sqlxtc-persist-wal-XXXXXX";
	wal_opts_t wo = { 0 };
	wal_t *w = NULL;
	int fd, i;
	uint64_t lsn = 0;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	wo.path = path;
	CK(wal_open(&wo, &w) == XTC_OK);
	for (i = 0; i < 10; i++)
		CK(wal_commit_sync(w, "rec", 3, &lsn) == XTC_OK);   /* off-loop append */
	g_rec_count = 0; CK(wal_scan(path, count_cb, NULL) == XTC_OK);
	CK(g_rec_count == 10);

	CK(wal_truncate(w) == XTC_OK);
	g_rec_count = 0; CK(wal_scan(path, count_cb, NULL) == XTC_OK);
	CK(g_rec_count == 0);                /* log is empty after truncation */

	for (i = 0; i < 4; i++)
		CK(wal_commit_sync(w, "rec2", 4, &lsn) == XTC_OK);
	g_rec_count = 0; CK(wal_scan(path, count_cb, NULL) == XTC_OK);
	CK(g_rec_count == 4);                /* new records after truncation scan */
	CK(lsn > 10);                        /* LSNs keep advancing, never repeat */

	wal_close(w);
	unlink(path);
	if (g_fail) return 1;
	printf("  ok   WAL truncation: 10 records -> truncate -> 0 -> 4 new "
	    "records, LSNs monotonic across the truncation\n");
	return 0;
}

int
main(void)
{
	if (scenario_reopen() != 0) return 1;
	if (scenario_wal_truncate() != 0) return 1;
	printf("All sqlxtc persistence tests passed.\n");
	return 0;
}

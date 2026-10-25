/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_page_cksum_more.c
 *	The page checksum against the damage it exists for, and the
 *	recovery around it.  Each case builds a store of ROWS rows, closes
 *	it cleanly (so the next open trusts the page file and skips the
 *	log), damages the page file, and reopens:
 *	  torn        the second half of the victim row's page is the
 *	              page's OLD image (a torn write);
 *	  misdirect   the victim row's page holds another page's bytes (a
 *	              misdirected write: an intact page at the wrong place);
 *	  repair      after a refused read: a write is refused too, the
 *	              close does not mark the base clean, and the next open
 *	              rebuilds from the log -- every row correct again;
 *	  superblock  page 0 damaged: the open rebuilds from the log;
 *	  oldformat   a store whose superblock says "XBT1" (1.59, no page
 *	              checksums) opens and serves every row (rebuilt from
 *	              the log, which 1.60 reads unchanged);
 *	  fence       a leaf's right link and upper fence damaged: 1.59
 *	              livelocked in move_right, now the read fails at once.
 *	The run is bounded by alarm(120): a hang is a failure, not a wait.
 *	A refused read must be an error naming the checksum, never data.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "t_tmp.h"

#define ROWS   400
#define VICTIM 123
#define PS     4096

static char g_path[256], g_wal[300];

static void
fill(void)
{
	sx_db *h = NULL;
	char sql[160];
	int i;

	CK(sx_storage_open(g_path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT)",
	    NULL) == SX_OK);
	for (i = 0; i < ROWS; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, "
		    "'ROW%05d-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA')", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	CK(sx_storage_close() == SX_OK);
}

/* The page holding the victim row's value in the page file (-1 if none). */
static off_t
victim_page(const char *tagfmt, int k)
{
	char tag[32], *buf;
	struct stat sb;
	off_t o, at = -1;
	int fd;

	snprintf(tag, sizeof tag, tagfmt, k);
	if ((fd = open(g_path, O_RDONLY)) < 0)
		return -1;
	if (fstat(fd, &sb) != 0 || (buf = malloc((size_t)sb.st_size)) == NULL) {
		close(fd);
		return -1;
	}
	if (pread(fd, buf, (size_t)sb.st_size, 0) == sb.st_size)
		for (o = 0; o + (off_t)strlen(tag) < sb.st_size; o++)
			if (memcmp(buf + o, tag, strlen(tag)) == 0) {
				at = o / PS;
				break;
			}
	free(buf);
	close(fd);
	return at;
}

static int
rw_page(off_t pg, void *buf, int wr)
{
	int fd = open(g_path, O_RDWR), ok;

	if (fd < 0)
		return -1;
	ok = wr ? pwrite(fd, buf, PS, pg * PS) == PS :
	    pread(fd, buf, PS, pg * PS) == PS;
	if (wr)
		(void)fsync(fd);
	close(fd);
	return ok ? 0 : -1;
}

/* SELECT v of row k: 1 = the right value, 0 = refused with an error that
 * names the checksum, -1 = anything else (wrong data, other error). */
static int
read_row(sx_db *h, int k)
{
	char sql[80], want[64];
	const char *v, *e;
	sx_stmt *st = NULL;
	int rc, r = -1;

	snprintf(sql, sizeof sql, "SELECT v FROM t WHERE k = %d", k);
	snprintf(want, sizeof want,
	    "ROW%05d-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", k);
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -1;
	rc = sx_step(st);
	if (rc == SX_ROW) {
		v = sx_column_text(st, 0);
		if (v != NULL && strcmp(v, want) == 0)
			r = 1;
		else
			fprintf(stderr, "  row %d read back as %s\n", k,
			    v ? v : "(null)");
	} else if (rc == SX_ERROR) {
		e = sx_errmsg(h);
		if (strstr(e, "checksum") != NULL)
			r = 0;
		else
			fprintf(stderr, "  row %d: error %s\n", k, e);
	}
	sx_finalize(st);
	return r;
}

static int
all_rows_ok(void)
{
	sx_db *h = NULL;
	int i, bad = 0;

	if (sx_open(":memory:", &h) != SX_OK)
		return 0;
	for (i = 0; i < ROWS; i++)
		if (read_row(h, i) != 1)
			bad++;
	sx_close(h);
	return bad == 0;
}

static void
setup(const char *name)
{
	int fd;

	t_tmpl(g_path, sizeof g_path, name);
	if ((fd = mkstemp(g_path)) >= 0)
		close(fd);
	snprintf(g_wal, sizeof g_wal, "%s-wal", g_path);
	fill();
}

static void
teardown(void)
{
	(void)sx_storage_close();
	unlink(g_path);
	unlink(g_wal);
}

/* Reopen the damaged store, read the victim: it must be refused. */
static void
expect_refused(const char *what)
{
	sx_db *h = NULL;
	struct bm_stats bs;

	CK(sx_storage_open(g_path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	if (h == NULL)
		return;
	CK(read_row(h, VICTIM) == 0);
	sx_storage_bm_stats(&bs);
	CK(bs.cksum_failures >= 1);
	printf("  %s: refused (%llu checksum failures)\n", what,
	    (unsigned long long)bs.cksum_failures);
	sx_close(h);
}

static void
t_torn(void)
{
	static unsigned char cur[PS], old[PS];
	sx_db *h = NULL;
	char sql[160];
	off_t pg;
	int i, d;

	setup("cktorn");
	CK((pg = victim_page("ROW%05d-", VICTIM)) > 0);
	if (pg <= 0) { teardown(); return; }
	CK(rw_page(pg, old, 0) == 0);     /* the page's image before ... */
	CK(sx_storage_open(g_path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	for (i = VICTIM - 3; h != NULL && i <= VICTIM + 3; i++) {
		snprintf(sql, sizeof sql, "UPDATE t SET v = 'ROW%05d-"
		    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' WHERE k = %d", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	CK(rw_page(pg, cur, 0) == 0);     /* ... and after some updates */
	/* Torn at a 512-byte sector boundary: the new image up to it, the
	 * old one after it -- where the two differ (the updates change the
	 * slots and the low end of the heap, not the whole page). */
	for (d = PS - 1; d >= 0 && old[d] == cur[d]; d--)
		;
	CK(d >= 0);
	if (d >= 512)
		memcpy(cur + (d & ~511), old + (d & ~511), PS - (size_t)(d & ~511));
	else
		memcpy(cur, old, 512);       /* the first sector did not land */
	CK(rw_page(pg, cur, 1) == 0);
	expect_refused("torn write");
	teardown();
}

static void
t_misdirect(void)
{
	static unsigned char a[PS];
	off_t pa, pb;

	setup("ckmisdir");
	CK((pb = victim_page("ROW%05d-", VICTIM)) > 0);
	CK((pa = victim_page("ROW%05d-", 10)) > 0);
	CK(pa != pb);
	if (pa <= 0 || pb <= 0 || pa == pb) { teardown(); return; }
	/* Page A's whole, valid image written at page B's offset. */
	CK(rw_page(pa, a, 0) == 0);
	CK(rw_page(pb, a, 1) == 0);
	expect_refused("misdirected write");
	teardown();
}

static void
t_repair(void)
{
	static unsigned char p[PS];
	sx_db *h = NULL;
	struct bm_stats bs;
	off_t pg;

	setup("ckrepair");
	CK((pg = victim_page("ROW%05d-", VICTIM)) > 0);
	if (pg <= 0) { teardown(); return; }
	CK(rw_page(pg, p, 0) == 0);
	p[PS - 100] ^= 0x40;              /* one bit, somewhere in the page */
	CK(rw_page(pg, p, 1) == 0);

	CK(sx_storage_open(g_path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	if (h == NULL) { teardown(); return; }
	CK(read_row(h, VICTIM) == 0);
	/* Fail-stopped: a write is refused, with the checksum named. */
	CK(sx_exec(h, "INSERT INTO t VALUES(100000, 'x')", NULL) != SX_OK);
	CK(strstr(sx_errmsg(h), "checksum") != NULL);
	sx_close(h);
	h = NULL;
	/* The close must not mark this base clean (an error is expected). */
	(void)sx_storage_close();

	/* The restart rebuilds from the log: every row is right again. */
	CK(sx_storage_open(g_path, 256) == SX_OK);
	sx_storage_bm_stats(&bs);
	CK(bs.cksum_failures == 0);
	CK(all_rows_ok());
	CK(sx_open(":memory:", &h) == SX_OK);
	if (h != NULL) {
		CK(sx_exec(h, "INSERT INTO t VALUES(100000, 'x')", NULL) ==
		    SX_OK);
		sx_close(h);
	}
	CK(sx_storage_close() == SX_OK);
	printf("  repair: write refused after the damage; restart rebuilt "
	    "all %d rows\n", ROWS);
	teardown();
}

static void
t_superblock(void)
{
	static unsigned char p[PS];

	setup("cksuper");
	CK(rw_page(0, p, 0) == 0);
	p[20] ^= 0x01;                    /* the height word of the superblock */
	CK(rw_page(0, p, 1) == 0);
	CK(sx_storage_open(g_path, 256) == SX_OK);
	CK(all_rows_ok());
	CK(sx_storage_close() == SX_OK);
	printf("  superblock: damaged page 0 -> rebuilt from the log\n");
	teardown();
}

static void
t_oldformat(void)
{
	static unsigned char p[PS];
	struct stat sb;
	off_t n, i;

	setup("ckold");
	/* A 1.59 store: superblock "XBT1" with no checksum slot, 40-byte
	 * node headers.  This build never reads its data pages (they are
	 * zeroed here to prove it): it rebuilds the page file from the log,
	 * whose format is unchanged. */
	CK(stat(g_path, &sb) == 0);
	n = sb.st_size / PS;
	CK(rw_page(0, p, 0) == 0);
	memcpy(p, "1TBX", 4);             /* 0x58425431, little-endian */
	memset(p + PS - 40, 0, 8);        /* no checksum slot in 1.59 */
	CK(rw_page(0, p, 1) == 0);
	memset(p, 0, PS);
	for (i = 1; i < n; i++)
		CK(rw_page(i, p, 1) == 0);
	CK(sx_storage_open(g_path, 256) == SX_OK);
	CK(all_rows_ok());
	CK(sx_storage_close() == SX_OK);
	/* The rebuilt store is a trusted 1.60 one from then on: with the
	 * log gone, the rows come from the page file alone. */
	unlink(g_wal);
	CK(sx_storage_open(g_path, 256) == SX_OK);
	CK(all_rows_ok());
	CK(sx_storage_close() == SX_OK);
	printf("  oldformat: a 1.59 store opens (rebuilt from the log), and "
	    "is trusted after its first clean close\n");
	teardown();
}

/* A leaf's routing bytes damaged: its right link and the length of its
 * upper fence.  1.59 livelocked here (move_right followed the bad link
 * forever, test hung for hours); the checksum refuses the page before
 * any routing reads it, so the read fails at once. */
static void
t_fence(void)
{
	static unsigned char p[PS];
	off_t pg;
	uint32_t rs;
	uint16_t hl;

	setup("ckfence");
	CK((pg = victim_page("ROW%05d-", VICTIM)) > 0);
	if (pg <= 0) { teardown(); return; }
	CK(rw_page(pg, p, 0) == 0);
	memcpy(&rs, p + 12, 4);           /* btnode_hdr.right_sibling */
	rs = rs == 0 ? (uint32_t)pg : 0;  /* point it back at itself, or cut it */
	memcpy(p + 12, &rs, 4);
	memcpy(&hl, p + 32, 2);           /* btnode_hdr.hi_fence_len */
	hl ^= 0x0101;
	memcpy(p + 32, &hl, 2);
	CK(rw_page(pg, p, 1) == 0);
	expect_refused("fence + right link");
	teardown();
}

int
main(void)
{
	/* A damaged page must fail a read, not hang it: bound the whole run
	 * (all six cases take well under a second here). */
	alarm(120);
	CK(sx_init() == SX_OK);
	t_torn();
	t_misdirect();
	t_repair();
	t_superblock();
	t_oldformat();
	t_fence();
	if (!g_fail)
		printf("ok test_page_cksum_more\n");
	return g_fail;
}

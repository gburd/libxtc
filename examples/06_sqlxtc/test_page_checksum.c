/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_page_checksum.c
 *	A corrupted data page must not be returned as committed data.
 *
 *	A clean close marks the page file trusted, and the next open reads
 *	it without replaying the log.  Data pages carry an LSN but no
 *	checksum, so a single bad byte in the page file -- a latent sector
 *	error, a misdirected or torn write that landed after the clean
 *	close -- is read back as if it were committed.  This test:
 *	  1. writes ROWS rows with a known value, closes cleanly;
 *	  2. flips one byte inside one row's value in the page file;
 *	  3. reopens and reads that row.
 *	PASS: the read reports an error, or the value is intact (the
 *	engine detected the damage and repaired it from the log).
 *	FAIL: the corrupted bytes come back as the row's value.
 *
 *	EXPECTED TO FAIL on main (1.59): no page checksum.  Not part of
 *	`make test` until page checksums land; run with
 *	`make test-page-checksum`.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "t_tmp.h"

#define ROWS   200
#define VICTIM 123

int
main(void)
{
	char path[256], wal[300], sql[160], tag[32], *buf = NULL;
	const char *v;
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	struct stat sb;
	off_t o, at = -1;
	int i, fd, rc, copies = 0;

	t_tmpl(path, sizeof path, "pagecksum");
	if ((fd = mkstemp(path)) >= 0)
		close(fd);
	snprintf(wal, sizeof wal, "%s-wal", path);

	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT)",
	    NULL) == SX_OK);
	for (i = 0; i < ROWS; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, "
		    "'ROW%05d-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA')", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	h = NULL;
	CK(sx_storage_close() == SX_OK);
	if (g_fail)
		goto out;

	/* One bad byte in the victim row's value, on disk. */
	snprintf(tag, sizeof tag, "ROW%05d-", VICTIM);
	CK((fd = open(path, O_RDWR)) >= 0);
	CK(fstat(fd, &sb) == 0);
	CK((buf = malloc((size_t)sb.st_size)) != NULL);
	if (g_fail)
		goto out;
	CK(pread(fd, buf, (size_t)sb.st_size, 0) == sb.st_size);
	for (o = 0; o + 64 < sb.st_size; o++)
		if (memcmp(buf + o, tag, strlen(tag)) == 0) {
			at = o + (off_t)strlen(tag) + 7;
			copies++;
		}
	CK(copies == 1);
	if (at >= 0)
		CK(pwrite(fd, "B", 1, at) == 1);
	(void)fsync(fd);
	close(fd);
	if (g_fail)
		goto out;

	/* Trusted reopen, read the victim. */
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_prepare(h, "SELECT v FROM t WHERE k = 123", -1, &st, NULL) ==
	    SX_OK);
	if (g_fail)
		goto out;
	rc = sx_step(st);
	if (rc == SX_ROW) {
		v = sx_column_text(st, 0);
		if (v != NULL && strchr(v, 'B') != NULL) {
			fprintf(stderr, "FAIL test_page_checksum: one corrupted "
			    "byte in the page file was returned as committed "
			    "data: %s\n", v);
			g_fail = 1;
		} else
			printf("value intact after corruption: %s\n",
			    v ? v : "(null)");
	} else
		printf("corrupted page refused (rc %d)\n", rc);
	sx_finalize(st);
	st = NULL;
out:
	if (st != NULL)
		sx_finalize(st);
	if (h != NULL)
		sx_close(h);
	(void)sx_storage_close();
	free(buf);
	unlink(path);
	unlink(wal);
	if (!g_fail)
		printf("ok test_page_checksum\n");
	return g_fail;
}

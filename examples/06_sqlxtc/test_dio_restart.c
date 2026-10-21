/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_dio_restart.c
 *	A clean shutdown of a DIRECT-I/O store (--storage-direct) must
 *	leave a trusted base, as a buffered one does (test_clean_restart).
 *	The superblock was read and written as 40 bytes from a stack buffer
 *	at offset 0 on the O_DIRECT fd: Linux refuses unaligned direct I/O
 *	(EINVAL), so the clean marker never reached the file and every open
 *	of a direct store rebuilt from the log.  Populate, close cleanly,
 *	delete the log, reopen: the rows must still be there.  Skips where
 *	the filesystem cannot do direct I/O (tmpfs).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "t_tmp.h"

static int
count_rows(void)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	int n = -1;
	if (sx_open(":memory:", &h) != SX_OK)
		return -1;
	(void)sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	if (sx_prepare(h, "SELECT count(*) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW) n = (int)sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(h);
	return n;
}

/* Can this filesystem do direct I/O on a fresh file next to `path`? */
static int
direct_ok(const char *path)
{
#if defined(O_DIRECT)
	char p[300];
	void *buf = NULL;
	int fd, ok;
	snprintf(p, sizeof p, "%s.probe", path);
	fd = open(p, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
	if (fd < 0) return 0;
	ok = posix_memalign(&buf, 4096, 4096) == 0 &&
	    (memset(buf, 0, 4096), pwrite(fd, buf, 4096, 0) == 4096);
	free(buf);
	close(fd);
	unlink(p);
	return ok;
#else
	(void)path;
	return 0;
#endif
}

int
main(void)
{
	char path[256], wal[300], dwb[300];
	sx_db *h = NULL;
	int i, fd, n, fail = 0;

	t_tmpl(path, sizeof path, "sqlxtc-diorst");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	snprintf(wal, sizeof wal, "%s-wal", path);
	snprintf(dwb, sizeof dwb, "%s.dwb", path);
	if (!direct_ok(path)) {
		printf("SKIP: filesystem does not support direct I/O here\n");
		unlink(path);
		return 0;
	}
	if (sx_init() != SX_OK) return 1;
	sx_storage_set_io(1, 0);
	if (sx_storage_open(path, 64) != SX_OK) { fprintf(stderr, "FAIL: open\n"); return 1; }
	if (sx_open(":memory:", &h) != SX_OK) return 1;
	(void)sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	for (i = 0; i < 50; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'row%d');", i, i);
		if (sx_exec(h, sql, NULL) != SX_OK) fail = 1;
	}
	sx_close(h);
	if (sx_storage_close() != SX_OK) {
		fprintf(stderr, "FAIL: clean close of a direct store returned an error\n");
		fail = 1;
	}
	unlink(wal);                       /* only a trusted base can serve now */
	if (sx_storage_open(path, 64) != SX_OK) {
		fprintf(stderr, "FAIL: reopen\n");
		fail = 1;
	}
	n = count_rows();
	printf("  rows after a clean close, a deleted log and a direct reopen: %d (want 50)\n", n);
	if (n != 50) fail = 1;
	(void)sx_storage_close();
	sx_storage_set_io(0, 0);
	(void)sx_shutdown();
	unlink(path); unlink(wal); unlink(dwb);
	if (fail) { fprintf(stderr, "FAIL: test_dio_restart\n"); return 1; }
	printf("  ok   a cleanly closed direct-I/O store reopens trusted\n");
	return 0;
}

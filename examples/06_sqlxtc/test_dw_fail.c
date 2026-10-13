/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_dw_fail.c
 *	Double-write and shutdown I/O errors must not be ignored.
 *
 *	1. The double-write slot cannot be written (its fd is swapped for a
 *	   read-only one): flush_frame and bm_checkpoint must NOT write the
 *	   home page (it would be unprotected against a torn write), must
 *	   keep it dirty, and bm_checkpoint must report the failure.
 *	2. Clean shutdown after a failed page flush must NOT mark the base
 *	   clean: the next open must replay the log, so every committed row
 *	   is still there even though the base never got it.
 *
 *	Before the fix dw_protect ignored its write/fsync results, the
 *	home page was written anyway, bm_checkpoint returned XTC_OK, and
 *	sx_storage_close set the clean flag after a failed flush.
 */
#include "bufmgr.h"
#include "bufmgr.c"
#include "engine.h"
#define T_NO_GFAIL
#include "t_tmp.h"
static int g_fail;

#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", \
	__FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int
test_dw_slot_failure(void)
{
	bm_opts_t o = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bm_frame_t *f;
	bm_pid_t pid;
	char path[256], dwp[300];
	unsigned char got = 0;
	int fd, saved, ro;

	t_tmpl(path, sizeof path, "sqlxtc-dwfail");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	snprintf(dwp, sizeof dwp, "%s.dwb", path);
	o.path = path; o.n_frames = 8; o.double_write = 1;
	if (bm_create(&o, &bm) != XTC_OK) return 1;
	if (bm_alloc_pid(bm, &f, &pid) != XTC_OK) return 1;
	((unsigned char *)bm_page(f))[0] = 0x5a;
	bm_unfix(bm, f, 1);

	saved = bm->dw_fd;
	if ((ro = open(dwp, O_RDONLY)) < 0) return 1;
	bm->dw_fd = ro;                       /* slot writes now fail (EBADF) */
	CHECK(flush_frame(bm, f) == 0);       /* not clean */
	CHECK(atomic_load(&f->dirty) == 1);   /* still dirty */
	CHECK(bm_checkpoint(bm) != XTC_OK);   /* failure reported */
	CHECK(pread(bm->fd, &got, 1, (off_t)pid * bm->page_size) != 1 ||
	    got != 0x5a);                     /* home page NOT written */
	bm->dw_fd = saved;
	close(ro);
	CHECK(bm_checkpoint(bm) == XTC_OK);   /* retry succeeds */
	CHECK(pread(bm->fd, &got, 1, (off_t)pid * bm->page_size) == 1 &&
	    got == 0x5a);
	bm_destroy(bm);
	unlink(path); unlink(dwp);
	printf("  dw: slot failure blocks the home write, keeps the page dirty,"
	    " and fails the checkpoint (%s)\n", g_fail ? "FAIL" : "ok");
	return 0;
}

static int
count_rows(void)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	int n = -1;
	if (sx_open(":memory:", &h) != SX_OK) return -1;
	if (sx_prepare(h, "SELECT count(*) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW) n = (int)sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(h);
	return n;
}

static int
test_shutdown_failure(void)
{
	char path[256], sql[96], side[300];
	sx_db *h = NULL;
	struct stat sb;
	struct rlimit rl;
	int fd, i, rows;

	t_tmpl(path, sizeof path, "sqlxtc-shutfail");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	CHECK(sx_storage_open(path, 64) == SX_OK);
	CHECK(sx_open(":memory:", &h) == SX_OK);
	CHECK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	for (i = 0; i < 40; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'r%d')", i, i);
		CHECK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	/* Make every page write that grows a file fail (EFBIG): cap the
	 * file size at the page file's current size.  Nothing was evicted,
	 * so every dirty page lies past EOF and its flush fails, while the
	 * superblock rewrite at offset 0 would still succeed -- exactly the
	 * case where marking the base clean loses the data. */
	snprintf(side, sizeof side, "%s.dwb", path);
	CHECK(stat(path, &sb) == 0);
	rl.rlim_cur = (rlim_t)sb.st_size;
	rl.rlim_max = RLIM_INFINITY;
	CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);
	CHECK(sx_storage_close() != SX_OK);   /* failure reported */
	rl.rlim_cur = RLIM_INFINITY;
	CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);
	/* The base was never written, so only a log replay can show the
	 * rows: a "clean" marker would make the reopen trust an empty base. */
	CHECK(sx_storage_open(path, 64) == SX_OK);
	rows = count_rows();
	CHECK(rows == 40);
	(void)sx_storage_close();
	printf("  shutdown: failed flush -> not marked clean -> reopen replays"
	    " the log (%d/40 rows) (%s)\n", rows, g_fail ? "FAIL" : "ok");
	unlink(path); unlink(side);
	snprintf(side, sizeof side, "%s-wal", path); unlink(side);
	return 0;
}

int
main(void)
{
	signal(SIGXFSZ, SIG_IGN);
	if (test_dw_slot_failure() != 0) { fprintf(stderr, "FAIL: dw setup\n"); return 1; }
	CHECK(sx_init() == SX_OK);
	if (test_shutdown_failure() != 0) { fprintf(stderr, "FAIL: shutdown setup\n"); return 1; }
	(void)sx_shutdown();
	if (g_fail) { fprintf(stderr, "FAIL: test_dw_fail\n"); return 1; }
	printf("  ok   double-write and shutdown I/O errors propagate; the"
	    " clean marker is never set after a failure\n");
	return 0;
}

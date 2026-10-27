/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_page_bytes.c
 *	A closed store's page file holds no memory it was not meant to.
 *	Every B-tree node rebuild (compaction, split, merge) built into a
 *	stack buffer whose header alone was zeroed, so the gap between a
 *	page's slot array and its cell heap carried whatever that stack held
 *	-- other rows' keys and values, pointers -- into the page file and
 *	the log; a removed entry's bytes stayed in its page too.  Runs a
 *	workload with every rebuild path (inserts that split, a secondary
 *	index, updates, deletes that merge, version GC at the clean close),
 *	closes the store cleanly, and reads every page of the file: every
 *	B-tree node page must have zero bytes outside its header, slot
 *	array, fences and live cells (t_pagebytes.h).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btnode.h"
#include "engine.h"
#include "t_pagebytes.h"
#include "t_tmp.h"

#define PS   4096
#define NROW 6000

static int g_fail;
#define TCK(c, ...) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
	g_fail = 1; } } while (0)

/* Leave non-zero bytes in the stack the engine's frames reuse. */
static void __attribute__((noinline))
poison_stack(void)
{
	volatile unsigned char junk[128 * 1024];
	size_t i;

	for (i = 0; i < sizeof junk; i++)
		junk[i] = 0xA5;
}

static void
exec(sx_db *h, const char *sql)
{
	poison_stack();
	TCK(sx_exec(h, sql, NULL) == SX_OK, "%s: %s", sql, sx_errmsg(h));
}

int
main(void)
{
	char path[256], wal[300], sql[256];
	sx_db *h = NULL;
	struct stat st;
	uint8_t pg[PS];
	off_t n, i;
	long dirty = 0, bad_pages = 0, nodes = 0;
	int fd, r;

	t_tmpl(path, sizeof path, "sqlxtc-pagebytes");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	TCK(sx_init() == SX_OK, "init");
	TCK(sx_storage_open(path, 2048) == SX_OK, "open");
	TCK(sx_open(":memory:", &h) == SX_OK, "sx_open");
	exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, g INTEGER, v TEXT)");
	exec(h, "CREATE INDEX t_g ON t(g)");
	for (r = 0; r < NROW; r++) {
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, %d, "
		    "'secret-%08d-xxxxxxxxxxxxxxxxxxxxxxxx')", (r * 7919) % NROW,
		    r % 37, r);
		exec(h, sql);
	}
	for (r = 0; r < NROW; r += 3) {
		snprintf(sql, sizeof sql, "UPDATE t SET g = %d WHERE k = %d",
		    r % 11, r);
		exec(h, sql);
	}
	for (r = 0; r < NROW; r++) {
		if (r % 5 == 0)
			continue;
		snprintf(sql, sizeof sql, "DELETE FROM t WHERE k = %d", r);
		exec(h, sql);
	}
	sx_close(h);
	TCK(sx_storage_close() == SX_OK, "close");

	if ((fd = open(path, O_RDONLY)) < 0 || fstat(fd, &st) != 0)
		return 1;
	n = st.st_size / PS;
	for (i = 1; i < n; i++) {
		const struct btnode_hdr *hd = (const void *)pg;
		long d;

		if (pread(fd, pg, PS, i * PS) != PS)
			break;
		/* Node pages only: the header names this page size and the
		 * leaf flag is 0/1 (free and out-of-line pages are skipped:
		 * out-of-line pages zero their own tail, ovf_write). */
		if (hd->page_size != PS || hd->is_leaf > 1 || hd->dead > 1)
			continue;
		nodes++;
		d = t_dead_bytes(pg);
		if (d != 0) {
			if (bad_pages++ < 3)
				fprintf(stderr, "  page %lld: %ld non-zero bytes "
				    "outside its live cells\n", (long long)i, d);
			dirty += d;
		}
	}
	close(fd);
	printf("  %lld pages, %ld B-tree nodes: %ld with unowned non-zero "
	    "bytes (%ld bytes)\n", (long long)n, nodes, bad_pages, dirty);
	TCK(nodes > 10, "the workload built only %ld node pages", nodes);
	TCK(bad_pages == 0, "%ld node pages carry %ld bytes of memory outside "
	    "their live header, slots and cells", bad_pages, dirty);
	unlink(path);
	unlink(wal);
	printf("test_page_bytes: %s\n", g_fail ? "FAILED" : "ok");
	return g_fail;
}

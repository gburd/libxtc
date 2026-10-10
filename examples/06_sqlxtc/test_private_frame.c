/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * A stale page-table walker must not pin a private frame during page-in.
 * Probe the actual try_pin gate while the real read is in progress.
 */
#include "bufmgr.h"
#include "xtc_aio.h"
#include <string.h>
static int probe_read(int, void *, uint32_t, int64_t);
static void *probe_clear(void *, int, size_t);
#define xtc_aio_pread probe_read
#define memset probe_clear
#include "bufmgr.c"
#undef xtc_aio_pread
#undef memset
#include "t_tmp.h"

static bm_t *pool;
static int armed, seen, unsafe;

static void *
probe_clear(void *buf, int value, size_t len)
{
	uint32_t i;
	if (armed) {
		for (i = 0; i < pool->n_frames; i++) {
			bm_frame_t *f = &pool->frames[i];
			if (f->page != buf) continue;
			seen++;
			if (try_pin(f)) {
				unsafe++;
				unpin_frame(pool, f);
			}
		}
	}
	return memset(buf, value, len);
}

static int
probe_read(int fd, void *buf, uint32_t len, int64_t off)
{
	uint32_t i;
	if (armed) {
		for (i = 0; i < pool->n_frames; i++) {
			bm_frame_t *f = &pool->frames[i];
			if (f->page != buf)
				continue;
			seen++;
			/* This is the same CAS a walker uses after obtaining a
			 * frame pointer before eviction and recycling. */
			if (try_pin(f)) {
				unsafe++;
				unpin_frame(pool, f);
			}
		}
	}
	return xtc_aio_pread(fd, buf, len, off);
}

int
main(void)
{
	bm_opts_t opts = BM_OPTS_DEFAULT;
	bm_frame_t *f;
	bm_pid_t pid;
	char path[256];
	int fd, i, rc = 1;

	t_tmpl(path, sizeof path, "sqlxtc-private-frame");
	fd = mkstemp(path);
	if (fd < 0) return 1;
	close(fd);
	opts.path = path;
	opts.n_frames = 4;
	opts.scan_resist = 0;
	if (bm_create(&opts, &pool) != XTC_OK) goto unlink_out;
	armed = 1;
	if (bm_alloc_pid(pool, &f, &pid) != XTC_OK) goto out;
	armed = 0;
	if (seen != 1 || unsafe != 0) {
		fprintf(stderr, "FAIL: private allocation seen=%d pinnable=%d\n", seen, unsafe);
		bm_unfix(pool, f, 0);
		goto out;
	}
	seen = unsafe = 0;
	((unsigned char *)bm_page(f))[0] = 0x72;
	bm_unfix(pool, f, 1);
	if (bm_checkpoint(pool) != XTC_OK) goto out;
	for (i = 0; i < 4 && !evict_one(pool); i++)
		;
	armed = 1;
	if (bm_fix_pid(pool, pid, &f) != XTC_OK) goto out;
	armed = 0;
	if (((unsigned char *)bm_page(f))[0] != 0x72) {
		bm_unfix(pool, f, 0);
		goto out;
	}
	bm_unfix(pool, f, 0);
	if (seen != 1 || unsafe != 0) {
		fprintf(stderr, "FAIL: private page-in seen=%d pinnable=%d\n", seen, unsafe);
		goto out;
	}
	puts("OK: private page-in remains unpinnable until publication");
	rc = 0;
out:
	bm_destroy(pool);
unlink_out:
	unlink(path);
	return rc;
}

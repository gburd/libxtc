/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * Writeback must hold a pin until I/O ownership is released, without
 * promoting CLOCK ref. Include the implementation for deterministic seams.
 */
#include "bufmgr.h"
static void flush_hook(bm_t *, bm_frame_t *);
#define BM_FLUSH_TEST_HOOK(bm, f) flush_hook(bm, f)
#include "bufmgr.c"
#include "t_tmp.h"

static int armed, drop, failures, hook_calls;

#define CHECK(c, msg) do { \
	if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void
check_owned(bm_t *bm, bm_frame_t *f)
{
	int reserved;
	CHECK(atomic_load(&f->io_busy) == 1, "writeback owns io_busy");
	reserved = try_reserve(f);
	CHECK(!reserved, "writeback pin excludes reservation");
	if (reserved)
		release_reservation(f);
	if (drop) {
		CHECK(bm_free_pid(bm, bm_frame_pid(f)) == XTC_OK, "drop during writeback");
		CHECK(atomic_load(&f->state) != BM_FREE, "drop waits for writeback pin");
		CHECK(atomic_load(&f->doomed) == 1, "drop deferred until I/O ends");
	}
}

static void
flush_hook(bm_t *bm, bm_frame_t *f)
{
	if (!armed)
		return;
	hook_calls++;
	check_owned(bm, f);
}

static int
run(int trickler, int doomed, int fail_io)
{
	bm_opts_t opts = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bm_frame_t *f, *run_f[1];
	bm_pid_t pid;
	char path[256];
	unsigned char *snap = NULL, got = 0;
	int fd, saved_fd = -1, rc, before = failures;

	t_tmpl(path, sizeof path, "sqlxtc-flush-pin");
	fd = mkstemp(path);
	if (fd < 0) return 1;
	close(fd);
	opts.path = path;
	opts.n_frames = 4;
	if (bm_create(&opts, &bm) != XTC_OK) goto err;
	if (bm_alloc_pid(bm, &f, &pid) != XTC_OK) goto err;
	((unsigned char *)bm_page(f))[0] = 0x72;
	bm_unfix(bm, f, 1);
	atomic_store(&f->ref, 0);
	/* A delayed writeback must not enter an evictor-owned frame. */
	CHECK(try_reserve(f), "reserve before writeback");
	if (trickler) {
		snap = xtc_aligned_alloc(4096, bm->page_size);
		if (snap == NULL) goto err;
		rc = tr_prepare(bm, f, snap);
		/* Let a broken implementation release its claimed I/O. */
		if (rc) {
			run_f[0] = f;
			(void)tr_emit_run(bm, snap, 1, pid, run_f);
		}
	} else {
		rc = flush_frame(bm, f);
	}
	CHECK(rc == 0, "writeback rejects reserved frame");
	release_reservation(f);
	/* Restore dirty only for fail-before controls that wrongly wrote it. */
	atomic_store(&f->dirty, 1);
	atomic_store(&f->io_busy, 1);
	rc = trickler ? tr_prepare(bm, f, snap) : flush_frame(bm, f);
	CHECK(rc == 0 && atomic_load(&f->pin) == 0 &&
	    atomic_load(&f->io_busy) == 1, "busy failure releases only its pin");
	atomic_store(&f->io_busy, 0);
	armed = 1;
	drop = doomed;
	hook_calls = 0;
	saved_fd = bm->fd;
	if (fail_io) {
		bm->fd = open(path, O_RDONLY);
		if (bm->fd < 0) { bm->fd = saved_fd; goto err; }
	}
	if (trickler) {
		rc = tr_prepare(bm, f, snap);
		CHECK(rc == 1, "trickler prepares dirty frame");
		if (rc) {
			check_owned(bm, f);
			run_f[0] = f;
			rc = tr_emit_run(bm, snap, 1, pid, run_f);
		}
	} else {
		rc = flush_frame(bm, f);
		CHECK(hook_calls == 1, "direct flush reached I/O seam");
	}
	armed = 0;
	CHECK(rc == !fail_io, "writeback reports I/O result");
	CHECK(atomic_load(&f->io_busy) == 0, "writeback releases io_busy");
	CHECK(atomic_load(&f->ref) == 0, "writeback does not promote ref");
	if (doomed) {
		CHECK(atomic_load(&f->state) == BM_FREE, "last I/O pin completes doomed drop");
		CHECK(atomic_load(&f->pin) == -1, "doomed frame returns unpinable");
		CHECK(atomic_load(&bm->free_n) == 4, "doomed frame returned exactly once");
	} else {
		CHECK(atomic_load(&f->pin) == 0, "writeback releases its pin");
		CHECK(atomic_load(&f->dirty) == fail_io, "failed write stays dirty");
	}
	if (fail_io) {
		close(bm->fd);
		bm->fd = saved_fd;
		saved_fd = -1;
		CHECK(flush_frame(bm, f) == 1, "failed write can be retried");
		CHECK(atomic_load(&f->dirty) == 0, "retry cleans frame");
	}
	CHECK(pread(bm->fd, &got, 1, (off_t)pid * bm->page_size) == 1 &&
	    got == 0x72, "writeback persists image");
	xtc_aligned_free(snap);
	bm_destroy(bm);
	unlink(path);
	printf("%s: %s%s%s\n", failures == before ? "OK" : "FAIL",
	    trickler ? "trickler" : "direct", doomed ? " doomed" : "",
	    fail_io ? " failed I/O" : "");
	return failures != before;
err:
	armed = 0;
	if (bm != NULL && saved_fd >= 0 && saved_fd != bm->fd)
		close(saved_fd);
	xtc_aligned_free(snap);
	if (bm != NULL) bm_destroy(bm);
	unlink(path);
	return 1;
}

int
main(void)
{
	int rc = 0;
	rc |= run(0, 0, 0);
	rc |= run(1, 0, 0);
	rc |= run(0, 1, 0);
	rc |= run(1, 1, 0);
	rc |= run(0, 0, 1);
	rc |= run(1, 0, 1);
	return rc;
}

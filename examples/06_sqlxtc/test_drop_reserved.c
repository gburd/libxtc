/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * Deterministically pause the real drop thread after its last pin decrement,
 * before its doomed finalizer reserves. Let the real CLOCK evictor run there.
 * Test-only last-unpin seam; production builds compile it away.
 */
#include "bufmgr.h"
static void after_unpin(bm_frame_t *);
#define BM_UNPIN_TEST_HOOK(frame) after_unpin(frame)
#include "bufmgr.c"
#include "t_tmp.h"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bm_frame_t *target;
static int paused, resume_drop, drop_rc;

static void
after_unpin(bm_frame_t *f)
{
	if (target == NULL || f != target)
		return;
	pthread_mutex_lock(&gate);
	paused = 1;
	pthread_cond_broadcast(&changed);
	while (!resume_drop)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
}

static void *
drop_thread(void *arg)
{
	drop_rc = bm_free_pid(arg, bm_frame_pid(target));
	return NULL;
}

int
main(int argc, char **argv)
{
	bm_opts_t opts = BM_OPTS_DEFAULT;
	bm_t *bm;
	bm_frame_t *f;
	bm_pid_t pid;
	pthread_t thread;
	char path[256];
	uint64_t resident_before, resident_after, pending;
	int fd, evicted, failed;

	(void)argv;
	/* Any argument runs the control: same drop, no competing eviction. */
	/* A broken synchronization path must fail rather than hang forever. */
	alarm(10);
	t_tmpl(path, sizeof path, "sqlxtc-drop-reserved");
	fd = mkstemp(path);
	if (fd < 0) return 2;
	close(fd);
	opts.path = path;
	opts.n_frames = 4;
	if (bm_create(&opts, &bm) != XTC_OK) return 2;
	unlink(path);
	if (bm_alloc_pid(bm, &f, &pid) != XTC_OK) return 2;
	bm_unfix(bm, f, 0);
	if (bm_checkpoint(bm) != XTC_OK) return 2;
	atomic_store(&f->state, BM_COOL);
	atomic_store(&f->ref, 0);
	atomic_store(&bm->clock, (uint32_t)(f - bm->frames));
	resident_before = atomic_load(&bm->resident);
	target = f;
	if (pthread_create(&thread, NULL, drop_thread, bm) != 0) return 2;
	pthread_mutex_lock(&gate);
	while (!paused)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	/* Drop owns active + deferred counts, but its pin is now zero. */
	if (atomic_load(&f->pin) != 0 || !atomic_load(&f->doomed) ||
	    atomic_load(&bm->pending_pid_drops) != 2) return 2;
	evicted = argc == 1 ? evict_one(bm) : 0;
	pthread_mutex_lock(&gate);
	resume_drop = 1;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	pthread_join(thread, NULL);
	target = NULL;
	resident_after = atomic_load(&bm->resident);
	pending = atomic_load(&bm->pending_pid_drops);
	bm_reclaim_quarantine(bm);
	failed = drop_rc != XTC_OK || resident_after != resident_before - 1 ||
	    pending != 0 || bm->quar_pids_n != 0 || bm->free_pids_n != 1;
	fprintf(stderr, "%s: evicted=%d resident=%llu (expected %llu) "
	    "pending=%llu quarantine=%u reusable=%u\n", failed ? "FAIL" : "OK",
	    evicted, (unsigned long long)resident_after,
	    (unsigned long long)(resident_before - 1),
	    (unsigned long long)pending, bm->quar_pids_n, bm->free_pids_n);
	bm_destroy(bm);
	return failed;
}

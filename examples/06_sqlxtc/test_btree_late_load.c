/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * A late disk reader must not publish an image older than a completed delete.
 * Include bufmgr.c to drive eviction and wrap its read completion without
 * exporting a production test API. Link with btree.c and btnode.c, NOT bufmgr.o.
 */
#include "bufmgr.h"
#include "btree.h"
#include "xtc_aio.h"
#include "t_tmp.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static int delayed_pread(int, void *, uint32_t, int64_t);
#define xtc_aio_pread delayed_pread
#include "bufmgr.c"
#undef xtc_aio_pread

static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int read_ready, resume_read;
static _Thread_local int delay_read;
static bm_t *pool;
static bm_pid_t target;
static int loader_rc;

/* Capture the leaf identity from the real insert, not a hardcoded page id. */
static uint64_t
capture_leaf(void *user, bm_pid_t pid, const void *image, uint32_t size,
    uint64_t lsn)
{
	(void)user;
	(void)image;
	(void)size;
	(void)lsn;
	target = pid;
	return 0;
}

static int
delayed_pread(int fd, void *buf, uint32_t len, int64_t off)
{
	int n = xtc_aio_pread(fd, buf, len, off);

	if (delay_read && off == (int64_t)target * len) {
		/* The old image is now in the loader's private frame. Pause
		 * BEFORE bm_fix_pid checks for an existing resident copy. */
		delay_read = 0;
		assert(n == (int)len);
		assert(pthread_mutex_lock(&gate_mu) == 0);
		read_ready = 1;
		assert(pthread_cond_broadcast(&gate_cv) == 0);
		while (!resume_read)
			assert(pthread_cond_wait(&gate_cv, &gate_mu) == 0);
		assert(pthread_mutex_unlock(&gate_mu) == 0);
	}
	return n;
}

static void *
loader(void *arg)
{
	bm_frame_t *f;

	(void)arg;
	delay_read = 1;
	loader_rc = bm_fix_pid(pool, target, &f);
	if (loader_rc == XTC_OK)
		bm_unfix(pool, f, 0);
	return NULL;
}

static void
evict_target(void)
{
	bm_frame_t *f;
	int i;

	for (i = 0; i < 128; i++) {
		f = ht_lookup_pin(pool, target, NULL);
		if (f == NULL)
			return;
		bm_unfix(pool, f, 0);
		(void)evict_one(pool);
	}
	fprintf(stderr, "FAIL: could not evict target page\n");
	abort();
}

static int
run(int evict_winner)
{
	bm_opts_t opts = BM_OPTS_DEFAULT;
	bt_smo_hook_t hook = { .leaf = capture_leaf };
	bt_t *bt;
	pthread_t thread;
	char path[64];
	int fd, rc;

	t_tmpl(path, sizeof path, "sqlxtc-late-load");
	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	opts.path = path;
	opts.n_frames = 16;
	opts.scan_resist = 0;
	assert(bm_create(&opts, &pool) == XTC_OK);
	assert(bt_open(pool, &bt) == XTC_OK);
	target = BM_PID_NONE;
	bt_set_smo_hook(&hook);
	assert(bt_insert(bt, "victim", 6, "value", 5) == XTC_OK);
	bt_set_smo_hook(NULL);
	assert(target != BM_PID_NONE);
	assert(bm_checkpoint(pool) == XTC_OK);
	evict_target();

	read_ready = resume_read = 0;
	loader_rc = XTC_E_INTERNAL;
	assert(pthread_create(&thread, NULL, loader, NULL) == 0);
	assert(pthread_mutex_lock(&gate_mu) == 0);
	while (!read_ready)
		assert(pthread_cond_wait(&gate_cv, &gate_mu) == 0);
	assert(pthread_mutex_unlock(&gate_mu) == 0);

	/* A second loader wins publication. Its delete really succeeds;
	 * the miss check BEFORE releasing the delayed reader proves it. */
	assert(bt_delete(bt, "victim", 6) == XTC_OK);
	assert(bt_lookup(bt, "victim", 6, NULL, 0, NULL) == XTC_E_NOTFOUND);
	assert(bm_checkpoint(pool) == XTC_OK);
	if (evict_winner)
		evict_target();

	assert(pthread_mutex_lock(&gate_mu) == 0);
	resume_read = 1;
	assert(pthread_cond_broadcast(&gate_cv) == 0);
	assert(pthread_mutex_unlock(&gate_mu) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(loader_rc == XTC_OK);
	assert(bm_dbg_dup_pid(pool) == 0);
	rc = bt_lookup(bt, "victim", 6, NULL, 0, NULL);
	if (rc != XTC_E_NOTFOUND)
		fprintf(stderr, "FAIL: evict_winner=%d: completed delete "
		    "resurrected by late loader (lookup=%d)\n", evict_winner, rc);
	else
		printf("OK: evict_winner=%d: completed delete stays absent\n",
		    evict_winner);
	bt_close(bt);
	bm_destroy(pool);
	assert(unlink(path) == 0);
	return rc != XTC_E_NOTFOUND;
}

int
main(void)
{
	(void)alarm(30);
	/* Control: resident winner lets the existing publication dedup work.
	 * Regression: eviction removes that witness, not its newer disk image. */
	if (run(0) || run(1))
		return 1;
	return 0;
}

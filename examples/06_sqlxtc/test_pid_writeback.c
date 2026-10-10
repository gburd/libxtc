/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 * A reclaimed pid must not be reused while its old write is in flight.
 */
#include "bufmgr.h"
static void reuse_hook(bm_t *, bm_frame_t *);
#define BM_FLUSH_TEST_HOOK(bm, f) reuse_hook(bm, f)
#include "bufmgr.c"
#include "t_tmp.h"

static int armed, bad;
static bm_pid_t replacement;

static void
reuse_hook(bm_t *bm, bm_frame_t *old)
{
	bm_frame_t *fresh;
	bm_pid_t pid = bm_frame_pid(old);
	if (!armed) return;
	armed = 0;
	if (bm_free_pid(bm, pid) != XTC_OK) abort();
	bm_reclaim_quarantine(bm);
	if (bm_alloc_pid(bm, &fresh, &replacement) != XTC_OK) abort();
	if (replacement == pid) bad = 1;
	((unsigned char *)bm_page(fresh))[0] = 0x72;
	bm_unfix(bm, fresh, 1);
	if (bm_checkpoint(bm) != XTC_OK) abort();
}

/* Delete every position in mixed-home clusters, including wraparound.
 * Membership of every survivor and the count must survive each deletion. */
static int
check_collision_removal(bm_t *bm)
{
	static const bm_pid_t pids[] = {63, 127, 191, 64, 128, 1};
	unsigned shift, first, step, i, victim, live, count;
	int rc = 1;

	pthread_mutex_lock(&bm->pid_mu);
	quar_set_remove(bm, 63); /* empty, not yet allocated */
	for (shift = 0; shift <= 32; shift += 32) {
		for (first = 0; first < 6; first++) {
			for (i = 0; i < 6; i++)
				if (quar_set_add(bm, pids[i] + shift) != XTC_OK)
					goto out;
			if (quar_set_add(bm, pids[first] + shift) != XTC_OK)
				goto out; /* duplicate must not change the count */
			quar_set_remove(bm, 255 + shift); /* absent, same home */
			if (bm->quar_set_n != 6 || bm->quar_set_cap != 64)
				goto out;
			live = 63;
			for (step = 0; step < 6; step++) {
				victim = (first + step) % 6;
				quar_set_remove(bm, pids[victim] + shift);
				quar_set_remove(bm, pids[victim] + shift);
				live &= ~(1u << victim);
				count = 0;
				for (i = 0; i < 6; i++) {
					int expected = (live >> i) & 1u;
					if (quar_set_has(bm, pids[i] + shift) != expected)
						goto out;
					count += expected;
				}
				if (bm->quar_set_n != count)
					goto out;
			}
		}
	}
	rc = 0;
out:
	pthread_mutex_unlock(&bm->pid_mu);
	if (rc != 0)
		fprintf(stderr, "FAIL: retired-pid collision cluster removal\n");
	return rc;
}

int
main(void)
{
	bm_opts_t opts = BM_OPTS_DEFAULT;
	bm_t *bm;
	bm_frame_t *f;
	bm_pid_t pid, later;
	char path[256];
	int fd, rc = 1;

	t_tmpl(path, sizeof path, "sqlxtc-pid-writeback");
	fd = mkstemp(path);
	if (fd < 0) return 1;
	close(fd);
	opts.path = path;
	opts.n_frames = 8;
	if (bm_create(&opts, &bm) != XTC_OK) goto unlink_out;
	if (check_collision_removal(bm) != 0) goto out;
	if (bm_alloc_pid(bm, &f, &pid) != XTC_OK) goto out;
	((unsigned char *)bm_page(f))[0] = 0x31;
	bm_unfix(bm, f, 1);
	armed = 1;
	if (!flush_frame(bm, f)) goto out;
	if (bad) {
		unsigned char got = 0;
		if (pread(bm->fd, &got, 1, (off_t)replacement * bm->page_size) != 1)
			goto out;
		fprintf(stderr, "FAIL: pid reused during old write; disk=0x%02x expected=0x72\n", got);
		goto out;
	}
	bm_reclaim_quarantine(bm);
	/* Reissuable is not live: a stale descent must not reload the old
	 * disk image between draining quarantine and publishing reuse. */
	if (bm_fix_pid(bm, pid, &f) != XTC_E_AGAIN) {
		fprintf(stderr, "FAIL: retired pid reloaded after quarantine drain\n");
		goto out;
	}
	if (bm_alloc_pid(bm, &f, &later) != XTC_OK) goto out;
	bm_unfix(bm, f, 0);
	if (later != pid) {
		fprintf(stderr, "FAIL: completed write did not release quarantined pid\n");
		goto out;
	}
	puts("OK: pid quarantine waits for old writeback completion");
	rc = 0;
out:
	bm_destroy(bm);
unlink_out:
	unlink(path);
	return rc;
}

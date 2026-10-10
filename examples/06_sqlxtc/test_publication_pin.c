/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * A bucket-head release publishes ownership too: a later pin=1 store
 * must not erase a fast reader's pin. Re-enter the real bm_fix_pid at
 * publication, while the publisher still holds the stripe lock. This
 * is a deterministic interleaving, not a mocked lookup or timed race.
 *
 * Standalone build (from this directory, using the existing link flags):
 * make -s -B -n test_flush_pin | sed \
 *   's/-o test_flush_pin test_flush_pin.c/-o \/tmp\/test_publication_pin test_publication_pin.c/' | sh
 * timeout 15 /tmp/test_publication_pin
 */
#include "bufmgr.h"
static void publish_hook(bm_t *, bm_frame_t *);
#define BM_PUBLISH_TEST_HOOK(bm, f) publish_hook(bm, f)
#include "bufmgr.c"
#include "t_tmp.h"

static bm_frame_t *reader;
static int armed, hook_calls, reader_rc, published_pin, reader_pin;

static void
publish_hook(bm_t *bm, bm_frame_t *f)
{
	if (!armed)
		return;
	armed = 0;
	hook_calls++;
	published_pin = atomic_load(&f->pin);
	CK(published_pin == 1);
	CK(atomic_load(&bm->buckets[f->pid % bm->nbucket]) == f);
	/* A slow-path lookup would deadlock on the publisher's stripe.
	 * The real lock-free HIT must acquire a second, independent pin. */
	reader_rc = bm_fix_pid(bm, bm_frame_pid(f), &reader);
	CK(reader_rc == XTC_OK);
	CK(reader == f);
	reader_pin = atomic_load(&f->pin);
	CK(reader_pin == 2);
}

static int
run(int allocate, int scan_resist)
{
	bm_opts_t opts = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bm_frame_t *original = NULL, *replacement = NULL;
	bm_pid_t pid = BM_PID_NONE, replacement_pid;
	unsigned char expected[4096];
	char path[256];
	int fd, rc, after_publish, after_unfix, failed;

	g_fail = 0;
	memset(expected, allocate ? 0 : 0x72, sizeof expected);
	t_tmpl(path, sizeof path, "sqlxtc-publication-pin");
	fd = mkstemp(path);
	if (fd < 0)
		return 1;
	close(fd);
	opts.path = path;
	opts.n_frames = 4;
	opts.scan_resist = scan_resist;
	if (bm_create(&opts, &bm) != XTC_OK)
		goto err;
	if (!allocate) {
		/* A real persisted page, reopened cold to force the miss path. */
		if (bm_alloc_pid(bm, &original, &pid) != XTC_OK)
			goto err;
		memcpy(bm_page(original), expected, sizeof expected);
		bm_unfix(bm, original, 1);
		if (bm_checkpoint(bm) != XTC_OK)
			goto err;
		bm_destroy(bm);
		bm = NULL;
		opts.reopen = 1;
		if (bm_create(&opts, &bm) != XTC_OK)
			goto err;
	}
	reader = NULL;
	hook_calls = 0;
	reader_rc = XTC_E_INTERNAL;
	published_pin = reader_pin = -99;
	armed = 1;
	rc = allocate ? bm_alloc_pid(bm, &original, &pid) :
	    bm_fix_pid(bm, pid, &original);
	armed = 0;
	CK(rc == XTC_OK);
	CK(hook_calls == 1);
	if (rc != XTC_OK || reader_rc != XTC_OK || reader != original)
		goto err;
	after_publish = atomic_load(&original->pin);
	CK(after_publish == 2);
	CK(memcmp(bm_page(reader), expected, sizeof expected) == 0);
	bm_unfix(bm, reader, 0);
	after_unfix = atomic_load(&original->pin);
	CK(after_unfix == 1);
	printf("%s scan_resist=%d: publication=%d reader=%d resumed=%d "
	    "reader-unfix=%d\n", allocate ? "alloc" : "fix", scan_resist,
	    published_pin, reader_pin, after_publish, after_unfix);

	/* Model a merge dropping the page while the original caller still
	 * owns its pin. Reallocation must neither recycle nor overwrite it. */
	CK(bm_free_pid(bm, pid) == XTC_OK);
	CK(atomic_load(&original->state) != BM_FREE);
	if (bm_alloc_pid(bm, &replacement, &replacement_pid) != XTC_OK)
		goto err;
	CK(replacement != original);
	memset(bm_page(replacement), 0xa5, sizeof expected);
	CK(bm_frame_pid(original) == pid);
	CK(memcmp(bm_page(original), expected, sizeof expected) == 0);
	bm_unfix(bm, replacement, 1);
	/* Do not underflow the known-broken count during test cleanup. */
	if (after_unfix == 1 && replacement != original)
		bm_unfix(bm, original, 0);
	failed = g_fail;
	bm_destroy(bm);
	unlink(path);
	return failed;
err:
	armed = 0;
	fprintf(stderr, "FAIL: publication test setup/interleaving\n");
	bm_destroy(bm);
	unlink(path);
	return 1;
}

int
main(void)
{
	int rc = 0;

	rc |= run(0, 0);
	rc |= run(0, 1);
	rc |= run(1, 0);
	rc |= run(1, 1);
	return rc;
}

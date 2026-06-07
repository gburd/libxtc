/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_redo_page.c
 *	Physiological redo apply (bm_apply_page_image): a full-page
 *	after-image is written onto a page only when its on-disk LSN is
 *	older than the image's -- the page-LSN gate that makes redo
 *	idempotent.  Verifies: a fresh (zero-filled) page accepts the
 *	image; replaying the same image is a no-op; an older image is
 *	refused; a newer image overwrites; a wrong-sized image is
 *	rejected.  Plain asserts; no loop.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "xtc.h"
#include "t_tmp.h"

#define PAGE_SZ 4096

static int g_fail;
#define CK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* Build a page image: LSN at the front (lsn_off 0), then a fill byte. */
static void
mk_image(uint8_t *img, uint64_t lsn, uint8_t fill)
{
	memset(img, fill, PAGE_SZ);
	memcpy(img, &lsn, 8);
}

/* Read page `pid` and check its LSN + a body byte. */
static int
page_is(bm_t *bm, bm_pid_t pid, uint64_t want_lsn, uint8_t want_fill)
{
	bm_frame_t *f;
	uint64_t got;
	uint8_t body;
	if (bm_fix_pid(bm, pid, &f) != XTC_OK)
		return 0;
	memcpy(&got, bm_page(f), 8);
	body = ((uint8_t *)bm_page(f))[100];
	bm_unfix(bm, f, 0);
	return got == want_lsn && body == want_fill;
}

int
main(void)
{
	char path[256];
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	uint8_t img[PAGE_SZ];
	int fd;

	t_tmpl(path, sizeof path, "redopage-bt");
	if ((fd = mkstemp(path)) >= 0) close(fd);

	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 8; bo.lsn_off = 0;
	CK(bm_create(&bo, &bm) == XTC_OK);

	/* fresh page 5 (zero LSN) accepts an image with LSN 100 */
	mk_image(img, 100, 0xAA);
	CK(bm_apply_page_image(bm, 5, img, PAGE_SZ) == 1);
	CK(page_is(bm, 5, 100, 0xAA));

	/* replaying the identical image is a no-op (100 not > 100) */
	CK(bm_apply_page_image(bm, 5, img, PAGE_SZ) == 0);
	CK(page_is(bm, 5, 100, 0xAA));

	/* an older image (LSN 50) is refused; page unchanged */
	mk_image(img, 50, 0xBB);
	CK(bm_apply_page_image(bm, 5, img, PAGE_SZ) == 0);
	CK(page_is(bm, 5, 100, 0xAA));

	/* a newer image (LSN 150) overwrites */
	mk_image(img, 150, 0xCC);
	CK(bm_apply_page_image(bm, 5, img, PAGE_SZ) == 1);
	CK(page_is(bm, 5, 150, 0xCC));

	/* a wrong-sized image is rejected */
	CK(bm_apply_page_image(bm, 5, img, PAGE_SZ - 1) == XTC_E_INVAL);

	bm_destroy(bm);

	/* ---- recLSN horizon: oldest dirty page's first-dirty LSN ---- */
	{
		bm_opts_t b2 = BM_OPTS_DEFAULT;
		bm_t *bm2 = NULL;
		bm_frame_t *f;
		char p2[256];
		int fd2;

		t_tmpl(p2, sizeof p2, "reclsn-bt");
		if ((fd2 = mkstemp(p2)) >= 0) close(fd2);
		b2.path = p2; b2.page_size = PAGE_SZ; b2.n_frames = 8; b2.lsn_off = 0;
		CK(bm_create(&b2, &bm2) == XTC_OK);

		CK(bm_min_rec_lsn(bm2) == 0);          /* nothing dirty yet */
		bm_set_lsn(bm2, 10);
		CK(bm_fix_pid(bm2, 1, &f) == XTC_OK);
		bm_latch_exclusive(f); ((uint8_t *)bm_page(f))[64] = 1;
		bm_unlatch(f); bm_unfix(bm2, f, 1);
		bm_set_lsn(bm2, 20);
		CK(bm_fix_pid(bm2, 2, &f) == XTC_OK);
		bm_latch_exclusive(f); ((uint8_t *)bm_page(f))[64] = 1;
		bm_unlatch(f); bm_unfix(bm2, f, 1);
		CK(bm_min_rec_lsn(bm2) == 10);         /* oldest dirty page */
		/* re-dirtying page 1 at a later LSN does not move its recLSN */
		bm_set_lsn(bm2, 30);
		CK(bm_fix_pid(bm2, 1, &f) == XTC_OK);
		bm_latch_exclusive(f); ((uint8_t *)bm_page(f))[65] = 1;
		bm_unlatch(f); bm_unfix(bm2, f, 1);
		CK(bm_min_rec_lsn(bm2) == 10);         /* still page 1's first-dirty LSN */
		CK(bm_checkpoint(bm2) == XTC_OK);      /* flush all: no dirty pages */
		CK(bm_min_rec_lsn(bm2) == 0);
		bm_destroy(bm2);
		unlink(p2);
		{ char s[280]; snprintf(s, sizeof s, "%s.dwb", p2); unlink(s); }
	}

	unlink(path);
	{ char s[280]; snprintf(s, sizeof s, "%s.dwb", path); unlink(s); }

	if (g_fail) { fprintf(stderr, "test_redo_page: FAILED\n"); return 1; }
	printf("test_redo_page: page-LSN-gated image apply (redo idempotent)\n");
	return 0;
}

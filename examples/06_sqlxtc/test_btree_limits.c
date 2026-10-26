/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_limits.c
 *	Entry-size limits of the B-tree, borrowed from RethinkDB's design
 *	rule that every leaf entry is bounded relative to the page
 *	(leaf_node.cc leaf_epsilon), so every split/merge guarantee holds.
 *
 *	Each case reproduces a defect of 1.56.0 (and earlier):
 *	  L1  an OLC lookup of a value > 1024 B returned it truncated
 *	  L2  a second ~2.1 KB value into a 4 KiB tree failed (1-entry
 *	      leaf could not split) and leaked a page
 *	  L3  a count-balanced split left the receiving half too full
 *	  L4  a key > 1024 B overflowed the split's separator buffer
 *	  L5  16 KiB pages: internal splits failed (8 KiB scratch)
 *	plus the bound itself: an entry above bt_max_inline() is refused
 *	cleanly with no change to the tree, and a byte-balanced split
 *	property run with random sizes up to the bound never fails; and on
 *	4 KiB pages a 700-byte row stays inline (the bound is 1010 B).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "xtc.h"
#include "t_tmp.h"

static int g_checks, g_fail;

#define CHECK(cond, ...)                                                     \
	do {                                                                 \
		g_checks++;                                                  \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                        \
			fprintf(stderr, "\n");                               \
			g_fail = 1;                                          \
		}                                                            \
	} while (0)

static bm_t *
mk(char *p, uint32_t ps, uint32_t nf)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	int fd;

	t_tmpl(p, 64, "btlim");
	fd = mkstemp(p);
	if (fd < 0) { perror("mkstemp"); exit(2); }
	(void)close(fd);
	bo.path = p;
	bo.page_size = ps;
	bo.n_frames = nf;
	bo.cool_pct = 20;
	if (bm_create(&bo, &bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); exit(2); }
	return bm;
}

static void
fill(unsigned char *b, size_t n, uint32_t seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		b[i] = (unsigned char)(seed * 131u + i * 7u + (i >> 8));
}

static void
done(bt_t *bt, bm_t *bm, const char *p)
{
	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(p);
}

/* L1: an OLC (default) lookup returns a large value intact. */
static void
t_olc_large_value(uint32_t ps)
{
	char p[64];
	bm_t *bm = mk(p, ps, 256);
	bt_t *bt;
	static unsigned char v[4096], out[4096];
	uint16_t vl = 0;
	uint16_t n = 2000;

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	fill(v, n, 1);
	CHECK(bt_insert(bt, "big", 3, v, n) == XTC_OK, "insert %u B", n);
	memset(out, 0xEE, sizeof out);
	CHECK(bt_lookup(bt, "big", 3, out, sizeof out, &vl) == XTC_OK, "lookup");
	CHECK(vl == n, "vlen %u want %u", vl, n);
	CHECK(memcmp(out, v, n) == 0, "L1: value of %u B returned intact", n);
	done(bt, bm, p);
}

/* L2: two entries at the inline bound always fit (the leaf splits). */
static void
t_two_max_entries(uint32_t ps)
{
	char p[64];
	bm_t *bm = mk(p, ps, 256);
	bt_t *bt;
	static unsigned char v[16384], out[16384];
	uint16_t m, vl;
	int i;

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	m = 2100;   /* L2: 1.56 failed the 2nd such entry in a 4 KiB tree */
	for (i = 0; i < 8; i++) {
		char k = (char)('a' + i);
		fill(v, m, (uint32_t)i);
		CHECK(bt_insert(bt, &k, 1, v, m) == XTC_OK,
		    "L2: max-size entry #%d (%u B) inserted", i, m);
	}
	for (i = 0; i < 8; i++) {
		char k = (char)('a' + i);
		fill(v, m, (uint32_t)i);
		vl = 0;
		CHECK(bt_lookup(bt, &k, 1, out, sizeof out, &vl) == XTC_OK &&
		    vl == m && memcmp(out, v, m) == 0, "L2: entry #%d intact", i);
	}
	done(bt, bm, p);
}

/* L3: mixed sizes -- a byte-balanced split leaves room on both sides. */
static void
t_mixed_split(uint32_t ps)
{
	char p[64], k[16];
	bm_t *bm = mk(p, ps, 256);
	bt_t *bt;
	static unsigned char v[16384], out[16384];
	uint16_t vl, big;
	int i, found = 0;

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	big = 2600;   /* L3: 1.56's count split failed the next 1000+ B row */
	for (i = 0; i < 16; i++) {
		snprintf(k, sizeof k, "k%03d", i);
		fill(v, 50, (uint32_t)i);
		CHECK(bt_insert(bt, k, 4, v, 50) == XTC_OK, "small %d", i);
	}
	fill(v, big, 99);
	CHECK(bt_insert(bt, "k100", 4, v, big) == XTC_OK, "big");
	fill(v, big, 77);
	CHECK(bt_insert(bt, "k200", 4, v, big) == XTC_OK,
	    "L3: second large entry after mixed rows inserted");
	for (i = 0; i < 16; i++) {
		snprintf(k, sizeof k, "k%03d", i);
		if (bt_lookup(bt, k, 4, out, sizeof out, &vl) == XTC_OK)
			found++;
	}
	CHECK(found == 16, "small rows still found: %d/16", found);
	fill(v, big, 77);
	CHECK(bt_lookup(bt, "k200", 4, out, sizeof out, &vl) == XTC_OK &&
	    vl == big && memcmp(out, v, big) == 0, "k200 intact");
	done(bt, bm, p);
}

/* L4 + bound: a key above BT_MAX_KEY or an entry above bt_max_inline is
 * refused with XTC_E_INVAL and the tree is unchanged. */
static void
t_refuse_oversize(void)
{
	char p[64];
	bm_t *bm = mk(p, 4096, 256);
	bt_t *bt;
	static unsigned char lk[3][1600], v[65535];
	uint16_t vl;
	int i, ok = 0;
	bt_stats_t st0, st1;

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	/* keys of 1500 B: above the cap -> INVAL, no crash (was a stack
	 * overflow in the split separator buffer) */
	for (i = 0; i < 3; i++) {
		memset(lk[i], 'a' + i, 1500);
		CHECK(bt_insert(bt, lk[i], 1500, "v", 1) == XTC_E_INVAL,
		    "L4: 1500 B key refused");
	}
	/* keys at the cap work and split */
	for (i = 0; i < 3; i++) {
		uint16_t kl = bt_max_key(bt);
		memset(lk[i], 'a' + i, kl);
		if (bt_insert(bt, lk[i], kl, "v", 1) == XTC_OK)
			ok++;
	}
	CHECK(ok == 3, "keys at bt_max_key (%u B) inserted: %d/3", bt_max_key(bt), ok);
	bt_get_stats(bt, &st0);
	fill(v, sizeof v, 5);
	/* a key one byte over the cap, with a value that would go out of
	 * line: refused before ANY page is written */
	memset(lk[0], 'z', sizeof lk[0]);
	CHECK(bt_insert(bt, lk[0], (uint16_t)(bt_max_key(bt) + 1), v, 9000) ==
	    XTC_E_INVAL, "key above bt_max_key refused (with a large value)");
	CHECK(bt_max_value(bt, 1) == 0xFFFFu,
	    "4 KiB pages: max value is the API limit (%u)", bt_max_value(bt, 1));
	bt_get_stats(bt, &st1);
	CHECK(st1.inserts == st0.inserts && st1.splits == st0.splits &&
	    st1.ovf_writes == st0.ovf_writes,
	    "refused insert changed nothing (no split, no overflow pages)");
	CHECK(bt_lookup(bt, lk[0], bt_max_key(bt), v, sizeof v, &vl) ==
	    XTC_E_NOTFOUND, "refused key absent");
	CHECK(bt_lookup(bt, lk[0], 1500, v, sizeof v, &vl) == XTC_E_INVAL,
	    "oversize lookup key -> INVAL");
	CHECK(bt_delete(bt, lk[0], 1500) == XTC_E_INVAL,
	    "oversize delete key -> INVAL");
	done(bt, bm, p);
}

/* L5: large pages split internal nodes. */
static void
t_big_pages(uint32_t ps)
{
	char p[64], k[32];
	bm_t *bm = mk(p, ps, 4096);
	bt_t *bt;
	bt_stats_t st;
	unsigned char out[64];
	uint16_t vl;
	/* Sequential keys fill pages (append splits), so a 32 KiB-page
	 * tree needs ~1.5M of them before the internal level splits. */
	int i, bad = 0, miss = 0, n = ps >= 32768 ? 1600000 : 300000;

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	for (i = 0; i < n; i++) {
		snprintf(k, sizeof k, "key%010d", i);
		if (bt_insert(bt, k, 13, "vvvvvvvv", 8) != XTC_OK)
			bad++;
	}
	for (i = 0; i < n; i += 37) {
		snprintf(k, sizeof k, "key%010d", i);
		if (bt_lookup(bt, k, 13, out, sizeof out, &vl) != XTC_OK)
			miss++;
	}
	bt_get_stats(bt, &st);
	CHECK(bad == 0 && miss == 0,
	    "L5: %u B pages, %d inserts: failed=%d misses=%d height=%llu",
	    ps, n, bad, miss, (unsigned long long)st.height);
	CHECK(st.height >= 3, "%u B pages: internal levels split (height %llu)",
	    ps, (unsigned long long)st.height);
	done(bt, bm, p);
}

/* Property: random sizes up to the bound, random keys, inserts, upserts
 * and deletes against an oracle; nothing may fail or go missing. */
#define PN 6000
static uint16_t o_len[PN];
static uint32_t o_seed[PN];
static unsigned char o_live[PN];

static uint32_t
rnd(uint64_t *s)
{
	*s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
	return (uint32_t)*s;
}

static void
t_property(uint32_t ps, uint64_t seed)
{
	char p[64], k[16];
	bm_t *bm = mk(p, ps, 8192);
	bt_t *bt;
	static unsigned char v[65535], out[65535];
	uint64_t s = seed;
	uint16_t vl, maxv;
	int op, errs = 0, wrong = 0, live = 0, i;

	memset(o_live, 0, sizeof o_live);
	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	maxv = (uint16_t)(bt_max_inline(bt) - 7);   /* key is 7 bytes */
	for (op = 0; op < 60000; op++) {
		int id = (int)(rnd(&s) % PN), r = (int)(rnd(&s) % 100);

		snprintf(k, sizeof k, "p%06d", id);
		if (r < 70) {
			/* skew toward small, with a tail up to the bound */
			uint16_t n = (uint16_t)((rnd(&s) % 4 == 0)
			    ? rnd(&s) % (maxv + 1u) : rnd(&s) % 200u);
			uint32_t sd = rnd(&s);
			fill(v, n, sd);
			if (bt_insert(bt, k, 7, v, n) != XTC_OK) { errs++; continue; }
			if (!o_live[id]) live++;
			o_live[id] = 1; o_len[id] = n; o_seed[id] = sd;
		} else if (r < 90) {
			int rc = bt_delete(bt, k, 7);
			if (o_live[id] ? rc != XTC_OK : rc != XTC_E_NOTFOUND) wrong++;
			if (o_live[id]) live--;
			o_live[id] = 0;
		} else {
			int rc = bt_lookup(bt, k, 7, out, sizeof out, &vl);
			if (o_live[id]) {
				fill(v, o_len[id], o_seed[id]);
				if (rc != XTC_OK || vl != o_len[id] ||
				    memcmp(out, v, vl) != 0) wrong++;
			} else if (rc != XTC_E_NOTFOUND) wrong++;
		}
	}
	for (i = 0; i < PN; i++) {
		int rc;
		snprintf(k, sizeof k, "p%06d", i);
		rc = bt_lookup(bt, k, 7, out, sizeof out, &vl);
		if (o_live[i]) {
			fill(v, o_len[i], o_seed[i]);
			if (rc != XTC_OK || vl != o_len[i] || memcmp(out, v, vl) != 0)
				wrong++;
		} else if (rc != XTC_E_NOTFOUND) wrong++;
	}
	CHECK(errs == 0 && wrong == 0,
	    "property ps=%u seed=%llu: insert errors=%d wrong=%d (live %d)",
	    ps, (unsigned long long)seed, errs, wrong, live);
	done(bt, bm, p);
}


/* P3: out-of-line values.  Sizes from just above the inline bound up to
 * bt_max_value; lookups (OLC + latched), cursor scans, upserts in both
 * directions (inline <-> out-of-line), deletes, and page accounting: every
 * data page written is freed again once its value is gone. */
static void
t_overflow(uint32_t ps)
{
	char p[64], k[16];
	bm_t *bm = mk(p, ps, 512);
	bt_t *bt;
	static unsigned char v[65535], out[65535];
	uint16_t vl;
	uint32_t sizes[8], mx;
	int i, nsz = 0, bad = 0;
	bt_stats_t st;
	bt_cursor_t *c;

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	mx = bt_max_value(bt, 6);
	CHECK(mx >= 32768u, "ps=%u: max value %u >= 32 KiB", ps, mx);
	sizes[nsz++] = bt_max_inline(bt) - 6u + 1u;   /* first out-of-line size */
	sizes[nsz++] = 2000;
	sizes[nsz++] = 2100;
	sizes[nsz++] = 4096;
	sizes[nsz++] = 9000;
	sizes[nsz++] = 30000;
	sizes[nsz++] = mx;
	/* insert 200 keys with sizes cycling, plus small rows between */
	for (i = 0; i < 200; i++) {
		uint32_t n = (i % 3 == 0) ? 40u : sizes[i % nsz];
		snprintf(k, sizeof k, "o%05d", i);
		fill(v, n, (uint32_t)i);
		if (bt_insert(bt, k, 6, v, (uint16_t)n) != XTC_OK) bad++;
	}
	CHECK(bad == 0, "ps=%u: overflow inserts failed: %d", ps, bad);
	for (i = 0; i < 200; i++) {
		uint32_t n = (i % 3 == 0) ? 40u : sizes[i % nsz];
		snprintf(k, sizeof k, "o%05d", i);
		fill(v, n, (uint32_t)i);
		vl = 0;
		memset(out, 0, n);
		if (bt_lookup(bt, k, 6, out, sizeof out, &vl) != XTC_OK ||
		    vl != n || memcmp(out, v, n) != 0) bad++;
	}
	CHECK(bad == 0, "ps=%u: overflow lookups wrong: %d", ps, bad);
	/* a small cap gets the true length and a prefix */
	snprintf(k, sizeof k, "o%05d", 4);
	fill(v, sizes[4 % nsz], 4u);
	CHECK(bt_lookup(bt, k, 6, out, 100, &vl) == XTC_OK &&
	    vl == sizes[4 % nsz] && memcmp(out, v, 100) == 0,
	    "ps=%u: short-cap lookup of an out-of-line value", ps);
	/* cursor scan sees every value whole, in order */
	CHECK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK, "cursor");
	for (i = 0; i < 200; i++) {
		const void *ck, *cv; uint16_t ckl, cvl;
		uint32_t n = (i % 3 == 0) ? 40u : sizes[i % nsz];
		if (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) != XTC_OK) { bad++; break; }
		fill(v, n, (uint32_t)i);
		if (cvl != n || memcmp(cv, v, n) != 0) bad++;
	}
	bt_cursor_close(c);
	CHECK(bad == 0, "ps=%u: cursor values wrong: %d", ps, bad);
	/* upserts: out-of-line -> inline, inline -> out-of-line, ovf -> ovf */
	for (i = 0; i < 200; i++) {
		uint32_t n = (i % 2 == 0) ? 30u : 12000u;
		snprintf(k, sizeof k, "o%05d", i);
		fill(v, n, (uint32_t)i + 7u);
		if (bt_insert(bt, k, 6, v, (uint16_t)n) != XTC_OK) bad++;
	}
	for (i = 0; i < 200; i++) {
		uint32_t n = (i % 2 == 0) ? 30u : 12000u;
		snprintf(k, sizeof k, "o%05d", i);
		fill(v, n, (uint32_t)i + 7u);
		if (bt_lookup(bt, k, 6, out, sizeof out, &vl) != XTC_OK ||
		    vl != n || memcmp(out, v, n) != 0) bad++;
	}
	CHECK(bad == 0, "ps=%u: upserted values wrong: %d", ps, bad);
	/* delete everything: every data page must come back */
	for (i = 0; i < 200; i++) {
		snprintf(k, sizeof k, "o%05d", i);
		if (bt_delete(bt, k, 6) != XTC_OK) bad++;
	}
	bt_get_stats(bt, &st);
	CHECK(bad == 0, "ps=%u: deletes failed: %d", ps, bad);
	CHECK(st.ovf_pages == st.ovf_freed,
	    "ps=%u: ovf pages written %llu == freed %llu (no leak)", ps,
	    (unsigned long long)st.ovf_pages, (unsigned long long)st.ovf_freed);
	CHECK(st.ovf_writes > 0 && st.ovf_reads > 0, "ovf path exercised");
	done(bt, bm, p);
}

/* P5: bt_seek_lt against an oracle over random keys, including keys that
 * sit exactly on leaf boundaries, empty prefixes, and out-of-line values. */
static void
t_seek_lt(uint32_t ps)
{
	char p[64], k[16], q[16], got[64];
	bm_t *bm = mk(p, ps, 2048);
	bt_t *bt;
	static unsigned char live[20000];
	static unsigned char v[8192], out[8192];
	uint64_t s = 99 + ps;
	uint16_t gl, vl;
	int i, wrong = 0, n = 20000;

	memset(live, 0, sizeof live);
	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	CHECK(bt_seek_lt(bt, "z", 1, got, sizeof got, &gl, NULL, 0, NULL) ==
	    XTC_E_NOTFOUND, "empty tree: nothing below z");
	for (i = 0; i < n; i++) {
		if (rnd(&s) % 3 == 0) continue;   /* holes */
		snprintf(k, sizeof k, "s%07d", i);
		fill(v, i % 50 == 0 ? 5000u : 20u, (uint32_t)i);
		if (bt_insert(bt, k, 8, v, (uint16_t)(i % 50 == 0 ? 5000 : 20)) == XTC_OK)
			live[i] = 1;
	}
	/* delete a run to make empty-ish leaves */
	for (i = 5000; i < 9000; i++) {
		snprintf(k, sizeof k, "s%07d", i);
		if (live[i] && bt_delete(bt, k, 8) == XTC_OK) live[i] = 0;
	}
	for (i = 0; i <= n; i += 7) {
		int j, rc;
		snprintf(q, sizeof q, "s%07d", i);
		for (j = i - 1; j >= 0 && !live[j]; j--)
			;
		rc = bt_seek_lt(bt, q, 8, got, sizeof got, &gl, out, sizeof out, &vl);
		if (j < 0) {
			if (rc != XTC_E_NOTFOUND) wrong++;
		} else {
			snprintf(k, sizeof k, "s%07d", j);
			fill(v, j % 50 == 0 ? 5000u : 20u, (uint32_t)j);
			if (rc != XTC_OK || gl != 8 || memcmp(got, k, 8) != 0 ||
			    vl != (j % 50 == 0 ? 5000 : 20) ||
			    memcmp(out, v, vl) != 0)
				wrong++;
		}
	}
	/* a bound past everything returns the last key */
	CHECK(bt_seek_lt(bt, "t", 1, got, sizeof got, &gl, NULL, 0, NULL) ==
	    XTC_OK && gl == 8, "seek below 't' finds the last key");
	CHECK(wrong == 0, "ps=%u: bt_seek_lt wrong answers: %d", ps, wrong);
	done(bt, bm, p);
}

/* The inline bound on 4 KiB pages: an entry of up to (cap - 2K) / 3 =
 * 1018 bytes (1008 of key + value; 1010 before the 1.60 page checksum
 * took 8 bytes of the node header) stays in its leaf.  It was cap/4 - K
 * = 516 (506 of key + value), and every TPC-C customer row (about 660 B)
 * took a 4 KiB out-of-line page of its own.  2,000 rows of 700 B with
 * 20 B keys: none out of line, about 5 rows a leaf, all intact. */
static void
t_inline_bound(void)
{
	char p[64], k[24];
	bm_t *bm = mk(p, 4096, 1024);
	bt_t *bt;
	bt_stats_t st;
	static unsigned char v[1100], out[1100];
	uint16_t vl;
	int i, bad = 0;

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	CHECK(bt_max_inline(bt) == 1008, "4 KiB inline bound %u (want 1008)",
	    bt_max_inline(bt));
	for (i = 0; i < 2000; i++) {
		snprintf(k, sizeof k, "row%017d", (i * 7919) % 2000);
		fill(v, 700, (uint32_t)i);
		if (bt_insert(bt, k, 20, v, 700) != XTC_OK) bad++;
	}
	for (i = 0; i < 2000; i++) {
		snprintf(k, sizeof k, "row%017d", (i * 7919) % 2000);
		fill(v, 700, (uint32_t)i);
		if (bt_lookup(bt, k, 20, out, sizeof out, &vl) != XTC_OK || vl != 700 ||
		    memcmp(out, v, 700) != 0) bad++;
	}
	bt_get_stats(bt, &st);
	CHECK(bad == 0, "700 B rows: %d failed", bad);
	CHECK(st.ovf_writes == 0, "700 B rows inline: %llu out of line",
	    (unsigned long long)st.ovf_writes);
	CHECK(st.splits < 2000 / 3, "%llu leaves for 2000 700 B rows",
	    (unsigned long long)st.splits + 1);
	done(bt, bm, p);
}

int
main(void)
{
	uint64_t sd;

	t_olc_large_value(4096);
	t_olc_large_value(16384);    /* > 1024 B value: the L1 case */
	t_two_max_entries(4096);
	t_two_max_entries(16384);
	t_mixed_split(4096);
	t_mixed_split(16384);
	t_refuse_oversize();
	t_inline_bound();
	t_seek_lt(4096);
	t_seek_lt(16384);
	t_overflow(4096);
	t_overflow(16384);
	t_big_pages(16384);
	t_big_pages(32768);
	for (sd = 1; sd <= 4; sd++) {
		t_property(4096, sd);
		t_property(16384, sd + 100);
	}
	if (g_fail) {
		fprintf(stderr, "test_btree_limits: FAILED (%d checks)\n", g_checks);
		return 1;
	}
	printf("test_btree_limits: ALL PASS (%d checks)\n", g_checks);
	return 0;
}

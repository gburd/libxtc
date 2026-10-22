/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sx_size.c
 *	Store size attribution: where do the bytes of a sqlxtc store go?
 *
 *	    sx_size STORE.xstore [STORE.xstore-wal]
 *
 *	Reads a CLOSED store's page file directly (no buffer manager):
 *	walks the B-tree from the superblock's root, classifies every page
 *	(internal, leaf, out-of-line value, unreachable), and attributes
 *	every leaf entry to the table, index or catalog it belongs to --
 *	entries, row versions, live rows, slot / key / value bytes, the
 *	out-of-line pages, the record's column encoding, and what the same
 *	record would take in SQLite's varint format.  With a log path it
 *	also breaks the log down by record type.  Read-only.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btnode.h"
#include "btovf.h"
#include "wal.h"
#include "xlog.h"
#include "xstore.h"
#include "xtc.h"

#define NCAT      512
#define VKLEN     20
#define IDX_LEAD  0xFEu
#define CAT_TID   0u
#define IDXCAT_TID 0xFFFFFFFDu
#define STAGE_TID  0xFFFFFFFEu

/* One attribution bucket: a table (by id), an index (by id) or a
 * reserved namespace. */
struct cat {
	int      used;
	int      is_idx;
	uint32_t id;
	char     name[64];
	uint64_t entries, live, tomb, rows;   /* rows: distinct rowids */
	uint64_t slot_b, key_full_b, key_sfx_b, val_b;
	uint64_t ovf_n, ovf_logical_b, ovf_pages, ovf_used_b;
	uint64_t rec_b, rec_sqlite_b;         /* logical record bytes, SQLite est. */
	uint64_t n_int, n_real, n_text, n_null, text_b;
	uint64_t int_b_now, int_b_sqlite;
	uint64_t idx_elems, idx_num, idx_num_b, idx_txt, idx_txt_b;
	uint64_t pages, page_used_b;          /* leaves whose first entry is ours */
	int64_t  last_rid;
	int      have_last;
};

static struct cat g_cat[NCAT];
static int g_ncat;
static int g_fd;
static uint32_t g_ps;
static uint64_t g_npages;
static uint8_t *g_seen;        /* reachability bitmap */

static struct cat *
cat_get(int is_idx, uint32_t id)
{
	int i;

	for (i = 0; i < g_ncat; i++)
		if (g_cat[i].is_idx == is_idx && g_cat[i].id == id)
			return &g_cat[i];
	if (g_ncat == NCAT)
		return &g_cat[NCAT - 1];
	g_cat[g_ncat].used = 1;
	g_cat[g_ncat].is_idx = is_idx;
	g_cat[g_ncat].id = id;
	if (!is_idx && id == CAT_TID)
		snprintf(g_cat[g_ncat].name, 64, "(catalog)");
	else if (!is_idx && id == IDXCAT_TID)
		snprintf(g_cat[g_ncat].name, 64, "(index catalog)");
	else if (!is_idx && id == STAGE_TID)
		snprintf(g_cat[g_ncat].name, 64, "(spill staging)");
	else
		snprintf(g_cat[g_ncat].name, 64, "%s %u", is_idx ? "index" : "table", id);
	return &g_cat[g_ncat++];
}

static uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3];
}

static int64_t
be64s(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return (int64_t)(v ^ 0x8000000000000000ull);
}

static int
rd(uint64_t pid, uint8_t *buf)
{
	return pread(g_fd, buf, g_ps, (off_t)(pid * g_ps)) == (ssize_t)g_ps ? 0 : -1;
}

static void
mark(uint64_t pid)
{
	if (pid < g_npages)
		g_seen[pid >> 3] |= (uint8_t)(1u << (pid & 7));
}

/* SQLite's varint length and integer serial-type body size. */
static int
vlen(uint64_t v)
{
	int n = 1;

	while (v > 0x7f && n < 9) {
		v >>= 7;
		n++;
	}
	return n;
}

static int
sqlite_int_body(int64_t v)
{
	uint64_t u;

	if (v == 0 || v == 1)
		return 0;
	u = v < 0 ? ~(uint64_t)v : (uint64_t)v;
	if (u <= 127) return 1;
	if (u <= 32767) return 2;
	if (u <= 8388607) return 3;
	if (u <= 2147483647) return 4;
	if (u <= 140737488355327LL) return 6;
	return 8;
}

static void
note_record(struct cat *c, const uint8_t *rec, int reclen)
{
	int ncol, i, hdr = 0, body = 0;

	c->rec_b += (uint64_t)reclen;
	ncol = xstore_rec_ncol(rec, reclen);
	for (i = 0; i < ncol; i++) {
		int64_t iv = 0;
		double rv = 0;
		const uint8_t *p = NULL;
		int n = 0, cls = xstore_rec_col(rec, reclen, i, &iv, &rv, &p, &n);

		switch (cls) {
		case XSTORE_C_INT:
			c->n_int++;
			c->int_b_sqlite += (uint64_t)sqlite_int_body(iv);
			hdr += 1;
			body += sqlite_int_body(iv);
			break;
		case XSTORE_C_REAL:
			c->n_real++;
			hdr += 1;
			body += 8;
			break;
		case XSTORE_C_TEXT:
		case XSTORE_C_BLOB:
			c->n_text++;
			c->text_b += (uint64_t)n;
			hdr += vlen((uint64_t)n * 2 + 13);
			body += n;
			break;
		default:
			c->n_null++;
			hdr += 1;
			break;
		}
	}
	hdr += vlen((uint64_t)hdr + 1);
	c->rec_sqlite_b += (uint64_t)(hdr + body);
}

/* An index entry [0xFE][id:4][elem...][rowid:8]: count its elements. */
static void
note_index(struct cat *c, const uint8_t *k, int kl)
{
	int off = 5, end = kl - 8;

	while (off < end) {
		uint8_t t = k[off], d = (t & 0x80) ? 0xFF : 0;   /* DESC: complemented */
		int start = off;

		c->idx_elems++;
		t ^= d;
		if (t >= 0x0C && t <= 0x1E) {
			/* a number (xstore.c xs_tup_num): tag 0x15 +- class k;
			 * k bytes of m, then 8 more if m is odd; class 9 is 8 */
			int cl = t >= 0x15 ? t - 0x15 : 0x15 - t, nb = cl == 9 ? 8 : cl;
			/* m's low byte, un-complemented (DESC and negative) */
			uint8_t lo = (nb > 0 && off + nb < kl) ?
			    (uint8_t)(k[off + nb] ^ d ^ (t < 0x15 ? 0xFF : 0)) : 0;

			c->idx_num++;
			off += 1 + nb + ((cl < 9 && (lo & 1u)) ? 8 : 0);
			c->idx_num_b += (uint64_t)(off - start);
		} else if (t == 0x25 || t == 0x35) {
			off++;
			while (off + 1 < end && !((k[off] ^ d) == 0 && (k[off + 1] ^ d) == 0))
				off += ((k[off] ^ d) == 0) ? 2 : 1;
			off += 2;
			c->idx_txt++;
			c->idx_txt_b += (uint64_t)(off - start);
		} else {
			off++;
		}
	}
}

static void
note_name(const uint8_t *k, const uint8_t *v, uint16_t vl)
{
	uint32_t tid = be32(k);
	int64_t rid = be64s(k + 4);
	char nm[64];
	int i;

	if (vl < 2 || (v[0] & 1))
		return;
	for (i = 0; i + 1 < vl && i < 63 && v[1 + i]; i++)
		nm[i] = (char)v[1 + i];
	nm[i] = '\0';
	if (tid == CAT_TID)
		snprintf(cat_get(0, (uint32_t)rid)->name, 64, "%s", nm);
	else if (tid == IDXCAT_TID)
		snprintf(cat_get(1, (uint32_t)rid)->name, 64, "%s", nm);
}

struct pstat {
	uint64_t n, used, slots, fences, prefix;
};
static struct pstat g_leaf, g_inner;
static uint64_t g_ovf_pages, g_ovf_used;

static void
walk_leaf(const uint8_t *pg)
{
	const struct btnode_hdr *h = (const void *)pg;
	static uint8_t key[2048], val[64 * 1024];
	struct cat *first;
	int s;

	g_leaf.n++;
	g_leaf.used += sizeof *h + (uint64_t)h->count * 10u + h->space_used;
	g_leaf.slots += h->count;
	g_leaf.fences += (uint64_t)h->lo_fence_len + h->hi_fence_len;
	g_leaf.prefix += h->prefix_len;
	first = NULL;
	for (s = 0; s < h->count; s++) {
		const void *ks, *vv;
		uint16_t ksl, vlv, kl;
		struct cat *c;
		int flag;
		const uint8_t *vp;
		uint32_t vlog;

		if (btnode_get(pg, s, &ks, &ksl, &vv, &vlv) != 0 ||
		    btnode_full_key(pg, s, key, sizeof key, &kl) != 0)
			continue;
		flag = btnode_vflag(pg, s) != 0;
		if (kl >= 5 && key[0] == IDX_LEAD)
			c = cat_get(1, be32(key + 1));
		else if (kl == VKLEN)
			c = cat_get(0, be32(key));
		else
			c = cat_get(0, 0xFFFFFFFFu);
		if (first == NULL) {
			first = c;
			c->pages++;
			c->page_used_b += sizeof *h + (uint64_t)h->count * 10u +
			    h->space_used;
		}
		c->entries++;
		c->slot_b += 10;
		c->key_full_b += kl;
		c->key_sfx_b += ksl;
		c->val_b += vlv;
		vp = vv;
		vlog = vlv;
		if (flag) {
			uint32_t np = ((const uint8_t *)vv)[1], i, off = 0, tot;

			tot = bt_ovf_get32((const uint8_t *)vv + 4);
			c->ovf_n++;
			c->ovf_logical_b += tot;
			for (i = 0; i < np; i++) {
				uint64_t pid = bt_ovf_get64((const uint8_t *)vv +
				    BT_OVF_HDR + 8u * i);
				uint8_t op[65536];
				uint32_t len;

				mark(pid);
				c->ovf_pages++;
				g_ovf_pages++;
				if (rd(pid, op) != 0)
					continue;
				len = bt_ovf_get32(op + 12);
				c->ovf_used_b += len;
				g_ovf_used += len + BT_OVF_PGHDR;
				if (off + len <= sizeof val)
					memcpy(val + off, op + BT_OVF_PGHDR, len);
				off += len;
			}
			vp = val;
			vlog = off;
		}
		if (c->is_idx) {
			note_index(c, key, kl);
			continue;
		}
		if (kl != VKLEN)
			continue;
		if (!c->have_last || c->last_rid != be64s(key + 4)) {
			c->rows++;
			c->last_rid = be64s(key + 4);
			c->have_last = 1;
			if (vlog >= 1 && !(vp[0] & 1))
				c->live++;
		}
		if (vlog >= 1 && (vp[0] & 1))
			c->tomb++;
		if (be32(key) == CAT_TID || be32(key) == IDXCAT_TID)
			note_name(key, vp, (uint16_t)vlog);
		else if (vlog > 1 && !(vp[0] & 1) && be32(key) != STAGE_TID)
			note_record(c, vp + 1, (int)vlog - 1);
	}
}

/* Depth-first over the tree, children in key order (so a table's
 * versions are visited newest-first per rowid, as stored). */
static int
walk(uint64_t pid, int depth)
{
	uint8_t *pg = malloc(g_ps);
	const struct btnode_hdr *h;
	int s;

	if (pg == NULL || depth > 32 || rd(pid, pg) != 0) {
		free(pg);
		return -1;
	}
	mark(pid);
	h = (const void *)pg;
	if (h->is_leaf) {
		walk_leaf(pg);
		free(pg);
		return 0;
	}
	g_inner.n++;
	g_inner.used += sizeof *h + (uint64_t)h->count * 10u + h->space_used;
	g_inner.slots += h->count;
	g_inner.fences += (uint64_t)h->lo_fence_len + h->hi_fence_len;
	for (s = 0; s < h->count; s++) {
		const void *vv;
		uint16_t vl;
		uint64_t child = 0;

		if (btnode_get(pg, s, NULL, NULL, &vv, &vl) != 0 || vl < 8)
			continue;
		memcpy(&child, vv, 8);
		(void)walk(child, depth + 1);
	}
	free(pg);
	return 0;
}

/* ---- the log ---- */
struct lstat {
	uint64_t frames, frame_b, recs[16], rec_b[16], redo_b, undo_b;
	uint64_t upd_tables[NCAT];
};
static struct lstat g_log;

static int
log_cb(uint64_t lsn, const void *buf, uint32_t len, void *user)
{
	const uint8_t *p = buf;
	uint32_t off = 0;

	(void)lsn;
	(void)user;
	g_log.frames++;
	g_log.frame_b += len + 20u;      /* [lsn:8][len:4] body [crc:8] */
	while (off < len) {
		int n = xl_record_len(p + off, len - off);
		uint8_t t;

		if (n <= 0)
			break;
		t = p[off] < 16 ? p[off] : 0;
		g_log.recs[t]++;
		g_log.rec_b[t] += (uint64_t)n;
		if (t == XL_UPDATE || t == XL_CLR) {
			xl_hdr_t h;
			xl_body_t b;

			if ((t == XL_UPDATE ? xl_parse_update(p + off, (uint32_t)n, &h, &b)
			    : xl_parse_clr(p + off, (uint32_t)n, &h, &b)) == XTC_OK) {
				g_log.redo_b += b.redo_len;
				g_log.undo_b += b.undo_len;
			}
		}
		off += (uint32_t)n;
	}
	return 0;
}

static double
mb(uint64_t b)
{
	return (double)b / 1e6;
}

int
main(int argc, char **argv)
{
	struct stat st;
	uint8_t sb[64];
	uint64_t root, reach = 0, i;
	struct cat *c;
	int k;

	if (argc < 2) {
		fprintf(stderr, "usage: sx_size STORE.xstore [STORE.xstore-wal]\n");
		return 2;
	}
	if ((g_fd = open(argv[1], O_RDONLY)) < 0 || fstat(g_fd, &st) != 0 ||
	    pread(g_fd, sb, sizeof sb, 0) != (ssize_t)sizeof sb) {
		perror(argv[1]);
		return 1;
	}
	if (be32(sb) != 0 && *(uint32_t *)sb != 0x58425431u) {
		fprintf(stderr, "%s: not a sqlxtc page file\n", argv[1]);
		return 1;
	}
	memcpy(&g_ps, sb + 4, 4);
	memcpy(&root, sb + 8, 8);
	if (g_ps < 512 || g_ps > 65536) {
		fprintf(stderr, "bad page size %u\n", g_ps);
		return 1;
	}
	g_npages = (uint64_t)st.st_size / g_ps;
	g_seen = calloc((size_t)(g_npages / 8 + 1), 1);
	mark(0);
	(void)walk(root, 0);
	for (i = 0; i < g_npages; i++)
		if (g_seen[i >> 3] & (1u << (i & 7)))
			reach++;

	printf("page file %s: %.1f MB, %" PRIu64 " pages of %u B\n", argv[1],
	    mb((uint64_t)st.st_size), g_npages, g_ps);
	printf("  inner  %10" PRIu64 " pages %8.1f MB  fill %5.1f%%  slots/page %.1f\n",
	    g_inner.n, mb(g_inner.n * g_ps),
	    g_inner.n ? 100.0 * (double)g_inner.used / ((double)g_inner.n * g_ps) : 0,
	    g_inner.n ? (double)g_inner.slots / (double)g_inner.n : 0);
	printf("  leaf   %10" PRIu64 " pages %8.1f MB  fill %5.1f%%  slots/page %.1f  "
	    "prefix %.1f B  fences %.1f B/page\n",
	    g_leaf.n, mb(g_leaf.n * g_ps),
	    g_leaf.n ? 100.0 * (double)g_leaf.used / ((double)g_leaf.n * g_ps) : 0,
	    g_leaf.n ? (double)g_leaf.slots / (double)g_leaf.n : 0,
	    g_leaf.n ? (double)g_leaf.prefix / (double)g_leaf.n : 0,
	    g_leaf.n ? (double)g_leaf.fences / (double)g_leaf.n : 0);
	printf("  ovf    %10" PRIu64 " pages %8.1f MB  fill %5.1f%%\n", g_ovf_pages,
	    mb(g_ovf_pages * g_ps),
	    g_ovf_pages ? 100.0 * (double)g_ovf_used / ((double)g_ovf_pages * g_ps) : 0);
	printf("  unreachable (free/leaked) %" PRIu64 " pages %.1f MB\n",
	    g_npages - reach, mb((g_npages - reach) * g_ps));

	printf("\n%-26s %10s %9s %9s %7s %7s %7s %8s %8s %8s %6s\n", "object", "entries",
	    "rows", "live", "keyfull", "keysfx", "val", "leaf_pg", "ovf_pg", "MB", "fill%");
	for (k = 0; k < g_ncat; k++) {
		c = &g_cat[k];
		if (c->entries == 0)
			continue;
		printf("%-26.26s %10" PRIu64 " %9" PRIu64 " %9" PRIu64 " %7.1f %7.1f "
		    "%7.1f %8" PRIu64 " %8" PRIu64 " %8.1f %6.1f\n", c->name, c->entries,
		    c->rows, c->live, (double)c->key_full_b / (double)c->entries,
		    (double)c->key_sfx_b / (double)c->entries,
		    (double)c->val_b / (double)c->entries, c->pages, c->ovf_pages,
		    mb((c->pages + c->ovf_pages) * g_ps),
		    c->pages ? 100.0 * (double)c->page_used_b / ((double)c->pages * g_ps) : 0);
	}
	printf("\nrecord encoding (inline + out-of-line, live and old versions):\n");
	for (k = 0; k < g_ncat; k++) {
		c = &g_cat[k];
		if (c->is_idx || c->rec_b == 0)
			continue;
		printf("  %-24.24s rec %6.1f B avg (sqlite fmt %6.1f)  int %" PRIu64
		    " (sqlite body %.2f B avg)  real %" PRIu64 "  text %" PRIu64
		    " (%.1f B avg)  null %" PRIu64 "  ovf %" PRIu64 " (%.0f B avg)\n",
		    c->name, (double)c->rec_b / (double)(c->entries - c->tomb),
		    (double)c->rec_sqlite_b / (double)(c->entries - c->tomb), c->n_int,
		    c->n_int ? (double)c->int_b_sqlite / (double)c->n_int : 0, c->n_real,
		    c->n_text, c->n_text ? (double)c->text_b / (double)c->n_text : 0,
		    c->n_null, c->ovf_n,
		    c->ovf_n ? (double)c->ovf_logical_b / (double)c->ovf_n : 0);
	}
	printf("\nindex entries:\n");
	for (k = 0; k < g_ncat; k++) {
		c = &g_cat[k];
		if (!c->is_idx || c->entries == 0)
			continue;
		printf("  %-24.24s %10" PRIu64 " entries  key %.1f B (stored %.1f)  num elems %"
		    PRIu64 " (%.1f B)  text elems %" PRIu64 " (%.1f B)\n", c->name,
		    c->entries, (double)c->key_full_b / (double)c->entries,
		    (double)c->key_sfx_b / (double)c->entries, c->idx_num,
		    c->idx_num ? (double)c->idx_num_b / (double)c->idx_num : 0, c->idx_txt,
		    c->idx_txt ? (double)c->idx_txt_b / (double)c->idx_txt : 0);
	}
	if (argc > 2) {
		if (stat(argv[2], &st) != 0) {
			perror(argv[2]);
			return 1;
		}
		(void)wal_scan(argv[2], log_cb, NULL);
		printf("\nlog %s: %.1f MB on disk, %" PRIu64 " frames, %.1f MB framed\n",
		    argv[2], mb((uint64_t)st.st_size), g_log.frames, mb(g_log.frame_b));
		for (k = 1; k < 16; k++)
			if (g_log.recs[k])
				printf("  type %2d  %10" PRIu64 " records  %8.1f MB\n", k,
				    g_log.recs[k], mb(g_log.rec_b[k]));
		printf("  redo payload %.1f MB, undo payload %.1f MB\n", mb(g_log.redo_b),
		    mb(g_log.undo_b));
	}
	close(g_fd);
	return 0;
}

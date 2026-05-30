/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/pcache_obj.c
 *	An xtc_slab-backed SQLite page cache (sqlite3_pcache_methods2).
 *
 *	One xtc_slab per cache supplies the page objects; all pages in
 *	a cache share one object-size class (header + szPage + szExtra),
 *	so the slab never fragments.  A chained hash table maps the
 *	page key to its resident page; an LRU list of unpinned pages
 *	feeds recycling for purgeable caches.  All methods run under
 *	SQLite's mutex, so no internal locking is required.
 */

#include "pcache.h"
#include "sqlite/sqlite3.h"

#include <stdlib.h>
#include <string.h>

#include "xtc_int.h"
#include "xtc_slab.h"
#include "xtc_stats.h"

/* A resident page.  The page buffer and extra region follow this
 * header inside one slab object:
 *
 *	[ pcache_pg | <align pad> | pBuf (szPage) | pExtra (szExtra) ]
 */
struct pcache_pg {
	sqlite3_pcache_page  page;       /* {pBuf, pExtra}; given to SQLite */
	unsigned             key;
	int                  pinned;
	int                  in_hash;
	struct pcache_pg    *hnext;      /* hash chain */
	struct pcache_pg    *lru_prev;   /* unpinned LRU (head = MRU) */
	struct pcache_pg    *lru_next;
};

struct pcache_obj {
	int                  sz_page;
	int                  sz_extra;
	int                  purgeable;
	int                  max_pages;  /* suggested cap (xCachesize) */
	size_t               data_off;   /* header size, rounded for align */
	size_t               obj_size;   /* slab object size */
	xtc_slab_t          *slab;
	struct pcache_pg   **buckets;
	int                  nbucket;    /* power of two */
	int                  npage;      /* pages currently in the hash */
	struct pcache_pg    *lru_head;   /* MRU */
	struct pcache_pg    *lru_tail;   /* LRU; recycle victim */
};

/* ---- instrumentation ---- */
static xtc_counter_t *g_c_hit;
static xtc_counter_t *g_c_miss;
static xtc_counter_t *g_c_alloc;
static xtc_counter_t *g_c_recycle;
static xtc_gauge_t   *g_g_live;
static long           g_live;        /* live pages across all caches */
static int            g_registered;

static size_t
round_up(size_t v, size_t a)
{
	return (v + (a - 1)) & ~(a - 1);
}

/* ---- hash helpers ---- */

static unsigned
bucket_of(struct pcache_obj *c, unsigned key)
{
	/* nbucket is a power of two. */
	return key & (unsigned)(c->nbucket - 1);
}

static struct pcache_pg *
hash_find(struct pcache_obj *c, unsigned key)
{
	struct pcache_pg *p = c->buckets[bucket_of(c, key)];
	for (; p != NULL; p = p->hnext)
		if (p->key == key)
			return p;
	return NULL;
}

static void
hash_insert(struct pcache_obj *c, struct pcache_pg *p)
{
	unsigned b = bucket_of(c, p->key);
	p->hnext = c->buckets[b];
	c->buckets[b] = p;
	p->in_hash = 1;
	c->npage++;
	g_live++;
}

static void
hash_remove(struct pcache_obj *c, struct pcache_pg *p)
{
	unsigned b = bucket_of(c, p->key);
	struct pcache_pg **pp = &c->buckets[b];
	while (*pp != NULL) {
		if (*pp == p) {
			*pp = p->hnext;
			break;
		}
		pp = &(*pp)->hnext;
	}
	p->in_hash = 0;
	c->npage--;
	g_live--;
}

static int
hash_grow(struct pcache_obj *c)
{
	int new_n = c->nbucket << 1;
	struct pcache_pg **nb;
	int i;

	nb = (struct pcache_pg **)calloc((size_t)new_n, sizeof *nb);
	if (nb == NULL)
		return -1;            /* keep the old table; not fatal */
	/* Rehash. */
	for (i = 0; i < c->nbucket; i++) {
		struct pcache_pg *p = c->buckets[i];
		while (p != NULL) {
			struct pcache_pg *next = p->hnext;
			unsigned b = p->key & (unsigned)(new_n - 1);
			p->hnext = nb[b];
			nb[b] = p;
			p = next;
		}
	}
	free(c->buckets);
	c->buckets = nb;
	c->nbucket = new_n;
	return 0;
}

/* ---- LRU helpers (unpinned pages only) ---- */

static void
lru_add_head(struct pcache_obj *c, struct pcache_pg *p)
{
	p->lru_prev = NULL;
	p->lru_next = c->lru_head;
	if (c->lru_head != NULL)
		c->lru_head->lru_prev = p;
	c->lru_head = p;
	if (c->lru_tail == NULL)
		c->lru_tail = p;
}

static void
lru_remove(struct pcache_obj *c, struct pcache_pg *p)
{
	if (p->lru_prev != NULL)
		p->lru_prev->lru_next = p->lru_next;
	else if (c->lru_head == p)
		c->lru_head = p->lru_next;
	if (p->lru_next != NULL)
		p->lru_next->lru_prev = p->lru_prev;
	else if (c->lru_tail == p)
		c->lru_tail = p->lru_prev;
	p->lru_prev = p->lru_next = NULL;
}

static void
page_init_bufs(struct pcache_obj *c, struct pcache_pg *p)
{
	p->page.pBuf = (char *)p + c->data_off;
	p->page.pExtra = (char *)p->page.pBuf + c->sz_page;
}

/* Prepare a page (recycled or freshly allocated) to be returned for a
 * NEW key.  Zeroing pExtra is essential: SQLite stores its PgHdr
 * there and decides a fetched page is already valid -- skipping the
 * read from disk -- when PgHdr->pPage is non-NULL.  A recycled page
 * still carries the previous occupant's PgHdr, so without this zero
 * SQLite would trust stale bytes for the new page number and corrupt
 * the b-tree.  (A fresh slab page only happens to work because new
 * mmap memory is zero.) */
static void
page_claim(struct pcache_obj *c, struct pcache_pg *p, unsigned key)
{
	p->key = key;
	p->pinned = 1;
	if (c->sz_extra > 0)
		memset(p->page.pExtra, 0, (size_t)c->sz_extra);
}

/* ---- sqlite3_pcache_methods2 ---- */

static int
pcache_init(void *arg)
{
	(void)arg;
	return SQLITE_OK;
}

static void
pcache_shutdown(void *arg)
{
	(void)arg;
}

static sqlite3_pcache *
pcache_create(int sz_page, int sz_extra, int purgeable)
{
	struct pcache_obj *c;
	xtc_slab_opts_t so;

	c = (struct pcache_obj *)calloc(1, sizeof *c);
	if (c == NULL)
		return NULL;
	c->sz_page = sz_page;
	c->sz_extra = sz_extra;
	c->purgeable = purgeable;
	c->max_pages = 100;
	c->data_off = round_up(sizeof(struct pcache_pg), 16);
	c->obj_size = c->data_off + (size_t)sz_page + (size_t)sz_extra;

	c->nbucket = 256;
	c->buckets = (struct pcache_pg **)calloc((size_t)c->nbucket,
	    sizeof *c->buckets);
	if (c->buckets == NULL) {
		free(c);
		return NULL;
	}

	memset(&so, 0, sizeof so);
	so.name = "sqlxtc.pcache";
	so.obj_size = c->obj_size;
	so.align = 64;
	if (xtc_slab_create(&so, &c->slab) != XTC_OK) {
		free(c->buckets);
		free(c);
		return NULL;
	}
	return (sqlite3_pcache *)c;
}

static void
pcache_cachesize(sqlite3_pcache *pc, int n)
{
	struct pcache_obj *c = (struct pcache_obj *)pc;
	c->max_pages = n > 0 ? n : 1;
}

static int
pcache_pagecount(sqlite3_pcache *pc)
{
	struct pcache_obj *c = (struct pcache_obj *)pc;
	return c->npage;
}

static sqlite3_pcache_page *
pcache_fetch(sqlite3_pcache *pc, unsigned key, int create_flag)
{
	struct pcache_obj *c = (struct pcache_obj *)pc;
	struct pcache_pg *p;

	p = hash_find(c, key);
	if (p != NULL) {
		if (!p->pinned) {
			lru_remove(c, p);
			p->pinned = 1;
		}
		xtc_counter_inc(g_c_hit);
		return &p->page;
	}

	xtc_counter_inc(g_c_miss);
	if (create_flag == 0)
		return NULL;

	/* Recycle an unpinned page when the purgeable cache is at (or
	 * over) its size and an unpinned victim exists.  This is the
	 * pcache's own job: keep itself near max_pages.  It applies
	 * whatever the createFlag -- the flag governs whether to fall
	 * back to a fresh allocation when no victim is available, not
	 * whether to reuse one. */
	if (c->purgeable && c->npage >= c->max_pages &&
	    c->lru_tail != NULL) {
		p = c->lru_tail;
		lru_remove(c, p);
		hash_remove(c, p);
		page_claim(c, p, key);
		hash_insert(c, p);
		xtc_counter_inc(g_c_recycle);
		return &p->page;
	}

	/* Otherwise allocate a fresh page from the slab.  If the slab
	 * is exhausted, fall back to recycling the LRU tail. */
	p = (struct pcache_pg *)xtc_slab_alloc(c->slab);
	if (p == NULL) {
		if (c->lru_tail == NULL)
			return NULL;
		p = c->lru_tail;
		lru_remove(c, p);
		hash_remove(c, p);
		page_init_bufs(c, p);
		page_claim(c, p, key);
		hash_insert(c, p);
		xtc_counter_inc(g_c_recycle);
		return &p->page;
	}

	memset(p, 0, c->data_off);
	page_init_bufs(c, p);
	page_claim(c, p, key);
	if (c->npage + 1 > c->nbucket)
		(void)hash_grow(c);
	hash_insert(c, p);
	xtc_counter_inc(g_c_alloc);
	return &p->page;
}

static void
pcache_unpin(sqlite3_pcache *pc, sqlite3_pcache_page *pp, int discard)
{
	struct pcache_obj *c = (struct pcache_obj *)pc;
	struct pcache_pg *p = (struct pcache_pg *)pp;

	if (!p->pinned)
		return;
	p->pinned = 0;

	if (discard || !c->purgeable) {
		if (discard) {
			if (p->in_hash)
				hash_remove(c, p);
			xtc_slab_free(c->slab, p);
			return;
		}
		/* Non-purgeable cache keeps the page resident but not
		 * eligible for recycling; leave it in the hash, off the
		 * LRU. */
		return;
	}
	lru_add_head(c, p);
}

static void
pcache_rekey(sqlite3_pcache *pc, sqlite3_pcache_page *pp,
         unsigned old_key, unsigned new_key)
{
	struct pcache_obj *c = (struct pcache_obj *)pc;
	struct pcache_pg *p = (struct pcache_pg *)pp;

	(void)old_key;
	if (p->in_hash)
		hash_remove(c, p);
	p->key = new_key;
	hash_insert(c, p);
}

static void
pcache_truncate(sqlite3_pcache *pc, unsigned limit)
{
	struct pcache_obj *c = (struct pcache_obj *)pc;
	int i;

	for (i = 0; i < c->nbucket; i++) {
		struct pcache_pg *p = c->buckets[i];
		while (p != NULL) {
			struct pcache_pg *next = p->hnext;
			if (p->key >= limit) {
				if (!p->pinned)
					lru_remove(c, p);
				hash_remove(c, p);
				xtc_slab_free(c->slab, p);
			}
			p = next;
		}
	}
}

static void
pcache_destroy(sqlite3_pcache *pc)
{
	struct pcache_obj *c = (struct pcache_obj *)pc;
	int i;

	/* Account live pages out before tearing the slab down. */
	for (i = 0; i < c->nbucket; i++) {
		struct pcache_pg *p = c->buckets[i];
		for (; p != NULL; p = p->hnext)
			g_live--;
	}
	xtc_slab_destroy(c->slab);
	free(c->buckets);
	free(c);
}

static void
pcache_shrink(sqlite3_pcache *pc)
{
	struct pcache_obj *c = (struct pcache_obj *)pc;

	while (c->lru_tail != NULL) {
		struct pcache_pg *p = c->lru_tail;
		lru_remove(c, p);
		hash_remove(c, p);
		xtc_slab_free(c->slab, p);
	}
}

static const sqlite3_pcache_methods2 pcache_methods = {
	1,                  /* iVersion */
	NULL,               /* pArg */
	pcache_init,
	pcache_shutdown,
	pcache_create,
	pcache_cachesize,
	pcache_pagecount,
	pcache_fetch,
	pcache_unpin,
	pcache_rekey,
	pcache_truncate,
	pcache_destroy,
	pcache_shrink
};

int
pcache_register(void)
{
	if (g_registered)
		return SQLITE_OK;

	(void)xtc_counter_create("sqlxtc.pcache.fetch_hit", &g_c_hit);
	(void)xtc_counter_create("sqlxtc.pcache.fetch_miss", &g_c_miss);
	(void)xtc_counter_create("sqlxtc.pcache.slab_alloc", &g_c_alloc);
	(void)xtc_counter_create("sqlxtc.pcache.recycle", &g_c_recycle);
	(void)xtc_gauge_create("sqlxtc.pcache.live_pages", &g_g_live);

	if (sqlite3_config(SQLITE_CONFIG_PCACHE2, &pcache_methods)
	    != SQLITE_OK)
		return SQLITE_ERROR;

	g_registered = 1;
	return SQLITE_OK;
}

void
pcache_get_stats(pcache_stats_t *out)
{
	if (out == NULL)
		return;
	memset(out, 0, sizeof *out);
	if (g_c_hit != NULL) out->fetch_hit = xtc_counter_read(g_c_hit);
	if (g_c_miss != NULL) out->fetch_miss = xtc_counter_read(g_c_miss);
	if (g_c_alloc != NULL) out->slab_alloc = xtc_counter_read(g_c_alloc);
	if (g_c_recycle != NULL) out->recycle = xtc_counter_read(g_c_recycle);
	out->live_pages = (uint64_t)(g_live < 0 ? 0 : g_live);
	if (g_g_live != NULL)
		xtc_gauge_set(g_g_live, g_live);
}

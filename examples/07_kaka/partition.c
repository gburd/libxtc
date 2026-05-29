/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/partition.c -- in-memory partition log core.
 *
 *	Phase 1 storage: a growable vector of offset slots, each holding
 *	a malloc'd copy of one record's key+value bytes.  Append is
 *	amortized O(1); read is O(1) by offset minus base.  Because the
 *	owning xtc_proc is the only writer, none of this needs a lock --
 *	the proc mailbox is the serialization point.
 */

#include "partition.h"

#include <stdlib.h>
#include <string.h>

struct slot {
	uint8_t *bytes;       /* key||value, malloc'd */
	uint32_t key_len;
	uint32_t value_len;
};

struct plog {
	struct slot *slots;
	uint64_t     base;     /* offset of slots[0]; 0 until compaction */
	uint64_t     count;    /* live records */
	uint64_t     cap;      /* slots allocated */
};

int
plog_create(plog_t **out)
{
	plog_t *l;
	if (out == NULL) return -1;
	l = calloc(1, sizeof *l);
	if (l == NULL) return -1;
	l->cap = 1024;
	l->slots = calloc((size_t)l->cap, sizeof *l->slots);
	if (l->slots == NULL) { free(l); return -1; }
	*out = l;
	return 0;
}

void
plog_destroy(plog_t *l)
{
	uint64_t i;
	if (l == NULL) return;
	for (i = 0; i < l->count; i++)
		free(l->slots[i].bytes);
	free(l->slots);
	free(l);
}

int64_t
plog_append(plog_t *l, const kaka_record_t *rec)
{
	struct slot *s;
	size_t total;
	if (l == NULL || rec == NULL) return -1;

	if (l->count == l->cap) {
		uint64_t ncap = l->cap * 2;
		struct slot *ns;
		if (ncap < l->cap) return -1;        /* overflow */
		ns = realloc(l->slots, (size_t)ncap * sizeof *ns);
		if (ns == NULL) return -1;
		l->slots = ns;
		l->cap = ncap;
	}

	total = (size_t)rec->key_len + rec->value_len;
	s = &l->slots[l->count];
	s->bytes = (total > 0) ? malloc(total) : NULL;
	if (total > 0 && s->bytes == NULL) return -1;
	if (rec->key_len)
		memcpy(s->bytes, rec->key, rec->key_len);
	if (rec->value_len)
		memcpy(s->bytes + rec->key_len, rec->value, rec->value_len);
	s->key_len = rec->key_len;
	s->value_len = rec->value_len;

	return (int64_t)(l->base + l->count++);
}

uint64_t
plog_high_water(const plog_t *l)
{
	return l == NULL ? 0 : l->base + l->count;
}

int
plog_read(plog_t *l, uint64_t offset, kaka_record_t *rec)
{
	uint64_t idx;
	struct slot *s;
	if (l == NULL || rec == NULL) return -1;
	if (offset < l->base) return -1;          /* compacted away */
	idx = offset - l->base;
	if (idx >= l->count) return 0;            /* past high-water */
	s = &l->slots[idx];
	rec->key = s->bytes;
	rec->key_len = s->key_len;
	rec->value = (s->bytes != NULL) ? s->bytes + s->key_len : NULL;
	rec->value_len = s->value_len;
	return 1;
}

uint64_t
plog_count(const plog_t *l)
{
	return l == NULL ? 0 : l->count;
}

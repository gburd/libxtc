/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/stream.c
 *	Async streams: a next()-function sequence with map/filter/for_each
 *	combinators, and a demand-channel adapter.  See src/inc/xtc_stream.h.
 *
 *	Each stream is a small tagged struct: a base stream holds a user
 *	next()+ctx; a map/filter stream holds a pointer to the source
 *	stream plus the transform.  next() is resolved by the tag.
 */

#include "xtc_int.h"
#include "xtc_stream.h"

enum stream_kind { S_BASE, S_MAP, S_FILTER, S_DEMAND };

struct xtc_stream {
	enum stream_kind kind;
	/* base */
	xtc_stream_next_fn next_fn;
	void              *ctx;
	/* combinator source (map/filter) -- owned */
	xtc_stream_t      *src;
	/* map / filter transform */
	void *(*map_fn)(void *v, void *user);
	int   (*pred_fn)(void *v, void *user);
	void   *user;
	/* demand adapter */
	xtc_chan_demand_t *ch;
};

int
xtc_stream_create(xtc_stream_next_fn next, void *ctx, xtc_stream_t **out)
{
	struct xtc_stream *s;
	int rc;
	if (next == NULL || out == NULL) return XTC_E_INVAL;
	*out = NULL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	s->kind = S_BASE;
	s->next_fn = next;
	s->ctx = ctx;
	*out = s;
	return XTC_OK;
}

void
xtc_stream_destroy(xtc_stream_t *s)
{
	if (s == NULL) return;
	if (s->src != NULL)          /* recurse into wrapped source */
		xtc_stream_destroy(s->src);
	__os_free(s);
}

int
xtc_stream_next(xtc_stream_t *s, void **out)
{
	if (s == NULL || out == NULL) return XTC_E_INVAL;
	*out = NULL;

	switch (s->kind) {
	case S_BASE:
		return s->next_fn(s->ctx, out);

	case S_DEMAND: {
		void *v = NULL;
		int rc;
		/* Grant one unit of demand, then try to pull one item. */
		(void)xtc_chan_demand_ask(s->ch, 1);
		rc = xtc_chan_demand_try_recv(s->ch, &v);
		if (rc == XTC_OK) { *out = v; return XTC_OK; }
		if (rc == XTC_E_INVAL) return XTC_E_NOTFOUND;  /* closed+drained */
		return XTC_E_AGAIN;
	}

	case S_MAP: {
		void *v = NULL;
		int rc = xtc_stream_next(s->src, &v);
		if (rc != XTC_OK) return rc;
		*out = s->map_fn(v, s->user);
		return XTC_OK;
	}

	case S_FILTER:
		for (;;) {
			void *v = NULL;
			int rc = xtc_stream_next(s->src, &v);
			if (rc != XTC_OK) return rc;
			if (s->pred_fn(v, s->user)) { *out = v; return XTC_OK; }
			/* predicate rejected: pull the next one */
		}
	}
	return XTC_E_INTERNAL;   /* unreachable */
}

static int
wrap(xtc_stream_t *src, enum stream_kind kind, xtc_stream_t **out,
     struct xtc_stream **sp)
{
	struct xtc_stream *s;
	int rc;
	if (src == NULL || out == NULL) return XTC_E_INVAL;
	*out = NULL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	s->kind = kind;
	s->src = src;      /* the new stream OWNS the source */
	*sp = s;
	*out = s;
	return XTC_OK;
}

int
xtc_stream_map(xtc_stream_t *s, void *(*fn)(void *, void *), void *user,
               xtc_stream_t **out)
{
	struct xtc_stream *m = NULL;
	int rc;
	if (fn == NULL) return XTC_E_INVAL;
	if ((rc = wrap(s, S_MAP, out, &m)) != XTC_OK) return rc;
	m->map_fn = fn;
	m->user = user;
	return XTC_OK;
}

int
xtc_stream_filter(xtc_stream_t *s, int (*pred)(void *, void *), void *user,
                  xtc_stream_t **out)
{
	struct xtc_stream *f = NULL;
	int rc;
	if (pred == NULL) return XTC_E_INVAL;
	if ((rc = wrap(s, S_FILTER, out, &f)) != XTC_OK) return rc;
	f->pred_fn = pred;
	f->user = user;
	return XTC_OK;
}

int
xtc_stream_for_each(xtc_stream_t *s, int (*fn)(void *, void *), void *user)
{
	if (s == NULL || fn == NULL) return XTC_E_INVAL;
	for (;;) {
		void *v = NULL;
		int rc = xtc_stream_next(s, &v);
		if (rc == XTC_E_NOTFOUND) return XTC_OK;   /* end of stream */
		if (rc == XTC_E_AGAIN) continue;           /* retry */
		if (rc != XTC_OK) return rc;               /* error */
		if ((rc = fn(v, user)) != 0) return rc;    /* early stop */
	}
}

int
xtc_stream_from_demand(xtc_chan_demand_t *ch, xtc_stream_t **out)
{
	struct xtc_stream *s;
	int rc;
	if (ch == NULL || out == NULL) return XTC_E_INVAL;
	*out = NULL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	s->kind = S_DEMAND;
	s->ch = ch;
	*out = s;
	return XTC_OK;
}

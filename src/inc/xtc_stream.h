/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_stream.h
 *	Async streams: a lazy, pull-based sequence of values with
 *	composable combinators (map, filter, for_each).  A stream is
 *	nothing more than a next() function that produces the next value
 *	(or signals end-of-stream), plus a small vtable so combinators can
 *	wrap one stream in another without materializing the whole
 *	sequence -- the Elixir Stream / Rust Iterator shape.
 *
 *	Pull-based composes naturally with the demand channel (R9): a
 *	consumer pulls one value at a time, so a slow map/filter stage
 *	exerts backpressure just by pulling more slowly.  Use
 *	xtc_stream_from_demand to turn an xtc_chan_demand into a stream.
 *
 *	Values are void*; the stream never owns them (it neither copies
 *	nor frees).  A combinator's transform decides ownership.
 */

#ifndef XTC_STREAM_H
#define XTC_STREAM_H

#include <stddef.h>

#include "xtc.h"
#include "xtc_chan.h"

typedef struct xtc_stream xtc_stream_t;

/*
 * Pull one value.  On XTC_OK, *out holds the next value.  Returns
 * XTC_E_NOTFOUND at end-of-stream, XTC_E_AGAIN if no value is available
 * yet but the stream is not finished (the caller should retry, e.g.
 * after a demand-channel item arrives), or another XTC_E_* on error.
 */
typedef int (*xtc_stream_next_fn)(void *ctx, void **out);

/*
 * PUBLIC: int  xtc_stream_create __P((xtc_stream_next_fn, void *, xtc_stream_t **));
 * PUBLIC: void xtc_stream_destroy __P((xtc_stream_t *));
 * PUBLIC: int  xtc_stream_next __P((xtc_stream_t *, void **));
 * PUBLIC: int  xtc_stream_map __P((xtc_stream_t *, void *(*)(void *, void *), void *, xtc_stream_t **));
 * PUBLIC: int  xtc_stream_filter __P((xtc_stream_t *, int (*)(void *, void *), void *, xtc_stream_t **));
 * PUBLIC: int  xtc_stream_for_each __P((xtc_stream_t *, int (*)(void *, void *), void *));
 * PUBLIC: int  xtc_stream_from_demand __P((xtc_chan_demand_t *, xtc_stream_t **));
 */

/* Create a stream from a next() function and its context. */
int  xtc_stream_create(xtc_stream_next_fn next, void *ctx,
                       xtc_stream_t **out);

/* Destroy a stream.  Destroying a combinator stream (map/filter) also
 * destroys the source stream it wraps, recursively; destroy only the
 * outermost stream.  Does not touch the underlying demand channel of a
 * from_demand stream (the caller owns that). */
void xtc_stream_destroy(xtc_stream_t *s);

/* Pull the next value.  See xtc_stream_next_fn for the return codes. */
int  xtc_stream_next(xtc_stream_t *s, void **out);

/* map: each pulled value v becomes fn(v, user).  Lazy: fn runs only when
 * the resulting stream is pulled.  The new stream OWNS `s`. */
int  xtc_stream_map(xtc_stream_t *s,
                    void *(*fn)(void *v, void *user), void *user,
                    xtc_stream_t **out);

/* filter: values for which pred(v, user) returns 0 are skipped.  The new
 * stream OWNS `s`. */
int  xtc_stream_filter(xtc_stream_t *s,
                       int (*pred)(void *v, void *user), void *user,
                       xtc_stream_t **out);

/* Drain the stream, calling fn(v, user) for each value until
 * end-of-stream.  A nonzero fn return stops early and is returned.  An
 * XTC_E_AGAIN from the source is treated as "retry": for_each spins on
 * it, so use it only with a source that will eventually produce or
 * finish (e.g. a fully-buffered demand channel that has been closed). */
int  xtc_stream_for_each(xtc_stream_t *s,
                         int (*fn)(void *v, void *user), void *user);

/* Adapt a demand channel into a stream: each pull grants one unit of
 * demand and returns the next buffered item (XTC_E_AGAIN until one
 * arrives, XTC_E_NOTFOUND once the channel is closed and drained). */
int  xtc_stream_from_demand(xtc_chan_demand_t *ch, xtc_stream_t **out);

#endif /* XTC_STREAM_H */

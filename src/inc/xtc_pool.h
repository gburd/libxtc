/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_pool.h
 *	A bounded resource pool: a fixed set of caller-owned resources
 *	(connections, buffers, handles) that fibers check out and return.
 *	Checkout blocks (up to a timeout) when all resources are busy, so
 *	the pool doubles as a concurrency limiter.  This is the checkout /
 *	return half of the connection-pool pattern; the supervisor's
 *	bounded dynamic pool (xtc_sup_opts_t.max_children) is the
 *	spawn-workers half.
 *
 *	The pool does not create or own the resources: the caller adds
 *	them with xtc_pool_add before use and is responsible for freeing
 *	them after xtc_pool_destroy.  The pool only tracks which are free
 *	vs checked out, and blocks a checkout until one is free.
 */

#ifndef XTC_POOL_H
#define XTC_POOL_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_pool xtc_pool_t;

/*
 * PUBLIC: int    xtc_pool_create __P((size_t, xtc_pool_t **));
 * PUBLIC: void   xtc_pool_destroy __P((xtc_pool_t *));
 * PUBLIC: int    xtc_pool_add __P((xtc_pool_t *, void *));
 * PUBLIC: int    xtc_pool_checkout __P((xtc_pool_t *, int64_t, void **));
 * PUBLIC: int    xtc_pool_checkin __P((xtc_pool_t *, void *));
 * PUBLIC: size_t xtc_pool_available __P((const xtc_pool_t *));
 * PUBLIC: size_t xtc_pool_capacity __P((const xtc_pool_t *));
 */

/* Create a pool sized for `capacity` resources.  Add resources with
 * xtc_pool_add before checking any out. */
int  xtc_pool_create(size_t capacity, xtc_pool_t **out);

/* Destroy the pool.  Does NOT free the resources -- the caller owns
 * them.  It is the caller's responsibility not to destroy a pool with
 * resources still checked out. */
void xtc_pool_destroy(xtc_pool_t *p);

/* Add a resource to the pool (as free/available).  Returns XTC_E_RESOURCE
 * if the pool is already at capacity. */
int  xtc_pool_add(xtc_pool_t *p, void *resource);

/* Check out a free resource into *out.  Blocks the calling fiber until
 * one is free or `timeout_ns` elapses (XTC_E_AGAIN).  timeout_ns == 0
 * means try-once (XTC_E_AGAIN immediately if none free); a negative
 * timeout blocks indefinitely. */
int  xtc_pool_checkout(xtc_pool_t *p, int64_t timeout_ns, void **out);

/* Return a previously checked-out resource, unblocking one waiter.
 * Returns XTC_E_INVAL if `resource` was not checked out from this pool. */
int  xtc_pool_checkin(xtc_pool_t *p, void *resource);

/* Number of resources currently free (available for checkout). */
size_t xtc_pool_available(const xtc_pool_t *p);

/* Total resources added to the pool (free + checked out). */
size_t xtc_pool_capacity(const xtc_pool_t *p);

#endif /* XTC_POOL_H */

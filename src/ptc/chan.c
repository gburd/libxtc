/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/ptc/chan.c
 *	The L3 channel implementations: oneshot, mpsc bounded, watch.
 *	All bounded with explicit caps; all charged through xtc_res so
 *	clients cannot exhaust the host.
 */

#include "xtc_int.h"
#include "xtc_chan.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ===== oneshot ===================================================== */

struct xtc_chan_oneshot {
	xtc_res_t *res;
	_Atomic int  state;       /* 0 empty, 1 written, 2 closed */
	_Atomic(void *) value;
	xtc_waker_t  waker;
	_Atomic int  has_waker;
};

int
xtc_chan_oneshot_create(xtc_res_t *res, xtc_chan_oneshot_t **out)
{
	xtc_chan_oneshot_t *c;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if (res != NULL) {
		if ((rc = xtc_res_acquire(res, XTC_RES_CHANNELS, 1)) != XTC_OK)
			return rc;
	}
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK) {
		if (res != NULL) xtc_res_release(res, XTC_RES_CHANNELS, 1);
		return rc;
	}
	c->res = res;
	atomic_store_explicit(&c->state, 0, memory_order_relaxed);
	atomic_store_explicit(&c->value, NULL, memory_order_relaxed);
	atomic_store_explicit(&c->has_waker, 0, memory_order_relaxed);
	*out = c;
	return XTC_OK;
}

void
xtc_chan_oneshot_destroy(xtc_chan_oneshot_t *c)
{
	if (c == NULL) return;
	if (c->res != NULL) xtc_res_release(c->res, XTC_RES_CHANNELS, 1);
	__os_free(c);
}

int
xtc_chan_oneshot_send(xtc_chan_oneshot_t *c, void *msg)
{
	int expected = 0;
	if (c == NULL) return XTC_E_INVAL;
	if (!atomic_compare_exchange_strong_explicit(
	        &c->state, &expected, 1,
	        memory_order_release, memory_order_relaxed))
		return XTC_E_INVAL;       /* already sent or closed */
	atomic_store_explicit(&c->value, msg, memory_order_release);
	if (atomic_load_explicit(&c->has_waker, memory_order_acquire))
		(void)xtc_waker_wake(&c->waker);
	return XTC_OK;
}

int
xtc_chan_oneshot_try_recv(xtc_chan_oneshot_t *c, void **out)
{
	if (c == NULL || out == NULL) return XTC_E_INVAL;
	if (atomic_load_explicit(&c->state, memory_order_acquire) != 1)
		return XTC_E_AGAIN;
	*out = atomic_load_explicit(&c->value, memory_order_acquire);
	return XTC_OK;
}

int
xtc_chan_oneshot_set_waker(xtc_chan_oneshot_t *c, const xtc_waker_t *w)
{
	if (c == NULL || w == NULL) return XTC_E_INVAL;
	c->waker = *w;
	atomic_store_explicit(&c->has_waker, 1, memory_order_release);
	/* If a value is already there, fire immediately (one-shot). */
	if (atomic_load_explicit(&c->state, memory_order_acquire) == 1)
		(void)xtc_waker_wake(&c->waker);
	return XTC_OK;
}

/* ===== mpsc bounded =============================================== */

struct xtc_chan_mpsc {
	xtc_res_t *res;
	size_t       cap;
	_Atomic(void *) *slots;
	_Atomic uint64_t  head;     /* next slot to write */
	_Atomic uint64_t  tail;     /* next slot to read */
	_Atomic int       closed;
	xtc_waker_t       waker;
	_Atomic int       has_waker;
};

int
xtc_chan_mpsc_create(xtc_res_t *res, size_t capacity, xtc_chan_mpsc_t **out)
{
	xtc_chan_mpsc_t *c;
	int rc;
	size_t i;

	if (out == NULL || capacity == 0) return XTC_E_INVAL;
	/* Round up to power of two for cheap masking. */
	{
		size_t p = 1;
		while (p < capacity) p <<= 1;
		capacity = p;
	}
	if (res != NULL) {
		if ((rc = xtc_res_acquire(res, XTC_RES_CHANNELS, 1)) != XTC_OK)
			return rc;
	}
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK) {
		if (res != NULL) xtc_res_release(res, XTC_RES_CHANNELS, 1);
		return rc;
	}
	if ((rc = __os_calloc(capacity, sizeof *c->slots,
	    (void **)&c->slots)) != XTC_OK) {
		if (res != NULL) xtc_res_release(res, XTC_RES_CHANNELS, 1);
		__os_free(c);
		return rc;
	}
	for (i = 0; i < capacity; i++)
		atomic_store_explicit(&c->slots[i], NULL, memory_order_relaxed);
	c->res = res;
	c->cap = capacity;
	atomic_store_explicit(&c->head, 0, memory_order_relaxed);
	atomic_store_explicit(&c->tail, 0, memory_order_relaxed);
	atomic_store_explicit(&c->closed, 0, memory_order_relaxed);
	atomic_store_explicit(&c->has_waker, 0, memory_order_relaxed);
	*out = c;
	return XTC_OK;
}

void
xtc_chan_mpsc_destroy(xtc_chan_mpsc_t *c)
{
	if (c == NULL) return;
	if (c->res != NULL) {
		size_t inflight = xtc_chan_mpsc_len(c);
		xtc_res_release(c->res, XTC_RES_CHAN_SLOTS, (int64_t)inflight);
		xtc_res_release(c->res, XTC_RES_CHANNELS, 1);
	}
	__os_free(c->slots);
	__os_free(c);
}

int
xtc_chan_mpsc_try_send(xtc_chan_mpsc_t *c, void *msg)
{
	uint64_t head, tail;
	if (c == NULL) return XTC_E_INVAL;
	if (atomic_load_explicit(&c->closed, memory_order_acquire))
		return XTC_E_INVAL;
	if (msg == NULL) return XTC_E_INVAL;     /* NULL is the empty sentinel */

	/* Charge a global slot first; release if we fail to enqueue. */
	if (c->res != NULL) {
		int rc = xtc_res_acquire(c->res, XTC_RES_CHAN_SLOTS, 1);
		if (rc != XTC_OK) return rc;
	}

	for (;;) {
		head = atomic_load_explicit(&c->head, memory_order_relaxed);
		tail = atomic_load_explicit(&c->tail, memory_order_acquire);
		if (head - tail >= c->cap) {
			if (c->res != NULL)
				xtc_res_release(c->res, XTC_RES_CHAN_SLOTS, 1);
			return XTC_E_AGAIN;
		}
		if (atomic_compare_exchange_weak_explicit(
		        &c->head, &head, head + 1,
		        memory_order_acq_rel, memory_order_relaxed))
			break;
	}
	atomic_store_explicit(&c->slots[head & (c->cap - 1)], msg,
	    memory_order_release);
	if (atomic_load_explicit(&c->has_waker, memory_order_acquire))
		(void)xtc_waker_wake(&c->waker);
	return XTC_OK;
}

int
xtc_chan_mpsc_try_recv(xtc_chan_mpsc_t *c, void **out)
{
	uint64_t tail, head;
	void *v;
	if (c == NULL || out == NULL) return XTC_E_INVAL;
	tail = atomic_load_explicit(&c->tail, memory_order_relaxed);
	head = atomic_load_explicit(&c->head, memory_order_acquire);
	if (tail >= head) {
		if (atomic_load_explicit(&c->closed, memory_order_acquire))
			return XTC_E_INVAL;
		return XTC_E_AGAIN;
	}
	/* Spin until the slot is non-NULL (the producer might have
	 * reserved it but not yet stored). */
	for (;;) {
		v = atomic_load_explicit(&c->slots[tail & (c->cap - 1)],
		    memory_order_acquire);
		if (v != NULL) break;
	}
	atomic_store_explicit(&c->slots[tail & (c->cap - 1)], NULL,
	    memory_order_relaxed);
	atomic_store_explicit(&c->tail, tail + 1, memory_order_release);
	if (c->res != NULL)
		xtc_res_release(c->res, XTC_RES_CHAN_SLOTS, 1);
	*out = v;
	return XTC_OK;
}

int
xtc_chan_mpsc_set_waker(xtc_chan_mpsc_t *c, const xtc_waker_t *w)
{
	if (c == NULL || w == NULL) return XTC_E_INVAL;
	c->waker = *w;
	atomic_store_explicit(&c->has_waker, 1, memory_order_release);
	if (atomic_load_explicit(&c->head, memory_order_acquire) >
	    atomic_load_explicit(&c->tail, memory_order_acquire))
		(void)xtc_waker_wake(&c->waker);
	return XTC_OK;
}

int
xtc_chan_mpsc_close(xtc_chan_mpsc_t *c)
{
	if (c == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&c->closed, 1, memory_order_release);
	if (atomic_load_explicit(&c->has_waker, memory_order_acquire))
		(void)xtc_waker_wake(&c->waker);
	return XTC_OK;
}

size_t
xtc_chan_mpsc_len(const xtc_chan_mpsc_t *c)
{
	uint64_t head, tail;
	if (c == NULL) return 0;
	head = atomic_load_explicit(&c->head, memory_order_acquire);
	tail = atomic_load_explicit(&c->tail, memory_order_acquire);
	return (size_t)(head - tail);
}

/* ===== watch ====================================================== */

struct xtc_chan_watch {
	xtc_res_t *res;
	_Atomic(void *) value;
	_Atomic int     has_value;
};

int
xtc_chan_watch_create(xtc_res_t *res, xtc_chan_watch_t **out)
{
	xtc_chan_watch_t *c;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if (res != NULL) {
		if ((rc = xtc_res_acquire(res, XTC_RES_CHANNELS, 1)) != XTC_OK)
			return rc;
	}
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK) {
		if (res != NULL) xtc_res_release(res, XTC_RES_CHANNELS, 1);
		return rc;
	}
	c->res = res;
	atomic_store_explicit(&c->value, NULL, memory_order_relaxed);
	atomic_store_explicit(&c->has_value, 0, memory_order_relaxed);
	*out = c;
	return XTC_OK;
}

void
xtc_chan_watch_destroy(xtc_chan_watch_t *c)
{
	if (c == NULL) return;
	if (c->res != NULL) xtc_res_release(c->res, XTC_RES_CHANNELS, 1);
	__os_free(c);
}

int
xtc_chan_watch_send(xtc_chan_watch_t *c, void *value)
{
	if (c == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&c->value, value, memory_order_release);
	atomic_store_explicit(&c->has_value, 1, memory_order_release);
	return XTC_OK;
}

int
xtc_chan_watch_recv(xtc_chan_watch_t *c, void **out)
{
	if (c == NULL || out == NULL) return XTC_E_INVAL;
	if (!atomic_load_explicit(&c->has_value, memory_order_acquire))
		return XTC_E_AGAIN;
	*out = atomic_load_explicit(&c->value, memory_order_acquire);
	return XTC_OK;
}

/* ===== mpmc bounded =============================================== */

#include <pthread.h>

struct xtc_chan_mpmc {
	xtc_res_t       *res;
	size_t           cap;
	void           **slots;
	size_t           head;       /* next write */
	size_t           tail;       /* next read */
	size_t           n;
	int              closed;
	pthread_mutex_t  lock;
};

int
xtc_chan_mpmc_create(xtc_res_t *res, size_t capacity, xtc_chan_mpmc_t **out)
{
	xtc_chan_mpmc_t *c;
	int rc;
	if (out == NULL || capacity == 0) return XTC_E_INVAL;
	if (res != NULL) {
		if ((rc = xtc_res_acquire(res, XTC_RES_CHANNELS, 1)) != XTC_OK)
			return rc;
	}
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK) {
		if (res != NULL) xtc_res_release(res, XTC_RES_CHANNELS, 1);
		return rc;
	}
	if ((rc = __os_calloc(capacity, sizeof *c->slots,
	    (void **)&c->slots)) != XTC_OK) {
		if (res != NULL) xtc_res_release(res, XTC_RES_CHANNELS, 1);
		__os_free(c);
		return rc;
	}
	c->res = res;
	c->cap = capacity;
	(void)pthread_mutex_init(&c->lock, NULL);
	*out = c;
	return XTC_OK;
}

void
xtc_chan_mpmc_destroy(xtc_chan_mpmc_t *c)
{
	if (c == NULL) return;
	if (c->res != NULL) {
		xtc_res_release(c->res, XTC_RES_CHAN_SLOTS, (int64_t)c->n);
		xtc_res_release(c->res, XTC_RES_CHANNELS, 1);
	}
	(void)pthread_mutex_destroy(&c->lock);
	__os_free(c->slots);
	__os_free(c);
}

int
xtc_chan_mpmc_try_send(xtc_chan_mpmc_t *c, void *msg)
{
	int rc = XTC_OK;
	if (c == NULL || msg == NULL) return XTC_E_INVAL;
	if (c->res != NULL) {
		if ((rc = xtc_res_acquire(c->res, XTC_RES_CHAN_SLOTS, 1)) != XTC_OK)
			return rc;
	}
	(void)pthread_mutex_lock(&c->lock);
	if (c->closed) {
		rc = XTC_E_INVAL;
	} else if (c->n >= c->cap) {
		rc = XTC_E_AGAIN;
	} else {
		c->slots[c->head] = msg;
		c->head = (c->head + 1) % c->cap;
		c->n++;
	}
	(void)pthread_mutex_unlock(&c->lock);
	if (rc != XTC_OK && c->res != NULL)
		xtc_res_release(c->res, XTC_RES_CHAN_SLOTS, 1);
	return rc;
}

int
xtc_chan_mpmc_try_recv(xtc_chan_mpmc_t *c, void **out)
{
	int rc = XTC_E_AGAIN;
	if (c == NULL || out == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&c->lock);
	if (c->n > 0) {
		*out = c->slots[c->tail];
		c->tail = (c->tail + 1) % c->cap;
		c->n--;
		rc = XTC_OK;
	} else if (c->closed) {
		rc = XTC_E_INVAL;
	}
	(void)pthread_mutex_unlock(&c->lock);
	if (rc == XTC_OK && c->res != NULL)
		xtc_res_release(c->res, XTC_RES_CHAN_SLOTS, 1);
	return rc;
}

int
xtc_chan_mpmc_close(xtc_chan_mpmc_t *c)
{
	if (c == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&c->lock);
	c->closed = 1;
	(void)pthread_mutex_unlock(&c->lock);
	return XTC_OK;
}

size_t
xtc_chan_mpmc_len(const xtc_chan_mpmc_t *c)
{
	size_t n;
	if (c == NULL) return 0;
	(void)pthread_mutex_lock((pthread_mutex_t *)&c->lock);
	n = c->n;
	(void)pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
	return n;
}

/* ===== broadcast (lossy multi-receiver ring) ===================== */

struct xtc_chan_broadcast_recv {
	struct xtc_chan_broadcast *chan;
	uint64_t                   cursor;     /* next sequence to read */
	struct xtc_chan_broadcast_recv *next;   /* in chan->recvs */
};

struct xtc_chan_broadcast {
	xtc_res_t   *res;
	size_t       cap;
	void       **slots;
	uint64_t    *seqs;          /* per-slot sequence number */
	_Atomic uint64_t  pos;       /* monotonic publish counter */
	pthread_mutex_t   lock;
	struct xtc_chan_broadcast_recv *recvs;
};

int
xtc_chan_broadcast_create(xtc_res_t *res, size_t capacity,
                          xtc_chan_broadcast_t **out)
{
	xtc_chan_broadcast_t *c;
	int rc;
	if (out == NULL || capacity == 0) return XTC_E_INVAL;
	if (res != NULL) {
		if ((rc = xtc_res_acquire(res, XTC_RES_CHANNELS, 1)) != XTC_OK)
			return rc;
	}
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK) {
		if (res != NULL) xtc_res_release(res, XTC_RES_CHANNELS, 1);
		return rc;
	}
	if ((rc = __os_calloc(capacity, sizeof *c->slots,
	    (void **)&c->slots)) != XTC_OK) goto fail0;
	if ((rc = __os_calloc(capacity, sizeof *c->seqs,
	    (void **)&c->seqs)) != XTC_OK) goto fail1;
	c->res = res;
	c->cap = capacity;
	atomic_store_explicit(&c->pos, 0, memory_order_relaxed);
	(void)pthread_mutex_init(&c->lock, NULL);
	*out = c;
	return XTC_OK;
fail1:	__os_free(c->slots);
fail0:	if (res != NULL) xtc_res_release(res, XTC_RES_CHANNELS, 1);
	__os_free(c);
	return rc;
}

void
xtc_chan_broadcast_destroy(xtc_chan_broadcast_t *c)
{
	struct xtc_chan_broadcast_recv *r, *n;
	if (c == NULL) return;
	for (r = c->recvs; r != NULL; r = n) { n = r->next; __os_free(r); }
	if (c->res != NULL) xtc_res_release(c->res, XTC_RES_CHANNELS, 1);
	(void)pthread_mutex_destroy(&c->lock);
	__os_free(c->seqs);
	__os_free(c->slots);
	__os_free(c);
}

int
xtc_chan_broadcast_send(xtc_chan_broadcast_t *c, void *msg)
{
	uint64_t seq;
	if (c == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&c->lock);
	seq = atomic_load_explicit(&c->pos, memory_order_relaxed);
	c->slots[seq % c->cap] = msg;
	c->seqs[seq % c->cap]  = seq + 1;     /* +1 so 0 = unset */
	atomic_store_explicit(&c->pos, seq + 1, memory_order_release);
	(void)pthread_mutex_unlock(&c->lock);
	return XTC_OK;
}

int
xtc_chan_broadcast_subscribe(xtc_chan_broadcast_t *c,
                             xtc_chan_broadcast_recv_t **out_recv)
{
	xtc_chan_broadcast_recv_t *r;
	int rc;
	if (c == NULL || out_recv == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *r, (void **)&r)) != XTC_OK)
		return rc;
	r->chan = c;
	(void)pthread_mutex_lock(&c->lock);
	r->cursor = atomic_load_explicit(&c->pos, memory_order_acquire);
	r->next = c->recvs;
	c->recvs = r;
	(void)pthread_mutex_unlock(&c->lock);
	*out_recv = r;
	return XTC_OK;
}

void
xtc_chan_broadcast_unsubscribe(xtc_chan_broadcast_recv_t *r)
{
	xtc_chan_broadcast_t *c;
	xtc_chan_broadcast_recv_t **pp;
	if (r == NULL) return;
	c = r->chan;
	(void)pthread_mutex_lock(&c->lock);
	for (pp = &c->recvs; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == r) { *pp = r->next; break; }
	}
	(void)pthread_mutex_unlock(&c->lock);
	__os_free(r);
}

int
xtc_chan_broadcast_recv(xtc_chan_broadcast_recv_t *r, void **out, int *lagged)
{
	xtc_chan_broadcast_t *c;
	uint64_t pos;
	if (r == NULL || out == NULL) return XTC_E_INVAL;
	c = r->chan;
	if (lagged) *lagged = 0;

	(void)pthread_mutex_lock(&c->lock);
	pos = atomic_load_explicit(&c->pos, memory_order_acquire);
	if (r->cursor >= pos) {
		(void)pthread_mutex_unlock(&c->lock);
		return XTC_E_AGAIN;
	}
	/* Lag check: if we're behind by > cap, we missed slots that
	 * were overwritten.  Skip ahead to the oldest still-readable
	 * slot and report how many we lost. */
	if (pos - r->cursor > c->cap) {
		if (lagged)
			*lagged = (int)(pos - r->cursor - c->cap);
		r->cursor = pos - c->cap;
	}
	*out = c->slots[r->cursor % c->cap];
	r->cursor++;
	(void)pthread_mutex_unlock(&c->lock);
	return XTC_OK;
}

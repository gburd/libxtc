/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/ptc/res.c
 *	Implementation of the xtc_res accountant.
 */

#include "xtc_int.h"
#include "xtc_res.h"

#include <string.h>

static int64_t
__cap_for(const xtc_res_caps_t *c, xtc_res_kind_t k)
{
	switch (k) {
	case XTC_RES_TASKS:       return c->tasks;
	case XTC_RES_CHANNELS:    return c->channels;
	case XTC_RES_CHAN_SLOTS:  return c->chan_slots;
	case XTC_RES_FDS:         return c->fds;
	case XTC_RES_MEM_BYTES:   return c->mem_bytes;
	case XTC_RES_INBOX_MSGS:  return c->inbox_msgs;
	case XTC_RES__COUNT:      break;
	}
	return 0;
}

/* PUBLIC: int xtc_res_init __P((xtc_res_t *, const xtc_res_caps_t *)); */
int
xtc_res_init(xtc_res_t *r, const xtc_res_caps_t *caps)
{
	int i;
	if (r == NULL) return XTC_E_INVAL;
	if (caps == NULL) {
		xtc_res_caps_t d = XTC_RES_CAPS_DEFAULT;
		r->caps = d;
	} else {
		r->caps = *caps;
	}
	for (i = 0; i < XTC_RES__COUNT; i++) {
		atomic_store_explicit(&r->used[i],    0, memory_order_relaxed);
		atomic_store_explicit(&r->high[i],    0, memory_order_relaxed);
		atomic_store_explicit(&r->rejects[i], 0, memory_order_relaxed);
		r->alert_pct[i] = 0.0;
		atomic_store_explicit(&r->alert_armed[i], 1,
		    memory_order_relaxed);
	}
	r->alert_fn = NULL;
	r->alert_user = NULL;
	return XTC_OK;
}

/* PUBLIC: int xtc_res_acquire __P((xtc_res_t *, xtc_res_kind_t, int64_t)); */
int
xtc_res_acquire(xtc_res_t *r, xtc_res_kind_t k, int64_t n)
{
	int64_t cap, cur, next, observed_high;

	if (r == NULL || n < 0 || (int)k < 0 || (int)k >= XTC_RES__COUNT)
		return XTC_E_INVAL;
	cap = __cap_for(&r->caps, k);

	for (;;) {
		cur = atomic_load_explicit(&r->used[k], memory_order_relaxed);
		next = cur + n;
		if (cap > 0 && next > cap) {
			(void)atomic_fetch_add_explicit(&r->rejects[k], 1,
			    memory_order_relaxed);
			return XTC_E_RESOURCE;
		}
		if (atomic_compare_exchange_weak_explicit(
		        &r->used[k], &cur, next,
		        memory_order_relaxed, memory_order_relaxed))
			break;
	}
	/* Update high-water mark (best-effort; lossy is OK here). */
	observed_high = atomic_load_explicit(&r->high[k],
	    memory_order_relaxed);
	if (next > observed_high) {
		(void)atomic_compare_exchange_strong_explicit(
		    &r->high[k], &observed_high, next,
		    memory_order_relaxed, memory_order_relaxed);
	}

	/* Threshold alert: fire if we crossed the configured % of cap. */
	if (r->alert_fn != NULL && r->alert_pct[k] > 0.0 && cap > 0) {
		int64_t threshold = (int64_t)(r->alert_pct[k] * (double)cap);
		if (next >= threshold) {
			int expected = 1;
			if (atomic_compare_exchange_strong_explicit(
			    &r->alert_armed[k], &expected, 0,
			    memory_order_acq_rel, memory_order_acquire))
				r->alert_fn(k, next, cap, r->alert_user);
		}
	}
	return XTC_OK;
}

/* PUBLIC: void xtc_res_release __P((xtc_res_t *, xtc_res_kind_t, int64_t)); */
void
xtc_res_release(xtc_res_t *r, xtc_res_kind_t k, int64_t n)
{
	int64_t prev;
	int64_t cap;
	if (r == NULL || n <= 0 || (int)k < 0 || (int)k >= XTC_RES__COUNT)
		return;
	prev = atomic_fetch_sub_explicit(&r->used[k], n,
	    memory_order_relaxed);
	if (prev < n) {
		/* Underflow.  Clamp to zero. */
		atomic_store_explicit(&r->used[k], 0, memory_order_relaxed);
	}
	/* Re-arm the alert if used dropped below the threshold. */
	cap = __cap_for(&r->caps, k);
	if (r->alert_pct[k] > 0.0 && cap > 0) {
		int64_t cur = atomic_load_explicit(&r->used[k],
		    memory_order_relaxed);
		int64_t threshold = (int64_t)(r->alert_pct[k] * (double)cap);
		if (cur < threshold)
			atomic_store_explicit(&r->alert_armed[k], 1,
			    memory_order_release);
	}
}

/* PUBLIC: int64_t xtc_res_used __P((const xtc_res_t *, xtc_res_kind_t)); */
int64_t xtc_res_used(const xtc_res_t *r, xtc_res_kind_t k) {
	if (r == NULL || (int)k < 0 || (int)k >= XTC_RES__COUNT) return 0;
	return atomic_load_explicit(&r->used[k], memory_order_relaxed);
}
/* PUBLIC: int64_t xtc_res_high __P((const xtc_res_t *, xtc_res_kind_t)); */
int64_t xtc_res_high(const xtc_res_t *r, xtc_res_kind_t k) {
	if (r == NULL || (int)k < 0 || (int)k >= XTC_RES__COUNT) return 0;
	return atomic_load_explicit(&r->high[k], memory_order_relaxed);
}
/* PUBLIC: int64_t xtc_res_rejects __P((const xtc_res_t *, xtc_res_kind_t)); */
int64_t xtc_res_rejects(const xtc_res_t *r, xtc_res_kind_t k) {
	if (r == NULL || (int)k < 0 || (int)k >= XTC_RES__COUNT) return 0;
	return atomic_load_explicit(&r->rejects[k], memory_order_relaxed);
}
/* PUBLIC: void xtc_res_set_cap __P((xtc_res_t *, xtc_res_kind_t, int64_t)); */
void xtc_res_set_cap(xtc_res_t *r, xtc_res_kind_t k, int64_t cap) {
	if (r == NULL || (int)k < 0 || (int)k >= XTC_RES__COUNT) return;
	switch (k) {
	case XTC_RES_TASKS:       r->caps.tasks      = cap; break;
	case XTC_RES_CHANNELS:    r->caps.channels   = cap; break;
	case XTC_RES_CHAN_SLOTS:  r->caps.chan_slots = cap; break;
	case XTC_RES_FDS:         r->caps.fds        = cap; break;
	case XTC_RES_MEM_BYTES:   r->caps.mem_bytes  = cap; break;
	case XTC_RES_INBOX_MSGS:  r->caps.inbox_msgs = cap; break;
	case XTC_RES__COUNT:      break;
	}
}

/* PUBLIC: int xtc_res_set_alert __P((xtc_res_t *, xtc_res_kind_t, double)); */
int
xtc_res_set_alert(xtc_res_t *r, xtc_res_kind_t k, double pct)
{
	if (r == NULL || (int)k < 0 || (int)k >= XTC_RES__COUNT)
		return XTC_E_INVAL;
	if (pct < 0.0 || pct > 1.0) return XTC_E_INVAL;
	r->alert_pct[k] = pct;
	atomic_store_explicit(&r->alert_armed[k], 1, memory_order_release);
	return XTC_OK;
}

/* PUBLIC: int xtc_res_set_alert_fn __P((xtc_res_t *, void (*)(xtc_res_kind_t, int64_t, int64_t, void *), void *)); */
int
xtc_res_set_alert_fn(xtc_res_t *r,
                     void (*fn)(xtc_res_kind_t, int64_t, int64_t, void *),
                     void *user)
{
	if (r == NULL) return XTC_E_INVAL;
	r->alert_fn = fn;
	r->alert_user = user;
	return XTC_OK;
}

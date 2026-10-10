/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
#include "xtc_int.h"
#include "xtc_batch.h"

#ifdef XTC_ENABLE_SCHED_POLICY
#include "xtc_dst_inject.h" /* DST bug-injection harness (no-op in prod) */
#include "loop_int.h"
#include "preempt_int.h"
#include "xtc_async.h"
#include "proc_int.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

enum batch_slot_state {
	B_FREE,
	B_WAITING,
	B_READY,
	B_BLOCKED,
	B_TERMINAL,
	B_DELIVERING
};
struct batch_slot {
	uint64_t handle, charge;
	void *request;
	enum batch_slot_state state;
	int outcome, cancel;
};
struct xtc_batch {
	xtc_loop_t *loop;
	xtc_batch_opts_t opts;
	xtc_batch_stats_t stats;
	xtc_notify_t *notify;
	xtc_pid_t driver;
	struct batch_slot *slots;
	xtc_batch_item_t *items;
	xtc_batch_result_t *results;
	uint32_t *indices;
	uint32_t live, active;
	uint64_t last_handle;
	int closed, cancelling, callback, class_index;
};

/* Publish only committed membership. Called under the owner's short bracket. */
static void
batch_hint(xtc_batch_t *b)
{
	uint32_t i, ready = 0;
	if (b->class_index < 0)
		return;
	for (i = 0; i < b->opts.capacity; ++i)
		ready += b->slots[i].state == B_READY;
	__xtc_sched_batch_hint(b->loop, b->class_index, ready, b->active);
}

static void
batch_inc(uint64_t *n)
{
	if (*n != UINT64_MAX)
		++*n;
}

static int
batch_error(int rc)
{
	return rc <= XTC_E_INVAL && rc >= XTC_E_IO;
}

/* NULL TLS is allowed only for externally serialized setup, or explicitly
 * quiescent stats/destroy. It is NOT evidence that another thread is idle. */
static int
batch_owner(xtc_batch_t *b, int quiescent)
{
	if (b == NULL)
		return XTC_E_INVAL;
	if (__xtc_current_loop == b->loop)
		return XTC_OK;
	if (__xtc_current_loop != NULL)
		return XTC_E_INVAL;
	if (quiescent
	    || atomic_load_explicit(&b->loop->sched.phase, memory_order_acquire)
	           == 0)
		return XTC_OK;
	return XTC_E_INVAL;
}

/* Caller holds the short unsafe bracket, never across user code or a park. */
static int
batch_mutable(xtc_batch_t *b)
{
	int rc = batch_owner(b, 0);
	if (rc == XTC_OK && b->callback)
		rc = XTC_E_DEADLK;
	return rc;
}

static struct batch_slot *
batch_find(xtc_batch_t *b, uint64_t handle)
{
	uint32_t i;
	if (handle == 0)
		return NULL;
	for (i = 0; i < b->opts.capacity; ++i)
		if (b->slots[i].state != B_FREE
#if !XTC_DST_BUG(XTC_DST_BUG_BATCHSTALE)
		    && b->slots[i].handle == handle
#endif
		    )
			return &b->slots[i];
	return NULL;
}

static void
batch_terminal(xtc_batch_t *b, struct batch_slot *s, int outcome)
{
	if (s->state == B_READY || s->state == B_BLOCKED) {
		b->active--;
#if !XTC_DST_BUG(XTC_DST_BUG_BATCHRES)
		b->stats.reserved_units -= s->charge;
#endif
	}
	s->state = B_TERMINAL;
	s->outcome = outcome;
	batch_hint(b);
}

static void
batch_retire(xtc_batch_t *b, struct batch_slot *s)
{
	if (s->outcome == XTC_OK)
		batch_inc(&b->stats.completed);
	else if (s->outcome == XTC_E_ABORTED)
		batch_inc(&b->stats.cancelled);
	else
		batch_inc(&b->stats.failed);
	memset(s, 0, sizeof(*s));
	--b->live;
}

static void
batch_deliver(xtc_batch_t *b)
{
	struct batch_slot *s;
	uint32_t i;
	for (i = 0; i < b->opts.capacity; ++i) {
		(void)xtc_mask_enter();
		__xtc_unsafe_enter();
		s = &b->slots[i];
		if (s->state != B_TERMINAL) {
			__xtc_unsafe_leave();
			(void)xtc_mask_leave();
			continue;
		}
		/* Persistent delivery-started survives an explicit callback
		 * exit. */
		s->state = B_DELIVERING;
		b->callback = 1;
		__xtc_unsafe_leave();
		b->opts.complete(s->handle, s->request, s->outcome,
		                 b->opts.user);
		__xtc_unsafe_enter();
		batch_retire(b, s);
		b->callback = 0;
		__xtc_unsafe_leave();
		(void)xtc_mask_leave();
	}
}

/* Installed before the first callback/yield. Proc-table removal happens AFTER
 * this hook and any other (possibly parking) hooks, so this is not a join. */
static void
batch_cleanup(void *arg)
{
	xtc_batch_t *b = arg;
	struct batch_slot *s;
	uint32_t i;
	__xtc_unsafe_enter();
	b->closed = 1;
	b->callback = 0;
	for (i = 0; i < b->opts.capacity; ++i) {
		s = &b->slots[i];
		if (s->state == B_DELIVERING)
			batch_retire(b, s);
		else if (s->state != B_FREE && s->state != B_TERMINAL)
			batch_terminal(b, s,
			               b->cancelling || s->cancel ?
			                   XTC_E_ABORTED :
			                   XTC_E_INTERNAL);
	}
	__xtc_unsafe_leave();
	batch_deliver(b);
}

static size_t
batch_vector(xtc_batch_t *b)
{
	struct batch_slot *s, *first;
	uint32_t i;
	size_t n = 0, j;
	/* A peer can run between delivery and vector construction. Consume its
	 * cancellation under this same bracket before exposing any member. */
	for (i = 0; i < b->opts.capacity; ++i) {
		s = &b->slots[i];
		if ((s->state == B_WAITING || s->state == B_READY
		     || s->state == B_BLOCKED)
		    && (b->cancelling || s->cancel))
			batch_terminal(b, s, XTC_E_ABORTED);
	}
	/* ponytail: bounded O(capacity * max_active) FCFS scan; add a queue
	 * only if measured large-capacity workloads justify another linked
	 * invariant. */
#if XTC_DST_BUG(XTC_DST_BUG_BATCHCAP)
	while (b->active < b->opts.capacity) {
#else
	while (b->active < b->opts.max_active) {
#endif
		first = NULL;
		for (i = 0; i < b->opts.capacity; ++i) {
			s = &b->slots[i];
			if (s->state == B_WAITING
			    && (first == NULL || s->handle < first->handle))
				first = s;
		}
		if (first == NULL
		    || first->charge > b->opts.resource_capacity
		                           - b->stats.reserved_units)
			break;
		first->state = B_READY;
		b->stats.reserved_units += first->charge;
		b->active++;
	}
	for (i = 0; i < b->opts.capacity; ++i) {
		s = &b->slots[i];
		if (s->state != B_READY)
			continue;
		/* Slot reuse must not change stable admission order in the
		 * vector. */
		for (j = n; j > 0 && b->items[j - 1].handle > s->handle; --j) {
			b->indices[j] = b->indices[j - 1];
			b->items[j] = b->items[j - 1];
		}
		b->indices[j] = i;
		b->items[j].handle = s->handle;
		b->items[j].request = s->request;
		b->results[n].state = XTC_BATCH_UNSET;
		b->results[n].error = 0;
		n++;
	}
	batch_hint(b);
	return n;
}

static void
batch_driver(void *arg)
{
	xtc_batch_t *b = arg;
	struct batch_slot *s;
	xtc_batch_result_t *r;
	size_t n, j;
	uint32_t i;
	int rc, finished, pending;
	if (xtc_proc_at_exit(batch_cleanup, b) != XTC_OK) {
		batch_cleanup(b);
		return;
	}
	for (;;) {
		__xtc_unsafe_enter();
		for (i = 0; i < b->opts.capacity; ++i) {
			s = &b->slots[i];
			if ((s->state == B_WAITING || s->state == B_READY
			     || s->state == B_BLOCKED)
			    && (b->cancelling || s->cancel))
				batch_terminal(b, s, XTC_E_ABORTED);
		}
		__xtc_unsafe_leave();
		batch_deliver(b);
		(void)xtc_mask_enter();
		__xtc_unsafe_enter();
		n = batch_vector(b);
		finished = b->closed && b->live == 0;
		pending = 0;
		for (i = 0; i < b->opts.capacity; ++i)
			pending |= b->slots[i].state == B_TERMINAL;
		if (n != 0) {
			b->callback = 1;
			batch_inc(&b->stats.iterations);
			if (n > b->stats.max_vector)
				b->stats.max_vector = n;
			if (UINT64_MAX - b->stats.total_members < n)
				b->stats.total_members = UINT64_MAX;
			else
				b->stats.total_members += n;
		}
		__xtc_unsafe_leave();
		if (n == 0) {
			if (!finished && !pending)
				(void)xtc_notify_wait(b->notify, -1);
			/* Wait has unlinked its stack waiter before kill may
			 * unwind. */
			(void)xtc_mask_leave();
			if (finished)
				return;
			continue;
		}
		rc = b->opts.step(b->items, b->results, n, b->opts.user);
		__xtc_unsafe_enter();
		for (j = 0; j < n; ++j) {
			s = &b->slots[b->indices[j]];
			r = &b->results[j];
			if (rc != XTC_OK)
				batch_terminal(b, s,
				               batch_error(rc) ?
				                   rc :
				                   XTC_E_INTERNAL);
			else if (r->state == XTC_BATCH_DONE)
#if XTC_DST_BUG(XTC_DST_BUG_BATCHDONE)
				s->state = B_READY;
#else
				batch_terminal(b, s, XTC_OK);
#endif
			else if (r->state == XTC_BATCH_BLOCKED)
				s->state = B_BLOCKED;
			else if (r->state == XTC_BATCH_ERROR)
				batch_terminal(b, s,
				               batch_error(r->error) ?
				                   r->error :
				                   XTC_E_INTERNAL);
			else if (r->state != XTC_BATCH_CONTINUE)
				batch_terminal(b, s, XTC_E_INTERNAL);
		}
		b->callback = 0;
		batch_hint(b);
		__xtc_unsafe_leave();
		(void)xtc_mask_leave();
		batch_deliver(b);
		xtc_yield();
	}
}

static void
batch_free(xtc_batch_t *b)
{
	xtc_notify_destroy(b->notify);
	__os_free(b->indices);
	__os_free(b->results);
	__os_free(b->items);
	__os_free(b->slots);
	__os_free(b);
}

static int
batch_alloc(size_t count, size_t size, void **out)
{
	if (count > SIZE_MAX / size)
		return XTC_E_RANGE;
	return __os_calloc(count, size, out);
}
#endif

/* PUBLIC: int xtc_batch_create __P((xtc_loop_t *, const xtc_batch_opts_t *, xtc_batch_t **)); */
int
xtc_batch_create(xtc_loop_t *loop, const xtc_batch_opts_t *o, xtc_batch_t **out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	xtc_batch_t *b;
	xtc_proc_opts_t po = { 0 };
	int i, rc, found, lane = -1;
	if (loop == NULL || o == NULL || out == NULL || o->size != sizeof(*o))
		return XTC_E_INVAL;
	if (o->version != XTC_BATCH_VERSION)
		return XTC_E_VERSION;
	if (o->capacity == 0 || o->max_active == 0
	    || o->max_active > o->capacity || o->resource_capacity == 0
	    || o->step == NULL || o->complete == NULL)
		return XTC_E_INVAL;
	if (__xtc_current_loop != NULL && __xtc_current_loop != loop)
		return XTC_E_INVAL;
	if (atomic_load_explicit(&loop->sched.phase, memory_order_acquire) != 0)
		return XTC_E_AGAIN;
	found = o->sched_class == NULL;
	for (i = 0; i < loop->n_classes; ++i)
		if (loop->classes[i].in_use
		    && o->sched_class == &loop->classes[i]) {
			found = 1;
			lane = i;
		}
	if (!found)
		return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof(*b), (void **)&b)) != XTC_OK)
		return rc;
	b->loop = loop;
	b->opts = *o;
	b->class_index = lane;
	if ((rc = batch_alloc(o->capacity, sizeof(*b->slots),
	                      (void **)&b->slots))
	        != XTC_OK
	    || (rc = batch_alloc(o->max_active, sizeof(*b->items),
	                         (void **)&b->items))
	           != XTC_OK
	    || (rc = batch_alloc(o->max_active, sizeof(*b->results),
	                         (void **)&b->results))
	           != XTC_OK
	    || (rc = batch_alloc(o->max_active, sizeof(*b->indices),
	                         (void **)&b->indices))
	           != XTC_OK
	    || (rc = xtc_notify_create(&b->notify)) != XTC_OK) {
		batch_free(b);
		return rc;
	}
	if (lane >= 0
	    && (rc = __xtc_sched_batch_attach(loop, lane)) != XTC_OK) {
		batch_free(b);
		return rc;
	}
	po.name = "batch";
	po.sched_class = o->sched_class;
	/* Last fallible operation: only now publish the borrowed group
	 * argument. */
	rc = xtc_proc_spawn(loop, batch_driver, b, &po, &b->driver);
	if (rc != XTC_OK) {
		if (lane >= 0)
			__xtc_sched_batch_detach(loop, lane);
		batch_free(b);
		return rc;
	}
	*out = b;
	return XTC_OK;
#else
	(void)loop;
	(void)o;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_submit __P((xtc_batch_t *, void *, uint64_t, uint64_t *)); */
int
xtc_batch_submit(xtc_batch_t *b, void *request, uint64_t charge, uint64_t *out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	uint32_t i;
	int rc;
	__xtc_unsafe_enter();
	rc = batch_mutable(b);
	if (rc == XTC_OK) {
		if (out == NULL || charge == 0)
			rc = XTC_E_INVAL;
		else if (charge > b->opts.resource_capacity
		         || b->last_handle == UINT64_MAX)
			rc = XTC_E_RANGE;
		else if (b->closed || b->live == b->opts.capacity)
			rc = XTC_E_AGAIN;
		if (rc != XTC_OK)
			batch_inc(&b->stats.rejections);
		else {
			for (i = 0; i < b->opts.capacity; ++i)
				if (b->slots[i].state == B_FREE)
					break;
			b->slots[i].handle = ++b->last_handle;
			b->slots[i].request = request;
			b->slots[i].charge = charge;
			b->slots[i].state = B_WAITING;
			b->live++;
			*out = b->last_handle;
#if !XTC_DST_BUG(XTC_DST_BUG_BATCHWAKE)
			(void)xtc_notify_signal(b->notify);
#endif
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)request;
	(void)charge;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_cancel __P((xtc_batch_t *, uint64_t)); */
int
xtc_batch_cancel(xtc_batch_t *b, uint64_t handle)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	struct batch_slot *s;
	int rc;
	__xtc_unsafe_enter();
	rc = batch_mutable(b);
	if (rc == XTC_OK) {
		s = batch_find(b, handle);
		if (s == NULL)
			rc = XTC_E_INVAL;
		else {
			/* A committed terminal result wins; never rewrite it.
			 */
			s->cancel = 1;
			(void)xtc_notify_signal(b->notify);
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)handle;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_resume __P((xtc_batch_t *, uint64_t)); */
int
xtc_batch_resume(xtc_batch_t *b, uint64_t handle)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	struct batch_slot *s;
	int rc;
	__xtc_unsafe_enter();
	rc = batch_mutable(b);
	if (rc == XTC_OK) {
		s = batch_find(b, handle);
		if (s == NULL || (s->state != B_BLOCKED && s->state != B_READY))
			rc = XTC_E_INVAL;
		else {
			s->state = B_READY;
			batch_hint(b);
			(void)xtc_notify_signal(b->notify);
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)handle;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_close __P((xtc_batch_t *, enum xtc_batch_close_mode)); */
int
xtc_batch_close(xtc_batch_t *b, enum xtc_batch_close_mode mode)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int rc;
	__xtc_unsafe_enter();
	rc = batch_mutable(b);
	if (rc == XTC_OK) {
		if (mode != XTC_BATCH_DRAIN && mode != XTC_BATCH_CANCEL)
			rc = XTC_E_INVAL;
		else {
			b->closed = 1;
			b->cancelling |= mode == XTC_BATCH_CANCEL;
			(void)xtc_notify_signal(b->notify);
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)mode;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_destroy __P((xtc_batch_t *)); */
int
xtc_batch_destroy(xtc_batch_t *b)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int rc;
	__xtc_unsafe_enter();
	rc = batch_owner(b, 1);
	if (rc == XTC_OK) {
		if (b->callback)
			rc = XTC_E_DEADLK;
		else if (!b->closed || b->live != 0)
			rc = XTC_E_AGAIN;
		else if (__xtc_proc_loop_contains(b->loop, b->driver))
			rc = XTC_E_AGAIN;
		else {
			if (b->class_index >= 0)
				__xtc_sched_batch_detach(b->loop, b->class_index);
			batch_free(b);
			rc = XTC_OK;
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_get_stats __P((xtc_batch_t *, xtc_batch_stats_t *)); */
int
xtc_batch_get_stats(xtc_batch_t *b, xtc_batch_stats_t *out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	uint32_t i;
	int rc;
	__xtc_unsafe_enter();
	rc = batch_owner(b, 1);
	if (rc == XTC_OK) {
		if (out == NULL || out->size != sizeof(*out))
			rc = XTC_E_INVAL;
		else if (out->version != XTC_BATCH_VERSION)
			rc = XTC_E_VERSION;
		else {
			*out = b->stats;
			out->size = sizeof(*out);
			out->version = XTC_BATCH_VERSION;
			for (i = 0; i < b->opts.capacity; ++i) {
				if (b->slots[i].state == B_WAITING)
					out->queued++;
				if (b->slots[i].state == B_READY)
					out->ready++;
				if (b->slots[i].state == B_BLOCKED)
					out->blocked++;
			}
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

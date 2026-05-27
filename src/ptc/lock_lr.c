/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/lock_lr.c
 *	Left-Right lock implementation, ported faithfully from
 *	postgres/lrlck/src/backend/storage/lmgr/lrlock.c.
 *
 *	Differences from the PG version:
 *	  - Backend slots are assigned lazily from a process-global
 *	    atomic counter (xtc has no MyProcNumber); see __slot_for().
 *	  - Memory comes from mmap(MAP_ANONYMOUS|MAP_PRIVATE) so we
 *	    can MADV_FREE pages back to the OS in COW mode.
 *	  - SeqCst fence on the read path uses C11 atomic_thread_fence.
 *	  - No shared-memory init-in-place variant (xtc has no shmem
 *	    request system yet — coming in M16 for the PG adapter).
 *	  - XTC_LRLOCK_COW: data[1] is allocated lazily on first
 *	    write_begin, and posix_madvise(MADV_FREE) is called on the
 *	    stale copy after publish so the OS can reclaim its pages.
 *	    On the next write_begin we always memcpy from the read
 *	    copy, so it's correct even if pages were reclaimed.
 */

#include "xtc_int.h"
#include "xtc_lrlock.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- per-process global slot allocator ---- */

#define XTC_LRLOCK_MAX_GLOBAL_SLOTS  4096

static _Atomic int  __global_slot_counter = 0;
static __thread int __my_global_slot = -1;

static int
__slot_for(xtc_lrlock_t *lr)
{
	(void)lr;
	if (__my_global_slot < 0) {
		int s = atomic_fetch_add_explicit(&__global_slot_counter, 1,
		    memory_order_relaxed);
		if (s >= XTC_LRLOCK_MAX_GLOBAL_SLOTS) {
			/* Hard cap.  Caller must have spawned more threads than
			 * any one lock's max_readers can accommodate. */
			return -1;
		}
		__my_global_slot = s;
	}
	return __my_global_slot;
}

/* ---- cache-line padded epoch entry ---- */

#define XTC_CACHE_LINE  64

typedef union epoch_entry {
	struct {
		_Atomic uint32_t epoch;     /* even = idle, odd = active */
		uint32_t         enters;    /* nesting depth, owner-only */
	};
	char pad[XTC_CACHE_LINE];
} epoch_entry_t;

/* ---- oplog header per entry ---- */

typedef struct op_header {
	uint32_t op_size;
} op_header_t;

#define XTC_LRLOCK_SPIN_LIMIT  20
#define XTC_LRLOCK_OPLOG_INITIAL  4096

/* round n up to multiple of a (a is power of two) */
#define XTC_ALIGN(n, a)  (((n) + (a) - 1) & ~(((size_t)(a)) - 1))

struct xtc_lrlock {
	char                *name;
	size_t               data_size;
	xtc_lrlock_apply_fn  apply_fn;
	xtc_lrlock_sync_fn   sync_fn;

	/* Two copies of the protected data.  In COW mode data[1] may
	 * be NULL until first write_begin. */
	void                *data[2];
	size_t               data_alloc[2];   /* mmap rounded sizes */

	_Atomic uint32_t     read_idx;        /* 0 or 1 */

	/* Per-slot epoch counters (cache-line padded). */
	epoch_entry_t       *epochs;
	int                  max_readers;

	/* Active-reader bitmask: ceil(max_readers/64) words. */
	_Atomic uint64_t    *active_mask;
	int                  nbitmask_words;

	/* Writer's snapshot of epochs taken at the start of each publish. */
	uint32_t            *last_seen_epochs;

	/* Operation log. */
	uint8_t             *oplog;
	size_t               oplog_used;
	size_t               oplog_capacity;
	int                  oplog_count;

	pthread_mutex_t      writer_mutex;
	int                  writer_owned;     /* debug aid */
	int                  first_publish_done;
	unsigned             flags;             /* XTC_LRLOCK_COW etc. */
};

/* ---- helpers: mmap-backed data buffer ---- */

static void *
__mmap_data(size_t bytes, size_t *out_alloc)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	size_t alloc;
	void  *p;
	if (pg == 0) pg = 4096;
	alloc = XTC_ALIGN(bytes, pg);
	p = mmap(NULL, alloc, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) return NULL;
	*out_alloc = alloc;
	return p;
}

static void
__munmap_data(void *p, size_t alloc)
{
	if (p != NULL && alloc > 0) (void)munmap(p, alloc);
}

/* ---- oplog growth ---- */

static int
__oplog_grow(xtc_lrlock_t *lr, size_t needed)
{
	size_t new_cap = lr->oplog_capacity;
	void  *p = NULL;
	if (new_cap == 0) new_cap = XTC_LRLOCK_OPLOG_INITIAL;
	while (new_cap < lr->oplog_used + needed) new_cap *= 2;
	if (__os_realloc(lr->oplog, new_cap, &p) != XTC_OK) return XTC_E_RESOURCE;
	lr->oplog = p;
	lr->oplog_capacity = new_cap;
	return XTC_OK;
}

/* ---- snapshot + wait ---- */

static void
__snapshot_epochs(xtc_lrlock_t *lr)
{
	int w;
	for (w = 0; w < lr->nbitmask_words; w++) {
		int base  = w * 64;
		int nbits = (lr->max_readers - base) > 64 ? 64
		           : lr->max_readers - base;
		uint64_t word;
		memset(&lr->last_seen_epochs[base], 0, (size_t)nbits * sizeof(uint32_t));
		word = atomic_load_explicit(&lr->active_mask[w], memory_order_acquire);
		while (word != 0) {
			int bit = __builtin_ctzll(word);
			int i   = base + bit;
			lr->last_seen_epochs[i] = atomic_load_explicit(
			    &lr->epochs[i].epoch, memory_order_acquire);
			word &= word - 1;
		}
	}
}

static void
__wait_for_readers(xtc_lrlock_t *lr)
{
	int spin = 0;
	for (;;) {
		int all_done = 1, i;
		for (i = 0; i < lr->max_readers; i++) {
			uint32_t last = lr->last_seen_epochs[i];
			uint32_t cur;
			if ((last & 1u) == 0) continue;        /* idle in snapshot */
			cur = atomic_load_explicit(&lr->epochs[i].epoch,
			    memory_order_acquire);
			if (cur == last) { all_done = 0; break; }
		}
		if (all_done) return;
		if (spin < XTC_LRLOCK_SPIN_LIMIT) {
			spin++;
#if defined(__x86_64__) || defined(__i386__)
			__asm__ __volatile__ ("pause");
#endif
		} else {
			(void)sched_yield();
		}
	}
}

/* ---- oplog replay ---- */

static void
__replay_oplog(xtc_lrlock_t *lr, void *target)
{
	size_t pos = 0;
	while (pos + sizeof(op_header_t) <= lr->oplog_used) {
		op_header_t hdr;
		memcpy(&hdr, lr->oplog + pos, sizeof hdr);
		pos += sizeof hdr;
		if (pos + hdr.op_size > lr->oplog_used) break;
		if (lr->apply_fn != NULL)
			lr->apply_fn(target, lr->oplog + pos, hdr.op_size);
		pos += XTC_ALIGN(hdr.op_size, 8);
	}
}

/* ---- COW: ensure data[1] exists and matches data[0] ---- */

static int
__cow_ensure_write_copy(xtc_lrlock_t *lr)
{
	int read_idx = (int)atomic_load_explicit(&lr->read_idx,
	    memory_order_acquire);
	int wr_idx   = 1 - read_idx;
	if (lr->data[wr_idx] == NULL) {
		size_t alloc;
		void *m = __mmap_data(lr->data_size, &alloc);
		if (m == NULL) return XTC_E_RESOURCE;
		memcpy(m, lr->data[read_idx], lr->data_size);
		lr->data[wr_idx]       = m;
		lr->data_alloc[wr_idx] = alloc;
	} else if (lr->flags & XTC_LRLOCK_COW) {
		/* Pages may have been MADV_FREE'd since last use; make sure
		 * the write copy reflects the current read copy. */
		memcpy(lr->data[wr_idx], lr->data[read_idx], lr->data_size);
	}
	return XTC_OK;
}

/* ---- creation / destruction ---- */

int
xtc_lrlock_create(size_t data_size, xtc_lrlock_apply_fn apply_fn,
                  xtc_lrlock_sync_fn sync_fn, const char *name,
                  xtc_lrlock_t **out)
{
	xtc_lrlock_opts_t o = { 0 };
	o.name           = name;
	o.data_size      = data_size;
	o.apply_fn       = apply_fn;
	o.sync_fn        = sync_fn;
	o.max_readers    = 64;
	o.oplog_capacity = XTC_LRLOCK_OPLOG_INITIAL;
	o.flags          = 0;
	return xtc_lrlock_create_ex(&o, out);
}

int
xtc_lrlock_create_ex(const xtc_lrlock_opts_t *opts, xtc_lrlock_t **out)
{
	xtc_lrlock_t *lr;
	int rc, max_readers, nb;
	size_t oplog_cap;

	if (out == NULL || opts == NULL || opts->data_size == 0)
		return XTC_E_INVAL;

	max_readers = opts->max_readers > 0 ? opts->max_readers : 64;
	if (max_readers > XTC_LRLOCK_MAX_GLOBAL_SLOTS)
		max_readers = XTC_LRLOCK_MAX_GLOBAL_SLOTS;
	nb = (max_readers + 63) / 64;
	oplog_cap = opts->oplog_capacity > 0
	    ? opts->oplog_capacity : XTC_LRLOCK_OPLOG_INITIAL;

	if ((rc = __os_calloc(1, sizeof *lr, (void **)&lr)) != XTC_OK) return rc;
	if (opts->name != NULL) (void)__os_strdup(opts->name, &lr->name);
	lr->data_size      = opts->data_size;
	lr->apply_fn       = opts->apply_fn;
	lr->sync_fn        = opts->sync_fn;
	lr->max_readers    = max_readers;
	lr->nbitmask_words = nb;
	lr->oplog_capacity = oplog_cap;
	lr->flags          = opts->flags;

	(void)pthread_mutex_init(&lr->writer_mutex, NULL);

	/* Allocate the data copies.  In COW mode only data[0] is
	 * allocated up front; data[1] is created lazily. */
	lr->data[0] = __mmap_data(lr->data_size, &lr->data_alloc[0]);
	if (lr->data[0] == NULL) { rc = XTC_E_RESOURCE; goto fail; }
	if (!(lr->flags & XTC_LRLOCK_COW)) {
		lr->data[1] = __mmap_data(lr->data_size, &lr->data_alloc[1]);
		if (lr->data[1] == NULL) { rc = XTC_E_RESOURCE; goto fail; }
	}

	atomic_store_explicit(&lr->read_idx, 0u, memory_order_relaxed);

	if ((rc = __os_calloc((size_t)max_readers, sizeof(epoch_entry_t),
	    (void **)&lr->epochs)) != XTC_OK) goto fail;
	if ((rc = __os_calloc((size_t)nb, sizeof(_Atomic uint64_t),
	    (void **)&lr->active_mask)) != XTC_OK) goto fail;
	if ((rc = __os_calloc((size_t)max_readers, sizeof(uint32_t),
	    (void **)&lr->last_seen_epochs)) != XTC_OK) goto fail;
	if ((rc = __os_calloc(1, lr->oplog_capacity, (void **)&lr->oplog)) != XTC_OK)
		goto fail;

	*out = lr;
	return XTC_OK;

fail:
	if (lr->oplog) __os_free(lr->oplog);
	if (lr->last_seen_epochs) __os_free(lr->last_seen_epochs);
	if (lr->active_mask) __os_free(lr->active_mask);
	if (lr->epochs) __os_free(lr->epochs);
	if (lr->data[1]) __munmap_data(lr->data[1], lr->data_alloc[1]);
	if (lr->data[0]) __munmap_data(lr->data[0], lr->data_alloc[0]);
	(void)pthread_mutex_destroy(&lr->writer_mutex);
	if (lr->name) __os_free(lr->name);
	__os_free(lr);
	return rc;
}

void
xtc_lrlock_destroy(xtc_lrlock_t *lr)
{
	if (lr == NULL) return;
	__os_free(lr->oplog);
	__os_free(lr->last_seen_epochs);
	__os_free(lr->active_mask);
	__os_free(lr->epochs);
	__munmap_data(lr->data[1], lr->data_alloc[1]);
	__munmap_data(lr->data[0], lr->data_alloc[0]);
	(void)pthread_mutex_destroy(&lr->writer_mutex);
	if (lr->name) __os_free(lr->name);
	__os_free(lr);
}

/* ---- reader API ---- */

const void *
xtc_lrlock_read_begin(xtc_lrlock_t *lr)
{
	int slot;
	uint32_t idx, enters;
	if (lr == NULL) return NULL;
	slot = __slot_for(lr);
	if (slot < 0 || slot >= lr->max_readers) return NULL;

	enters = lr->epochs[slot].enters;
	if (enters != 0) {
		/* Nested read: epoch already odd; just acquire-load idx. */
		idx = atomic_load_explicit(&lr->read_idx, memory_order_acquire);
		lr->epochs[slot].enters = enters + 1;
		return lr->data[idx];
	}

	/* Step 1: announce activity in the bitmask. */
	atomic_fetch_or_explicit(&lr->active_mask[slot / 64],
	    (uint64_t)1 << (slot & 63u), memory_order_release);
	/* Step 2: bump our epoch to odd. */
	(void)atomic_fetch_add_explicit(&lr->epochs[slot].epoch, 1u,
	    memory_order_acq_rel);
	/* Step 3: SeqCst fence — guarantees writer sees our odd epoch
	 * before we observe the current read_idx. */
	atomic_thread_fence(memory_order_seq_cst);
	/* Step 4: load read_idx. */
	idx = atomic_load_explicit(&lr->read_idx, memory_order_acquire);
	lr->epochs[slot].enters = 1;
	return lr->data[idx];
}

void
xtc_lrlock_read_end(xtc_lrlock_t *lr)
{
	int slot;
	uint32_t enters;
	if (lr == NULL) return;
	slot = __my_global_slot;
	if (slot < 0 || slot >= lr->max_readers) return;
	enters = lr->epochs[slot].enters;
	if (enters == 0) return;
	enters--;
	lr->epochs[slot].enters = enters;
	if (enters == 0) {
		/* Bump epoch to even. */
		(void)atomic_fetch_add_explicit(&lr->epochs[slot].epoch, 1u,
		    memory_order_acq_rel);
		/* Clear the active-reader bit. */
		atomic_fetch_and_explicit(&lr->active_mask[slot / 64],
		    ~((uint64_t)1 << (slot & 63u)), memory_order_release);
	}
}

/* ---- writer API ---- */

void *
xtc_lrlock_write_begin(xtc_lrlock_t *lr)
{
	int read_idx;
	if (lr == NULL) return NULL;
	(void)pthread_mutex_lock(&lr->writer_mutex);
	lr->writer_owned = 1;
	if ((lr->flags & XTC_LRLOCK_COW) || lr->data[1] == NULL) {
		if (__cow_ensure_write_copy(lr) != XTC_OK) {
			lr->writer_owned = 0;
			(void)pthread_mutex_unlock(&lr->writer_mutex);
			return NULL;
		}
	}
	read_idx = (int)atomic_load_explicit(&lr->read_idx, memory_order_acquire);
	return lr->data[1 - read_idx];
}

void
xtc_lrlock_apply_op(xtc_lrlock_t *lr, const void *op, size_t op_size)
{
	op_header_t hdr;
	int read_idx;
	size_t entry;
	if (lr == NULL || !lr->writer_owned || op == NULL || op_size == 0) return;
	if (op_size > UINT32_MAX) return;
	hdr.op_size = (uint32_t)op_size;

	entry = sizeof hdr + XTC_ALIGN(op_size, 8);
	if (lr->oplog_used + entry > lr->oplog_capacity) {
		if (__oplog_grow(lr, entry) != XTC_OK) return;
	}
	memcpy(lr->oplog + lr->oplog_used, &hdr, sizeof hdr);
	memcpy(lr->oplog + lr->oplog_used + sizeof hdr, op, op_size);
	lr->oplog_used += entry;
	lr->oplog_count++;

	read_idx = (int)atomic_load_explicit(&lr->read_idx, memory_order_relaxed);
	if (lr->apply_fn != NULL)
		lr->apply_fn(lr->data[1 - read_idx], op, op_size);
}

static void
__publish_common(xtc_lrlock_t *lr, int force_full_sync)
{
	int old_idx, new_idx;

	if (!lr->first_publish_done && !force_full_sync) {
		/* Bring the write copy in sync with the read copy before
		 * the very first swap, so apply_op'd state is replayed on
		 * top of a known-good base.  In force_full_sync mode the
		 * caller is treating the write copy as authoritative, so
		 * we skip this step — the post-swap sync below will copy
		 * the writer's state to the now-stale buffer. */
		old_idx = (int)atomic_load_explicit(&lr->read_idx,
		    memory_order_relaxed);
		if (lr->sync_fn != NULL)
			lr->sync_fn(lr->data[1 - old_idx], lr->data[old_idx],
			    lr->data_size);
		if (lr->oplog_used > 0)
			__replay_oplog(lr, lr->data[1 - old_idx]);
	}
	lr->first_publish_done = 1;

	atomic_thread_fence(memory_order_seq_cst);

	old_idx = (int)atomic_load_explicit(&lr->read_idx, memory_order_relaxed);
	new_idx = 1 - old_idx;
	(void)atomic_exchange_explicit(&lr->read_idx, (uint32_t)new_idx,
	    memory_order_acq_rel);
	atomic_thread_fence(memory_order_seq_cst);

	__snapshot_epochs(lr);
	__wait_for_readers(lr);

	/* Bring the now-stale copy up to date. */
	if (force_full_sync) {
		if (lr->sync_fn != NULL)
			lr->sync_fn(lr->data[old_idx], lr->data[new_idx],
			    lr->data_size);
	} else if (lr->oplog_used > 0) {
		/* Hybrid: replay if oplog small, else full sync. */
		if ((size_t)lr->oplog_count * 256 <= lr->data_size) {
			__replay_oplog(lr, lr->data[old_idx]);
		} else if (lr->sync_fn != NULL) {
			lr->sync_fn(lr->data[old_idx], lr->data[new_idx],
			    lr->data_size);
		} else {
			__replay_oplog(lr, lr->data[old_idx]);
		}
	}

	lr->oplog_used  = 0;
	lr->oplog_count = 0;

	/* COW: hint to the OS that the now-stale copy can be freed.
	 * Next write_begin will memcpy from the (current) read copy,
	 * so any reclaim is safe. */
	if (lr->flags & XTC_LRLOCK_COW) {
#if defined(MADV_FREE)
		(void)madvise(lr->data[old_idx], lr->data_alloc[old_idx],
		    MADV_FREE);
#elif defined(MADV_DONTNEED)
		(void)madvise(lr->data[old_idx], lr->data_alloc[old_idx],
		    MADV_DONTNEED);
#endif
	}
}

void
xtc_lrlock_publish(xtc_lrlock_t *lr)
{
	if (lr == NULL || !lr->writer_owned) return;
	__publish_common(lr, 0);
}

void
xtc_lrlock_publish_full_sync(xtc_lrlock_t *lr)
{
	if (lr == NULL || !lr->writer_owned) return;
	__publish_common(lr, 1);
}

void
xtc_lrlock_write_end(xtc_lrlock_t *lr)
{
	if (lr == NULL || !lr->writer_owned) return;
	lr->writer_owned = 0;
	(void)pthread_mutex_unlock(&lr->writer_mutex);
}

const void *
xtc_lrlock_read_data(xtc_lrlock_t *lr)
{
	int idx;
	if (lr == NULL) return NULL;
	idx = (int)atomic_load_explicit(&lr->read_idx, memory_order_acquire);
	return lr->data[idx];
}

void *
xtc_lrlock_write_data(xtc_lrlock_t *lr)
{
	int idx;
	if (lr == NULL) return NULL;
	idx = (int)atomic_load_explicit(&lr->read_idx, memory_order_acquire);
	return lr->data[1 - idx];
}

void
xtc_lrlock_mark_ready(xtc_lrlock_t *lr)
{
	if (lr == NULL) return;
	lr->first_publish_done = 1;
}

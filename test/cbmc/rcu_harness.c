/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/rcu_harness.c
 *	CBMC bounded model check of epoch-based reclamation
 *	(src/ptc/rcu.c: read_lock/read_unlock + retire + synchronize).
 *
 *	INVARIANT PROVED: a reader in a read-side never observes a freed
 *	object.  Equivalently: an object retired in epoch E is not
 *	reclaimed until every reader that entered its read-side at an
 *	epoch <= E has left.  A reader that holds the read lock and
 *	dereferences a retired-but-not-yet-freed object always sees live
 *	memory.
 *
 *	WHAT IS MODELLED: the single-global-epoch reclamation protocol
 *	transcribed faithfully from rcu.c --
 *	  read_lock:  publish the global epoch into the reader's slot
 *	              (active_epoch), on first nest.
 *	  read_unlock: clear the slot (active_epoch = 0).
 *	  retire:     stamp the object with the CURRENT epoch (the bucket
 *	              key e % N in rcu.c); mark it retired-not-freed.
 *	  synchronize: advance the global epoch (old -> old+1); wait for
 *	              every reader with active_epoch in (0, old] to leave;
 *	              then reclaim what was retired two epochs back
 *	              (new_e - 2) -- the "one full grace period ahead"
 *	              rule from rcu.c.
 *	The real rcu.c cannot be included standalone (slab allocator, the
 *	per-fiber slot hash table, pthread registry, DST yield) -- none
 *	of which bear on the grace-period safety property, which lives
 *	entirely in the epoch stamping + drain + N-2 reclaim rule
 *	modelled here.  A single object + a single reader is enough to
 *	expose a premature free (the reader still referencing it).  If
 *	rcu.c's epoch/drain/reclaim rule drifts, this must be updated in
 *	lockstep.
 *
 *	BOUND: one reader racing one retire+synchronize cycle (plus a
 *	second synchronize to actually reach the reclaim epoch, since
 *	reclaim is two grace periods behind).  CBMC explores every
 *	interleaving of the epoch atomics.
 *
 *	Run: cbmc rcu_harness.c --unwind 6
 */

#include <stdint.h>
#include <stdatomic.h>

#define XTC_RCU_NBUCKETS 4

static _Atomic uint64_t g_epoch;             /* the global epoch */
static _Atomic uint64_t reader_active;       /* reader's slot: 0 = not reading */

/* The one retired object: its liveness and the epoch it was retired
 * at.  obj_live drops to 0 exactly when it is reclaimed. */
static _Atomic int      obj_live;            /* 1 until reclaimed */
static _Atomic int      obj_retired;         /* 1 once retired */
static _Atomic uint64_t obj_retire_epoch;

/* retire: stamp with current epoch, mark retired (rcu.c buckets it on
 * e % N and frees it two epochs later). */
static void
rcu_retire(void)
{
	uint64_t e = atomic_load_explicit(&g_epoch, memory_order_relaxed);
	atomic_store_explicit(&obj_retire_epoch, e, memory_order_relaxed);
	atomic_store_explicit(&obj_retired, 1, memory_order_release);
}

/* synchronize: advance epoch, drain readers in <= old, reclaim the
 * object if the grace period (>= 2 epochs since retire) has elapsed. */
#define DRAIN_SPINS 4
static void
rcu_synchronize(void)
{
	uint64_t old = atomic_load_explicit(&g_epoch, memory_order_acquire);
	uint64_t new_e = old + 1;
	int i;

	atomic_store_explicit(&g_epoch, new_e, memory_order_release);

	/* Wait for the reader if it is active in epoch <= old. */
	for (i = 0; i < DRAIN_SPINS; i++) {
		uint64_t a = atomic_load_explicit(&reader_active,
		    memory_order_acquire);
		if (a == 0 || a > old)
			break;              /* reader left, or entered after old */
		if (i == DRAIN_SPINS - 1)
			__CPROVER_assume(0);/* deeper drain than modelled: prune */
	}

	/* Reclaim the object iff it was retired and the global epoch has
	 * advanced two full grace periods past its retire epoch (rcu.c:
	 * bucket (E % N) is recycle-safe once epoch reaches E + 2). */
	if (new_e >= 2 &&
	    atomic_load_explicit(&obj_retired, memory_order_acquire) &&
	    atomic_load_explicit(&obj_retire_epoch, memory_order_relaxed)
	        <= new_e - 2) {
		atomic_store_explicit(&obj_live, 0, memory_order_release);
	}
}

/* Reader: enter read-side, take a reference to the object THROUGH THE
 * LIVE STRUCTURE, dereference it (must be live), leave.  RCU's
 * contract: a reader only ever reaches an object that is still linked
 * into the structure when the reader looks -- once retired (unlinked),
 * a newly-arriving reader can no longer reach it.  So the reader
 * obtains a reference here only if the object has not yet been retired;
 * having obtained one inside the read-side, the object must stay live
 * until the reader leaves (reclaim must wait for this reader). */
static void
reader(void)
{
	uint64_t e = atomic_load_explicit(&g_epoch, memory_order_acquire);
	int have_ref;

	atomic_store_explicit(&reader_active, e == 0 ? 1 : e,
	    memory_order_release);   /* publish our entry epoch (nonzero) */

	/* Reach the object through the live structure: we get a reference
	 * only if it is still linked (not yet retired).  A reader arriving
	 * after the unlink simply never finds it. */
	have_ref = (atomic_load_explicit(&obj_retired, memory_order_acquire)
	    == 0) && (atomic_load_explicit(&obj_live, memory_order_acquire)
	    == 1);

	if (have_ref) {
		/* We hold a reference obtained inside the read-side, before
		 * the object was retired.  It MUST stay live until we leave
		 * the read-side -- reclaim may not free it out from under us. */
		__CPROVER_assert(atomic_load_explicit(&obj_live,
		    memory_order_acquire) == 1,
		    "reader in a read-side never observes a freed object");
	}

	atomic_store_explicit(&reader_active, 0, memory_order_release);
}

/* The reclaimer: retire the object, then run two synchronize cycles
 * (reclaim is two grace periods behind, so it takes two to free). */
static void
reclaimer(void)
{
	rcu_retire();
	rcu_synchronize();
	rcu_synchronize();
}

int
main(void)
{
	atomic_store_explicit(&g_epoch, 1, memory_order_relaxed);
	atomic_store_explicit(&reader_active, 0, memory_order_relaxed);
	atomic_store_explicit(&obj_live, 1, memory_order_relaxed);
	atomic_store_explicit(&obj_retired, 0, memory_order_relaxed);
	atomic_store_explicit(&obj_retire_epoch, 0, memory_order_relaxed);

	__CPROVER_ASYNC_1: reader();
	reclaimer();
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/xtc_rcu.h
 *	Read-Copy-Update epoch reclamation.  Wait-free readers,
 *	deferred-reclaim writers.  This is the foundation that
 *	xtc_lrlock (M13b) and xtc_chash (M11.5) build on.
 *
 *	Model:
 *	  - A global epoch counter advances on each "grace period".
 *	  - Each thread records the epoch it entered a read-side
 *	    critical section.
 *	  - To free an object, a writer hands it to xtc_rcu_retire;
 *	    the object is held until every thread has either left its
 *	    read-side or moved to a strictly newer epoch.
 *
 *	This first cut uses a single global epoch + a per-thread slot
 *	registered lazily.  A periodic helper (or any writer's call to
 *	xtc_rcu_synchronize) advances the epoch.  M13a-rev2 will ship
 *	a per-NUMA-node bucketing for scaling.
 */

#ifndef XTC_RCU_H
#define XTC_RCU_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef void (*xtc_rcu_free_fn)(void *p);

/*
 * PUBLIC: int  xtc_rcu_init __P((void));
 * PUBLIC: void xtc_rcu_fini __P((void));
 *
 * PUBLIC: void xtc_rcu_read_lock __P((void));
 * PUBLIC: void xtc_rcu_read_unlock __P((void));
 *
 * PUBLIC: void xtc_rcu_retire __P((void *, xtc_rcu_free_fn));
 * PUBLIC: void xtc_rcu_synchronize __P((void));
 *
 * PUBLIC: uint64_t xtc_rcu_current_epoch __P((void));
 */

/* Initialise the RCU subsystem.  Idempotent.  Called automatically by
 * the first read_lock or retire; explicit init lets callers fail
 * early if storage allocation fails. */
int  xtc_rcu_init(void);

/* Finalise: drain all pending callbacks and free per-thread state.
 * Safe to call only when no readers are active. */
void xtc_rcu_fini(void);

/* Mark the start / end of a read-side critical section.  Reads
 * between lock and unlock see a consistent snapshot of any
 * RCU-protected pointer that was loaded after the lock.
 *
 * Lock/unlock are nestable on the same thread (refcount-style).
 * No system call, no atomic compare-exchange on the fast path. */
void xtc_rcu_read_lock(void);
void xtc_rcu_read_unlock(void);

/* Schedule `p` to be freed (via fn(p)) after every reader currently
 * inside a read-side has finished.  The actual free happens lazily
 * on the next epoch advance + a writer's synchronize call. */
void xtc_rcu_retire(void *p, xtc_rcu_free_fn fn);

/* Advance the global epoch and reclaim everything that's now safe.
 * Safe to call from any thread.  Blocks briefly while waiting for
 * readers in the previous epoch to drain. */
void xtc_rcu_synchronize(void);

uint64_t xtc_rcu_current_epoch(void);

#endif /* XTC_RCU_H */

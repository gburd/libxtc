/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_rcu.h
 *	Read-Copy-Update epoch reclamation.  Wait-free readers,
 *	deferred-reclaim writers.  xtc_chash and xtc_cskip build on it.
 *
 *	Model:
 *	  - A global epoch counter advances on each "grace period".
 *	  - Each thread records the epoch it entered a read-side
 *	    critical section.
 *	  - To free an object, a writer hands it to xtc_rcu_retire;
 *	    the object is held until every thread has either left its
 *	    read-side or moved to a strictly newer epoch.
 *
 *	There is one global epoch and a reader slot per thread (per
 *	fiber on a fiber), registered lazily.  A writer's call to
 *	xtc_rcu_synchronize advances the epoch.
 */

#ifndef XTC_RCU_H
#define XTC_RCU_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

#ifdef __cplusplus
extern "C" {
#endif

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
XTC_API int  xtc_rcu_init(void);

/* Finalise: drain all pending callbacks and free per-thread state.
 * Safe to call only when no readers are active. */
XTC_API void xtc_rcu_fini(void);

/* Mark the start / end of a read-side critical section.  Reads
 * between lock and unlock see a consistent snapshot of any
 * RCU-protected pointer that was loaded after the lock.
 *
 * Lock/unlock are nestable on the same thread or fiber
 * (refcount-style).  A read-side on a fiber may span a yield, a park
 * or a migration to another loop.  A fiber that is killed or exits
 * inside one, or whose loop is finalized while it is parked there,
 * ends it at that point; a parked fiber on a loop that stopped but
 * still exists keeps it (xtc_rcu(3)).  No system call, no lock, no atomic
 * compare-exchange on the fast path (a fiber's first read-side takes a
 * lock once, to register its slot); an outermost read_lock runs one
 * full memory fence after publishing its epoch. */
XTC_API void xtc_rcu_read_lock(void);
XTC_API void xtc_rcu_read_unlock(void);

/* Schedule `p` to be freed (via fn(p)) after every reader currently
 * inside a read-side has finished.  The actual free happens lazily
 * on the next epoch advance + a writer's synchronize call. */
XTC_API void xtc_rcu_retire(void *p, xtc_rcu_free_fn fn);

/* Advance the global epoch and reclaim everything that's now safe.
 * Safe to call from any thread, and from several at once: each call
 * advances the epoch by one.  Blocks while waiting for readers in the
 * previous epoch to drain. */
XTC_API void xtc_rcu_synchronize(void);

XTC_API uint64_t xtc_rcu_current_epoch(void);

#ifdef __cplusplus
}
#endif

#endif /* XTC_RCU_H */

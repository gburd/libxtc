/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/xtc_lockmgr.h
 *	Heavyweight lock manager: per-object queue locks with
 *	deadlock detection.  Models Berkeley DB's lock manager
 *	(~/ws/libdb/src/lock/) at full 9-mode parity.
 *
 *	Modes follow BDB's RIW lattice (the default for transactional
 *	access).  The conflict matrix is configurable at create-time so
 *	callers can supply their own (e.g. a 5-mode subset, or the BDB
 *	"CDB" 5-mode for concurrent data store, or the PG 8-mode set).
 *
 *	Default 9×9 conflict matrix (1 = conflict, 0 = compatible),
 *	verbatim from libdb's `db_riw_conflicts`:
 *
 *	         NL   S   X   WT  IX  IS  IWR RU  WW
 *	    NL    0   0   0   0   0   0   0   0   0
 *	    S     0   0   1   0   1   0   1   0   1
 *	    X     0   1   1   1   1   1   1   1   1
 *	    WT    0   0   0   0   0   0   0   0   0   (waiter placeholder)
 *	    IX    0   1   1   0   0   0   0   1   1
 *	    IS    0   0   1   0   0   0   0   0   1
 *	    IWR   0   1   1   0   0   0   0   1   1
 *	    RU    0   0   1   0   1   0   1   0   0   (read-uncommitted)
 *	    WW    0   1   1   0   1   1   1   0   1   (was-written)
 *
 *	Mode encyclopedia:
 *	  NL  — no lock granted (placeholder)
 *	  S   — shared / read
 *	  X   — exclusive / write
 *	  WT  — wait placeholder; not granted, doesn't block
 *	  IX  — intent exclusive (will lock children with X)
 *	  IS  — intent shared
 *	  IWR — intent read+write (combo, used by index access)
 *	  RU  — read-uncommitted (degree-1 isolation; sees dirty writes)
 *	  WW  — was-written (released X but txn not committed; blocks
 *	        new readers but not other ex-writers in the same view)
 *
 *	Deadlock detection runs periodically by default; callers can
 *	also force a check or configure on-every-conflict mode.  Six
 *	victim policies match BDB.
 */

#ifndef XTC_LOCKMGR_H
#define XTC_LOCKMGR_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_lockmgr xtc_lockmgr_t;
typedef uint64_t           xtc_locker_t;     /* opaque locker ID */

typedef enum xtc_lock_mode {
	XTC_LOCK_NL   = 0,   /* not granted / no lock */
	XTC_LOCK_S    = 1,   /* shared / read */
	XTC_LOCK_X    = 2,   /* exclusive / write */
	XTC_LOCK_WAIT = 3,   /* wait placeholder (BDB's WT) */
	XTC_LOCK_IX   = 4,   /* intent exclusive (BDB's IW) */
	XTC_LOCK_IS   = 5,   /* intent shared (BDB's IR) */
	XTC_LOCK_IWR  = 6,   /* intent read+write (BDB's RIW) */
	XTC_LOCK_RU   = 7,   /* read-uncommitted / degree-1 (BDB's DR) */
	XTC_LOCK_WW   = 8,   /* was-written downgrade state */
	XTC_LOCK_NMODES = 9
} xtc_lock_mode_t;

typedef enum xtc_lock_victim_policy {
	XTC_LOCK_VICTIM_DEFAULT   = 0,    /* alias for RANDOM (BDB default) */
	XTC_LOCK_VICTIM_RANDOM    = 1,
	XTC_LOCK_VICTIM_OLDEST    = 2,
	XTC_LOCK_VICTIM_YOUNGEST  = 3,
	XTC_LOCK_VICTIM_MIN_LOCKS = 4,
	XTC_LOCK_VICTIM_MAX_LOCKS = 5,
	XTC_LOCK_VICTIM_MIN_WRITE = 6,
	XTC_LOCK_VICTIM_MAX_WRITE = 7,
	XTC_LOCK_VICTIM_EXPIRE    = 8     /* only victims with expired timeouts */
} xtc_lock_victim_policy_t;

typedef enum xtc_lock_detect_mode {
	XTC_LOCK_DETECT_PERIODIC  = 0,    /* background thread runs every period_ns */
	XTC_LOCK_DETECT_ON_BLOCK  = 1,    /* every conflict triggers detector synchronously */
	XTC_LOCK_DETECT_NONE      = 2     /* no detection; caller must drive */
} xtc_lock_detect_mode_t;

typedef struct xtc_lockmgr_opts {
	int      n_partitions;            /* hash buckets; 0 = default 64 */
	int64_t  detect_interval_ns;      /* default 100 ms (PERIODIC mode) */
	xtc_lock_victim_policy_t victim;
	xtc_lock_detect_mode_t   detect_mode;

	/* Optional custom conflict matrix.  If NULL, the 9-mode RIW
	 * default above is used.  If non-NULL, must be n_modes*n_modes
	 * bytes (row-major, [held*n_modes + requested]). */
	const uint8_t *conflicts;
	int            n_modes;           /* 0 = default 9 */
} xtc_lockmgr_opts_t;

#define XTC_LOCKMGR_OPTS_DEFAULT { \
	.n_partitions       = 64, \
	.detect_interval_ns = 100LL * 1000 * 1000, \
	.victim             = XTC_LOCK_VICTIM_DEFAULT, \
	.detect_mode        = XTC_LOCK_DETECT_PERIODIC, \
	.conflicts          = NULL, \
	.n_modes            = 0 \
}

/* Lock-vec compound op kinds.  Mirrors BDB's lockreq_t. */
typedef enum xtc_lock_op {
	XTC_LOCK_OP_GET     = 0,
	XTC_LOCK_OP_PUT     = 1,
	XTC_LOCK_OP_PUT_ALL = 2,    /* release_all for the locker; obj ignored */
	XTC_LOCK_OP_UPGRADE = 3,    /* atomic mode upgrade (caller must already hold) */
	XTC_LOCK_OP_DOWNGRADE = 4
} xtc_lock_op_t;

typedef struct xtc_lock_req {
	xtc_lock_op_t   op;
	xtc_lock_mode_t mode;
	const void     *obj;
	size_t          obj_size;
	int64_t         timeout_ns;       /* ignored for non-GET ops */
} xtc_lock_req_t;

typedef struct xtc_lockmgr_stat {
	int       n_held;             /* total granted entries */
	int       n_waiting;          /* total waiters */
	int       n_objects;          /* live lock objects */
	int       n_lockers;          /* allocated locker IDs */
	uint64_t  n_acquires;         /* lifetime acquire count */
	uint64_t  n_releases;
	uint64_t  n_deadlocks_found;  /* victim-aborts performed */
	uint64_t  n_timeouts;         /* timed-out waits */
} xtc_lockmgr_stat_t;

/*
 * PUBLIC: int  xtc_lockmgr_create __P((const xtc_lockmgr_opts_t *, xtc_lockmgr_t **));
 * PUBLIC: void xtc_lockmgr_destroy __P((xtc_lockmgr_t *));
 *
 * PUBLIC: int  xtc_lockmgr_id __P((xtc_lockmgr_t *, xtc_locker_t *));
 * PUBLIC: int  xtc_lockmgr_id_free __P((xtc_lockmgr_t *, xtc_locker_t));
 * PUBLIC: int  xtc_lockmgr_id_set_timeout __P((xtc_lockmgr_t *, xtc_locker_t, int64_t));
 *
 * PUBLIC: int  xtc_lock_get __P((xtc_lockmgr_t *, xtc_locker_t, const void *, size_t, xtc_lock_mode_t, int64_t));
 * PUBLIC: int  xtc_lock_put __P((xtc_lockmgr_t *, xtc_locker_t, const void *, size_t));
 * PUBLIC: int  xtc_lock_release_all __P((xtc_lockmgr_t *, xtc_locker_t));
 * PUBLIC: int  xtc_lock_upgrade __P((xtc_lockmgr_t *, xtc_locker_t, const void *, size_t, xtc_lock_mode_t));
 * PUBLIC: int  xtc_lock_downgrade __P((xtc_lockmgr_t *, xtc_locker_t, const void *, size_t, xtc_lock_mode_t));
 * PUBLIC: int  xtc_lock_vec __P((xtc_lockmgr_t *, xtc_locker_t, xtc_lock_req_t *, int, int *));
 *
 * PUBLIC: int  xtc_lockmgr_check_deadlocks __P((xtc_lockmgr_t *, int *));
 * PUBLIC: int  xtc_lockmgr_failchk __P((xtc_lockmgr_t *, xtc_locker_t));
 * PUBLIC: int  xtc_lockmgr_stat __P((const xtc_lockmgr_t *, xtc_lockmgr_stat_t *));
 * PUBLIC: int  xtc_lockmgr_n_held __P((const xtc_lockmgr_t *));
 * PUBLIC: int  xtc_lockmgr_n_waiting __P((const xtc_lockmgr_t *));
 */

int  xtc_lockmgr_create(const xtc_lockmgr_opts_t *opts, xtc_lockmgr_t **out);
void xtc_lockmgr_destroy(xtc_lockmgr_t *mgr);

int  xtc_lockmgr_id(xtc_lockmgr_t *mgr, xtc_locker_t *out);
int  xtc_lockmgr_id_free(xtc_lockmgr_t *mgr, xtc_locker_t l);

/* Set a deadline for this locker.  When DETECT_PERIODIC fires, lockers
 * past their timeout are considered "expired" and get higher victim
 * priority under VICTIM_EXPIRE policy.  timeout_ns < 0 = no expiry. */
int  xtc_lockmgr_id_set_timeout(xtc_lockmgr_t *mgr, xtc_locker_t l,
                                int64_t timeout_ns);

/* Single acquire.  timeout_ns: -1 = forever, 0 = NOWAIT.  Returns
 * XTC_OK / XTC_E_AGAIN / XTC_E_DEADLK / XTC_E_INVAL / XTC_E_NOMEM. */
int  xtc_lock_get(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                  const void *obj, size_t obj_size,
                  xtc_lock_mode_t mode, int64_t timeout_ns);

int  xtc_lock_put(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                  const void *obj, size_t obj_size);

int  xtc_lock_release_all(xtc_lockmgr_t *mgr, xtc_locker_t locker);

/* Atomic mode change on a held lock.  Upgrade may block (returns AGAIN
 * on NOWAIT-style timeout=0).  Downgrade is always non-blocking and
 * promotes any waiters that the new (weaker) mode no longer conflicts
 * with. */
int  xtc_lock_upgrade(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                      const void *obj, size_t obj_size,
                      xtc_lock_mode_t new_mode);
int  xtc_lock_downgrade(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                        const void *obj, size_t obj_size,
                        xtc_lock_mode_t new_mode);

/* Atomic compound: all ops are validated, then executed.  If any
 * GET would block in the middle, the prior GETs are NOT rolled back
 * (matching BDB's behaviour); set timeout_ns=0 in every req for
 * fully-atomic semantics.  *out_executed receives the number of
 * ops that succeeded. */
int  xtc_lock_vec(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                  xtc_lock_req_t *reqs, int n_reqs, int *out_executed);

int  xtc_lockmgr_check_deadlocks(xtc_lockmgr_t *mgr, int *n_aborted);

/* Mark a locker as "failed" (typically because the owning thread
 * died).  Releases all its locks and aborts any waits. */
int  xtc_lockmgr_failchk(xtc_lockmgr_t *mgr, xtc_locker_t locker);

int  xtc_lockmgr_stat(const xtc_lockmgr_t *mgr, xtc_lockmgr_stat_t *out);
int  xtc_lockmgr_n_held(const xtc_lockmgr_t *mgr);
int  xtc_lockmgr_n_waiting(const xtc_lockmgr_t *mgr);

#endif /* XTC_LOCKMGR_H */

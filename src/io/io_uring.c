/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_uring.c
 *	The Linux io_uring backend.  Maps the readiness-style xtc_io
 *	contract onto io_uring's completion model using IORING_OP_POLL_ADD
 *	with POLL_MULTISHOT so a single SQE delivers many CQEs as the fd
 *	repeatedly becomes ready.
 *
 *	M6 ships this as the preferred Linux backend when liburing is
 *	available at configure time.  The internal data structures
 *	parallel the epoll backend; tags travel via the SQE's user_data.
 */

#define _GNU_SOURCE

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_URING)

#include "io_int.h"
#include "xtc_tail.h"    /* xtc_pid_t, XTC_TAIL_REAP */
#include "tail_int.h"   /* XTC_TAIL_REAP: the CQE-consumed event */
#include "aio_int.h"      /* __xtc_aio_done_set: cross-thread completion flag */
#include "xtc_inject.h"   /* io.uring.sq_full / sqe_reserved test hooks */
#include "preempt_int.h"  /* __xtc_unsafe_*, __xtc_mtx_* (preemption) */

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <liburing.h>
#include <poll.h>

#define WAKEUP_INTEREST  POLLIN

/*
 * io-wq worker cap.  Each io_uring ring has a kernel io-wq worker pool
 * (bounded + unbounded); with flags=0 and no cap the BOUNDED pool
 * defaults to ~max(nr_online_cpus, 4) workers PER RING.  A libxtc
 * executor creates one ring per carrier loop, so on a big box
 * (N carriers x M cores) the process accumulates N*M kernel io-wq
 * threads -- thousands on a 192-core host -- which sit idle but still
 * cost the CFS load balancer (update_sg_lb_stats) real CPU walking
 * their runqueues.  A carrier ring does socket poll + a bounded amount
 * of file AIO; it does not need per-cpu io-wq breadth.  Cap it.
 *
 * values[0] = bounded max, values[1] = unbounded max (0 = leave the
 * kernel default for that class).  Process-global, read once per ring
 * at init; settable before the executor starts via
 * xtc_io_set_iowq_max_workers.  0 for a field means "leave kernel
 * default"; the shipped default caps the BOUNDED pool (the one that
 * scales with nr_cpus) to a small value and leaves unbounded alone.
 * On kernels without the register op (< 5.15) the call returns ENOSYS
 * and we silently keep the old behavior.
 */
#define XTC_IOWQ_BOUND_DEFAULT   4u   /* per-ring bounded io-wq cap */
static _Atomic unsigned __xtc_iowq_bound   = XTC_IOWQ_BOUND_DEFAULT;
static _Atomic unsigned __xtc_iowq_unbound = 0u;   /* 0 = kernel default */

/*
 * PUBLIC: void xtc_io_set_iowq_max_workers __P((unsigned, unsigned));
 *
 * Set the per-ring io_uring io-wq worker caps applied to every ring
 * created afterward: bound = bounded-work workers (the ones that scale
 * with nr_cpus and cause the thread explosion on big boxes), unbound =
 * unbounded-work workers.  0 for either leaves the kernel default for
 * that class.  Must be called before xtc_exec_run / xtc_loop_run
 * creates the rings.  A no-op effect on kernels lacking the register
 * operation.
 */
XTC_API void
xtc_io_set_iowq_max_workers(unsigned bound, unsigned unbound)
{
	atomic_store_explicit(&__xtc_iowq_bound, bound, memory_order_relaxed);
	atomic_store_explicit(&__xtc_iowq_unbound, unbound,
	    memory_order_relaxed);
}

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

/*
 * Registrations are indexed by fd (io->tab), so reg/mod/del are O(1).
 * They were a linked list walked on every call, and xtc_proc_wait_fd
 * does a reg and a del per park: with 30k idle connections an echo
 * server fell from 133k to 3.3k round trips/s, with the conn fiber in
 * this lookup in 12 of 12 gdb samples.
 */
static struct __xtc_uring_fd *
__find_fd(xtc_io_t *io, int fd)
{
	return (fd >= 0 && fd < io->tab_cap) ? io->tab[fd] : NULL;
}

/* Make io->tab[fd] addressable.  Grows by doubling; new slots are NULL. */
static int
__tab_reserve(xtc_io_t *io, int fd)
{
	struct __xtc_uring_fd **nt;
	int ncap, rc;

	if (fd < io->tab_cap)
		return XTC_OK;
	ncap = io->tab_cap > 0 ? io->tab_cap : 64;
	while (ncap <= fd) {
		if (ncap > INT_MAX / 2)
			return XTC_E_RESOURCE;
		ncap *= 2;
	}
	if ((rc = __os_realloc(io->tab, (size_t)ncap * sizeof *nt,
	    (void **)&nt)) != XTC_OK)
		return rc;
	memset(nt + io->tab_cap, 0,
	    (size_t)(ncap - io->tab_cap) * sizeof *nt);
	io->tab = nt;
	io->tab_cap = ncap;
	return XTC_OK;
}

static void
__zombie_link(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	uf->prev = NULL;
	uf->next = io->zombies;
	if (io->zombies != NULL)
		io->zombies->prev = uf;
	io->zombies = uf;
	io->n_zombies++;
}

static void
__zombie_unlink(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	if (uf->prev != NULL)
		uf->prev->next = uf->next;
	else
		io->zombies = uf->next;
	if (uf->next != NULL)
		uf->next->prev = uf->prev;
	uf->next = uf->prev = NULL;
	io->n_zombies--;
}

static short
__interest_to_pollmask(uint32_t interest)
{
	short m = 0;
	if (interest & XTC_IO_READABLE) m |= POLLIN;
	if (interest & XTC_IO_WRITABLE) m |= POLLOUT;
	return m;
}

static uint32_t
__pollres_to_flags(uint32_t res)
{
	uint32_t f = 0;
	if (res & POLLIN)  f |= XTC_IO_READABLE;
	if (res & POLLOUT) f |= XTC_IO_WRITABLE;
	if (res & POLLHUP) f |= XTC_IO_HUP;
	if (res & POLLERR) f |= XTC_IO_ERR;
	if (res & POLLNVAL) f |= XTC_IO_ERR;
	return f;
}

#if defined(XTC_DIAGNOSTIC)
#include <stdio.h>     /* the single-issuer violation report below */
#include <stdlib.h>    /* abort */
#endif

/*
 * Every SQ submit goes through here so the DIAGNOSTIC single-issuer
 * check lives in exactly ONE place instead of nine.
 *
 * The invariant: a ring is created on one thread (xtc_loop_init's
 * caller, which for an executor is the exec-init thread) and submitted
 * to only by its owner, the thread that polls it (__own makes the caller
 * the owner on every entry point, so this holds by construction).  The
 * creator submits nothing: the wakeup POLL_ADD is queued at init and
 * armed by the first poll (see __xtc_io_register_wakeup).  A submit from
 * any other thread would be a data race on the single-producer SQ, the
 * same shape as the eight fixed cross-loop defects.
 *
 * Zero cost in a normal build; XTC_DIAGNOSTIC turns it into an immediate,
 * located abort.
 */
static uint64_t __self_tok(void);

static int
__ring_submit(xtc_io_t *io)
{
	int rc;
	unsigned queued;
#if defined(XTC_DIAGNOSTIC)
	if (atomic_load_explicit(&io->owner_tok, memory_order_acquire) !=
	    __self_tok()) {
		fprintf(stderr, "XTC DIAGNOSTIC: io_uring SQ submit from a "
		    "non-owner thread (io=%p): the submission queue is "
		    "single-producer and this ring already has an owner. "
		    "A cross-thread submit corrupts the SQ.\n", (void *)io);
		abort();
	}
#endif
	queued = io_uring_sq_ready(&io->ring);
	rc = io_uring_submit(&io->ring);
	/*
	 * A submit that does not reach the kernel leaves any fiber parked on
	 * one of these SQEs waiting for a completion that will never be
	 * posted -- indistinguishable, from the reap side, from a completion
	 * that vanished.  Every caller here historically discarded this rc.
	 * Record the failure so the absence has an explanation.
	 *
	 * BOTH failure shapes count, and the second is the subtle one:
	 * io_uring_submit returns a NEGATIVE errno, or the NUMBER of SQEs it
	 * consumed -- which can be FEWER than were queued.  A short submit is
	 * a positive return, so a `rc < 0` test alone reports success while
	 * some SQE never reached the kernel.  That is precisely the state a
	 * consumer is looking at (SUBMIT recorded, no completion, no
	 * SUBMIT_FAIL), so leaving it invisible would let this instrument
	 * exonerate the very path under suspicion.
	 */
	if (__xtc_tail_on(XTC_TAIL_SCHED) &&
	    (rc < 0 || (unsigned)rc < queued)) {
		xtc_pid_t lp;
		memset(&lp, 0, sizeof lp);
		lp.loop_id = (uint16_t)(io->tail_loop_id >= 0 ?
		    (unsigned)io->tail_loop_id : XTC_TAIL_LOOP_NONE);
		/*
		 * detail: the negated errno for an error, or -- for a short
		 * submit, which has no errno -- the count left behind, offset
		 * by XTC_TAIL_SHORT_SUBMIT_BASE so the two cannot be
		 * confused with each other.
		 */
		__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_SUBMIT_FAIL, lp,
		    rc < 0 ? (uint64_t)(unsigned int)-rc :
		    (uint64_t)XTC_TAIL_SHORT_SUBMIT_BASE +
		    (uint64_t)(queued - (unsigned)rc));
	}
	return rc;
}

/*
 * Submit a POLL_ADD SQE for the given uring_fd.  POLL_MULTISHOT means
 * the kernel will keep firing CQEs as the fd becomes ready, until we
 * cancel.  user_data is the uring_fd pointer.
 */
/*
 * An SQE, flushing the SQ once if it is full.  NULL if still none.
 * "io.uring.sq_full" forces the NULL (tests drive the pending queue).
 *
 * The caller MUST hold the preemption-unsafe bracket (__xtc_unsafe_enter)
 * from here until the SQE is prepared and submitted.  io_uring_get_sqe
 * advances the userspace sqe_tail BEFORE the slot is written, and the SQ
 * is single-producer with no lock: an involuntary preemption tick landing
 * between the reserve and the prep runs other fibers on this same thread,
 * and the first of them to submit publishes the reserved slot with its
 * STALE contents (the op that used the slot SQ-size submits ago: an old
 * write to an old offset, its user_data naming a dead stack frame).  The
 * preempted fiber then fills a slot the kernel already consumed and its
 * own submit finds nothing to send, so its completion never comes (the
 * test_wal_kill preempt-nokill hang).  "io.uring.sqe_reserved" fires
 * inside that window so a test can raise the preemption signal there.
 */
static struct io_uring_sqe *
__get_sqe(xtc_io_t *io)
{
	struct io_uring_sqe *sqe;
	if (xtc_inject_check("io.uring.sq_full"))
		return NULL;
	sqe = io_uring_get_sqe(&io->ring);
	if (sqe == NULL) {
		(void)__ring_submit(io);
		sqe = io_uring_get_sqe(&io->ring);
	}
	if (sqe != NULL)
		XTC_INJECTION_POINT("io.uring.sqe_reserved");
	return sqe;
}

static int
__submit_poll_add(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	struct io_uring_sqe *sqe = __get_sqe(io);
	if (sqe == NULL)
		return XTC_E_AGAIN;
	/*
	 * MULTISHOT for user fds (continuous notification, epoll-EPOLLET /
	 * kqueue-EV_CLEAR semantics).  Single-shot for the internal wakeup
	 * pipe so a drained poll reports no stale wakeup on the next poll
	 * (the W3 coalesce contract).  The lost-wakeup window a naive
	 * single-shot has (drain-then-rearm: a cross-thread write landing
	 * between draining the pipe and re-arming is missed) is closed in
	 * the drain path by RE-ARMING BEFORE draining -- see the wakeup
	 * branch of xtc_io_poll. */
	if (uf->is_wakeup)
		io_uring_prep_poll_add(sqe, uf->fd,
		    __interest_to_pollmask(uf->interest));
	else
		io_uring_prep_poll_multishot(sqe, uf->fd,
		    __interest_to_pollmask(uf->interest));
	io_uring_sqe_set_data(sqe, uf);
	/* The fd-readiness ASK, keyed by the parked task (uf->tag).  The
	 * wakeup pipe has no task and reports 0; a re-arm of an existing
	 * multishot emits again, which is correct -- it is a fresh ask. */
	if (__xtc_tail_on(XTC_TAIL_SCHED)) {
		xtc_pid_t lp;
		memset(&lp, 0, sizeof lp);
		lp.loop_id = (uint16_t)(io->tail_loop_id >= 0 ?
		    (unsigned)io->tail_loop_id : XTC_TAIL_LOOP_NONE);
		__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_SUBMIT, lp,
		    uf->is_wakeup ? 0 : (uint64_t)(uintptr_t)uf->tag);
	}
	return XTC_OK;
}

static int
__submit_poll_remove(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	struct io_uring_sqe *sqe = __get_sqe(io);
	if (sqe == NULL)
		return XTC_E_AGAIN;
	io_uring_prep_poll_remove(sqe, (__u64)(uintptr_t)uf);
	io_uring_sqe_set_data(sqe, NULL);   /* discard the cancel CQE */
	return XTC_OK;
}

/*
 * Node lifecycle.  Every SQE a node needs goes through __node_kick, and
 * an SQE that cannot be had (SQ full even after a flush) is never
 * dropped: the node is queued on io->pending and retried at the top of
 * the next xtc_io_poll, before it can block.  Dropping one was a leak
 * (a cancel never sent leaves a multishot armed against a dead node
 * forever) or a lost wakeup (a wakeup re-arm never sent).
 *
 *   live, !armed  -> POLL_ADD (first arm, or re-arm after the kernel
 *                    ended the poll)
 *   dead,  armed  -> POLL_REMOVE (wait on zombies for the terminal CQE)
 *   dead, !armed  -> nothing in flight: free
 */
static void
__node_free(struct __xtc_uring_fd *uf)
{
	__os_free(uf);
}

/* Queue a node for the next xtc_io_poll's __flush_pending. */
static void
__node_defer(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	if (!uf->pending) {
		uf->pending = 1;
		uf->pnext = io->pending;
		io->pending = uf;
	}
}

static void
__node_kick(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	int rc = XTC_OK;

	if (!uf->dead && !uf->armed) {
		if ((rc = __submit_poll_add(io, uf)) == XTC_OK)
			uf->armed = 1;
	} else if (uf->dead && uf->armed) {
		rc = __submit_poll_remove(io, uf);
	}
	if (rc != XTC_OK)
		__node_defer(io, uf);
}

/*
 * Retry every queued node (owner thread, top of xtc_io_poll).  Kept out
 * of the XTC_NOALLOC region below because it can free: a node retired
 * while it sat on the queue is released here, once.
 */
static void
__flush_pending(xtc_io_t *io)
{
	struct __xtc_uring_fd *list = io->pending, *uf;

	io->pending = NULL;
	while ((uf = list) != NULL) {
		list = uf->pnext;
		uf->pnext = NULL;
		uf->pending = 0;
		if (uf->dead && !uf->armed)
			__node_free(uf);
		else
			__node_kick(io, uf);
	}
	(void)__ring_submit(io);
}

/*
 * Ring geometry.  When the CQ fills, the kernel ENDS each multishot poll
 * whose CQE it cannot post (its final CQE goes to the overflow list
 * without IORING_CQE_F_MORE).  The reap path re-arms such a poll, so
 * overflow costs a round trip, not readiness.  A larger CQ would make it
 * rarer but costs per-user locked memory; see __xtc_io_backend_init.
 */
#define XTC_URING_SQ_ENTRIES   256u
#define XTC_URING_CQ_ENTRIES   0u     /* 0 = kernel default (2 x SQ) */

/*
 * io_uring_queue_exit releases a ring's memory ASYNCHRONOUSLY (the kernel
 * tears it down on a workqueue), so a process that creates and destroys
 * rings back to back can transiently see -ENOMEM from the next setup
 * while earlier rings are still being freed -- measured on Linux 7.2: a
 * tight create/exit loop fails within 14-200 iterations, and every
 * failure clears within 4 ms.  1.55.0 hit it too, just less often (its
 * rings were smaller).  Retry ENOMEM briefly; anything else, or ENOMEM
 * that persists, is a real error.  Ring setup is not a hot path.
 */
#define XTC_URING_INIT_RETRIES 200    /* x 1 ms */

/*
 * Setup tiers, best first.  An unknown flag fails the whole setup with
 * -EINVAL, so on EINVAL the next tier is tried; the ring keeps the first
 * tier the kernel accepts.
 *
 * DEFER_TASKRUN + SINGLE_ISSUER (Linux 6.1; the paper's recommended
 * mode, "High-Performance DBMSs with io_uring", PVLDB 19(9) 2026, sec.
 * 2/5.1): completion task_work is queued on the ring and run only when
 * the issuer enters the kernel to wait for completions -- never by an
 * interrupt, never on an unrelated system call.  A loop polls its ring
 * from one thread, so it IS a single issuer; but SINGLE_ISSUER binds the
 * ring to one task, and a loop's thread can change (xtc_exec_run starts
 * new workers on every call; xtc_loop_init and xtc_loop_run may run on
 * different threads).  So a DEFER ring is created ON the thread that
 * runs the loop, and rebuilt for a new one (__own, __ring_rebind).
 *
 * COOP_TASKRUN (5.19): task_work no longer IPIs a submitter running in
 * user mode; it runs at that thread's next kernel entry, or wakes it if
 * it sleeps.  No binding.
 *
 * TASKRUN_FLAG (both): the kernel raises IORING_SQ_TASKRUN while such
 * work is pending, which is what makes the non-blocking peek in the reap
 * loop (io_uring_peek_cqe -> cq_ring_needs_flush) enter and run it
 * instead of reporting an empty CQ.
 *
 * Measured, rexis one loop, memtier 4x16 10:1 (64-vCPU x86-64, Linux
 * 6.12): COOP +10.4% pipe 1 / +7.0% pipe 16 over flags 0; DEFER a further
 * +5.5% / +2.7%.  Same SQE mix and syscall count: it is all completion
 * signalling (task_work_add per request 1.84 -> 0.007).
 *
 * WHEN each tier is used.  xtc_io_init builds the ring at the best NON-
 * binding tier (COOP, else plain).  The DEFER tier is taken only when a
 * thread starts RUNNING the loop (__xtc_io_set_owner -> __own): the ring
 * is rebuilt then, with nothing on it yet.  Two reasons.  First, a DEFER
 * ring must be bound to its one thread anyway.  Second, freeing a DEFER
 * ring costs a full RCU grace period in the kernel's ring-exit worker
 * (io_ring_exit_work -> synchronize_rcu, Linux 6.1-6.12 at least), and
 * those frees queue up: 8 threads creating and destroying loops in a
 * tight loop (test_xproc /entry_mt_parent) left 64 exit workers blocked
 * and 25 GB of kernel memory unreclaimable until the OOM killer ran.  A
 * loop that is created and destroyed without running -- and every bare
 * xtc_io_t -- never becomes DEFER, so it never pays that.
 *
 * NOT applied to the preempt ring: its timeout CQE is read with no
 * kernel entry at all (__xtc_io_uring_preempt_due), so it depends on the
 * IPI to post it while a fiber computes.
 *
 * "io.uring.setup_einval" makes every optional tier fail as an older
 * kernel would, "io.uring.no_defer" only the DEFER tier.
 */
#define XTC_URING_DEFER \
	(IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | \
	 IORING_SETUP_TASKRUN_FLAG)
#define XTC_URING_COOP \
	(IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG)

/* What xtc_io_init asks for, best first. */
static const unsigned __xtc_uring_init_tiers[] = { XTC_URING_COOP, 0u };
#define XTC_URING_N_INIT_TIERS \
	((int)(sizeof __xtc_uring_init_tiers / sizeof __xtc_uring_init_tiers[0]))

/* The DEFER tier, once refused by this kernel, is not tried again. */
static _Atomic int __xtc_uring_no_defer;

/* Is this optional flag set refused by a test injection? */
static int
__tier_injected(unsigned fl)
{
	if (fl == 0u)
		return 0;
	if (xtc_inject_check("io.uring.setup_einval"))
		return 1;
	return (fl & IORING_SETUP_DEFER_TASKRUN) != 0 &&
	    xtc_inject_check("io.uring.no_defer");
}

/*
 * Create *ring with the first of tiers[0..n-1] the kernel accepts
 * (EINVAL = an unknown flag: try the next); *got returns the set used.
 */
static int
__ring_setup(struct io_uring *ring, unsigned cq_entries, int retries,
    const unsigned *tiers, int n, unsigned *got)
{
	struct io_uring_params prm;
	int rc, k, t = 0;

	for (k = 0; ; k++) {
		memset(&prm, 0, sizeof prm);
		prm.flags = tiers[t];
		if (cq_entries != 0) {
			prm.flags |= IORING_SETUP_CQSIZE;
			prm.cq_entries = cq_entries;
		}
		if (__tier_injected(tiers[t]))
			rc = -EINVAL;
		else
			rc = io_uring_queue_init_params(XTC_URING_SQ_ENTRIES,
			    ring, &prm);
		if (rc == -EINVAL && t + 1 < n) {
			t++;          /* older kernel: next tier */
			k--;
			continue;
		}
		if (rc == 0)
			*got = tiers[t];
		if (rc != -ENOMEM || k >= retries)
			return rc;
		(void)__os_sleep_ns(1000LL * 1000);   /* XTC_BLOCKING_OK: ring setup, once per loop */
	}
}

/* Cap a ring's io-wq pool; see XTC_IOWQ_BOUND_DEFAULT.  Best-effort: a
 * kernel without the register op (< 5.15) returns -ENOSYS. */
static void
__ring_cap_iowq(struct io_uring *ring)
{
	unsigned vals[2];

	vals[0] = atomic_load_explicit(&__xtc_iowq_bound,
	    memory_order_relaxed);
	vals[1] = atomic_load_explicit(&__xtc_iowq_unbound,
	    memory_order_relaxed);
	if (vals[0] != 0u || vals[1] != 0u)
		(void)io_uring_register_iowq_max_workers(ring, vals);
}

/*
 * A per-thread owner token: unique for the life of the process, so a
 * thread that reuses a dead thread's pthread_t still reads as new.
 */
static _Atomic uint64_t __xtc_uring_tok_next = 1;
static XTC_THREAD_LOCAL uint64_t __xtc_uring_tok;

static uint64_t
__self_tok(void)
{
	if (__xtc_uring_tok == 0)
		__xtc_uring_tok = atomic_fetch_add_explicit(
		    &__xtc_uring_tok_next, 1, memory_order_relaxed);
	return __xtc_uring_tok;
}

static int __ring_rebind(xtc_io_t *, int);
static int __poll(xtc_io_t *, xtc_io_event_t *, int, int64_t, int *);

/*
 * Make the calling thread this io's owner before it touches the ring.
 * Every ring entry point (poll, reg/mod/del, aio submit) calls it with
 * run == 0; __xtc_io_set_owner, at the start of a loop run, with run ==
 * 1.
 *
 * Fast path: the caller already owns the ring (a relaxed load and a TLS
 * read).  Otherwise the caller takes the io over, which is legal only
 * when no other thread is running its loop: the io is single-owner, and
 * a foreign thread reaches it only through xtc_io_wakeup and
 * __xtc_io_defer_del_fd.  owner_active says whether one is (set by
 * __xtc_io_set_owner, cleared by __xtc_io_owner_leave); a takeover while
 * it is set is refused, which is what a DIAGNOSTIC build used to abort
 * on.
 *
 * The ring follows the owner.  A SINGLE_ISSUER (DEFER) ring is bound to
 * the thread that created it, so a new owner gets a new ring
 * (__ring_rebind).  A loop run (run == 1) also upgrades a COOP ring to
 * DEFER the same way, on the kernels that have it (see
 * __xtc_uring_tiers' comment for why that waits until the loop runs).
 */
static int
__own(xtc_io_t *io, int run)
{
	uint64_t me = __self_tok(), cur;
	int rc, defer;

	cur = atomic_load_explicit(&io->owner_tok, memory_order_relaxed);
	defer = run && !io->ring_single && io->ring_try_defer &&
	    !atomic_load_explicit(&__xtc_uring_no_defer, memory_order_relaxed);
	if (cur == me && !defer)
		return XTC_OK;
	if (io->ring_dead)
		return XTC_E_NOMEM;
	if (cur != me && cur != 0 &&
	    atomic_load_explicit(&io->owner_active, memory_order_acquire)) {
#if defined(XTC_DIAGNOSTIC)
		fprintf(stderr, "XTC DIAGNOSTIC: io_uring ring touched by a "
		    "non-owner thread (io=%p) while its owner runs the loop: "
		    "the ring is single-producer.\n", (void *)io);
		abort();
#endif
		return XTC_E_INTERNAL;
	}
	if ((io->ring_single || defer) &&
	    (rc = __ring_rebind(io, defer)) != XTC_OK)
		return rc;
	io->owner_tid = pthread_self();
	atomic_store_explicit(&io->owner_set, 1, memory_order_relaxed);
	atomic_store_explicit(&io->owner_tok, me, memory_order_release);
	return XTC_OK;
}

/*
 * Rebuild the ring on the calling thread (see __own): as DEFER if
 * want_defer (falling back to the current flags if the kernel refuses
 * DEFER), else with the current flags.
 *
 * Polls are moved, not lost: every live registration is re-armed on the
 * new ring, and a poll is level-checked when armed, so readiness that
 * arrived while the old ring was idle -- or that a leaving owner reaped
 * but could not deliver -- is reported by the new one.  Retired nodes
 * waiting for a terminal CQE are freed: io_uring_queue_exit cancels
 * their polls, and no CQE of the old ring can be reaped any more.
 *
 * File AIO cannot move: a read or write in flight belongs to the old
 * ring.  The previous owner reaps it before leaving
 * (__xtc_io_owner_leave), so n_aio is 0 here on every path the library
 * drives; if it is not, the rebuild is refused rather than strand the
 * parked fiber's completion.  A ring whose only submit so far is to be
 * re-sent (the wakeup poll, queued at init) loses nothing.
 *
 * Out of memory for the new ring leaves the io without one (ring_dead):
 * every later entry point reports XTC_E_NOMEM.
 */
static int
__ring_rebind(xtc_io_t *io, int want_defer)
{
	struct io_uring nr;
	struct __xtc_uring_fd *p, *n;
	unsigned tiers[2], got = 0;
	int i, rc, nt = 0;

	if (io->n_aio != 0)
		return XTC_E_INTERNAL;
	if (want_defer)
		tiers[nt++] = XTC_URING_DEFER;
	tiers[nt++] = io->ring_flags;
	/* Old ring first: the memlock budget may not fit two. */
	io_uring_queue_exit(&io->ring);
	rc = __ring_setup(&nr, XTC_URING_CQ_ENTRIES, XTC_URING_INIT_RETRIES,
	    tiers, nt, &got);
	if (rc < 0) {
		io->ring_dead = 1;
		memset(&io->ring, 0, sizeof io->ring);
		return rc == -ENOMEM ? XTC_E_NOMEM : XTC_E_INTERNAL;
	}
	/* A pre-6.1 kernel: stop asking (but not for a test's refusal). */
	if (want_defer && got != XTC_URING_DEFER &&
	    !__tier_injected(XTC_URING_DEFER))
		atomic_store_explicit(&__xtc_uring_no_defer, 1,
		    memory_order_relaxed);
	__ring_cap_iowq(&nr);
	io->ring = nr;
	io->ring_flags = got;
	io->ring_single = (got & IORING_SETUP_SINGLE_ISSUER) != 0;
	io->n_rebinds++;
	for (p = io->zombies; p != NULL; p = n) {
		n = p->next;
		p->armed = 0;
		__zombie_unlink(io, p);
		if (!p->pending)
			__node_free(p);
	}
	for (i = 0; i < io->tab_cap; i++) {
		if ((p = io->tab[i]) == NULL)
			continue;
		p->armed = 0;
		__node_defer(io, p);
	}
	return XTC_OK;
}

/* Main-ring setup flags and SQEs the kernel has consumed (tests). */
int
__xtc_io_uring_info(xtc_io_t *io, unsigned *flags, unsigned *sq_consumed,
    unsigned *rebinds)
{
	if (io == NULL)
		return XTC_E_INVAL;
	if (flags != NULL)
		*flags = io->ring_dead ? 0u : io->ring.flags;
	if (sq_consumed != NULL)
		*sq_consumed = io->ring_dead ? 0u :
		    io_uring_smp_load_acquire(io->ring.sq.khead);
	if (rebinds != NULL)
		*rebinds = io->n_rebinds;
	return XTC_OK;
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	int rc;

	/* Ring memory comes from a per-user budget (RLIMIT_MEMLOCK, commonly
	 * 8 MiB).  The default geometry (CQ = 2 x SQ) fits ~200 rings; a
	 * 4096-entry CQ fits only ~67 and, once the budget fragments, a
	 * smaller fallback ring does not fit either (measured on Linux 7.2).
	 * So the CQ stays at the default size: an executor with one ring per
	 * loop must start on a 96-core host.  CQ overflow is not lossy (the
	 * reap path re-arms a poll the kernel ended), only slower. */
	rc = __ring_setup(&io->ring, XTC_URING_CQ_ENTRIES,
	    XTC_URING_INIT_RETRIES, __xtc_uring_init_tiers,
	    XTC_URING_N_INIT_TIERS, &io->ring_flags);
	if (rc < 0) {
		errno = -rc;
		return rc == -ENOMEM ? XTC_E_NOMEM : XTC_E_INTERNAL;
	}
	io->ring_single = 0;
	/* DEFER is 6.1+, COOP 5.19+: a kernel that refused COOP lacks it. */
	io->ring_try_defer = io->ring_flags != 0;
	/*
	 * Cap this ring's kernel io-wq worker pool so N carriers on an
	 * M-core box do not accumulate N*M idle io-wq threads (see the
	 * XTC_IOWQ_BOUND_DEFAULT comment).  The register op works on a
	 * disabled ring (no submitter bound yet).
	 */
	__ring_cap_iowq(&io->ring);
	io->tab = NULL;
	io->tab_cap = 0;
	io->zombies = NULL;
	io->pending = NULL;
	io->n_zombies = 0;
	io->n_aio = 0;
	io->ring_dead = 0;
	io->carry = NULL;
	io->n_carry = io->cap_carry = 0;
	io->n_rebinds = 0;
	atomic_store_explicit(&io->owner_tok, 0, memory_order_relaxed);
	atomic_store_explicit(&io->owner_active, 0, memory_order_relaxed);
	io->pending_del = NULL;
	io->n_pending_del = 0;
	io->cap_pending_del = 0;
	atomic_store_explicit(&io->has_pending_del, 0, memory_order_relaxed);
	atomic_store_explicit(&io->owner_set, 0, memory_order_relaxed);
	io->tail_loop_id = -1;   /* label only; the owning loop publishes it */
	if (pthread_mutex_init(&io->del_lock, NULL) != 0) {
		io_uring_queue_exit(&io->ring);
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	struct __xtc_uring_fd *p, *n;
	int i;
	/* A queued node is also live (in tab) or a zombie, except a retired
	 * one with nothing in flight: only those are freed from here. */
	for (p = io->pending; p != NULL; p = n) {
		n = p->pnext;
		if (p->dead && !p->armed)
			__node_free(p);
	}
	io->pending = NULL;
	for (i = 0; i < io->tab_cap; i++)
		if (io->tab[i] != NULL)
			__node_free(io->tab[i]);
	__os_free(io->tab);
	io->tab = NULL;
	io->tab_cap = 0;
	/* Free any zombies whose terminal CQE never arrived; the ring is
	 * being torn down so no CQE can reference them after this. */
	for (p = io->zombies; p != NULL; p = n) {
		n = p->next;
		__node_free(p);
	}
	io->zombies = NULL;
	io->n_zombies = 0;
	if (io->pending_del != NULL) {
		__os_free(io->pending_del);
		io->pending_del = NULL;
	}
	io->n_pending_del = 0;
	io->cap_pending_del = 0;
	__os_free(io->carry);
	io->carry = NULL;
	io->n_carry = io->cap_carry = 0;
	(void)pthread_mutex_destroy(&io->del_lock);
	if (!io->ring_dead)
		io_uring_queue_exit(&io->ring);
}

/*
 * Allocate, link and arm a fresh registration node for fd.
 *
 * The node pointer IS the user_data of at most ONE poll in flight, and
 * that invariant is what reclamation depends on: the reap path frees a
 * zombie when it sees a CQE without IORING_CQE_F_MORE, which only means
 * "the last completion for this node" if no SECOND poll is in flight
 * under the same pointer.  A caller changing a registration must
 * therefore retire the old node and add a new one (see xtc_io_mod_fd)
 * -- never re-arm in place while armed.  Re-arming after the terminal
 * CQE (armed == 0, see xtc_io_poll) keeps the invariant: the old poll
 * is already gone.
 *
 * Does not flush the SQ; the caller does, so it can batch the add with
 * the matching retire.
 */
static int
__add_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag, int is_wakeup)
{
	struct __xtc_uring_fd *uf;
	int rc;
	if ((rc = __tab_reserve(io, fd)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc(1, sizeof *uf, (void **)&uf)) != XTC_OK)
		return rc;
	uf->fd = fd;
	uf->interest = interest;
	uf->tag = tag;
	uf->is_wakeup = is_wakeup;
	io->tab[fd] = uf;
	if (is_wakeup)
		__node_defer(io, uf);  /* the first poll arms it */
	else
		__node_kick(io, uf);   /* arms now, or queues the arm */
	return XTC_OK;
}

/*
 * Retire a live registration: drop it from the table and, if its poll
 * is still in the kernel, cancel it and park the node on the zombie list
 * until the terminal CQE drains.
 *
 * Do not free an armed node: the multishot poll submitted with
 * user_data == uf may still have CQEs in flight (an already-ready
 * notification, plus the -ECANCELED terminal CQE the poll_remove
 * triggers).  Freeing now and then draining those CQEs is a
 * use-after-free.  An UNARMED node has nothing in flight -- the kernel
 * already ended its poll -- and waiting for a terminal CQE that has come
 * and gone is how nodes leaked (4488 of 5000 per cycle in the CQ
 * overflow reproducer), so it is freed now.
 *
 * Clears the table slot only if it still names THIS node, so it retires
 * exactly this node even while a replacement for the same fd is already
 * installed (the xtc_io_mod_fd sequence).  Does not flush the SQ.
 */
static void
__retire_fd(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	if (io->tab[uf->fd] == uf)
		io->tab[uf->fd] = NULL;
	uf->dead = 1;
	if (uf->armed) {
		__zombie_link(io, uf);
		__node_kick(io, uf);   /* sends, or queues, the cancel */
	} else if (!uf->pending) {
		__node_free(uf);
	}
	/* else: unarmed and queued; __flush_pending frees it. */
}

/*
 * The wakeup poll is NOT submitted here, on the creating thread: it is
 * queued, and the first xtc_io_poll -- run by the thread that owns the
 * loop -- arms it.  A poll's completion task_work runs on the task that
 * SUBMITTED it, so a creator-submitted wakeup poll made the first
 * cross-thread wake of every loop depend on the creator: under
 * COOP_TASKRUN a creator computing in user mode held the owner's wake
 * until its next tick or system call.  A byte written before the arm is
 * not lost: the poll is level-checked when armed.
 */
int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	return __add_fd(io, fd, XTC_IO_READABLE, NULL, 1);
}

/*
 * Apply a QUEUED deferred delete for `fd` now, if one is pending.
 *
 * Why this exists.  __xtc_io_defer_del_fd queues a cross-thread
 * unregister and relies on the OWNING loop draining it at the top of its
 * next xtc_io_poll.  Until that drain runs the node is still in io->tab,
 * so it is indistinguishable from a live registration -- and a
 * re-register of the SAME fd is rejected as a duplicate.
 *
 * That turns into a deadlock rather than a transient error, because the
 * only thread that can drain the queue is the same thread that is now
 * being told XTC_E_INVAL: a consumer whose wait fails and retries never
 * returns to its poll, so the pending delete is never applied and every
 * retry fails identically.  Reported from a PostgreSQL fiber workload
 * (fd 447 rejected with a matching pending_del[0] == 447 on the same io),
 * and reproducible in ~60 lines: register, have a foreign thread
 * defer-delete, then re-register the same fd.
 *
 * Draining just this fd here is safe precisely BECAUSE reg_fd runs on the
 * owner: both this and __drain_pending_del perform the real
 * xtc_io_del_fd on the owner thread, so the fd table and the
 * single-producer SQ ring keep their single-owner discipline.  We remove
 * the fd from the queue under del_lock before acting on it, so the later
 * drain cannot delete it a second time -- and a second delete would be
 * the dangerous case, since by then the number could name a DIFFERENT
 * live registration.
 *
 * Keyed on the fd NUMBER because that is all the deferred queue carries.
 * That is sound HERE and only here: we are about to register this exact
 * fd on this exact io, so consuming a delete queued for it is the caller's
 * own intent, not a guess about somebody else's registration.
 */
static void
__apply_pending_del_for(xtc_io_t *io, int fd)
{
	int found = 0, i, j;

	if (atomic_load_explicit(&io->has_pending_del,
	    memory_order_acquire) == 0)
		return;
	(void)__xtc_mtx_lock(&io->del_lock);
	for (i = 0; i < io->n_pending_del; i++) {
		if (io->pending_del[i] != fd)
			continue;
		found = 1;
		for (j = i; j < io->n_pending_del - 1; j++)
			io->pending_del[j] = io->pending_del[j + 1];
		io->n_pending_del--;
		i--;                     /* re-test this slot: fd may repeat */
	}
	if (io->n_pending_del == 0)
		atomic_store_explicit(&io->has_pending_del, 0,
		    memory_order_relaxed);
	(void)__xtc_mtx_unlock(&io->del_lock);
	if (found)
		(void)xtc_io_del_fd(io, fd);
}

/*
 * The fiber-reachable SQ producers (reg/mod/del fd, aio submit) and the
 * poll/reap path all mutate state that only this io's owner thread may
 * touch and that has no lock: the SQ ring (liburing's sqe_tail/sqe_head
 * bookkeeping and the slot being filled), io->tab, io->pending and the
 * zombie list.  Under involuntary preemption a fiber can be redirected
 * to the scheduler at ANY instruction, and the scheduler then runs other
 * fibers -- and its own xtc_io_poll -- on this same thread, interleaving
 * with the half-done update.  So each public entry point holds the
 * preemption-unsafe bracket for its whole body: a tick that lands inside
 * defers to the next cooperative yield-check instead of redirecting.
 * None of these bodies yields, so the bracket never spans a park.
 */
static int __reg_fd(xtc_io_t *, int, uint32_t, void *);
static int __aio_submit(xtc_io_t *, xtc_aio_t *);
static int __mod_fd(xtc_io_t *, int, uint32_t, void *);
static int __del_fd(xtc_io_t *, int);

/* PUBLIC: int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int rc;
	if (io == NULL)
		return XTC_E_INVAL;
	__xtc_unsafe_enter();
	if ((rc = __own(io, 0)) == XTC_OK)
		rc = __reg_fd(io, fd, interest, tag);
	__xtc_unsafe_leave();
	return rc;
}

/* PUBLIC: int xtc_io_aio_submit __P((xtc_io_t *, xtc_aio_t *)); */
int
xtc_io_aio_submit(xtc_io_t *io, xtc_aio_t *a)
{
	int rc;
	if (io == NULL)
		return XTC_E_INVAL;
	__xtc_unsafe_enter();
	if ((rc = __own(io, 0)) == XTC_OK)
		rc = __aio_submit(io, a);
	__xtc_unsafe_leave();
	return rc;
}

/* PUBLIC: int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int rc;
	if (io == NULL)
		return XTC_E_INVAL;
	__xtc_unsafe_enter();
	if ((rc = __own(io, 0)) == XTC_OK)
		rc = __mod_fd(io, fd, interest, tag);
	__xtc_unsafe_leave();
	return rc;
}

/* PUBLIC: int xtc_io_del_fd __P((xtc_io_t *, int)); */
int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	int rc;
	if (io == NULL)
		return XTC_E_INVAL;
	__xtc_unsafe_enter();
	if ((rc = __own(io, 0)) == XTC_OK)
		rc = __del_fd(io, fd);
	__xtc_unsafe_leave();
	return rc;
}

static int
__reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int rc;
	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	/* A delete for this fd may be QUEUED but not yet drained (a
	 * cross-loop unregister whose owner has not polled since).  The node
	 * is still linked, so without this it reads as a live duplicate and
	 * the caller is told XTC_E_INVAL for a registration it is entitled
	 * to make -- and only this thread could have drained it, so the
	 * error never clears.  See __apply_pending_del_for. */
	__apply_pending_del_for(io, fd);
	if (__find_fd(io, fd) != NULL)
		return XTC_E_INVAL;        /* duplicate: genuinely live */
	if ((rc = __add_fd(io, fd, interest, tag, 0)) != XTC_OK)
		return rc;
	(void)__ring_submit(io);
	return XTC_OK;
}

static int
__aio_submit(xtc_io_t *io, xtc_aio_t *a)
{
	struct io_uring_sqe *sqe;
	if (io == NULL || a == NULL || a->fd < 0)
		return XTC_E_INVAL;
	/* Reject an unknown op BEFORE reserving a slot: io_uring_get_sqe
	 * cannot be undone, so a reserved-then-abandoned slot would be
	 * published, with its stale contents, by the next submit. */
	if (a->op < XTC_AIO_PREAD || a->op > XTC_AIO_PWRITEV)
		return XTC_E_INVAL;
	if ((sqe = __get_sqe(io)) == NULL)
		return XTC_E_AGAIN;
	switch (a->op) {
	case XTC_AIO_PREAD:
		io_uring_prep_read(sqe, a->fd, a->buf, a->len,
		    (unsigned long long)a->off);
		break;
	case XTC_AIO_PWRITE:
		io_uring_prep_write(sqe, a->fd, a->buf, a->len,
		    (unsigned long long)a->off);
		break;
	case XTC_AIO_PREADV:
		io_uring_prep_readv(sqe, a->fd,
		    (const struct iovec *)a->iov, (unsigned)a->iovcnt,
		    (unsigned long long)a->off);
		break;
	case XTC_AIO_PWRITEV:
		io_uring_prep_writev(sqe, a->fd,
		    (const struct iovec *)a->iov, (unsigned)a->iovcnt,
		    (unsigned long long)a->off);
		break;
	case XTC_AIO_FSYNC:
		io_uring_prep_fsync(sqe, a->fd, 0);
		break;
	case XTC_AIO_FDATASYNC:
		io_uring_prep_fsync(sqe, a->fd, IORING_FSYNC_DATASYNC);
		break;
	default:   /* unreachable: the range check above */
		io_uring_prep_nop(sqe);
		io_uring_sqe_set_data(sqe, NULL);
		return XTC_E_INVAL;
	}
	a->done = 0;   /* plain: our own thread, before the SQE is visible */
	a->res = 0;
	io->n_aio++;   /* until its CQE is reaped; see __ring_rebind */
	/* Low-bit tag distinguishes this completion from a poll-add CQE. */
	io_uring_sqe_set_data(sqe, (void *)((uintptr_t)a | 1u));
	/* Record the ASK, keyed by the task, so a later "no REAP" can be told
	 * apart from "never submitted".  Emitted before the submit: if the
	 * submit then fails, SUBMIT_FAIL follows and the pair reads correctly. */
	if (__xtc_tail_on(XTC_TAIL_SCHED)) {
		xtc_pid_t lp;
		memset(&lp, 0, sizeof lp);
		lp.loop_id = (uint16_t)(io->tail_loop_id >= 0 ?
		    (unsigned)io->tail_loop_id : XTC_TAIL_LOOP_NONE);
		__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_SUBMIT, lp,
		    (uint64_t)(uintptr_t)a->tag);
	}
	(void)__ring_submit(io);
	return XTC_OK;
}

static int
__mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct __xtc_uring_fd *uf;
	int rc;
	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	uf = __find_fd(io, fd);
	if (uf == NULL || uf->is_wakeup)
		return XTC_E_INVAL;
	/*
	 * Retire the old node and arm a FRESH one; do NOT cancel and
	 * re-arm in place under the same user_data.  Re-arming left two
	 * poll generations sharing one node, and then a single
	 * xtc_io_del_fd plus the reap path's "terminal CQE means the last
	 * completion" rule freed the node on the FIRST generation's
	 * terminal CQE while the second could still deliver against it --
	 * an invalid read plus a double free (reg -> mod -> del -> poll).
	 * One node, one poll, so the rule holds by construction.
	 *
	 * Add BEFORE retire: the new node replaces the old in io->tab, so a
	 * failed add leaves the old registration as the only one and fully
	 * intact, and the fd is never left unwatched in between.  The
	 * poll_remove names the OLD node's user_data, so it cannot cancel
	 * the replacement.
	 */
	if ((rc = __add_fd(io, fd, interest, tag, 0)) != XTC_OK)
		return rc;
	__retire_fd(io, uf);
	(void)__ring_submit(io);
	return XTC_OK;
}

static int
__del_fd(xtc_io_t *io, int fd)
{
	struct __xtc_uring_fd *uf;
	if (io == NULL || fd < 0) return XTC_E_INVAL;
	uf = __find_fd(io, fd);
	if (uf == NULL || uf->is_wakeup) return XTC_E_INVAL;
	__retire_fd(io, uf);
	(void)__ring_submit(io);
	return XTC_OK;
}

/*
 * Registration accounting for tests and diagnostics: live registrations
 * (excluding the wakeup fd), retired nodes still awaiting their terminal
 * CQE, and nodes queued for an SQE.  After every registration is deleted
 * and the CQ drained, zombies and pending must both reach 0.
 */
int
__xtc_io_reg_stats(xtc_io_t *io, int *live, int *zombies, int *pending)
{
	struct __xtc_uring_fd *p;
	int i, n = 0;
	if (io == NULL)
		return XTC_E_INVAL;
	for (i = 0; i < io->tab_cap; i++)
		if (io->tab[i] != NULL && !io->tab[i]->is_wakeup)
			n++;
	if (live != NULL)
		*live = n;
	if (zombies != NULL)
		*zombies = io->n_zombies;
	n = 0;
	for (p = io->pending; p != NULL; p = p->pnext)
		if (!p->is_wakeup)
			n++;
	if (pending != NULL)
		*pending = n;
	return XTC_OK;
}

/*
 * Cross-loop deferred unregister.  Called when a fiber that parked on
 * THIS io's loop was woken via a non-fd path and work-stolen, so its
 * xtc_proc_wait_fd cleanup runs on a DIFFERENT OS thread than this io's
 * owner.  Calling xtc_io_del_fd directly from that foreign thread would
 * race the single-owner fd table AND the single-producer SQ ring.
 * Instead, queue the fd under del_lock and nudge the owning loop; it
 * drains this queue at the top of its next xtc_io_poll and performs the
 * real unregister on its own thread.  Internal (declared in io_int.h).
 */
/*
 * The calling thread starts running this io's loop (an executor worker at
 * startup, xtc_loop_run on entry): it becomes the owner now, before any
 * fiber runs, so a cross-loop cleanup on a peer thread always defers its
 * unregister to it (a home loop whose fibers were all work-stolen can
 * run its own queue for a long window without polling).  On a
 * SINGLE_ISSUER ring this is also where the ring is bound to the thread,
 * or rebuilt for it if a previous run's thread owned it (see __own).  A
 * failure is reported by the loop's next poll.
 */
void
__xtc_io_set_owner(xtc_io_t *io)
{
	if (io == NULL)
		return;
	__xtc_unsafe_enter();
	if (__own(io, 1) == XTC_OK)
		atomic_store_explicit(&io->owner_active, 1,
		    memory_order_release);
	__xtc_unsafe_leave();
}

/*
 * The calling thread stops running this io's loop.  See io_int.h.
 *
 * Only a SINGLE_ISSUER ring needs work here.  Its completions are posted
 * only when its issuer waits for them, and the next owner gets a NEW ring
 * (__ring_rebind) -- so a file-AIO op still in the kernel would never
 * complete for the fiber parked on it.  Reap until none is left (this
 * waits for the I/O itself: a read or fsync in flight when the loop was
 * stopped), keeping everything the reap returns -- the AIO completions
 * and any readiness reaped alongside -- for the next owner's first poll.
 * A loop that ran to idle has nothing in flight: the common case is one
 * comparison.  The io stays owned by this thread, so a peer's cleanup
 * still queues to it, and a later run on this same thread keeps the ring.
 *
 * shortcut: the wait is unbounded, so an AIO op that never completes (a
 * read on an empty pipe: xtc_aio_* is for files) holds the loop's exit;
 * add an IORING_OP_ASYNC_CANCEL after a deadline if a consumer needs it.
 */
void
__xtc_io_owner_leave(xtc_io_t *io)
{
	xtc_io_event_t ev[64];
	xtc_io_event_t *nc;
	int n;

	if (io == NULL || atomic_load_explicit(&io->owner_tok,
	    memory_order_relaxed) != __self_tok())
		return;
	__xtc_unsafe_enter();
	while (io->ring_single && io->n_aio > 0 && !io->ring_dead) {
		n = 0;
		if (__poll(io, ev, (int)(sizeof ev / sizeof ev[0]),
		    100LL * 1000 * 1000, &n) != XTC_OK)
			break;
		if (n == 0)
			continue;
		if (io->n_carry + n > io->cap_carry) {
			int nc_cap = (io->n_carry + n) * 2;
			if (__os_realloc(io->carry, (size_t)nc_cap * sizeof *nc,
			    (void **)&nc) != XTC_OK)
				break;   /* shortcut: no memory loses these wakes; a parked fiber stays parked */
			io->carry = nc;
			io->cap_carry = nc_cap;
		}
		memcpy(io->carry + io->n_carry, ev, (size_t)n * sizeof *ev);
		io->n_carry += n;
	}
	atomic_store_explicit(&io->owner_active, 0, memory_order_release);
	__xtc_unsafe_leave();
}

int
__xtc_io_defer_del_fd(xtc_io_t *io, int fd)
{
	uint64_t cur;

	if (io == NULL || fd < 0) return XTC_E_INVAL;
	/*
	 * If we ARE the io's owner, unregister inline -- this is the common
	 * case (a fiber re-parking on the same fd on its own loop) and MUST
	 * be synchronous: deferring it would leave the old registration in
	 * io->tab so the immediate re-register in the next xtc_proc_wait_fd
	 * sees a duplicate and fails, stranding the fiber.  Only a genuinely
	 * different thread defers.  An io nobody has touched has no fd
	 * registered (registering makes the caller the owner), so there is
	 * nothing to delete: answer as __del_fd would, without binding the
	 * ring to this thread.
	 */
	cur = atomic_load_explicit(&io->owner_tok, memory_order_acquire);
	if (cur == 0)
		return XTC_E_INVAL;
	if (cur == __self_tok())
		return xtc_io_del_fd(io, fd);
	(void)__xtc_mtx_lock(&io->del_lock);
	if (io->n_pending_del >= io->cap_pending_del) {
		int newcap = io->cap_pending_del ? io->cap_pending_del * 2 : 16;
		int *np = NULL;
		if (__os_realloc(io->pending_del,
		    (size_t)newcap * sizeof(int), (void **)&np) != XTC_OK) {
			(void)__xtc_mtx_unlock(&io->del_lock);
			return XTC_E_NOMEM;
		}
		io->pending_del = np;
		io->cap_pending_del = newcap;
	}
	io->pending_del[io->n_pending_del++] = fd;
	atomic_store_explicit(&io->has_pending_del, 1, memory_order_release);
	(void)__xtc_mtx_unlock(&io->del_lock);
	/* Nudge the owner out of its I/O wait so it drains promptly. */
	return xtc_io_wakeup(io);
}

/*
 * Drain the deferred-unregister queue on the OWNING loop thread (called
 * from xtc_io_poll only).  Performs the real xtc_io_del_fd for each
 * queued fd on this thread, so the fd table + SQ ring stay
 * single-owner.
 */
static void
__drain_pending_del(xtc_io_t *io)
{
	int local[32];
	int n = 0, i;
	if (atomic_load_explicit(&io->has_pending_del,
	    memory_order_acquire) == 0)
		return;
	for (;;) {
		(void)__xtc_mtx_lock(&io->del_lock);
		n = io->n_pending_del;
		if (n > (int)(sizeof local / sizeof local[0]))
			n = (int)(sizeof local / sizeof local[0]);
		for (i = 0; i < n; i++)
			local[i] = io->pending_del[i];
		/* shift the remainder down */
		if (n < io->n_pending_del) {
			int r = io->n_pending_del - n, j;
			for (j = 0; j < r; j++)
				io->pending_del[j] = io->pending_del[n + j];
			io->n_pending_del = r;
		} else {
			io->n_pending_del = 0;
			atomic_store_explicit(&io->has_pending_del, 0,
			    memory_order_relaxed);
		}
		(void)__xtc_mtx_unlock(&io->del_lock);
		for (i = 0; i < n; i++)
			(void)xtc_io_del_fd(io, local[i]);
		if (n < (int)(sizeof local / sizeof local[0]))
			break;   /* fewer than a full batch remained -> done */
	}
}

/* XTC_NOALLOC_BEGIN: io_uring per-poll CQE reap path (PLAN.md 19.23).
 * KNOWN GAP (flagged in the s_noalloc rollout report, not silenced):
 * the zombie-reap branch a few lines below (the `if (uf != NULL &&
 * uf->dead)` block) calls __os_free(uf) to release a deregistered
 * fd's node once its terminal CQE drains.  That call is bounded to
 * once per xtc_io_del_fd (not once per steady-state poll), so it is
 * carved OUT of this marked region below rather than hidden behind an
 * XTC_NOALLOC_OK -- the region resumes immediately after it.  The
 * steady-state dispatch of a ready fd, an AIO completion, or the
 * wakeup event never allocates. */
/*
 * Record that a CQE's life ended here, and what tag it resolved to.
 *
 * tag == NULL means the completion was consumed and NOT handed to dispatch.
 * That is correct for the wakeup pipe and for a poll_remove cancel, and it is
 * also the signature of a completion dropped because its registration was
 * already torn down -- a timeline that cannot see the discard cannot tell a
 * deliberate one from a bug, which is the whole reason this exists.
 */
static void
__tail_reap(xtc_io_t *io, void *tag)
{
	xtc_pid_t lp;
	if (!__xtc_tail_on(XTC_TAIL_SCHED))
		return;
	memset(&lp, 0, sizeof lp);
	lp.loop_id = (uint16_t)(io->tail_loop_id >= 0 ?
	    (unsigned)io->tail_loop_id : XTC_TAIL_LOOP_NONE);
	__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_REAP, lp,
	    (uint64_t)(uintptr_t)tag);
}

static int
__poll(xtc_io_t *io, xtc_io_event_t *events, int max,
       int64_t timeout_ns, int *n_out)
{
	struct __kernel_timespec ts, *tsp;
	int got = 0;
	unsigned spent = 0, cap;
	struct io_uring_cqe *cqe;
	int rc;

	/* Owner-thread drain of any cross-loop deferred unregisters queued
	 * by foreign threads (see __xtc_io_defer_del_fd).  Done here so the
	 * fd table + SQ ring are only ever mutated by this io's own loop. */
	__drain_pending_del(io);
	/* Any arm/re-arm/cancel the SQ had no room for goes in now, before
	 * this poll can block on its outcome. */
	if (io->pending != NULL)
		__flush_pending(io);   /* may free a retired node: bounded per del */

	if (timeout_ns < 0) {
		tsp = NULL;
	} else {
		ts.tv_sec  = timeout_ns / 1000000000LL;
		ts.tv_nsec = timeout_ns % 1000000000LL;
		tsp = &ts;
	}

	/* Wait for at least one CQE (or timeout). */
	rc = io_uring_wait_cqe_timeout(&io->ring, &cqe, tsp);
	if (rc == -ETIME || rc == -EINTR) return XTC_OK;
	if (rc < 0) {
		errno = -rc;
		return XTC_E_INTERNAL;
	}

	/* Drain the available CQEs until max events are DELIVERED.  The
	 * budget counts only what is handed back: CQEs that are consumed and
	 * dropped (cancel results, completions for retired nodes) are cheap,
	 * and counting them let a CQ full of such leftovers -- the state CQ
	 * overflow leaves behind -- return n == 0 again and again while real
	 * readiness waited behind it.
	 *
	 * CQ overflow needs no separate handling here: when the CQ ring is
	 * empty and the kernel holds overflowed CQEs (IORING_SQ_CQ_OVERFLOW),
	 * io_uring_peek_cqe enters the kernel with GETEVENTS, which flushes
	 * them into the ring, so this loop drains the overflow list too.
	 *
	 * We coalesce all wakeup CQEs into a single XTC_IO_WAKEUP event per
	 * poll, matching the M2 W3 coalesce contract. */
	int wakeup_emitted = 0, rearmed = 0, drop;
	/* Hard bound on CQEs consumed per call, so a steady stream that
	 * delivers nothing (repeated wakeups after the first is coalesced)
	 * cannot keep us here: one full CQ beyond the event budget. */
	cap = (unsigned)max + io->ring.cq.ring_entries;
	while (got < max && spent++ < cap) {
		struct __xtc_uring_fd *uf;
		void *data;
		if (cqe == NULL) {
			if (io_uring_peek_cqe(&io->ring, &cqe) != 0) break;
		}
		data = io_uring_cqe_get_data(cqe);
		/* Low bit tags an async file-I/O completion (xtc_aio_t *);
		 * __xtc_uring_fd pointers are calloc-aligned, so bit 0 is 0. */
		if (((uintptr_t)data & 1u) != 0) {
			xtc_aio_t *a = (xtc_aio_t *)((uintptr_t)data & ~(uintptr_t)1);
			io->n_aio--;
			a->res = cqe->res;
			/* Release: publish res BEFORE the done flag the parked
			 * fiber acquires, so a fiber that sees done sees res.
			 * We are the REAPING thread; the fiber is another. */
			__xtc_aio_done_set(a);
			if (got < max) {
				events[got].fd = -1;
				events[got].tag = a->tag;
				events[got].flags = XTC_IO_AIO;
				got++;
			}
			__tail_reap(io, a->tag);
			io_uring_cqe_seen(&io->ring, cqe);
			cqe = NULL;
			continue;
		}
		uf = (struct __xtc_uring_fd *)data;
		if (uf != NULL && uf->dead) {
/* XTC_NOALLOC_END -- see the KNOWN GAP note above xtc_io_poll: the
 * next block's __os_free(uf) is a real, bounded (per-deregistration,
 * not per-poll) allocator touch left OUT of the marked region rather
 * than hidden behind an exception marker. */
			/* Registration was deleted.  Do not dispatch to it.
			 * Once its multishot poll delivers the terminal CQE
			 * (no IORING_CQE_F_MORE -- e.g. the -ECANCELED from
			 * poll_remove), no further CQE can reference uf, so it
			 * is safe to free (or, if it is still queued for a
			 * cancel the SQ had no room for, __flush_pending
			 * frees it). */
			if (!(cqe->flags & IORING_CQE_F_MORE)) {
				uf->armed = 0;
				__zombie_unlink(io, uf);
				if (!uf->pending)
					__node_free(uf);
			}
			/* Consumed and deliberately NOT dispatched: the
			 * registration was deleted.  Recorded with tag 0 so
			 * the discard is VISIBLE -- an invisible drop here is
			 * exactly the shape of a lost wake. */
			__tail_reap(io, NULL);
			io_uring_cqe_seen(&io->ring, cqe);
			cqe = NULL;
			continue;
/* XTC_NOALLOC_BEGIN: resuming the steady-state dispatch path (ready fd
 * / wakeup) after the zombie-reap carve-out above. */
		}
		/*
		 * A terminal CQE for a LIVE node means the kernel ended its
		 * poll: always for the single-shot wakeup poll, and for a
		 * multishot when the CQ was full at the moment it fired (the
		 * kernel then ends the poll and queues its final CQE on the
		 * overflow list).  Nothing is watching the fd any more, so
		 * re-arm it -- before, a registration that hit CQ overflow
		 * stayed in the table but never reported readiness again, and
		 * its eventual del parked the node on the zombie list waiting
		 * for a terminal CQE that had already come: 4488 of 5000 nodes
		 * leaked per cycle in the overflow reproducer.  -ECANCELED
		 * (the submitting thread exited) and -EOVERFLOW are re-armed
		 * too; any other error (e.g. -EBADF) is reported and NOT
		 * re-armed, since it would fail the same way on every poll.
		 */
		drop = 0;
		if (uf != NULL && !(cqe->flags & IORING_CQE_F_MORE)) {
			uf->armed = 0;
			/* The kernel ended the poll without an fd error:
			 * re-arm silently, nothing to report. */
			drop = !uf->is_wakeup && (cqe->res == -ECANCELED ||
			    cqe->res == -EOVERFLOW);
			if (cqe->res >= 0 || drop || uf->is_wakeup) {
				__node_kick(io, uf);
				rearmed = 1;
			}
		}
		if (uf != NULL && !drop) {
			if (uf->is_wakeup) {
				/* Re-arm the single-shot wakeup poll BEFORE draining
				 * the pipe.  Ordering is the fix for the cross-thread
				 * idle-loop wake miss (carrier report 2026-07-06): if
				 * we drained first and re-armed after, a foreign
				 * xtc_io_wakeup write landing in that window would
				 * find no armed poll and be missed, hanging the idle
				 * loop forever.  Re-arming first means the poll is
				 * always armed across the drain, so any write -- before
				 * or after the drain -- surfaces a CQE and breaks the
				 * next io_uring_wait.  (A write between re-arm and
				 * drain is caught by the already-armed poll; a byte
				 * still present at re-arm fires the poll immediately
				 * -- level-triggered -- which the next poll coalesces.)
				 * The re-arm was prepared just above (__node_kick);
				 * submit it now.  If the SQ had no room it is queued
				 * on io->pending, which the next poll flushes before
				 * it can block -- it is never dropped.
				 *
				 * Only the FIRST wakeup CQE of a poll is submitted
				 * inline (its drain follows).  A later one finds the
				 * pipe already drained by this call, so its re-arm
				 * waits for the end-of-poll submit: submitted here, a
				 * still-readable pipe completes it at once and the
				 * next peek finds another wakeup CQE -- a self-feeding
				 * io_uring_enter chain bounded only by the reap cap
				 * (max + CQ size, ~528 enters on the default ring). */
				if (!wakeup_emitted) {
					int drc;
					(void)__ring_submit(io);
					rearmed = 0;
					drc = __xtc_io_drain_wakeup(io);
					if (drc != XTC_OK) {
						__tail_reap(io, NULL);
						io_uring_cqe_seen(&io->ring, cqe);
						return drc;
					}
					if (got < max) {
						events[got].tag = NULL;
						events[got].flags = XTC_IO_WAKEUP;
						events[got].fd = -1;
						got++;
					}
					wakeup_emitted = 1;
				}
			} else {
				if (cqe->res < 0) {
					if (got < max) {
						events[got].tag = uf->tag;
						events[got].flags = XTC_IO_ERR;
						events[got].fd = uf->fd;
						got++;
					}
				} else {
					if (got < max) {
						events[got].tag = uf->tag;
						events[got].fd = uf->fd;
						events[got].flags =
						    __pollres_to_flags((uint32_t)cqe->res);
						got++;
					}
				}
			}
		}
		/* uf==NULL is a poll_remove cancel CQE (user_data NULL by
		 * design); the wakeup pipe has no task either.  Both report
		 * tag 0, which is why REAP alone is not a bug signal -- it is
		 * the JOIN that matters. */
		__tail_reap(io, (uf != NULL && !uf->is_wakeup) ? uf->tag : NULL);
		io_uring_cqe_seen(&io->ring, cqe);
		cqe = NULL;
	}
	if (rearmed)
		(void)__ring_submit(io);
	*n_out = got;
	/*
	 * We filled the budget, so there may be more in the CQ than we took.
	 * Recorded because LOOP_POLL only covers IDLE polls, leaving "polled
	 * and left some behind" -- the state that explains a pinned backlog on
	 * a live loop -- with no event at all.
	 */
	if (got >= max && __xtc_tail_on(XTC_TAIL_SCHED)) {
		xtc_pid_t lp;
		memset(&lp, 0, sizeof lp);
		lp.loop_id = (uint16_t)(io->tail_loop_id >= 0 ?
		    (unsigned)io->tail_loop_id : XTC_TAIL_LOOP_NONE);
		__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_POLL_FULL, lp,
		    (uint64_t)max);
	}
	return XTC_OK;
}
/* XTC_NOALLOC_END */

/*
 * Events a leaving owner reaped (__xtc_io_owner_leave) come first, and
 * alone: they are already consumed from the ring.  Rare.
 */
static int
__poll_carry(xtc_io_t *io, xtc_io_event_t *events, int max, int *n_out)
{
	int got = io->n_carry < max ? io->n_carry : max;

	memcpy(events, io->carry, (size_t)got * sizeof *events);
	memmove(io->carry, io->carry + got,
	    (size_t)(io->n_carry - got) * sizeof *events);
	io->n_carry -= got;
	*n_out = got;
	return XTC_OK;
}

/* PUBLIC: int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *)); */
/*
 * The reap path is an SQ producer too (wakeup / multishot re-arms, the
 * pending flush, zombie frees), so it holds the same bracket as the
 * fiber-side producers above.  The loop polls from the scheduler, where
 * no fiber is current and a tick cannot redirect anyway; the bracket is
 * for a fiber that polls directly.  It spans the blocking wait, which is
 * harmless: the tick source is per-thread CPU time, idle while blocked.
 */
int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	int rc;
	if (io == NULL || events == NULL || max <= 0 || n_out == NULL)
		return XTC_E_INVAL;
	*n_out = 0;
	__xtc_unsafe_enter();
	if ((rc = __own(io, 0)) == XTC_OK) {
		if (io->n_carry > 0)
			rc = __poll_carry(io, events, max, n_out);
		else
			rc = __poll(io, events, max, timeout_ns, n_out);
	}
	__xtc_unsafe_leave();
	return rc;
}

/*
 * ---- L2: io_uring ring-pointer preempt ---------------------------
 *
 * INSPIRED BY Glommio (Glauber Costa / ScyllaDB): its need_preempt()
 * (glommio src/reactor.rs) is just `*cq.khead != *cq.ktail` on a
 * dedicated "latency" ring whose only traffic is a TIMEOUT SQE
 * (glommio src/sys/uring.rs: preempt_pointers / the LatencyRing).  Two
 * relaxed/acquire loads of the ring's own head/tail -- NO syscall, NO
 * signal, NO async-signal-safety worry, and NO wrong-thread signal-
 * delivery bug (the class libxtc fought in v1.34.1).
 *
 * libxtc keeps the per-thread CPU-time POSIX timer + SIGVTALRM path
 * (src/ptc/preempt.c) as the PORTABLE fallback for epoll/kqueue/iocp/
 * select, where there is no ring to read; this is an io_uring-backend
 * OPTIMISATION, not a replacement.  On the uring backend the executor
 * arms THIS instead of the signal timer, so a long compute fiber gets
 * sliced with no signal delivered.
 *
 * We use a SEPARATE small ring (io->preempt_ring), not io->ring,
 * deliberately: the main ring's CQ head/tail move with every I/O
 * completion, so a pointer test there could not isolate "the timeout
 * fired".  The preempt ring sees only the rearmed timeout, so
 * io_uring_cq_ready(&preempt_ring) != 0 means exactly "a preempt tick
 * is due".
 */

#define XTC_URING_PREEMPT_UD  ((__u64)0x70726d7431ULL)   /* "prmt1" */

/* Submit (or resubmit) the single rearmed TIMEOUT SQE on the preempt
 * ring.  count=0, flags=0: a relative timeout that posts one CQE after
 * preempt_ts elapses.  We resubmit each time the CQE is reaped, so the
 * ring carries at most one timeout in flight. */
static int
__preempt_arm_timeout(xtc_io_t *io)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&io->preempt_ring);
	if (sqe == NULL)
		return XTC_E_AGAIN;
	io_uring_prep_timeout(sqe, &io->preempt_ts, 0, 0);
	io_uring_sqe_set_data64(sqe, XTC_URING_PREEMPT_UD);
	if (io_uring_submit(&io->preempt_ring) < 0)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/*
 * __xtc_io_uring_preempt_arm: create the dedicated preempt ring and arm
 * the first TIMEOUT SQE at `interval_ns`.  Called by the executor
 * worker (exec.c) on the uring backend when preemption is enabled,
 * INSTEAD of the SIGVTALRM timer.
 * Returns XTC_OK on success (the ring is now the preempt trigger),
 * XTC_E_* on failure (the caller then falls back to the signal timer).
 */
int
__xtc_io_uring_preempt_arm(xtc_io_t *io, int64_t interval_ns)
{
	int rc;
	if (io == NULL || interval_ns <= 0)
		return XTC_E_INVAL;
	if (io->preempt_armed)
		return XTC_OK;
	/* A tiny ring -- one timeout in flight -- so 8 entries is ample.
	 *
	 * Attach it to this loop's MAIN ring's io-wq via ATTACH_WQ so the
	 * kernel reuses that pool instead of creating a second one for this
	 * loop.  Today the preempt ring only ever carries TIMEOUT SQEs,
	 * which are handled inline and never dispatch to io-wq, so the
	 * current exposure is zero -- but that is an unenforced property of
	 * what we happen to submit, and a per-ring default-sized pool left
	 * unbounded is precisely the shape that produced the measured
	 * io-wq thread explosion the main ring's cap above exists to stop
	 * (the cap is registered on io->ring only; it never reached this
	 * ring).  Attaching makes "one io-wq pool per loop" structural.
	 *
	 * ATTACH_WQ shares only the worker pool, NOT the completion queue,
	 * so __xtc_io_uring_preempt_due's io_uring_cq_ready on this ring
	 * still sees timeout CQEs and nothing else.
	 *
	 * Best-effort: a kernel without ATTACH_WQ, or a main ring that is
	 * not attachable, fails the params init -- fall back to the plain
	 * unattached init so behavior is byte-for-byte what it was, and
	 * cap this ring directly in that case so it is bounded either way. */
	{
		struct io_uring_params p;
		memset(&p, 0, sizeof p);
		p.flags = IORING_SETUP_ATTACH_WQ;
		p.wq_fd = (__u32)io->ring.ring_fd;
		rc = io_uring_queue_init_params(8, &io->preempt_ring, &p);
		if (rc < 0) {
			rc = io_uring_queue_init(8, &io->preempt_ring, 0);
			if (rc >= 0) {
				unsigned vals[2];
				vals[0] = atomic_load_explicit(
				    &__xtc_iowq_bound, memory_order_relaxed);
				vals[1] = atomic_load_explicit(
				    &__xtc_iowq_unbound, memory_order_relaxed);
				if (vals[0] != 0u || vals[1] != 0u)
					(void)io_uring_register_iowq_max_workers(
					    &io->preempt_ring, vals);
			}
		}
	}
	if (rc < 0) {
		errno = -rc;
		return XTC_E_INTERNAL;
	}
	io->preempt_ts.tv_sec  = interval_ns / 1000000000LL;
	io->preempt_ts.tv_nsec = interval_ns % 1000000000LL;
	if ((rc = __preempt_arm_timeout(io)) != XTC_OK) {
		io_uring_queue_exit(&io->preempt_ring);
		return rc;
	}
	io->preempt_armed = 1;
	return XTC_OK;
}

/*
 * __xtc_io_uring_preempt_due: the Glommio need_preempt() check -- 1 if
 * the preempt interval elapsed since the last consult, else 0.  The
 * fast path is TWO relaxed/acquire loads (io_uring_cq_ready reads the
 * ring's own khead/ktail) -- no syscall.  When it IS due we reap the
 * timeout CQE and resubmit the next one, so the caller sees each
 * interval exactly once; that reap + resubmit is off the hot path
 * (only when a slice actually elapsed).
 */
int
__xtc_io_uring_preempt_due(xtc_io_t *io)
{
	struct io_uring_cqe *cqe;
	unsigned head, seen = 0;
	if (io == NULL || !io->preempt_armed)
		return 0;
	/* Two relaxed/acquire loads of the preempt ring's head/tail. */
	if (io_uring_cq_ready(&io->preempt_ring) == 0)
		return 0;
	/* Due: drain the timeout CQE(s) and rearm.  A timeout posts one
	 * CQE per interval; drain whatever is present (normally 1) so a
	 * caller that consulted late does not see a backlog of "due". */
	/* Fiber context (xtc_yield_check): the reap + re-arm is a
	 * preempt-ring producer, so bracket it like the main ring's. */
	__xtc_unsafe_enter();
	io_uring_for_each_cqe(&io->preempt_ring, head, cqe)
		seen++;
	if (seen > 0)
		io_uring_cq_advance(&io->preempt_ring, seen);
	(void)__preempt_arm_timeout(io);
	__xtc_unsafe_leave();
	return 1;
}

/*
 * __xtc_io_uring_preempt_disarm: tear down the preempt ring.  Safe if
 * never armed.
 */
void
__xtc_io_uring_preempt_disarm(xtc_io_t *io)
{
	if (io == NULL || !io->preempt_armed)
		return;
	io->preempt_armed = 0;
	io_uring_queue_exit(&io->preempt_ring);
}

#endif /* XTC_IO_BACKEND_URING */

#if !defined(XTC_IO_BACKEND_URING)
#include "xtc_io.h"   /* xtc_io_t (io_int.h is guarded out on this backend) */
/*
 * Portable no-op stubs so loop.c (xtc_yield_check) and exec.c can call
 * the ring-preempt seam unconditionally.  On every non-uring backend
 * these report "no ring preempt available", so the SIGVTALRM/kqueue
 * signal timer stays the preemption trigger (the portable fallback).
 */
int
__xtc_io_uring_preempt_arm(xtc_io_t *io, int64_t interval_ns)
{
	(void)io;
	(void)interval_ns;
	return XTC_E_NOSYS;
}

int
__xtc_io_uring_preempt_due(xtc_io_t *io)
{
	(void)io;
	return 0;
}

void
__xtc_io_uring_preempt_disarm(xtc_io_t *io)
{
	(void)io;
}

int
__xtc_io_reg_stats(xtc_io_t *io, int *live, int *zombies, int *pending)
{
	(void)io; (void)live; (void)zombies; (void)pending;
	return XTC_E_NOSYS;
}

int
__xtc_io_uring_info(xtc_io_t *io, unsigned *flags, unsigned *sq_consumed,
    unsigned *rebinds)
{
	(void)io; (void)flags; (void)sq_consumed; (void)rebinds;
	return XTC_E_NOSYS;
}

void
__xtc_io_owner_leave(xtc_io_t *io)
{
	(void)io;
}
#endif /* !XTC_IO_BACKEND_URING */

typedef int __xtc_io_uring_unused;

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_thread.c
 *	pthread implementation of the thread surface.
 */

#if defined(__linux__)
#define _GNU_SOURCE  /* for the 2-arg pthread_setname_np on glibc */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE  /* expose pthread_setname_np (1-arg) on macOS */
#endif

#include "xtc_int.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>
#if !defined(_WIN32)
#include <signal.h>        /* pthread_sigmask -- block signals on spawned threads */
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/cpuset.h>
#include <pthread_np.h>     /* pthread_setaffinity_np + cpuset_t */
#endif
#if defined(__sun) || defined(__illumos__)
#include <sys/processor.h>  /* processor_bind, P_LWPID, P_MYID */
#include <sys/procset.h>
#endif

#if defined(__APPLE__)
#include <sys/qos.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

#include "os_thread.h"

#include <stdlib.h>

/* Heap-allocated descriptor so the pthread_t outlives the call frame. */
struct __os_thread_state {
	pthread_t pth;
};

/*
 * PUBLIC: int __os_thread_create __P((__os_thread_t *, __os_thread_fn, void *));
 */
int
__os_thread_create(__os_thread_t *thr, __os_thread_fn fn, void *arg)
{
	struct __os_thread_state *st;
	int rc, cr;

	if (thr == NULL || fn == NULL)
		return XTC_E_INVAL;
	if ((rc = __os_malloc(sizeof(*st), (void **)&st)) != XTC_OK)
		return rc;
	cr = __os_pthread_create_masked(&st->pth, fn, arg);
	if (cr != 0) {
		__os_free(st);
		return XTC_E_INTERNAL;
	}
	thr->opaque = st;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_pthread_create_masked __P((pthread_t *, void *(*)(void *), void *));
 *
 * Create a pthread with ALL signals blocked, restoring the caller's mask
 * afterward.  Returns the pthread_create errno (0 on success), so raw
 * pthread_t call sites (the PSI slab thread, the deadlock detector) can
 * use it directly.  See the rationale in __os_thread_create: a runtime
 * thread must never inherit a permissive mask, or a process-directed
 * signal can land on it instead of the embedder's designated handler
 * thread (the carrier SIGCHLD-to-scheduler bug).  POSIX blocks; on
 * Windows (no signal mask) this is a plain pthread_create.
 */
int
__os_pthread_create_masked(pthread_t *out, void *(*fn)(void *), void *arg)
{
	int cr;
#if !defined(_WIN32)
	sigset_t all, prev;
	sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &prev);
#endif
	cr = pthread_create(out, NULL, fn, arg);
#if !defined(_WIN32)
	(void)pthread_sigmask(SIG_SETMASK, &prev, NULL);
#endif
	return cr;
}

/*
 * PUBLIC: int __os_thread_join __P((__os_thread_t *, void **));
 */
int
__os_thread_join(__os_thread_t *thr, void **retval)
{
	struct __os_thread_state *st;
	void *r;
	if (thr == NULL || thr->opaque == NULL)
		return XTC_E_INVAL;
	st = thr->opaque;
	if (pthread_join(st->pth, &r) != 0)
		return XTC_E_INTERNAL;
	if (retval != NULL)
		*retval = r;
	__os_free(st);
	thr->opaque = NULL;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_thread_detach __P((__os_thread_t *));
 */
int
__os_thread_detach(__os_thread_t *thr)
{
	struct __os_thread_state *st;
	if (thr == NULL || thr->opaque == NULL)
		return XTC_E_INVAL;
	st = thr->opaque;
	if (pthread_detach(st->pth) != 0)
		return XTC_E_INTERNAL;
	__os_free(st);
	thr->opaque = NULL;
	return XTC_OK;
}

/*
 * Self handles do not own a state struct; the opaque pointer holds
 * the pthread_t directly via a small static cell so the comparison
 * "this is me" works.
 *
 * PUBLIC: int __os_thread_self __P((__os_thread_t *));
 */
int
__os_thread_self(__os_thread_t *out)
{
	pthread_t *me;
	int rc;
	if (out == NULL)
		return XTC_E_INVAL;
	if ((rc = __os_malloc(sizeof(*me), (void **)&me)) != XTC_OK)
		return rc;
	*me = pthread_self();
	out->opaque = me;
	return XTC_OK;
}

/*
 * PUBLIC: void __os_thread_yield __P((void));
 */
void
__os_thread_yield(void)
{
	(void)sched_yield();
}

/*
 * PUBLIC: int __os_thread_setname __P((const char *));
 */
int
__os_thread_setname(const char *name)
{
	if (name == NULL)
		return XTC_E_INVAL;
#if defined(__linux__)
	{
		char buf[16];   /* glibc truncates at 15 + NUL. */
		strncpy(buf, name, sizeof buf - 1);
		buf[sizeof buf - 1] = '\0';
		(void)pthread_setname_np(pthread_self(), buf);
	}
#elif defined(__APPLE__)
	(void)pthread_setname_np(name);
#else
	/* No-op on platforms without a portable name API. */
	(void)name;
#endif
	return XTC_OK;
}

/*
 * Apply the default QoS class for a runtime reactor/worker thread to
 * the CALLING thread.  On macOS this is QOS_CLASS_USER_INITIATED:
 * latency-sensitive work that the user is actively waiting on, which
 * tells the Apple Silicon scheduler to prefer the performance (P)
 * cores and avoid demoting these threads onto the efficiency (E)
 * cores.  macOS does not permit hard CPU affinity, so a QoS hint is
 * the supported, idiomatic mechanism for P-core bias.  A no-op on
 * every other platform (where thread placement is governed by the
 * scheduler or explicit affinity elsewhere).
 *
 * PUBLIC: void __os_thread_apply_default_qos __P((void));
 */
void
__os_thread_apply_default_qos(void)
{
#if defined(__APPLE__)
	(void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
}

/*
 * Apply a CPU-affinity hint for the CALLING thread to logical CPU
 * `cpu` (>= 0) -- the "pin this reactor to a core" lever for a
 * thread-per-core runtime.
 *
 * Platform reality differs sharply, so this is a *hint*:
 *   - Linux: a hard pin via pthread_setaffinity_np to the single CPU.
 *   - macOS: there is NO hard CPU pinning.  The supported mechanism is
 *     the Mach THREAD_AFFINITY_POLICY "affinity tag": threads sharing a
 *     tag are hinted to run on a common L2 cache.  It is advisory, the
 *     scheduler may ignore it, and Apple Silicon ignores it outright --
 *     there the QoS class (__os_thread_apply_default_qos) is the only
 *     effective placement lever.  The tag is still set so Intel Macs
 *     (and any future arm64 scheduler that honours it) benefit; the
 *     call is reported XTC_OK even where the kernel makes it a no-op.
 *   - Other platforms: XTC_E_NOSYS until ported.
 *
 * PUBLIC: int __os_thread_set_affinity __P((int));
 */
int
__os_thread_set_affinity(int cpu)
{
	if (cpu < 0)
		return XTC_E_INVAL;
#if defined(__linux__)
	{
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET((unsigned)cpu, &set);
		if (pthread_setaffinity_np(pthread_self(), sizeof set, &set)
		    != 0)
			return XTC_E_INTERNAL;
		return XTC_OK;
	}
#elif defined(__FreeBSD__) || defined(__DragonFly__)
	{
		/* FreeBSD/DragonFly: pthread_setaffinity_np takes a cpuset_t
		 * (from <sys/cpuset.h>), the BSD analogue of Linux's
		 * cpu_set_t.  CPU_ZERO/CPU_SET operate on it the same way. */
		cpuset_t set;
		CPU_ZERO(&set);
		CPU_SET((unsigned)cpu, &set);
		if (pthread_setaffinity_np(pthread_self(), sizeof set, &set)
		    != 0)
			return XTC_E_INTERNAL;
		return XTC_OK;
	}
#elif defined(__sun) || defined(__illumos__)
	{
		/* illumos/Solaris: bind the calling LWP to a processor with
		 * processor_bind (there is no pthread affinity API).  P_MYID
		 * with P_LWPID is the running LWP. */
		if (processor_bind(P_LWPID, P_MYID, (processorid_t)cpu, NULL)
		    != 0)
			return XTC_E_INTERNAL;
		return XTC_OK;
	}
#elif defined(__APPLE__)
	{
		thread_affinity_policy_data_t pol;
		mach_port_t mt = pthread_mach_thread_np(pthread_self());
		/* Tag 0 means "no affinity"; map cpu N -> tag N+1 so distinct
		 * CPUs receive distinct, non-zero tags. */
		pol.affinity_tag = cpu + 1;
		(void)thread_policy_set(mt, THREAD_AFFINITY_POLICY,
		    (thread_policy_t)&pol, THREAD_AFFINITY_POLICY_COUNT);
		return XTC_OK;   /* advisory; ignored on Apple Silicon */
	}
#else
	(void)cpu;
	return XTC_E_NOSYS;
#endif
}

/*
 * Run fn exactly once across all threads racing on *once.  pthread_once
 * is the POSIX mechanism (InitOnceExecuteOnce on the coming Win32
 * backing, via the same __os_once_t shape).  Replaces hand-rolled
 * set-once guards with one race-free primitive.
 *
 * PUBLIC: int __os_call_once __P((__os_once_t *, void (*)(void)));
 */
int
__os_call_once(__os_once_t *once, void (*fn)(void))
{
	if (once == NULL || fn == NULL)
		return XTC_E_INVAL;
	return pthread_once(once, fn) == 0 ? XTC_OK : XTC_E_INTERNAL;
}

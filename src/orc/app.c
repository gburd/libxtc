/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/app.c
 *	The application container: glue between loop + supervisor +
 *	registry.  Thin by design.
 */

#include "xtc_int.h"
#include "xtc_app.h"
#include "xtc_exec.h"
#include "xtc_inspect.h"   /* xtc_proc_info: has a child left the proc table? */
#include "xtc_io.h"        /* XTC_IO_READABLE */
#include "xtc_proc.h"
#include "os_tuning.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

/*
 * One static child's drain bookkeeping.  xtc_app_start wraps each child
 * spec's fn in __kid_trampoline so the app knows every child's CURRENT
 * pid (restarts included) and whether its body returned.  Slots live in
 * a separate allocation from the app so a survivor on a borrowed loop
 * that runs after xtc_app_destroy touches leaked memory, not freed.
 */
struct app_kid {
	xtc_proc_fn          fn;
	void                *arg;
	xtc_restart_policy_t policy;
	_Atomic uint64_t     pid;       /* packed xtc_pid_t, 0 = not yet run */
	_Atomic int          returned;  /* body returned during the drain */
	struct app_kids     *kids;
};
struct app_kids {
	_Atomic int          draining;  /* shutdown message sent */
	int                  n;
	struct app_kid       k[];
};

struct xtc_app {
	char              *name;
	xtc_loop_t        *loop;       /* loop 0 (the supervisor's loop) */
	xtc_exec_t        *exec;       /* non-NULL for a multi-loop app */
	int                owns_loop;
	xtc_reg_t         *reg;
	xtc_supervisor_t  *root;
	xtc_sup_opts_t     sup_opts;
	int                no_tuning_check;   /* PLAN.md 19.21 */
	struct app_kids   *kids;              /* static children (19.27.10) */
	xtc_child_spec_t  *specs;             /* wrapped copies handed to sup */
	_Atomic int        shutting_down;     /* xtc_app_shutdown entered */
	_Atomic int        running;           /* inside xtc_app_run */
	int                leak_kids;         /* a survivor may still run */
	/* xtc_app_drain_on_signal parameters (the watcher reads them). */
	int64_t            sig_drain_ns, sig_force_ns;
	xtc_app_drain_report_t *sig_report;
	int                sig_armed;
	__os_thread_t      sig_thr;           /* the signal-driven drain */
	_Atomic int        sig_thr_started;
};

int
xtc_app_create(const xtc_app_opts_t *opts, xtc_app_t **out)
{
	xtc_app_t *a;
	xtc_app_opts_t defaults = XTC_APP_OPTS_DEFAULT;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if (opts == NULL) opts = &defaults;
	if ((rc = __os_calloc(1, sizeof *a, (void **)&a)) != XTC_OK) return rc;

	if (opts->name != NULL)
		(void)__os_strdup(opts->name, &a->name);

	if (opts->loop != NULL) {
		a->loop = opts->loop;
		a->owns_loop = 0;
	} else if (opts->n_loops > 1) {
		/* Multi-loop app: own an executor; the root supervisor runs on
		 * loop 0 and (via sup_opts.exec) places children across loops
		 * and stops the executor when it exits. */
		if ((rc = xtc_exec_init(&a->exec, opts->n_loops)) != XTC_OK)
			goto fail;
		/* A supervised app is a long-running service: run until the
		 * supervisor stops the executor, never idle-auto-stop. */
		xtc_exec_set_service_mode(a->exec, 1);
		a->loop = xtc_exec_loop(a->exec, 0);
		a->owns_loop = 0;   /* the executor owns its loops */
	} else {
		if ((rc = xtc_loop_init(&a->loop)) != XTC_OK) goto fail;
		a->owns_loop = 1;
	}

	if ((rc = xtc_reg_create(&a->reg)) != XTC_OK) goto fail;

	a->sup_opts = opts->sup;
	a->sup_opts.exec = a->exec;   /* NULL for single-loop; drives stop */
	a->no_tuning_check = opts->no_tuning_check;
	*out = a;
	return XTC_OK;

fail:
	if (a->reg) xtc_reg_destroy(a->reg);
	if (a->exec) (void)xtc_exec_fini(a->exec);
	if (a->loop && a->owns_loop) (void)xtc_loop_fini(a->loop);
	if (a->name) __os_free(a->name);
	__os_free(a);
	return rc;
}

/* ---- graceful drain (PLAN.md 19.20 / 19.27.10) ---- */

static uint64_t
__pid_pack(xtc_pid_t p)
{
	return ((uint64_t)p.loop_id << 48) | ((uint64_t)p.local_id << 32) |
	    (uint64_t)p.gen;
}

static xtc_pid_t
__pid_unpack(uint64_t v)
{
	xtc_pid_t p;
	p.loop_id  = (uint16_t)(v >> 48);
	p.local_id = (uint16_t)(v >> 32);
	p.gen      = (uint32_t)v;
	return p;
}

/* Cleanup-complete: the pid has LEFT the proc table.  Not `!alive` and
 * not XTC_KILL_DELIVERED -- both are reported before the at-exit hooks
 * and scope finalizers have run (see xtc_exit_pid_deadline).  A child
 * that has not run yet (pid 0) is not gone. */
static int
__kid_gone(struct app_kid *k)
{
	xtc_proc_info_t info;
	uint64_t v = atomic_load(&k->pid);
	return v != 0 && xtc_proc_info(__pid_unpack(v), &info) == XTC_E_NOTFOUND;
}

/*
 * Every static child runs through here.  Records the current pid; a
 * replacement spawned after the drain began skips its body.  When the
 * body RETURNS during a drain, a PERMANENT child parks instead of
 * exiting: an exit would make the still-running supervisor restart it
 * (burning restart intensity, possibly giving up and killing the slower
 * drainers early).  It parks in xtc_recv so the force phase's
 * xtc_exit_pid wakes it at once; the parked shell holds nothing.
 * TRANSIENT/TEMPORARY children simply exit (reason 0 is not restarted).
 */
static void
__kid_trampoline(void *arg)
{
	struct app_kid *k = arg;
	void *m;
	size_t sz;

	/* seq_cst pid-store / draining-load pairs with the shutdown's
	 * draining-store / pid-load: either we see draining, or it sees
	 * our pid and messages us. */
	atomic_store(&k->pid, __pid_pack(xtc_self()));
	if (!atomic_load(&k->kids->draining))
		k->fn(k->arg);
	if (!atomic_load(&k->kids->draining))
		return;                        /* ordinary exit */
	atomic_store(&k->returned, 1);
	if (k->policy != XTC_RESTART_PERMANENT)
		return;
	for (;;) {
		if (xtc_recv(&m, &sz, -1) == XTC_OK)
			__os_free(m);
		else
			(void)xtc_proc_sleep(10LL * 1000 * 1000);
	}
}

/* Stop what xtc_app_run is running. */
static void
__app_halt(xtc_app_t *a)
{
	if (a->exec != NULL)
		(void)xtc_exec_stop(a->exec);
	else
		(void)xtc_loop_stop(a->loop);
}

int
xtc_app_start(xtc_app_t *a, const xtc_child_spec_t *children, int n)
{
	int i, rc;
	if (a == NULL || n < 0 || (n > 0 && children == NULL))
		return XTC_E_INVAL;
	if (a->root != NULL) return XTC_E_INVAL;     /* already started */
	if (!a->no_tuning_check)
		__os_tuning_check();   /* PLAN.md 19.21: advisory NOTICE(s) */
	if ((rc = __os_calloc(1, sizeof *a->kids +
	    (size_t)n * sizeof a->kids->k[0], (void **)&a->kids)) != XTC_OK)
		return rc;
	a->kids->n = n;
	if (n > 0 && (rc = __os_calloc((size_t)n, sizeof *a->specs,
	    (void **)&a->specs)) != XTC_OK)
		goto fail;
	for (i = 0; i < n; i++) {
		a->kids->k[i].fn = children[i].fn;
		a->kids->k[i].arg = children[i].arg;
		a->kids->k[i].policy = children[i].policy;
		a->kids->k[i].kids = a->kids;
		a->specs[i] = children[i];
		a->specs[i].fn = __kid_trampoline;
		a->specs[i].arg = &a->kids->k[i];
	}
	if ((rc = xtc_sup_start(a->loop, &a->sup_opts, a->specs, n,
	    &a->root)) == XTC_OK)
		return XTC_OK;
fail:
	if (a->specs) __os_free(a->specs);
	__os_free(a->kids);
	a->specs = NULL;
	a->kids = NULL;
	return rc;
}

int
xtc_app_run(xtc_app_t *a)
{
	int rc;
	if (a == NULL || a->loop == NULL) return XTC_E_INVAL;
	/* A multi-loop app runs every loop until the supervisor exits and
	 * stops the executor; a single-loop app runs its one loop until the
	 * supervisor exits and the loop drains. */
	atomic_store_explicit(&a->running, 1, memory_order_release);
	rc = a->exec != NULL ? xtc_exec_run(a->exec) : xtc_loop_run(a->loop);
	atomic_store_explicit(&a->running, 0, memory_order_release);
#if !defined(_WIN32)
	/* A signal-triggered drain runs on its own thread (so an executor
	 * stop cannot freeze it); its report is complete once joined. */
	if (atomic_load_explicit(&a->sig_thr_started, memory_order_acquire)) {
		(void)__os_thread_join(&a->sig_thr, NULL);
		atomic_store_explicit(&a->sig_thr_started, 0,
		    memory_order_relaxed);
	}
#endif
	return rc;
}

int
xtc_app_stop(xtc_app_t *a)
{
	if (a == NULL) return XTC_E_INVAL;
	if (a->root == NULL) return XTC_OK;
	return xtc_sup_stop(a->root);
}

int
xtc_app_is_shutdown_msg(const void *msg, size_t len)
{
	return msg != NULL && len == sizeof XTC_APP_SHUTDOWN_MSG &&
	    memcmp(msg, XTC_APP_SHUTDOWN_MSG, sizeof XTC_APP_SHUTDOWN_MSG) == 0;
}

/* Poll until every child is done or the deadline passes: drain phase
 * (force == 0) done = returned or gone; force phase done = gone, and the
 * supervisor has exited too (so xtc_app_destroy's join does not wait).
 * Yields on a proc, sleeps off one; 1 ms granularity. */
static void
__wait_kids(xtc_app_t *a, int64_t deadline, int force)
{
	struct app_kids *ks = a->kids;
	int i, pending;
	int64_t now;
	uint64_t v;
	for (;;) {
		pending = (force && xtc_sup_alive(a->root)) ? 1 : 0;
		for (i = 0; i < ks->n; i++) {
			struct app_kid *k = &ks->k[i];
			if (__kid_gone(k))
				continue;
			if (!force && atomic_load(&k->returned))
				continue;
			pending++;
			/* Re-kill the CURRENT incarnation each pass: a restart
			 * racing the supervisor stop may have replaced it
			 * (xtc_exit_pid is idempotent per incarnation). */
			if (force && (v = atomic_load(&k->pid)) != 0)
				(void)xtc_exit_pid(__pid_unpack(v),
				    XTC_APP_SHUTDOWN_REASON);
		}
		(void)__os_clock_mono(&now);
		if (pending == 0 || now >= deadline)
			return;
		if (!xtc_pid_is_none(xtc_self()))
			(void)xtc_proc_sleep(1LL * 1000 * 1000);
		else
			(void)__os_sleep_ns(1LL * 1000 * 1000);
	}
}

int
xtc_app_shutdown(xtc_app_t *a, int64_t drain_ns, int64_t force_ns,
                 xtc_app_drain_report_t *out)
{
	struct app_kids *ks;
	xtc_app_drain_report_t r;
	xtc_pid_t self = xtc_self();
	int64_t t0, now;
	int i, rc, expected = 0;
	uint64_t bit;
	unsigned char *forced;

	if (a == NULL || a->root == NULL || a->kids == NULL ||
	    drain_ns < 0 || force_ns < 0)
		return XTC_E_INVAL;
	if (out != NULL && out->size < sizeof r)
		return XTC_E_INVAL;
	/* Nothing can drain unless xtc_app_run is driving the loop.  On a
	 * multi-loop app the supervisor stops the executor when it exits,
	 * which would freeze a shutdown running on a proc: plain thread only. */
	if (!atomic_load_explicit(&a->running, memory_order_acquire) ||
	    (a->exec != NULL && !xtc_pid_is_none(self)))
		return XTC_E_INVAL;
	ks = a->kids;
	/* A supervised child cannot drain its own app: step 3 would kill
	 * it before it could report or stop the loop. */
	for (i = 0; i < ks->n; i++)
		if (!xtc_pid_is_none(self) &&
		    __pid_pack(self) == atomic_load(&ks->k[i].pid))
			return XTC_E_INVAL;
	if (!atomic_compare_exchange_strong_explicit(&a->shutting_down,
	    &expected, 1, memory_order_acq_rel, memory_order_acquire))
		return XTC_E_INVAL;              /* one shutdown per app */

	(void)__os_clock_mono(&t0);
	memset(&r, 0, sizeof r);
	r.size = sizeof r;
	r.n_children = ks->n;

	/* 1-2. Cooperative phase: ask, then wait for return or exit. */
	atomic_store(&ks->draining, 1);
	for (i = 0; i < ks->n; i++) {
		uint64_t v = atomic_load(&ks->k[i].pid);
		if (v != 0)
			(void)xtc_send(__pid_unpack(v), XTC_APP_SHUTDOWN_MSG,
			    sizeof XTC_APP_SHUTDOWN_MSG);
	}
	__wait_kids(a, t0 + drain_ns, 0);
	if ((rc = __os_calloc((size_t)ks->n + 1, 1, (void **)&forced)) != XTC_OK)
		forced = NULL;          /* report degrades to counts-by-mask */
	for (i = 0; i < ks->n; i++)
		if (!atomic_load(&ks->k[i].returned) && !__kid_gone(&ks->k[i])) {
			if (forced != NULL) forced[i] = 1;
			if (i < 64) r.forced_mask |= 1ULL << i;
		}

	/* 3-4. Stop restarts, force-cancel, wait for cleanup-complete. */
	(void)xtc_sup_stop(a->root);
	(void)__os_clock_mono(&now);
	__wait_kids(a, now + force_ns, 1);

	/* Exclusive classes; children past bit 63 are counted, not masked. */
	for (i = 0; i < ks->n; i++) {
		bit = (i < 64) ? (1ULL << i) : 0;
		if (!__kid_gone(&ks->k[i])) {
			r.n_survivors++;
			r.survivor_mask |= bit;
			r.forced_mask &= ~bit;
		} else if (forced != NULL ? forced[i] : (r.forced_mask & bit) != 0) {
			r.n_forced++;
		} else {
			r.n_drained++;
		}
	}
	if (forced != NULL) __os_free(forced);
	/* A survivor on a BORROWED loop can still run (and read its slot)
	 * after destroy; an owned loop/exec is freed with it. */
	if (r.n_survivors > 0 && !a->owns_loop && a->exec == NULL)
		a->leak_kids = 1;

	/* 5. Let xtc_app_run return. */
	(void)__os_clock_mono(&now);
	r.elapsed_ns = now - t0;
	if (out != NULL)
		memcpy((char *)out + sizeof out->size, (char *)&r + sizeof r.size,
		    sizeof r - sizeof r.size);
	/* A survivor keeps the loop busy: stop it so xtc_app_run returns.
	 * With none, the loop/executor ends on its own -- and a borrowed loop
	 * is not left with a stale, sticky stop request. */
	if (r.n_survivors > 0)
		__app_halt(a);
	return r.n_survivors == 0 ? XTC_OK : XTC_E_AGAIN;
}

/* ---- opt-in SIGTERM/SIGINT trigger ---- */
#if !defined(_WIN32)
/* Process-wide self-pipe, created once and never closed, so a handler
 * that fires late can never write into a closed or reused fd. */
static int              g_sig_pipe[2] = { -1, -1 };
static xtc_app_t       *g_sig_app;
static struct sigaction g_sig_old_term, g_sig_old_int;
static pthread_mutex_t  g_sig_lock = PTHREAD_MUTEX_INITIALIZER;

/* Async-signal-safe: one write(2), errno preserved, nothing else. */
static void
__drain_sig_handler(int sig)
{
	int saved = errno;
	char b = 1;
	(void)sig;
	/* A full pipe (EAGAIN) already holds a pending request. */
	if (write(g_sig_pipe[1], &b, 1) < 0) { /* XTC_BLOCKING_OK: O_NONBLOCK */
	}
	errno = saved;
}

/* Runs the drain off every loop; xtc_app_run joins it. */
static void *
__drain_thread(void *arg)
{
	xtc_app_t *a = arg;
	(void)xtc_app_shutdown(a, a->sig_drain_ns, a->sig_force_ns,
	    a->sig_report);
	return NULL;
}

static void
__drain_watcher(void *arg)
{
	xtc_app_t *a = arg;
	uint32_t rev;
	char buf[16];
	void *m;
	size_t sz;
	for (;;) {
		if (atomic_load_explicit(&a->shutting_down, memory_order_acquire) ||
		    !xtc_sup_alive(a->root))
			return;                      /* app is ending some other way */
		rev = 0;
		if (xtc_proc_wait_fd(g_sig_pipe[0], XTC_IO_READABLE,
		    50LL * 1000 * 1000, &rev) != XTC_OK && rev == 0)
			(void)xtc_proc_sleep(50LL * 1000 * 1000);   /* not parked */
		if ((rev & XTC_WAIT_MAILBOX) && xtc_recv(&m, &sz, 0) == XTC_OK)
			__os_free(m);
		if (read(g_sig_pipe[0], buf, sizeof buf) > 0) {  /* XTC_BLOCKING_OK: O_NONBLOCK */
			if (__os_thread_create(&a->sig_thr, __drain_thread,
			    a) == XTC_OK)
				atomic_store_explicit(&a->sig_thr_started, 1,
				    memory_order_release);
			return;
		}
	}
}

static int
__nonblock_cloexec(int fd)
{
	int fl = fcntl(fd, F_GETFL);   /* XTC_BLOCKING_OK: flag query */
	if (fl == -1 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1)   /* XTC_BLOCKING_OK */
		return XTC_E_INTERNAL;
	fl = fcntl(fd, F_GETFD);       /* XTC_BLOCKING_OK */
	if (fl == -1 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == -1)   /* XTC_BLOCKING_OK */
		return XTC_E_INTERNAL;
	return XTC_OK;
}
#endif

int
xtc_app_drain_on_signal(xtc_app_t *a, int64_t drain_ns, int64_t force_ns,
                        xtc_app_drain_report_t *report)
{
#if defined(_WIN32)
	(void)a; (void)drain_ns; (void)force_ns; (void)report;
	return XTC_E_NOSYS;
#else
	struct sigaction sa;
	char buf[16];
	xtc_pid_t wpid;
	int rc = XTC_OK;

	/* Owned loop/exec only: the watcher reads the app, and on a borrowed
	 * loop it could outlive xtc_app_destroy. */
	if (a == NULL || a->root == NULL || drain_ns < 0 || force_ns < 0 ||
	    (!a->owns_loop && a->exec == NULL) ||
	    (report != NULL && report->size < sizeof *report))
		return XTC_E_INVAL;
	(void)pthread_mutex_lock(&g_sig_lock);
	if (g_sig_app != NULL) { rc = XTC_E_INVAL; goto out; }
	if (g_sig_pipe[0] < 0) {
		if (pipe(g_sig_pipe) != 0) { rc = XTC_E_INTERNAL; goto out; }
		if (__nonblock_cloexec(g_sig_pipe[0]) != XTC_OK ||
		    __nonblock_cloexec(g_sig_pipe[1]) != XTC_OK) {
			(void)close(g_sig_pipe[0]);
			(void)close(g_sig_pipe[1]);
			g_sig_pipe[0] = g_sig_pipe[1] = -1;
			rc = XTC_E_INTERNAL;
			goto out;
		}
	}
	while (read(g_sig_pipe[0], buf, sizeof buf) > 0)   /* XTC_BLOCKING_OK: O_NONBLOCK; drop stale bytes */
		;
	a->sig_drain_ns = drain_ns;
	a->sig_force_ns = force_ns;
	a->sig_report = report;
	if ((rc = xtc_proc_spawn(a->loop, __drain_watcher, a, NULL,
	    &wpid)) != XTC_OK)
		goto out;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = __drain_sig_handler;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	(void)sigaction(SIGTERM, &sa, &g_sig_old_term);
	(void)sigaction(SIGINT, &sa, &g_sig_old_int);
	g_sig_app = a;
	a->sig_armed = 1;
out:
	(void)pthread_mutex_unlock(&g_sig_lock);
	return rc;
#endif
}

void
xtc_app_destroy(xtc_app_t *a)
{
	if (a == NULL) return;
#if !defined(_WIN32)
	if (a->sig_armed) {
		(void)pthread_mutex_lock(&g_sig_lock);
		(void)sigaction(SIGTERM, &g_sig_old_term, NULL);
		(void)sigaction(SIGINT, &g_sig_old_int, NULL);
		g_sig_app = NULL;
		(void)pthread_mutex_unlock(&g_sig_lock);
	}
#endif
	/* Best-effort: if the supervisor is still alive we let the
	 * caller handle that.  Join with a generous timeout. */
	if (a->root != NULL)
		(void)xtc_sup_join(a->root, 5LL * 1000 * 1000 * 1000);
	if (a->reg)  xtc_reg_destroy(a->reg);
	if (a->exec) (void)xtc_exec_fini(a->exec);
	if (a->loop && a->owns_loop) (void)xtc_loop_fini(a->loop);
	if (a->specs) __os_free(a->specs);
	/* ponytail: a masked-forever survivor keeps a pointer into kids, so
	 * leak it in that case (bytes per child); free otherwise. */
	if (a->kids && !a->leak_kids) __os_free(a->kids);
	if (a->name) __os_free(a->name);
	__os_free(a);
}

xtc_loop_t *xtc_app_loop(const xtc_app_t *a)     { return a ? a->loop : NULL; }
xtc_exec_t *xtc_app_exec(const xtc_app_t *a)     { return a ? a->exec : NULL; }
xtc_reg_t  *xtc_app_registry(const xtc_app_t *a) { return a ? a->reg  : NULL; }

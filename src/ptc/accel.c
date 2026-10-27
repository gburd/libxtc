/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/ptc/accel.c
 *	Attached-compute (GPU / NPU) fiber-parking bridge -- see
 *	src/inc/xtc_accel.h for the scope statement.  This file is
 *	ALWAYS compiled; the device-discovery path self-gates on
 *	XTC_HAVE_ACCEL (defined by configure when a DRM/accel subsystem
 *	is present), returning "no devices" / XTC_E_NOSYS otherwise, per
 *	the module's "always linkable, NOSYS when unsupported"
 *	convention.
 *
 *	The two wait paths are thin, honest delegations to primitives
 *	that already exist:
 *	  - xtc_accel_wait_fence -> xtc_proc_wait_fd (a fence sync_file
 *	    fd becomes readable when the dma-fence signals; this is the
 *	    same loop-readiness park xtc_io/net use, no OS thread held;
 *	    mail in the caller's mailbox is left alone, see
 *	    __xtc_accel_fence_wait);
 *	  - xtc_accel_run_blocking -> xtc_blocking_run (the pool
 *	    fallback, one thread parked for the call).
 *	No vendor runtime is linked; the consumer submits the work and
 *	produces the fence fd (or the blocking closure).
 */

#include "xtc_int.h"
#include "xtc_accel.h"
#include "xtc_io.h"        /* XTC_IO_READABLE etc. */
#include "xtc_proc.h"      /* xtc_proc_wait_fd */
#include "xtc_blocking.h"  /* xtc_blocking_run */
#include "os_time.h"       /* __os_clock_mono (sim-virtualized) */

/* Internal; declared here and at its one caller (src/ptc/batch.c). */
int __xtc_accel_fence_wait(int, int64_t, int (*)(void *), void *);

#include <string.h>

#if defined(XTC_HAVE_ACCEL)
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>

/*
 * Read the kernel driver name backing a DRM/accel device into
 * drv[0..drvsz) from sysfs (.../device/driver is a symlink whose
 * basename is the driver, e.g. "xe", "intel_vpu", "amdgpu").  Best
 * effort: leaves drv as "" on any failure.  `cls` is "drm" or "accel",
 * `dev` the node name (e.g. "renderD128", "accel0").
 */
static void
read_driver(const char *cls, const char *dev, char *drv, size_t drvsz)
{
	char link[128];
	char target[256];
	ssize_t n;
	const char *base;

	drv[0] = '\0';
	if ((size_t)snprintf(link, sizeof link,
	    "/sys/class/%s/%s/device/driver", cls, dev) >= sizeof link)
		return;
	if ((n = readlink(link, target, sizeof target - 1)) <= 0)
		return;
	target[n] = '\0';
	base = strrchr(target, '/');
	base = (base != NULL) ? base + 1 : target;
	/* Bounded copy; precision (drvsz-1) satisfies -Wformat-truncation
	 * regardless of optimization level -- driver names are short. */
	(void)snprintf(drv, drvsz, "%.*s", (int)(drvsz - 1), base);
}

/*
 * Scan one device class directory (/dev/dri or /dev/accel) for nodes
 * whose name starts with `prefix` (e.g. "renderD", "accel"), appending
 * each as a device of kind `kind`.  Returns how many matched (the true
 * total), writing at most (max - *idx) into out starting at *idx.
 * *idx is advanced by the number actually written.
 */
static int
scan_class(const char *devdir, const char *sysclass, const char *prefix,
    xtc_accel_kind_t kind, xtc_accel_dev_t *out, int max, int *idx)
{
	DIR *d;
	struct dirent *e;
	int found = 0;
	size_t plen = strlen(prefix);

	if ((d = opendir(devdir)) == NULL)
		return 0;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, prefix, plen) != 0)
			continue;
		/* "renderD" must be followed by a digit; "accel" likewise --
		 * skip control nodes / dotfiles. */
		if (e->d_name[plen] < '0' || e->d_name[plen] > '9')
			continue;
		found++;
		if (out != NULL && *idx < max) {
			xtc_accel_dev_t *dv = &out[*idx];
			memset(dv, 0, sizeof *dv);
			dv->kind = kind;
			/* snprintf already truncates safely; the explicit
			 * precision (buffer size minus the NUL) tells the
			 * compiler the output is bounded so -Wformat-truncation
			 * is satisfied.  Device node names are short in practice
			 * ("renderD128", "accel0"); the bound is defensive. */
			(void)snprintf(dv->name, sizeof dv->name, "%.31s",
			    e->d_name);
			(void)snprintf(dv->node, sizeof dv->node, "%s/%.51s",
			    devdir, e->d_name);
			read_driver(sysclass, e->d_name, dv->driver,
			    sizeof dv->driver);
			(*idx)++;
		}
	}
	(void)closedir(d);
	return found;
}
#endif /* XTC_HAVE_ACCEL */

int
xtc_accel_probe(xtc_accel_dev_t *out, int max, int *out_n)
{
	if (out_n == NULL || (out == NULL && max != 0) || max < 0)
		return XTC_E_INVAL;
#if defined(XTC_HAVE_ACCEL)
	{
		int idx = 0, total = 0;
		/* GPUs: render nodes only (never the privileged cardN /
		 * control node -- render nodes are the unprivileged compute
		 * submission path). */
		total += scan_class("/dev/dri", "drm", "renderD",
		    XTC_ACCEL_KIND_GPU, out, max, &idx);
		/* NPUs: the accel subsystem. */
		total += scan_class("/dev/accel", "accel", "accel",
		    XTC_ACCEL_KIND_NPU, out, max, &idx);
		*out_n = total;
	}
#else
	*out_n = 0;   /* no accelerator support built in: nothing to report */
#endif
	return XTC_OK;
}

#if defined(XTC_HAVE_ACCEL)
/* Mail-blind back-off while the caller's mailbox is non-empty (see
 * __xtc_accel_fence_wait). */
#define FENCE_NAP_MIN_NS (50LL * 1000)
#define FENCE_NAP_MAX_NS (1000LL * 1000)

/* Is a message waiting in the calling proc's mailbox (or save queue)?
 * xtc_proc_wait_fd returns at once, without parking, while one is. */
static int
fence_mail_pending(void)
{
	xtc_mailbox_stats_t st = XTC_MAILBOX_STATS_INIT;
	if (xtc_proc_mailbox_stats(xtc_self(), &st) != XTC_OK)
		return 0;
	return st.depth > 0 || st.saved > 0;
}

/* Zero-timeout readiness probe of the fence fd. XTC_OK = signalled (or
 * ERR/HUP, the same verdict xtc_proc_wait_fd gives), XTC_E_AGAIN = not
 * yet, XTC_E_INVAL = not an open fd. */
static int
fence_probe(int fd)
{
	struct pollfd p;
	int n;
	p.fd = fd;
	p.events = POLLIN;
	p.revents = 0;
	do {
		n = poll(&p, 1, 0); /* XTC_BLOCKING_OK: zero timeout */
	} while (n < 0 && errno == EINTR);
	if (n < 0 || (p.revents & POLLNVAL))
		return XTC_E_INVAL;
	return n > 0 ? XTC_OK : XTC_E_AGAIN;
}
#endif

/*
 * Internal (batch.c): wait for fence_fd until the absolute
 * __os_clock_mono deadline deadline_ns (< 0: none).  After every wake,
 * stop(arg) (if non-NULL) is consulted; true returns XTC_E_ABORTED, so a
 * caller can end the wait by making stop true and waking the fiber with
 * xtc_proc_wake.  Any other wake -- a stray xtc_proc_wake, mail -- re-waits
 * for the time left, never for less.
 *
 * Mail: xtc_proc_wait_fd never parks while the caller's mailbox is
 * non-empty, and this wait must not consume the caller's messages.  While
 * mail is pending the fiber therefore probes the fd and sleeps, backing
 * off from 50 us to 1 ms per probe: at most 1 ms extra fence latency and
 * about a thousand wakeups a second, instead of the busy loop that starved
 * the whole loop before.  Once the mailbox is empty again it parks on the
 * fd as usual.  The clock is __os_clock_mono, which the simulator
 * virtualizes.
 */
int
__xtc_accel_fence_wait(int fence_fd, int64_t deadline_ns,
    int (*stop)(void *), void *arg)
{
#if defined(XTC_HAVE_ACCEL)
	const uint32_t interest = XTC_IO_READABLE | XTC_IO_ERR | XTC_IO_HUP;
	int64_t now = 0, remaining, nap = FENCE_NAP_MIN_NS;
	uint32_t revents;
	int rc;

	if (fence_fd < 0)
		return XTC_E_INVAL;
	for (;;) {
		if (stop != NULL && stop(arg))
			return XTC_E_ABORTED;
		remaining = -1;
		if (deadline_ns >= 0) {
			(void)__os_clock_mono(&now);
			remaining = deadline_ns > now ? deadline_ns - now : 0;
		}
		if (fence_mail_pending()) {
			if ((rc = fence_probe(fence_fd)) != XTC_E_AGAIN)
				return rc;
			if (remaining == 0)
				return XTC_E_AGAIN;
			if (remaining > 0 && nap > remaining)
				nap = remaining;
			if (xtc_proc_sleep(nap) != XTC_OK)
				return XTC_E_INVAL;          /* not on a proc */
			if (nap < FENCE_NAP_MAX_NS)
				nap *= 2;
			continue;
		}
		nap = FENCE_NAP_MIN_NS;
		revents = 0;
		rc = xtc_proc_wait_fd(fence_fd, interest, remaining, &revents);
		if (rc != XTC_OK)
			return rc;          /* AGAIN = deadline, INVAL = bad fd */
		if (revents & interest)
			return XTC_OK;      /* the fence signalled */
		/* Mail, a stray wake, or a stop request: re-evaluate. */
	}
#else
	(void)fence_fd;
	(void)deadline_ns;
	(void)stop;
	(void)arg;
	return XTC_E_NOSYS;
#endif
}

int
xtc_accel_wait_fence(int fence_fd, int64_t timeout_ns)
{
#if defined(XTC_HAVE_ACCEL)
	int64_t deadline = -1;

	/*
	 * A dma-fence exported as a sync_file becomes READABLE when it
	 * signals -- the identical readiness event the loop already
	 * multiplexes for sockets/pipes.  Park the fiber on it; no OS
	 * thread is held.  A deadline (not a relative budget) keeps the
	 * timeout exact across mail and spurious wakes.
	 */
	if (timeout_ns >= 0) {
		(void)__os_clock_mono(&deadline);
		deadline = timeout_ns > INT64_MAX - deadline ? INT64_MAX :
		    deadline + timeout_ns;
	}
	return __xtc_accel_fence_wait(fence_fd, deadline, NULL, NULL);
#else
	(void)fence_fd;
	(void)timeout_ns;
	return XTC_E_NOSYS;
#endif
}

int
xtc_accel_run_blocking(int (*fn)(void *), void *arg, int *out_result)
{
	if (fn == NULL)
		return XTC_E_INVAL;
	/*
	 * No XTC_HAVE_ACCEL gate: this needs no device support.  fn is the
	 * consumer's own synchronous accelerator call; the pool just runs
	 * it off the loop thread and resumes the fiber with its result.
	 */
	return xtc_blocking_run(fn, arg, out_result);
}

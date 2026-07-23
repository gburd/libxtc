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
 *	    same loop-readiness park xtc_io/net use, no OS thread held);
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

#include <string.h>

#if defined(XTC_HAVE_ACCEL)
#include <stdio.h>
#include <dirent.h>
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

int
xtc_accel_wait_fence(int fence_fd, int64_t timeout_ns)
{
#if defined(XTC_HAVE_ACCEL)
	uint32_t revents = 0;
	int rc;
	int64_t remaining = timeout_ns;

	if (fence_fd < 0)
		return XTC_E_INVAL;
	/*
	 * A dma-fence exported as a sync_file becomes READABLE when it
	 * signals -- the identical readiness event the loop already
	 * multiplexes for sockets/pipes.  Park the fiber on it; no OS
	 * thread is held.
	 *
	 * xtc_proc_wait_fd can also wake for a mailbox message (a message
	 * sent to this fiber while it waits on the fence).  That is NOT
	 * fence completion, so on a mailbox-only wakeup we re-wait for the
	 * remaining time rather than falsely report the accelerator done.
	 * (timeout_ns < 0 means wait forever, so no deadline math there.)
	 */
	for (;;) {
		rc = xtc_proc_wait_fd(fence_fd,
		    XTC_IO_READABLE | XTC_IO_ERR | XTC_IO_HUP, remaining,
		    &revents);
		if (rc == XTC_E_AGAIN)
			return XTC_E_AGAIN;          /* timeout, fence never fired */
		if (rc != XTC_OK)
			return rc;                   /* XTC_E_INVAL etc. */
		if (revents &
		    (XTC_IO_READABLE | XTC_IO_ERR | XTC_IO_HUP))
			return XTC_OK;               /* the fence signalled */
		/* Mailbox-only wakeup: re-wait.  With a finite timeout we do
		 * not extend it beyond the original budget -- pass 0 so a
		 * subsequent immediate return is a timeout, keeping the total
		 * bounded.  (A precise remaining-time recompute would need a
		 * clock read; the simple bound is sufficient and avoids adding
		 * a sim-nondeterministic clock call to this path.) */
		if (timeout_ns >= 0)
			remaining = 0;
	}
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

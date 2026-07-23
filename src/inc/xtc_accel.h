/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_accel.h
 *	Attached-compute (GPU / NPU) access -- the FIBER-PARKING BRIDGE
 *	over an async accelerator completion.
 *
 *	SCOPE, stated up front because it is deliberately narrow.  An
 *	accelerator (a discrete/integrated GPU, or an NPU / VPU) is, at
 *	the layer a concurrency runtime cares about, an ASYNC I/O DEVICE:
 *	you submit an opaque command buffer / compiled graph, it runs on
 *	a coprocessor, and it signals completion via a FENCE -- on Linux,
 *	a pollable sync_file / dma-fence fd, exactly the object epoll
 *	already waits on.  That is the ONLY thing this module does: it
 *	parks a fiber on such a completion and wakes it when the device
 *	is done, deterministically-testable under the sim backend.
 *
 *	WHAT THIS MODULE IS NOT, and never becomes:
 *	  - NOT a tensor / compute library.  It marshals no weights, runs
 *	    no kernels, owns no model format.
 *	  - NOT a device-memory allocator.  Host<->device transfer and
 *	    buffer lifetime are the consumer's (or the vendor runtime's).
 *	  - NOT a vendor-SDK portability shim.  libxtc does NOT link
 *	    Level Zero / CUDA / Vulkan / OpenVINO / ONNX.  The CONSUMER
 *	    links whichever runtime drives the device, does the submit,
 *	    and hands us the resulting fence fd (or a blocking closure).
 *	The abstraction stops at the fence.  Submission and math live
 *	above us; that boundary is what keeps this a concurrency
 *	primitive and not a decades-deep accelerator-runtime tarpit.
 *
 *	GPU AND NPU ARE ONE ABSTRACTION HERE.  On Linux both are DRM
 *	devices (/dev/dri/renderD* for a render GPU, /dev/accel/accel*
 *	for an NPU via the accel subsystem) whose completions are the
 *	SAME kernel object: a dma-fence exported to a sync_file fd.  A
 *	GPU fence fd and an NPU fence fd are indistinguishable to poll(2),
 *	so they are indistinguishable to us.  The device KIND
 *	(xtc_accel_kind_t) is a stats/observability tag only, never a
 *	behavioral branch.
 *
 *	TWO WAYS TO WAIT (pick per submission):
 *	  1. FENCE-FD (preferred, no OS thread held): the consumer's
 *	     runtime submits work and returns a completion fence exported
 *	     as an fd (Level Zero event/fence -> sync_file,
 *	     drmSyncobjExportSyncFile, VK_KHR_external_fence_fd, a CUDA
 *	     event bridged to an eventfd, ...).  xtc_accel_wait_fence
 *	     parks the calling fiber on that fd via the normal loop
 *	     readiness path and resumes it when the fence signals.
 *	  2. BLOCKING FALLBACK (a whole OS thread parked for the call):
 *	     when a runtime exposes only a synchronous "run and block"
 *	     call and no pollable fence, xtc_accel_run_blocking routes it
 *	     through the xtc_blocking thread pool -- correct, but holds a
 *	     pool thread for the duration, so it suits coarse-grained
 *	     inference, not thousands of tiny ops/sec.
 *
 *	AVAILABILITY.  When the build was configured without accelerator
 *	support (XTC_HAVE_ACCEL undefined -- no DRM/accel present, or
 *	--without-accel), the probe reports zero devices and the fence
 *	wait returns XTC_E_NOSYS, following the same "always linkable,
 *	NOSYS when unsupported" convention as the TLS and crypto modules.
 *	xtc_accel_run_blocking works regardless (it needs no device
 *	support -- it just runs the consumer's closure on the pool).
 */

#ifndef XTC_ACCEL_H
#define XTC_ACCEL_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

/* Device kind -- an OBSERVABILITY TAG only (see the header comment); it
 * never changes how a fence is waited on. */
typedef enum xtc_accel_kind {
	XTC_ACCEL_KIND_UNKNOWN = 0,
	XTC_ACCEL_KIND_GPU,        /* render node (/dev/dri/renderD*) */
	XTC_ACCEL_KIND_NPU         /* accel subsystem (/dev/accel/accel*) */
} xtc_accel_kind_t;

/* One discovered device.  `name` is a short human/stat label (e.g.
 * "renderD128", "accel0"); `node` is the device node path.  `driver`
 * is the kernel driver name if cheaply known (e.g. "xe", "intel_vpu"),
 * else "".  These are for selection/observability by the consumer and
 * its runtime -- libxtc itself opens none of them. */
typedef struct xtc_accel_dev {
	xtc_accel_kind_t kind;
	char             name[32];
	char             node[64];
	char             driver[32];
} xtc_accel_dev_t;

/*
 * PUBLIC: int xtc_accel_probe __P((xtc_accel_dev_t *, int, int *));
 *
 * Enumerate accelerator devices present on the host into out[0..max),
 * writing the total count found to *out_n (which may exceed max if the
 * buffer was too small -- only the first max are written).  Returns
 * XTC_OK on success (including zero devices), XTC_E_INVAL on a bad
 * argument.  When the build lacks accelerator support this reports
 * zero devices and returns XTC_OK (there is nothing to enumerate;
 * absence is not an error).  Pure discovery -- opens/submits nothing.
 */
XTC_API int xtc_accel_probe(xtc_accel_dev_t *out, int max, int *out_n);

/*
 * PUBLIC: int xtc_accel_wait_fence __P((int, int64_t));
 *
 * Park the CALLING FIBER until the completion fence `fence_fd` (a
 * pollable sync_file / dma-fence / eventfd the consumer's runtime
 * produced when it submitted work) signals, or `timeout_ns` elapses
 * (< 0 = wait forever).  On the fence signalling, resumes the fiber
 * and returns XTC_OK.  Returns XTC_E_AGAIN on timeout (the fence never
 * fired), XTC_E_INVAL on a bad fd, XTC_E_NOSYS when the build lacks
 * accelerator support.
 *
 * libxtc does NOT own, dup, or close `fence_fd` -- the caller retains
 * ownership and closes it after this returns.  This must be called
 * from within a fiber (it parks); calling it off a loop is an error.
 * It holds NO OS thread while waiting (that is the point vs
 * run_blocking).
 */
XTC_API int xtc_accel_wait_fence(int fence_fd, int64_t timeout_ns);

/*
 * PUBLIC: int xtc_accel_run_blocking __P((int (*)(void *), void *, int *));
 *
 * Run a synchronous accelerator call `fn(arg)` on the xtc_blocking
 * thread pool, parking the calling fiber until it returns; the fiber
 * resumes with fn's return value in *out_result (may be NULL).  For
 * runtimes that expose only a blocking submit-and-wait with no
 * pollable fence.  Returns XTC_OK (fn ran; see *out_result), or the
 * xtc_blocking error if the work could not be dispatched.  Holds one
 * pool thread for the duration of fn -- prefer xtc_accel_wait_fence
 * when a fence fd is available.  Works regardless of XTC_HAVE_ACCEL
 * (it needs no device support -- fn is the consumer's own closure).
 */
XTC_API int xtc_accel_run_blocking(int (*fn)(void *), void *arg,
            int *out_result);

#endif /* XTC_ACCEL_H */

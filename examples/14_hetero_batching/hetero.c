/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/14_hetero_batching/hetero.c
 *	Heterogeneous GPU + NPU concurrent offload over the async batch API.
 *
 *	Capstone of the async-batch accelerator track.  One executor, two
 *	loops; one batch group per loop; each group drives a DIFFERENT physical
 *	device, and the continuous-batching policy keeps BOTH devices fed at the
 *	same time:
 *	  group G (loop 0) -> Intel Arc GPU via OpenCL
 *	  group N (loop 1) -> Intel NPU ("AI Boost") via Level Zero
 *
 *	The workload is a pseudo-LLM decode loop.  Each batch MEMBER is a
 *	"request" -- a sequence that needs a VARIABLE number of decode-like
 *	iterations before it emits end-of-sequence (DONE).  Shallow requests
 *	finish in a couple of iterations and leave the batch; deep ones keep
 *	iterating.  New requests join the freed slots while deep ones keep
 *	going, so the device vector stays full and members leave/join mid-batch
 *	-- the vLLM continuous-batching shape, here split across two unlike
 *	accelerators running concurrently.
 *
 *	Three device backends sit behind one small vtable (struct device):
 *	  FAKE   -- pure-CPU, deterministic, the default; needs no GPU/NPU, runs
 *	            in CI and under the sim backend.  Both groups use it unless
 *	            --hetero selects real silicon.  SYNC or FUTURE exec.
 *	  OPENCL -- real GPU (XTC_WITH_OPENCL=1): submit() enqueues one NDRange
 *	            per ready member, bridges the cl_event to the engine's FUTURE
 *	            exec kind via the group's one long-lived waiter thread
 *	            (clWaitForEvents then xtc_promise_set).  collect() reads back via clEnqueueMapBuffer
 *	            (NOT clEnqueueReadBuffer -- that path crashes inside the
 *	            Lunar-Lake compute-runtime; see README).
 *	  LEVELZERO -- real NPU (XTC_WITH_LEVELZERO=1): submit() appends a small
 *	            Level Zero compute workload to the NPU command list and
 *	            executes it on the NPU command queue, signalling a ze_event;
 *	            the group's waiter thread does zeEventHostSynchronize then
 *	            xtc_promise_set -- the SAME FUTURE bridge as OpenCL.  The NPU
 *	            op is a device-memory copy chain (host->device->device->host)
 *	            per decode iteration: an honest "NPU command-queue round-trip"
 *	            that exercises the NPU queue + ze_event completion path.  It
 *	            is NOT a matmul and is labelled as such; the point proven is
 *	            that the batch engine FED the NPU and AWAITED its completion
 *	            concurrently with the GPU.
 *
 *	The same batch driver (submit/collect) feeds all backends unchanged; the
 *	--hetero mode wires group 0 -> OPENCL and group 1 -> LEVELZERO.  Without
 *	XTC_WITH_OPENCL / XTC_WITH_LEVELZERO neither real backend is built and
 *	the program has no OpenCL/Level-Zero dependency at all.
 */
#define _POSIX_C_SOURCE 200112L /* posix_memalign */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_batch.h"
#include "xtc_exec.h"
#include "xtc_future.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

#if defined(XTC_WITH_OPENCL) || defined(XTC_WITH_LEVELZERO)
#include <pthread.h>
#include <time.h>
#endif

#ifdef XTC_WITH_OPENCL
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#endif

#ifdef XTC_WITH_LEVELZERO
#include <level_zero/ze_api.h>
#endif

/* ---------------------------------------------------------------------- */
/* Request model: the batch member and request-as-carrier.                */
/* ---------------------------------------------------------------------- */

#define MAX_REQS 256
#define MAX_GROUPS 2
#define WORK_ELEMS 1024        /* per-request device scratch (uint32) */

/*
 * A request is one batch member.  It carries its own device-output location
 * (work[]) and a mock KV-slot id (slot), demonstrating the request-as-carrier
 * contract: the engine stores only the void* request; the consumer hangs all
 * per-member device state off it.  complete() firing is the cue to free the
 * slot.  "depth" is how many decode iterations this request needs before EOS;
 * "iter" counts how many have run.  Depth varies per request => members leave
 * and join mid-batch (continuous batching).
 */
struct req {
	int id;            /* stable request index */
	int slot;          /* mock KV-slot id (device scratch region) */
	int depth;         /* decode iterations needed before EOS */
	int iter;          /* iterations run so far */
	int done;          /* 1 once iter == depth */
	int group;         /* which group owns it (0=GPU, 1=NPU) */
	uint64_t handle;   /* batch handle */
	uint32_t acc;      /* accumulated check value (deterministic) */
	uint32_t work[WORK_ELEMS]; /* device output region */
	/* bookkeeping for assertions */
	int completions;   /* must end == 1 */
	int outcome;
};

/* ---------------------------------------------------------------------- */
/* Deterministic reference for one decode iteration (ground truth).       */
/* ---------------------------------------------------------------------- */

/*
 * One decode iteration advances a request's accumulator by a function of its
 * id and current iteration index.  Integer-only and identical on every
 * backend, so the fake-CPU, GPU, and NPU paths all agree on the final acc
 * for a given request -- the self-test checks this exactly on the fake path,
 * and the real backends use the same formula so a cross-check is possible.
 */
static uint32_t
decode_step(uint32_t acc, int id, int iter)
{
	uint32_t x = acc + (uint32_t)(id * 2654435761u) + (uint32_t)iter;

	x ^= x >> 15;
	x *= 2246822519u;
	x ^= x >> 13;
	return x;
}

/* Per-request depth: deterministic spread 1..MAXDEPTH keyed by id, so some
 * requests are shallow (leave early) and some deep (stay). */
#define MAXDEPTH 9
static int
req_depth(int id)
{
	return 1 + (id * 2654435761u >> 28) % MAXDEPTH;
}

/* ---------------------------------------------------------------------- */
/* Device backend vtable: one interface, three implementations.           */
/* ---------------------------------------------------------------------- */

struct group;

struct device {
	const char *name;
	int (*group_init)(struct group *g, int dev_index);
	int (*submit)(struct group *g, const xtc_batch_item_t *items, size_t n,
	    xtc_batch_exec_t *exec);
	void (*collect)(struct group *g, const xtc_batch_item_t *items,
	    xtc_batch_result_t *results, size_t n, int exec_status);
	void (*group_fini)(struct group *g);
};

/* ---------------------------------------------------------------------- */
/* Overlap tracking: prove both devices had in-flight ops at the same     */
/* instant (the headline "GPU busy AND NPU busy at the same time").        */
/* ---------------------------------------------------------------------- */

/*
 * Each group marks itself in-flight across a device op (submit..collect).
 * A shared atomic counter of concurrently-in-flight groups lets us record
 * the peak overlap and how many ops ran while the OTHER group was also busy.
 * Plain atomics on an int are enough for a two-group demo and cost nothing
 * in the fake path.
 */
#if defined(XTC_WITH_OPENCL) || defined(XTC_WITH_LEVELZERO)
#include <stdatomic.h>
static atomic_int g_inflight;      /* groups currently mid device op */
static atomic_int g_overlap_peak;  /* max concurrent in-flight groups */
#endif

/* ---------------------------------------------------------------------- */
/* Group / whole-run state.                                               */
/* ---------------------------------------------------------------------- */

struct demo;

struct group {
	struct demo *demo;
	const struct device *dev; /* per-group backend (hetero: differs!) */
	xtc_batch_t *batch;
	xtc_loop_t *loop;
	int dev_index;
	int gidx;                   /* group index 0/1 */
	unsigned submits;           /* device ops launched */
	unsigned collects;
	uint64_t in_flight_sum;     /* sum of per-op member counts */
	unsigned max_in_flight;
	unsigned reqs_done;
	unsigned overlap_ops;       /* ops that ran while the other was busy */
	unsigned n_terminal;        /* complete() calls on this group */
	xtc_notify_t *done_note;    /* complete() -> feeder: a slot freed */
#if defined(XTC_WITH_OPENCL) || defined(XTC_WITH_LEVELZERO)
	/* The group's one device-waiter thread and its one-op hand-off. */
	pthread_t waiter;
	pthread_mutex_t wmu;
	pthread_cond_t wcv;
	int (*wwait)(void *);
	void (*wrelease)(void *);
	void *wev;
	xtc_promise_t *wprom;       /* NULL = idle */
	int wquit, waiter_up;
#endif
#ifdef XTC_WITH_OPENCL
	cl_context ctx;
	cl_command_queue q;
	cl_program prog;
	cl_kernel kern;
	cl_mem dbuf;
	cl_uint *dbuf_host;         /* USE_HOST_PTR backing; coherent after */
	                           /* clFinish on this integrated UMA GPU */
	int cl_ready;
#endif
#ifdef XTC_WITH_LEVELZERO
	ze_context_handle_t zectx;
	ze_command_queue_handle_t zeq;
	ze_command_list_handle_t zelist;
	ze_event_pool_handle_t zepool;
	unsigned ze_evt_next;       /* rotating event index into zepool */
	atomic_int ze_evt_busy[64]; /* index in use until its event is destroyed */
	struct ze_ev *ze_evs;       /* per-index waiter argument (ZE_EVT_SLOTS) */
	ze_device_handle_t zedev;
	void *zdev_a, *zdev_b;      /* NPU device memory */
	void *zhost;                /* host-visible staging, one region per slot */
	int ze_ready;
#endif
};

struct demo {
	struct group groups[MAX_GROUPS];
	int ngroups;
	struct req reqs[MAX_REQS];
	int nreqs;
	int continuous;
	int hetero;                 /* 1 = GPU+NPU real backends */
	/* self-test / verification bookkeeping */
	unsigned completed, cancelled, failed;
};

static void
die(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	exit(1);
}

static void
must(int rc, const char *what)
{
	if (rc != XTC_OK) {
		fprintf(stderr, "FAIL: %s: %s "
		    "(build libxtc with --enable-sched-policy)\n",
		    what, xtc_strerror(rc));
		exit(1);
	}
}

/* ---------------------------------------------------------------------- */
/* FAKE backend: deterministic pure-CPU decode. SYNC or FUTURE.           */
/* ---------------------------------------------------------------------- */

static int fake_use_sync; /* 0 => FUTURE (default), 1 => SYNC */

static void
fake_decode(struct req *r)
{
	int k;

	r->acc = decode_step(r->acc, r->id, r->iter);
	for (k = 0; k < WORK_ELEMS; k++)
		r->work[k] = r->acc ^ (uint32_t)k;
	r->iter++;
}

static int
fake_submit(struct group *g, const xtc_batch_item_t *items, size_t n,
    xtc_batch_exec_t *exec)
{
	size_t i;

	(void)g;
	for (i = 0; i < n; i++)
		fake_decode(items[i].request);
	if (fake_use_sync) {
		exec->kind = XTC_BATCH_EXEC_SYNC;
		return XTC_OK;
	}
	exec->kind = XTC_BATCH_EXEC_FUTURE;
	{
		xtc_promise_t *prom;
		xtc_future_t *fut;
		if (xtc_future_new_pair(&prom, &fut) != XTC_OK)
			return XTC_E_NOMEM;
		exec->future = fut;
		xtc_promise_set(prom, 0, XTC_OK);
	}
	return XTC_OK;
}

static void
decide(struct group *g, struct req *r, xtc_batch_result_t *res)
{
	if (r->iter >= r->depth) {
		r->done = 1;
		g->reqs_done++;
		res->state = XTC_BATCH_DONE;
	} else
		res->state = XTC_BATCH_CONTINUE;
}

static void
fake_collect(struct group *g, const xtc_batch_item_t *items,
    xtc_batch_result_t *results, size_t n, int exec_status)
{
	size_t i;

	for (i = 0; i < n; i++) {
		struct req *r = items[i].request;
		if (exec_status != XTC_BATCH_EXEC_OK) {
			results[i].state = XTC_BATCH_ERROR;
			results[i].error = XTC_E_IO;
			continue;
		}
		decide(g, r, &results[i]);
	}
}

static int
fake_group_init(struct group *g, int dev_index)
{
	(void)g;
	(void)dev_index;
	return XTC_OK;
}

static void
fake_group_fini(struct group *g)
{
	(void)g;
}

static const struct device fake_device = {
	.name = "fake",
	.group_init = fake_group_init,
	.submit = fake_submit,
	.collect = fake_collect,
	.group_fini = fake_group_fini,
};

/* ---------------------------------------------------------------------- */
/* Overlap bookkeeping helpers (real backends only).                      */
/* ---------------------------------------------------------------------- */

#if defined(XTC_WITH_OPENCL) || defined(XTC_WITH_LEVELZERO)
static void
inflight_enter(struct group *g)
{
	int now = atomic_fetch_add(&g_inflight, 1) + 1;
	int peak = atomic_load(&g_overlap_peak);

	while (now > peak &&
	    !atomic_compare_exchange_weak(&g_overlap_peak, &peak, now))
		; /* retry */
	if (now >= 2)
		g->overlap_ops++;
}

static void
inflight_leave(void)
{
	atomic_fetch_sub(&g_inflight, 1);
}
#endif

/* ---------------------------------------------------------------------- */
/* FUTURE bridge: one long-lived waiter thread per device group.          */
/* ---------------------------------------------------------------------- */

#if defined(XTC_WITH_OPENCL) || defined(XTC_WITH_LEVELZERO)
/*
 * submit hands the group's waiter one (wait, release, promise) job and
 * returns; the waiter blocks in the device's own wait call, releases the
 * device event, and resolves the promise, which wakes the driver on its
 * loop.  A thread per op costs ~50-100 us/op (measured); one per device
 * costs ~13-19 us.  The engine keeps at most one op in flight per group, so
 * a one-entry hand-off is enough.  wait() returns 0 on success.
 */
static void *
dev_waiter(void *arg)
{
	struct group *g = arg;
	int (*wait)(void *);
	void (*release)(void *);
	void *ev;
	xtc_promise_t *prom;
	int st;

	pthread_mutex_lock(&g->wmu);
	for (;;) {
		while (g->wprom == NULL && !g->wquit)
			pthread_cond_wait(&g->wcv, &g->wmu);
		if (g->wprom == NULL)
			break;
		wait = g->wwait;
		release = g->wrelease;
		ev = g->wev;
		prom = g->wprom;
		g->wprom = NULL;
		pthread_mutex_unlock(&g->wmu);
		st = wait(ev);
		inflight_leave();
		release(ev);
		xtc_promise_set(prom, 0, st == 0 ? XTC_OK : XTC_E_IO);
		pthread_mutex_lock(&g->wmu);
	}
	pthread_mutex_unlock(&g->wmu);
	return NULL;
}

static int
dev_waiter_start(struct group *g)
{
	pthread_mutex_init(&g->wmu, NULL);
	pthread_cond_init(&g->wcv, NULL);
	if (pthread_create(&g->waiter, NULL, dev_waiter, g) != 0)
		return XTC_E_IO;
	g->waiter_up = 1;
	return XTC_OK;
}

static void
dev_waiter_stop(struct group *g)
{
	if (!g->waiter_up)
		return;
	pthread_mutex_lock(&g->wmu);
	g->wquit = 1;
	pthread_cond_signal(&g->wcv);
	pthread_mutex_unlock(&g->wmu);
	pthread_join(g->waiter, NULL);
	g->waiter_up = 0;
}

/* Hand the op's completion to the waiter and give the engine its future. */
static int
dev_waiter_post(struct group *g, int (*wait)(void *),
    void (*release)(void *), void *ev, xtc_batch_exec_t *exec)
{
	xtc_promise_t *prom;
	xtc_future_t *fut;

	if (xtc_future_new_pair(&prom, &fut) != XTC_OK)
		return XTC_E_NOMEM;
	pthread_mutex_lock(&g->wmu);
	g->wwait = wait;
	g->wrelease = release;
	g->wev = ev;
	g->wprom = prom;
	pthread_cond_signal(&g->wcv);
	pthread_mutex_unlock(&g->wmu);
	exec->kind = XTC_BATCH_EXEC_FUTURE;
	exec->future = fut;
	return XTC_OK;
}
#endif


/* ---------------------------------------------------------------------- */
/* OPENCL backend: real GPU, cl_event -> FUTURE bridge.                   */
/* ---------------------------------------------------------------------- */

#ifdef XTC_WITH_OPENCL

/*
 * Decode kernel: reproduce decode_step() for one request iteration over its
 * scratch region.  Integer-only so the GPU result equals the CPU reference
 * exactly (no float rounding, unlike Mandelbrot).  args: out buffer, base
 * offset, request id, iteration index, incoming acc; writes work[] and leaves
 * the new acc in out[base] (we read it back in collect).
 */
static const char *CL_SRC =
"inline uint dstep(uint acc, int id, int it){\n"
"  uint x = acc + (uint)(id*2654435761u) + (uint)it;\n"
"  x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; return x; }\n"
"__kernel void decode(__global uint *out, const uint base, const int id,\n"
"    const int it, const uint acc_in, const uint n){\n"
"  uint lid = get_global_id(0); if (lid >= n) return;\n"
"  uint acc = dstep(acc_in, id, it);\n"
"  out[base + lid] = acc ^ lid;\n"
"  if (lid == 0) out[base + n] = acc; /* stash new acc past the region */ }\n";

static cl_device_id cl_devs[8];
static cl_uint cl_ndev;
static cl_platform_id cl_plat;

static int
cl_discover(void)
{
	cl_int err;

	if (cl_ndev > 0)
		return XTC_OK;
	err = clGetPlatformIDs(1, &cl_plat, NULL);
	if (err != CL_SUCCESS)
		return XTC_E_IO;
	err = clGetDeviceIDs(cl_plat, CL_DEVICE_TYPE_ALL, 8, cl_devs, &cl_ndev);
	if (err != CL_SUCCESS || cl_ndev == 0)
		return XTC_E_IO;
	return XTC_OK;
}

static int
cl_group_init(struct group *g, int dev_index)
{
	cl_int err;
	char nm[128] = "";
	size_t cap = (size_t)(WORK_ELEMS + 1) * (size_t)g->demo->nreqs;

	if (cl_discover() != XTC_OK)
		return XTC_E_IO;
	if ((cl_uint)dev_index >= cl_ndev)
		return XTC_E_RANGE;
	g->dev_index = dev_index;
	clGetDeviceInfo(cl_devs[dev_index], CL_DEVICE_NAME, sizeof nm, nm, NULL);
	g->ctx = clCreateContext(NULL, 1, &cl_devs[dev_index], NULL, NULL, &err);
	if (err != CL_SUCCESS)
		return XTC_E_IO;
	g->q = clCreateCommandQueue(g->ctx, cl_devs[dev_index], 0, &err);
	if (err != CL_SUCCESS)
		return XTC_E_IO;
	g->prog = clCreateProgramWithSource(g->ctx, 1, &CL_SRC, NULL, &err);
	if (err != CL_SUCCESS)
		return XTC_E_IO;
	err = clBuildProgram(g->prog, 1, &cl_devs[dev_index], NULL, NULL, NULL);
	if (err != CL_SUCCESS) {
		char log[4096] = "";
		clGetProgramBuildInfo(g->prog, cl_devs[dev_index],
		    CL_PROGRAM_BUILD_LOG, sizeof log, log, NULL);
		fprintf(stderr, "cl build (dev %d): %s\n", dev_index, log);
		return XTC_E_IO;
	}
	g->kern = clCreateKernel(g->prog, "decode", &err);
	if (err != CL_SUCCESS)
		return XTC_E_IO;
	/*
	 * CL_MEM_USE_HOST_PTR over our own page-aligned host array.  On this
	 * integrated Lunar-Lake GPU host and device share memory (UMA), so the
	 * host array is coherent after clFinish and we read it DIRECTLY -- no
	 * clEnqueueReadBuffer and no clEnqueueMapBuffer.  Both of those route
	 * through a built-in copy-kernel build that SEGFAULTS inside this
	 * compute-runtime (NEO::Zebin::decodeZeInfo) under the batch engine's
	 * queue-usage pattern; reading the shared host pointer sidesteps that
	 * path entirely.  See README.
	 */
	if (posix_memalign((void **)&g->dbuf_host, 4096,
	    cap * sizeof(cl_uint)) != 0 || g->dbuf_host == NULL)
		return XTC_E_NOMEM;
	memset(g->dbuf_host, 0, cap * sizeof(cl_uint));
	g->dbuf = clCreateBuffer(g->ctx, CL_MEM_READ_WRITE |
	    CL_MEM_USE_HOST_PTR, cap * sizeof(cl_uint), g->dbuf_host, &err);
	if (err != CL_SUCCESS)
		return XTC_E_IO;
	if (dev_waiter_start(g) != XTC_OK)
		return XTC_E_IO;
	g->cl_ready = 1;
	fprintf(stderr, "group %d (GPU) -> cl device %d (%s)\n",
	    g->gidx, dev_index, nm);
	return XTC_OK;
}

static int
cl_wait_ev(void *ev)
{
	cl_event e = ev;
	return clWaitForEvents(1, &e) == CL_SUCCESS ? 0 : -1;
}

static void
cl_release_ev(void *ev)
{
	clReleaseEvent((cl_event)ev);
}

/* Members' iteration counters advance only once every enqueue succeeded;
 * on a mid-vector failure the kernels already queued are drained first. */
static int
cl_submit(struct group *g, const xtc_batch_item_t *items, size_t n,
    xtc_batch_exec_t *exec)
{
	cl_event ev = NULL;
	cl_int err;
	size_t i;
	int rc;

	if (!g->cl_ready)
		return XTC_E_IO;
	inflight_enter(g);
	for (i = 0; i < n; i++) {
		struct req *r = items[i].request;
		cl_uint base = (cl_uint)((size_t)r->slot * (WORK_ELEMS + 1));
		cl_int id = r->id, it = r->iter;
		cl_uint acc_in = r->acc, nn = WORK_ELEMS;
		size_t gws = WORK_ELEMS;

		clSetKernelArg(g->kern, 0, sizeof g->dbuf, &g->dbuf);
		clSetKernelArg(g->kern, 1, sizeof base, &base);
		clSetKernelArg(g->kern, 2, sizeof id, &id);
		clSetKernelArg(g->kern, 3, sizeof it, &it);
		clSetKernelArg(g->kern, 4, sizeof acc_in, &acc_in);
		clSetKernelArg(g->kern, 5, sizeof nn, &nn);
		if (ev != NULL)
			clReleaseEvent(ev);
		ev = NULL;
		err = clEnqueueNDRangeKernel(g->q, g->kern, 1, NULL, &gws, NULL,
		    0, NULL, &ev);
		if (err != CL_SUCCESS) {
			clFinish(g->q);
			inflight_leave();
			return XTC_E_IO;
		}
	}
	clFlush(g->q);
	if ((rc = dev_waiter_post(g, cl_wait_ev, cl_release_ev, ev, exec))
	    != XTC_OK) {
		clReleaseEvent(ev);
		clFinish(g->q);
		inflight_leave();
		return rc;
	}
	for (i = 0; i < n; i++)
		((struct req *)items[i].request)->iter++;
	return XTC_OK;
}

static void
cl_collect(struct group *g, const xtc_batch_item_t *items,
    xtc_batch_result_t *results, size_t n, int exec_status)
{
	size_t i;

	if (exec_status != XTC_BATCH_EXEC_OK) {
		clFinish(g->q); /* the kernels may still be running */
		for (i = 0; i < n; i++) {
			results[i].state = XTC_BATCH_ERROR;
			results[i].error = XTC_E_IO;
		}
		return;
	}
	/* The kernel has completed (the FUTURE resolved after clWaitForEvents).
	 * clFinish drains the queue so the UMA host array is coherent, then we
	 * read our own backing pointer DIRECTLY -- no map, no read-buffer, so no
	 * built-in copy-kernel build (which crashes this driver). */
	clFinish(g->q);
	for (i = 0; i < n; i++) {
		struct req *r = items[i].request;
		size_t base = (size_t)r->slot * (WORK_ELEMS + 1);
		int k;

		r->acc = g->dbuf_host[base + WORK_ELEMS]; /* new acc */
		for (k = 0; k < WORK_ELEMS; k++)
			r->work[k] = g->dbuf_host[base + (size_t)k];
		decide(g, r, &results[i]);
	}
}

static void
cl_group_fini(struct group *g)
{
	if (!g->cl_ready)
		return;
	dev_waiter_stop(g);
	clReleaseMemObject(g->dbuf);
	clReleaseKernel(g->kern);
	clReleaseProgram(g->prog);
	clReleaseCommandQueue(g->q);
	clReleaseContext(g->ctx);
	free(g->dbuf_host);
	g->dbuf_host = NULL;
	g->cl_ready = 0;
}

static const struct device opencl_device = {
	.name = "opencl",
	.group_init = cl_group_init,
	.submit = cl_submit,
	.collect = cl_collect,
	.group_fini = cl_group_fini,
};

#endif /* XTC_WITH_OPENCL */

/* ---------------------------------------------------------------------- */
/* LEVELZERO backend: real NPU, ze_event -> FUTURE bridge.                */
/* ---------------------------------------------------------------------- */

#ifdef XTC_WITH_LEVELZERO

#define ZCK(call, lbl) do { ze_result_t _r = (call); \
	if (_r != ZE_RESULT_SUCCESS) { \
		fprintf(stderr, "ze %s -> 0x%x\n", lbl, _r); \
		return XTC_E_IO; } } while (0)

/*
 * Event pool slots.  Each NPU op creates an event at a rotating pool index;
 * the waiter destroys it after the completion and only then marks the index
 * free (ze_evt_busy), so an index is never reused while its previous event
 * lives.  The engine runs one op per group at a time, so a few slots are
 * plenty; 64 leaves slack for the waiter's release lagging the next submit.
 */
#define ZE_EVT_SLOTS 64
/* Host staging regions: one per request slot modulo this (>= GROUP_CAP, so
 * the members of one op never share a region). */
#define ZE_STAGE_SLOTS 16

static ze_driver_handle_t ze_vpu_drv;
static ze_device_handle_t ze_vpu_dev;

static int
ze_discover(void)
{
	uint32_t ndrv = 0, i;
	ze_driver_handle_t drivers[8];

	if (ze_vpu_dev != NULL)
		return XTC_OK;
	if (zeInit(0) != ZE_RESULT_SUCCESS)
		return XTC_E_IO;
	if (zeDriverGet(&ndrv, NULL) != ZE_RESULT_SUCCESS || ndrv == 0)
		return XTC_E_IO;
	if (ndrv > 8)
		ndrv = 8;
	if (zeDriverGet(&ndrv, drivers) != ZE_RESULT_SUCCESS)
		return XTC_E_IO;
	for (i = 0; i < ndrv && ze_vpu_dev == NULL; i++) {
		uint32_t nd = 0, j;
		ze_device_handle_t devs[8];
		if (zeDeviceGet(drivers[i], &nd, NULL) != ZE_RESULT_SUCCESS)
			continue;
		if (nd > 8)
			nd = 8;
		if (zeDeviceGet(drivers[i], &nd, devs) != ZE_RESULT_SUCCESS)
			continue;
		for (j = 0; j < nd; j++) {
			ze_device_properties_t p;
			memset(&p, 0, sizeof p);
			p.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
			if (zeDeviceGetProperties(devs[j], &p) !=
			    ZE_RESULT_SUCCESS)
				continue;
			if (p.type == ZE_DEVICE_TYPE_VPU) {
				ze_vpu_drv = drivers[i];
				ze_vpu_dev = devs[j];
				fprintf(stderr,
				    "L0: VPU device \"%s\" (type=%d)\n",
				    p.name, p.type);
				break;
			}
		}
	}
	return ze_vpu_dev != NULL ? XTC_OK : XTC_E_IO;
}

struct ze_ev {
	struct group *g;
	ze_event_handle_t ev;
	unsigned idx;
};

static int
ze_group_init(struct group *g, int dev_index)
{
	ze_context_desc_t cdesc;
	ze_command_queue_desc_t qdesc;
	ze_command_list_desc_t ldesc;
	ze_device_mem_alloc_desc_t ddesc;
	ze_host_mem_alloc_desc_t hdesc;
	size_t bytes = (size_t)WORK_ELEMS * sizeof(uint32_t);
	int k;

	(void)dev_index;
	if (ze_discover() != XTC_OK)
		return XTC_E_IO;
	g->zedev = ze_vpu_dev;

	memset(&cdesc, 0, sizeof cdesc);
	cdesc.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC;
	ZCK(zeContextCreate(ze_vpu_drv, &cdesc, &g->zectx), "ctx");

	memset(&qdesc, 0, sizeof qdesc);
	qdesc.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC;
	qdesc.ordinal = 0; /* group 0: flags=COMPUTE|COPY on this NPU */
	qdesc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
	ZCK(zeCommandQueueCreate(g->zectx, g->zedev, &qdesc, &g->zeq), "queue");

	memset(&ldesc, 0, sizeof ldesc);
	ldesc.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC;
	ldesc.commandQueueGroupOrdinal = 0;
	ZCK(zeCommandListCreate(g->zectx, g->zedev, &ldesc, &g->zelist), "list");

	memset(&ddesc, 0, sizeof ddesc);
	ddesc.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
	ZCK(zeMemAllocDevice(g->zectx, &ddesc, bytes, 64, g->zedev,
	    &g->zdev_a), "dmem a");
	ZCK(zeMemAllocDevice(g->zectx, &ddesc, bytes, 64, g->zedev,
	    &g->zdev_b), "dmem b");

	memset(&hdesc, 0, sizeof hdesc);
	hdesc.stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC;
	ZCK(zeMemAllocHost(g->zectx, &hdesc, bytes * ZE_STAGE_SLOTS, 64,
	    &g->zhost), "hmem");
	g->ze_evs = calloc(ZE_EVT_SLOTS, sizeof *g->ze_evs);
	if (g->ze_evs == NULL)
		return XTC_E_NOMEM;
	for (k = 0; k < ZE_EVT_SLOTS; k++)
		atomic_store(&g->ze_evt_busy[k], 0);

	{
		ze_event_pool_desc_t epdesc;
		memset(&epdesc, 0, sizeof epdesc);
		epdesc.stype = ZE_STRUCTURE_TYPE_EVENT_POOL_DESC;
		epdesc.flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
		epdesc.count = ZE_EVT_SLOTS;
		ZCK(zeEventPoolCreate(g->zectx, &epdesc, 1, &g->zedev,
		    &g->zepool), "epool");
	}
	if (dev_waiter_start(g) != XTC_OK)
		return XTC_E_IO;
	g->ze_ready = 1;
	fprintf(stderr, "group %d (NPU) -> Level Zero VPU "
	    "(op: device-memory copy chain per iteration)\n", g->gidx);
	return XTC_OK;
}

static int
ze_wait_ev(void *arg)
{
	struct ze_ev *e = arg;
	return zeEventHostSynchronize(e->ev, UINT64_MAX) == ZE_RESULT_SUCCESS ?
	    0 : -1;
}

/* Destroy the event, then free its pool index for reuse. */
static void
ze_release_ev(void *arg)
{
	struct ze_ev *e = arg;
	zeEventDestroy(e->ev);
	atomic_store(&e->g->ze_evt_busy[e->idx], 0);
}

/*
 * NPU submit: for each ready member run the SAME decode_step on the host and
 * write the result into that member's own region of the host-visible
 * staging buffer, then append a device-memory copy chain (host -> device A
 * -> device B -> the member's host region) to the NPU command list and
 * execute it on the NPU command queue, signalling a ze_event.  collect()
 * copies each member's region into r->work only after the event fired, so
 * the data really made the round trip.
 *
 * HONEST LABEL: the compute (decode_step) runs on the host; the NPU work is a
 * COMMAND-QUEUE ROUND-TRIP (device-memory copies), not a matmul.  What this
 * proves is the batch engine FEEDING the NPU command queue and AWAITING its
 * ze_event completion -- concurrently with the GPU group -- via the FUTURE
 * bridge.  A native NPU compute kernel/graph is out of scope (needs a compiled
 * L0 module / OpenVINO IR; see README).
 *
 * Members in one op are serialized through the shared device buffers by the
 * barriers.  The command list is reset each op (zeCommandListReset).  An
 * event pool index is reused only after the waiter has destroyed the event
 * that last used it (ze_evt_busy); with one op in flight per group that is
 * always true by the next op, and if it ever is not the op fails cleanly.
 */
static int
ze_submit(struct group *g, const xtc_batch_item_t *items, size_t n,
    xtc_batch_exec_t *exec)
{
	ze_event_desc_t edesc;
	struct ze_ev *e;
	size_t bytes = (size_t)WORK_ELEMS * sizeof(uint32_t);
	size_t i;
	unsigned idx;
	int rc;

	if (!g->ze_ready)
		return XTC_E_IO;
	idx = g->ze_evt_next++ % ZE_EVT_SLOTS;
	if (atomic_exchange(&g->ze_evt_busy[idx], 1) != 0) {
		fprintf(stderr, "ze: event slot %u still in use\n", idx);
		return XTC_E_AGAIN;
	}
	e = &g->ze_evs[idx];
	e->g = g;
	e->idx = idx;
	inflight_enter(g);
	memset(&edesc, 0, sizeof edesc);
	edesc.stype = ZE_STRUCTURE_TYPE_EVENT_DESC;
	edesc.index = idx;
	edesc.signal = ZE_EVENT_SCOPE_FLAG_HOST;
	edesc.wait = ZE_EVENT_SCOPE_FLAG_HOST;
	if (zeEventCreate(g->zepool, &edesc, &e->ev) != ZE_RESULT_SUCCESS) {
		fprintf(stderr, "ze: eventCreate failed\n");
		atomic_store(&g->ze_evt_busy[idx], 0);
		inflight_leave();
		return XTC_E_IO;
	}
	if (zeCommandListReset(g->zelist) != ZE_RESULT_SUCCESS)
		goto fail;
	for (i = 0; i < n; i++) {
		struct req *r = items[i].request;
		uint32_t *stage = (uint32_t *)g->zhost +
		    (size_t)(r->slot % ZE_STAGE_SLOTS) * WORK_ELEMS;
		uint32_t acc = decode_step(r->acc, r->id, r->iter);
		int k;
		ze_event_handle_t sig = (i == n - 1) ? e->ev : NULL;

		for (k = 0; k < WORK_ELEMS; k++)
			stage[k] = acc ^ (uint32_t)k;
		if (zeCommandListAppendMemoryCopy(g->zelist, g->zdev_a, stage,
		    bytes, NULL, 0, NULL) != ZE_RESULT_SUCCESS ||
		    zeCommandListAppendBarrier(g->zelist, NULL, 0, NULL)
		    != ZE_RESULT_SUCCESS ||
		    zeCommandListAppendMemoryCopy(g->zelist, g->zdev_b,
		    g->zdev_a, bytes, NULL, 0, NULL) != ZE_RESULT_SUCCESS ||
		    zeCommandListAppendBarrier(g->zelist, NULL, 0, NULL)
		    != ZE_RESULT_SUCCESS ||
		    zeCommandListAppendMemoryCopy(g->zelist, stage, g->zdev_b,
		    bytes, sig, 0, NULL) != ZE_RESULT_SUCCESS ||
		    zeCommandListAppendBarrier(g->zelist, NULL, 0, NULL)
		    != ZE_RESULT_SUCCESS)
			goto fail;
	}
	if (zeCommandListClose(g->zelist) != ZE_RESULT_SUCCESS ||
	    zeCommandQueueExecuteCommandLists(g->zeq, 1, &g->zelist, NULL)
	    != ZE_RESULT_SUCCESS) {
		fprintf(stderr, "ze: close/execute failed\n");
		goto fail;
	}
	if ((rc = dev_waiter_post(g, ze_wait_ev, ze_release_ev, e, exec))
	    != XTC_OK) {
		(void)zeCommandQueueSynchronize(g->zeq, UINT64_MAX);
		zeEventDestroy(e->ev);
		atomic_store(&g->ze_evt_busy[idx], 0);
		inflight_leave();
		return rc;
	}
	/* Committed: advance the members (their new acc is in staging). */
	for (i = 0; i < n; i++) {
		struct req *r = items[i].request;
		r->acc = decode_step(r->acc, r->id, r->iter);
		r->iter++;
	}
	return XTC_OK;
fail:
	zeEventDestroy(e->ev);
	atomic_store(&g->ze_evt_busy[idx], 0);
	inflight_leave();
	return XTC_E_IO;
}

static void
ze_collect(struct group *g, const xtc_batch_item_t *items,
    xtc_batch_result_t *results, size_t n, int exec_status)
{
	size_t bytes = (size_t)WORK_ELEMS * sizeof(uint32_t);
	size_t i;

	if (exec_status != XTC_BATCH_EXEC_OK)
		(void)zeCommandQueueSynchronize(g->zeq, UINT64_MAX);
	for (i = 0; i < n; i++) {
		struct req *r = items[i].request;
		if (exec_status != XTC_BATCH_EXEC_OK) {
			results[i].state = XTC_BATCH_ERROR;
			results[i].error = XTC_E_IO;
			continue;
		}
		/* The event fired: the region holds the round-tripped data. */
		memcpy(r->work, (uint32_t *)g->zhost +
		    (size_t)(r->slot % ZE_STAGE_SLOTS) * WORK_ELEMS, bytes);
		decide(g, r, &results[i]);
	}
}

static void
ze_group_fini(struct group *g)
{
	if (!g->ze_ready)
		return;
	dev_waiter_stop(g);
	free(g->ze_evs);
	zeEventPoolDestroy(g->zepool);
	zeMemFree(g->zectx, g->zdev_a);
	zeMemFree(g->zectx, g->zdev_b);
	zeMemFree(g->zectx, g->zhost);
	zeCommandListDestroy(g->zelist);
	zeCommandQueueDestroy(g->zeq);
	zeContextDestroy(g->zectx);
	g->ze_ready = 0;
}

static const struct device levelzero_device = {
	.name = "levelzero",
	.group_init = ze_group_init,
	.submit = ze_submit,
	.collect = ze_collect,
	.group_fini = ze_group_fini,
};

#endif /* XTC_WITH_LEVELZERO */

/* ---------------------------------------------------------------------- */
/* Batch glue: route the engine's submit/collect to the group's backend.  */
/* ---------------------------------------------------------------------- */

static int
batch_submit(const xtc_batch_item_t *items, size_t n, void *user,
    xtc_batch_exec_t *exec)
{
	struct group *g = user;

	g->submits++;
	g->in_flight_sum += n;
	if ((unsigned)n > g->max_in_flight)
		g->max_in_flight = (unsigned)n;
	return g->dev->submit(g, items, n, exec);
}

static int
batch_collect(const xtc_batch_item_t *items, xtc_batch_result_t *results,
    size_t n, int exec_status, void *user)
{
	struct group *g = user;

	g->collects++;
	g->dev->collect(g, items, results, n, exec_status);
	return XTC_OK;
}

static void
batch_complete(uint64_t handle, void *request, int outcome, void *user)
{
	struct group *g = user;
	struct req *r = request;

	if (handle != r->handle || ++r->completions != 1)
		die("exactly-once terminal completion");
	r->outcome = outcome;
	if (outcome == XTC_OK)
		g->demo->completed++;
	else if (outcome == XTC_E_ABORTED)
		g->demo->cancelled++;
	else
		g->demo->failed++;
	g->n_terminal++;
	(void)xtc_notify_signal(g->done_note); /* wake the feeder */
}

/* ---------------------------------------------------------------------- */
/* Request setup: deterministic, varying-depth requests.                  */
/* ---------------------------------------------------------------------- */

static void
build_reqs(struct demo *d, int nreqs)
{
	int i;

	if (nreqs > MAX_REQS)
		nreqs = MAX_REQS;
	for (i = 0; i < nreqs; i++) {
		struct req *r = &d->reqs[i];
		memset(r, 0, sizeof *r);
		r->id = i;
		r->slot = i;
		r->depth = req_depth(i);
		r->acc = (uint32_t)(i + 1);
	}
	d->nreqs = nreqs;
}

/* ---------------------------------------------------------------------- */
/* Driver proc: feeds a group, continuous or static.                      */
/* ---------------------------------------------------------------------- */

struct feed_arg {
	struct demo *d;
	struct group *g;
	int first_req, last_req; /* this group's slice [first,last) */
};

/*
 * Both feeders keep up to GROUP_CAP requests admitted, so the device sees
 * the same queue depth; only the refill rule differs.  Continuous: submit
 * the next pending request whenever one completes.  Static: submit a
 * cohort of GROUP_CAP, wait until ALL of them complete, then the next
 * cohort.  The feeder sleeps on the group's notify (signalled by
 * complete()); it does not poll.
 */
#define GROUP_CAP 8    /* admitted live requests per group */
#define GROUP_ACTIVE 4 /* max requests per device op (vector width) */

static void
feeder(void *arg)
{
	struct feed_arg *fa = arg;
	struct demo *d = fa->d;
	struct group *g = fa->g;
	int next = fa->first_req, cohort_end, rc;
	unsigned target;

	while (next < fa->last_req) {
		if (d->continuous) {
			rc = xtc_batch_submit(g->batch, &d->reqs[next], 1,
			    &d->reqs[next].handle);
			if (rc == XTC_E_AGAIN) {
				must(xtc_notify_wait(g->done_note, -1), "wait");
				continue;
			}
			must(rc, "submit");
			next++;
			continue;
		}
		cohort_end = next + GROUP_CAP < fa->last_req ?
		    next + GROUP_CAP : fa->last_req;
		target = g->n_terminal + (unsigned)(cohort_end - next);
		for (; next < cohort_end; next++)
			must(xtc_batch_submit(g->batch, &d->reqs[next], 1,
			    &d->reqs[next].handle), "submit cohort");
		while (g->n_terminal < target)
			must(xtc_notify_wait(g->done_note, -1), "wait");
	}
	must(xtc_batch_close(g->batch, XTC_BATCH_DRAIN), "close");
}

/* ---------------------------------------------------------------------- */
/* Run.                                                                   */
/* ---------------------------------------------------------------------- */

struct run_stats {
	unsigned total_submits;
	uint64_t total_members;
	unsigned reqs_done;
	double avg_occupancy;
	unsigned per_group_done[MAX_GROUPS];
	unsigned per_group_submits[MAX_GROUPS];
	unsigned per_group_overlap[MAX_GROUPS];
	const char *per_group_backend[MAX_GROUPS];
	int overlap_peak;
};

static struct feed_arg feed_args[MAX_GROUPS];

/* Backend selection per group: hetero wires 0->OPENCL, 1->LEVELZERO. */
static const struct device *
group_backend(struct demo *d, int gidx)
{
	if (!d->hetero)
		return &fake_device;
#if defined(XTC_WITH_OPENCL) && defined(XTC_WITH_LEVELZERO)
	return gidx == 0 ? &opencl_device : &levelzero_device;
#elif defined(XTC_WITH_OPENCL)
	(void)gidx;
	return &opencl_device;
#elif defined(XTC_WITH_LEVELZERO)
	(void)gidx;
	return &levelzero_device;
#else
	(void)gidx;
	return &fake_device;
#endif
}

static struct run_stats
run(struct demo *d, int nreqs)
{
	xtc_exec_t *exec;
	xtc_exec_class_t cls;
	xtc_pid_t pid;
	struct run_stats rs;
	xtc_batch_async_opts_t opts;
	int i, per;

	memset(&rs, 0, sizeof rs);
	must(xtc_exec_init(&exec, d->ngroups), "exec_init");
	build_reqs(d, nreqs);
	per = (d->nreqs + d->ngroups - 1) / d->ngroups;
	for (i = 0; i < d->ngroups; i++) {
		struct group *g = &d->groups[i];
		g->demo = d;
		g->gidx = i;
		g->dev = group_backend(d, i);
		g->loop = xtc_exec_loop(exec, i);
		must(g->dev->group_init(g, i), "group_init");
		must(xtc_exec_class_create(g->loop, 100, 0, &cls), "class");
		memset(&opts, 0, sizeof opts);
		opts.size = sizeof opts;
		opts.version = XTC_BATCH_VERSION;
		opts.capacity = GROUP_CAP;
		opts.max_active = GROUP_ACTIVE;
		opts.resource_capacity = GROUP_CAP;
		opts.sched_class = cls;
		opts.submit = batch_submit;
		opts.collect = batch_collect;
		opts.complete = batch_complete;
		opts.user = g;
		must(xtc_notify_create(&g->done_note), "notify");
		must(xtc_batch_create_async(g->loop, &opts, &g->batch),
		    "create_async");
	}
	for (i = 0; i < d->ngroups; i++) {
		int slice = i * per;
		feed_args[i].d = d;
		feed_args[i].g = &d->groups[i];
		feed_args[i].first_req = slice;
		feed_args[i].last_req = slice + per < d->nreqs ?
		    slice + per : d->nreqs;
		must(xtc_proc_spawn(xtc_exec_loop(exec, i), feeder,
		    &feed_args[i], NULL, &pid), "spawn feeder");
	}
	must(xtc_exec_run(exec), "exec_run");

	for (i = 0; i < d->ngroups; i++) {
		struct group *g = &d->groups[i];
		rs.total_submits += g->submits;
		rs.total_members += g->in_flight_sum;
		rs.reqs_done += g->reqs_done;
		rs.per_group_done[i] = g->reqs_done;
		rs.per_group_submits[i] = g->submits;
		rs.per_group_overlap[i] = g->overlap_ops;
		rs.per_group_backend[i] = g->dev->name;
		must(xtc_batch_destroy(g->batch), "destroy");
		xtc_notify_destroy(g->done_note);
		g->dev->group_fini(g);
	}
#if defined(XTC_WITH_OPENCL) || defined(XTC_WITH_LEVELZERO)
	rs.overlap_peak = atomic_load(&g_overlap_peak);
#endif
	if (rs.total_submits > 0)
		rs.avg_occupancy = (double)rs.total_members /
		    (double)rs.total_submits;
	must(xtc_exec_fini(exec), "exec_fini");
	return rs;
}

/* ---------------------------------------------------------------------- */
/* Deterministic reference: final acc per request after `depth` decodes.  */
/* ---------------------------------------------------------------------- */

static uint32_t
checksum_reqs(const struct demo *d)
{
	uint32_t sum = 0;
	int i;

	for (i = 0; i < d->nreqs; i++)
		sum += d->reqs[i].acc;
	return sum;
}

static uint32_t
checksum_reference(const struct demo *d)
{
	uint32_t sum = 0;
	int i, it;

	for (i = 0; i < d->nreqs; i++) {
		uint32_t acc = (uint32_t)(i + 1);
		int depth = req_depth(i);
		for (it = 0; it < depth; it++)
			acc = decode_step(acc, i, it);
		sum += acc;
	}
	return sum;
}

/* ---------------------------------------------------------------------- */
/* Self-test (FAKE backend, no GPU/NPU, deterministic).                   */
/* ---------------------------------------------------------------------- */

static struct demo *
demo_new(int continuous, int devices, int hetero)
{
	struct demo *d = calloc(1, sizeof *d);

	if (d == NULL)
		die("oom");
	d->ngroups = devices;
	d->continuous = continuous;
	d->hetero = hetero;
	return d;
}

static void
self_test(void)
{
	struct demo *dc, *ds;
	struct run_stats rc, rsc;
	uint32_t sum_c, sum_s, ref;
	int i, min_depth, max_depth, g0_done, g1_done;

	/* 1. Determinism + exactly-once + checksum (continuous, FUTURE,
	 *    2 groups so both-groups-progress is covered). */
	fake_use_sync = 0;
	dc = demo_new(1, 2, 0);
	rc = run(dc, 48);
	if (dc->completed != (unsigned)dc->nreqs || dc->cancelled != 0 ||
	    dc->failed != 0)
		die("every request completes exactly once");
	for (i = 0; i < dc->nreqs; i++)
		if (dc->reqs[i].completions != 1 || !dc->reqs[i].done ||
		    dc->reqs[i].iter != dc->reqs[i].depth)
			die("request done exactly once at its depth");
	sum_c = checksum_reqs(dc);
	ref = checksum_reference(dc);
	if (sum_c != ref)
		die("fake checksum != scalar reference");

	/* 2. Both groups made progress (reqs_done > 0 on each). */
	g0_done = rc.per_group_done[0];
	g1_done = rc.per_group_done[1];
	if (!(g0_done > 0 && g1_done > 0))
		die("both groups must make progress");

	/* 3. Variable-length: shallow (1 iter) and deep (>1) coexist. */
	min_depth = 1 << 30;
	max_depth = 0;
	for (i = 0; i < dc->nreqs; i++) {
		if (dc->reqs[i].depth < min_depth)
			min_depth = dc->reqs[i].depth;
		if (dc->reqs[i].depth > max_depth)
			max_depth = dc->reqs[i].depth;
	}
	if (!(min_depth == 1 && max_depth > min_depth))
		die("variable-length requests (shallow=1, deep>1)");

	/* 4. Static mode (same queue depth, cohort refill) is equally correct.
	 *    Occupancy is reported, not asserted: which mode issues fewer device
	 *    ops depends on the depth mix, and it is not a wall-time claim. */
	fake_use_sync = 0;
	ds = demo_new(0, 2, 0);
	rsc = run(ds, 48);
	sum_s = checksum_reqs(ds);
	if (sum_s != ref)
		die("static checksum != reference");
	if (ds->completed != (unsigned)ds->nreqs)
		die("static: every request completes exactly once");

	/* 5. SYNC exec kind also works and matches. */
	fake_use_sync = 1;
	{
		struct demo *dy = demo_new(1, 2, 0);
		uint32_t s;
		(void)run(dy, 48);
		s = checksum_reqs(dy);
		if (s != ref)
			die("SYNC-kind checksum != reference");
		free(dy);
	}
	fake_use_sync = 0;

	printf("self-test checksum=%u (reference=%u)\n", sum_c, ref);
	printf("both groups progressed: group0=%d group1=%d requests\n",
	    g0_done, g1_done);
	printf("continuous occupancy=%.2f members/op, static=%.2f members/op\n",
	    rc.avg_occupancy, rsc.avg_occupancy);
	printf("variable depth: shallow=%d deep=%d iterations\n",
	    min_depth, max_depth);
	puts("hetero batching self-test: PASS");
	free(dc);
	free(ds);
}

/* ---------------------------------------------------------------------- */
/* CLI.                                                                   */
/* ---------------------------------------------------------------------- */

static int
arg_int(const char *a, const char *key, int *out)
{
	size_t kl = strlen(key);

	if (strncmp(a, key, kl) == 0 && a[kl] == '=') {
		*out = atoi(a + kl + 1);
		return 1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int i, nreqs = 48, continuous = 1, test = 0, hetero = 0;
	struct demo *d;
	struct run_stats rs;

	for (i = 1; i < argc; i++) {
		char *a = argv[i];
		if (strcmp(a, "--help") == 0) {
			puts(
"usage: hetero [options]\n"
"  --hetero                 GPU group (OpenCL) + NPU group (Level Zero),\n"
"                           both busy concurrently (needs the real backends)\n"
"  --mode=continuous|static batching mode (default continuous)\n"
"  --reqs=N                 number of pseudo-decode requests (default 48)\n"
"  --self-test              deterministic fake-backend checks, no GPU/NPU\n"
"  --help                   this message\n"
"Build the real backends with XTC_WITH_OPENCL=1 XTC_WITH_LEVELZERO=1.\n"
"Without --hetero the fake CPU backend runs (deterministic, no device).\n"
"Requires libxtc configured with --enable-sched-policy.");
			return 0;
		} else if (strcmp(a, "--self-test") == 0)
			test = 1;
		else if (strcmp(a, "--hetero") == 0)
			hetero = 1;
		else if (strcmp(a, "--mode=continuous") == 0)
			continuous = 1;
		else if (strcmp(a, "--mode=static") == 0)
			continuous = 0;
		else if (arg_int(a, "--reqs", &nreqs)) {
		} else {
			fprintf(stderr, "unknown argument: %s (try --help)\n", a);
			return 2;
		}
	}
	if (test) {
		self_test();
		return 0;
	}
	if (nreqs < 2 || nreqs > MAX_REQS) {
		fprintf(stderr, "--reqs must be 2..%d\n", MAX_REQS);
		return 2;
	}
	if (hetero) {
#if !defined(XTC_WITH_OPENCL) || !defined(XTC_WITH_LEVELZERO)
		fprintf(stderr, "--hetero needs both real backends: rebuild "
		    "with XTC_WITH_OPENCL=1 XTC_WITH_LEVELZERO=1\n");
		return 2;
#endif
	}
	d = demo_new(continuous, 2, hetero);
	rs = run(d, nreqs);
	printf("mode=%s hetero=%d reqs=%d\n",
	    continuous ? "continuous" : "static", hetero, d->nreqs);
	printf("completed=%u cancelled=%u failed=%u (exactly-once enforced)\n",
	    d->completed, d->cancelled, d->failed);
	printf("reqs_done=%u device_ops=%u avg_occupancy=%.2f members/op\n",
	    rs.reqs_done, rs.total_submits, rs.avg_occupancy);
	for (i = 0; i < 2; i++)
		printf("  group %d [%s]: reqs_done=%u device_ops=%u "
		    "overlap_ops=%u\n", i, rs.per_group_backend[i],
		    rs.per_group_done[i], rs.per_group_submits[i],
		    rs.per_group_overlap[i]);
	if (hetero) {
		printf("CONCURRENT: peak_overlap=%d groups in-flight at once; "
		    "GPU busy AND NPU busy at the same time: %s\n",
		    rs.overlap_peak, rs.overlap_peak >= 2 ? "YES" : "no");
	} else {
		uint32_t sum = checksum_reqs(d), ref = checksum_reference(d);
		printf("checksum=%u reference=%u %s\n", sum, ref,
		    sum == ref ? "OK" : "MISMATCH");
	}
	free(d);
	return 0;
}

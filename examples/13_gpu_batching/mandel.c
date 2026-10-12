/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/13_gpu_batching/mandel.c
 *	Continuous-batching Mandelbrot renderer over the async batch API.
 *
 *	Each batch MEMBER is a tile of the image.  A tile needs one or more
 *	PASSES through the device: a pass advances every pixel of the tile up
 *	to a per-pass iteration budget, and a tile is DONE once every pixel has
 *	either escaped or reached maxiter.  Shallow tiles (mostly outside the
 *	set) finish in one pass and leave the batch; deep tiles (inside/near the
 *	boundary) need many passes and stay.  Short tiles leaving while deep
 *	tiles continue, with new tiles joining the freed slots, is exactly the
 *	vLLM continuous-batching shape -- here driven by a GPU kernel instead of
 *	an LLM decode step.
 *
 *	Three device backends sit behind one small vtable (struct device):
 *	  FAKE  -- pure-CPU Mandelbrot, deterministic, the default, needs no GPU,
 *	           runs in CI and under the sim backend.  SYNC or FUTURE exec.
 *	  OPENCL-- real GPU via OpenCL; submit() enqueues an NDRange and bridges
 *	           the cl_event completion to the engine's FUTURE exec kind via a
 *	           detached worker thread that clWaitForEvents then
 *	           xtc_promise_set.  collect() reads back the per-tile buffer.
 *	The same batch driver (submit/collect) feeds all backends unchanged;
 *	only the vtable differs.  Compiled with XTC_WITH_OPENCL=1 for the GPU
 *	backend; without it, only FAKE is built and no OpenCL dependency exists.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_batch.h"
#include "xtc_exec.h"
#include "xtc_future.h"
#include "xtc_proc.h"

#ifdef XTC_WITH_OPENCL
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <pthread.h>
#endif

/* ---------------------------------------------------------------------- */
/* Tile model: the batch member and request-as-carrier.                   */
/* ---------------------------------------------------------------------- */

#define MAX_TILES 256
#define MAX_GROUPS 2
#define TILE_PX_MAX (64 * 64)

/*
 * A tile is one batch member.  It carries its own device-output location
 * (iters[]) and a mock KV-slot id (slot), demonstrating the design's
 * request-as-carrier contract: the engine stores only the void* request; the
 * consumer hangs all per-member device state off it.  complete() firing is the
 * cue to free the slot.
 */
struct tile {
	int id;            /* stable tile index, 0..ntiles-1 */
	int slot;          /* mock KV-slot id (device-side scratch index) */
	int tx, ty;        /* tile origin in pixels */
	int tw, th;        /* tile size in pixels */
	int img_w, img_h;  /* full image dims (for coordinate mapping) */
	int maxiter;       /* escape-iteration cap */
	int pass;          /* passes applied so far */
	int done;          /* 1 once every pixel escaped or hit maxiter */
	uint64_t handle;   /* batch handle */
	uint32_t iters[TILE_PX_MAX]; /* per-pixel escape iteration (output) */
	/* bookkeeping for assertions */
	int completions;   /* must end == 1 */
	int outcome;
};

/* Per-pass iteration budget: a tile advances this many iterations per pass.
 * Smaller => more passes for deep tiles => more continuous-batching churn. */
#define PASS_ITERS 32

/* ---------------------------------------------------------------------- */
/* Scalar reference Mandelbrot (ground truth for the self-test checksum).  */
/* ---------------------------------------------------------------------- */

/*
 * Map a tile pixel to the complex plane exactly as the kernel does, then run
 * the escape loop to completion (all maxiter).  Returns the escape iteration
 * count.  float math on purpose: it matches the GPU kernel's float path, so
 * the fake-CPU and reference checksums agree exactly, and the GPU checksum is
 * compared with a small tolerance (float rounding differs per vendor).
 */
static uint32_t
mandel_pixel(int gx, int gy, int img_w, int img_h, int maxiter)
{
	/* Region: centre (-0.5, 0), span 3.0, same as the proven prototype. */
	float cx = -0.5f, cy = 0.0f, scale = 3.0f;
	float x0 = cx + ((float)gx / (float)img_w - 0.5f) * scale;
	float y0 = cy + ((float)gy / (float)img_h - 0.5f) * scale;
	float x = 0.0f, y = 0.0f;
	uint32_t i = 0;

	while (x * x + y * y <= 4.0f && i < (uint32_t)maxiter) {
		float xt = x * x - y * y + x0;
		y = 2.0f * x * y + y0;
		x = xt;
		i++;
	}
	return i;
}

/* ---------------------------------------------------------------------- */
/* Device backend vtable: one interface, three implementations.           */
/* ---------------------------------------------------------------------- */

struct group;

struct device {
	const char *name;
	/* Create any per-group device state (queues, buffers, program). */
	int (*group_init)(struct group *g, int dev_index);
	/*
	 * Launch ONE device op over the n ready tiles and hand the engine a
	 * completion token via *exec.  Returns XTC_OK or an xtc_err.
	 */
	int (*submit)(struct group *g, const xtc_batch_item_t *items, size_t n,
	    xtc_batch_exec_t *exec);
	/*
	 * Read back each tile's output after the op completed, advance its
	 * pass count, and decide DONE vs CONTINUE per member.
	 */
	void (*collect)(struct group *g, const xtc_batch_item_t *items,
	    xtc_batch_result_t *results, size_t n, int exec_status);
	void (*group_fini)(struct group *g);
};

/* ---------------------------------------------------------------------- */
/* Group state (one per cl device).                                       */
/* ---------------------------------------------------------------------- */

struct demo;

struct group {
	struct demo *demo;
	xtc_batch_t *batch;
	xtc_loop_t *loop;
	int dev_index;              /* which cl device, or 0 for fake */
	unsigned submits;           /* submit() calls = device ops launched */
	unsigned collects;          /* collect() calls */
	uint64_t in_flight_sum;     /* sum of per-op member counts (occupancy) */
	unsigned max_in_flight;     /* widest op */
	unsigned tiles_done;        /* tiles that reached DONE here */
#ifdef XTC_WITH_OPENCL
	cl_context ctx;
	cl_command_queue q;
	cl_program prog;
	cl_kernel kern;
	cl_mem dbuf;                /* device output, TILE_PX_MAX * ntiles_cap */
	cl_event last_event;        /* current op's event (freed in collect) */
	int cl_ready;
	/* bridge: tiles touched by the current op, for read-back in collect */
	const struct tile *op_tiles[MAX_TILES];
	size_t op_n;
#endif
};

/* ---------------------------------------------------------------------- */
/* Whole-run state.                                                       */
/* ---------------------------------------------------------------------- */

struct demo {
	const struct device *dev;
	struct group groups[MAX_GROUPS];
	int ngroups;                /* = devices */
	struct tile tiles[MAX_TILES];
	int ntiles;
	int img_w, img_h, maxiter, tiles_x, tiles_y;
	int continuous;             /* 1 = continuous, 0 = static control */
	int finished;
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
/* FAKE backend: deterministic pure-CPU Mandelbrot. SYNC or FUTURE.       */
/* ---------------------------------------------------------------------- */

/*
 * Advance one pass over every tile pixel: continue each pixel's escape loop
 * from where the previous pass left off, up to PASS_ITERS more iterations.
 * We re-derive from scratch to maxiter-of-this-pass to keep the member state
 * entirely in iters[] (no per-pixel saved z, which a real multi-pass kernel
 * would checkpoint; the point here is variable PASS COUNT, exercised exactly).
 */
static void
fake_compute_pass(struct tile *t)
{
	int cap = (t->pass + 1) * PASS_ITERS;
	int px, py, k = 0;

	if (cap > t->maxiter)
		cap = t->maxiter;
	for (py = 0; py < t->th; py++) {
		for (px = 0; px < t->tw; px++, k++) {
			int gx = t->tx + px, gy = t->ty + py;
			t->iters[k] = mandel_pixel(gx, gy, t->img_w, t->img_h,
			    cap);
		}
	}
	t->pass++;
}

/* Did every pixel in the tile escape or reach the full maxiter? */
static int
tile_converged(const struct tile *t)
{
	int cap = t->pass * PASS_ITERS, n = t->tw * t->th, k;

	if (cap >= t->maxiter)
		return 1; /* ran the full budget */
	for (k = 0; k < n; k++) {
		if (t->iters[k] >= (uint32_t)cap)
			return 0; /* still iterating at the pass cap => not done */
	}
	return 1; /* all pixels escaped below the cap */
}

/*
 * FAKE submit: compute every ready tile's next pass inline. We default to the
 * FUTURE exec kind (an immediately-resolved promise) so the fake path drives
 * the SAME async await machinery the GPU path uses; a flag selects SYNC.
 */
static int fake_use_sync; /* 0 => FUTURE (default), 1 => SYNC */

static int
fake_submit(struct group *g, const xtc_batch_item_t *items, size_t n,
    xtc_batch_exec_t *exec)
{
	size_t i;

	(void)g;

	for (i = 0; i < n; i++)
		fake_compute_pass(items[i].request);
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
		/* Resolve immediately: the work already ran in submit. The
		 * engine's await still parks/wakes, proving the FUTURE path. */
		xtc_promise_set(prom, 0, XTC_OK);
	}
	return XTC_OK;
}

static void
fake_collect(struct group *g, const xtc_batch_item_t *items,
    xtc_batch_result_t *results, size_t n, int exec_status)
{
	size_t i;

	for (i = 0; i < n; i++) {
		struct tile *t = items[i].request;
		if (exec_status != XTC_BATCH_EXEC_OK) {
			results[i].state = XTC_BATCH_ERROR;
			results[i].error = XTC_E_IO;
			continue;
		}
		if (tile_converged(t)) {
			t->done = 1;
			g->tiles_done++;
			results[i].state = XTC_BATCH_DONE;
		} else
			results[i].state = XTC_BATCH_CONTINUE;
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
/* OPENCL backend: real GPU, cl_event -> FUTURE bridge.                   */
/* ---------------------------------------------------------------------- */

#ifdef XTC_WITH_OPENCL

static const char *CL_SRC =
"__kernel void mandel(__global uint *out, const uint W, const uint H,\n"
"    const int tx, const int ty, const uint tw, const uint th,\n"
"    const uint base, const uint cap){\n"
"  uint lid = get_global_id(0); if (lid >= tw*th) return;\n"
"  uint px = lid % tw, py = lid / tw;\n"
"  float scale = 3.0f, cx = -0.5f, cy = 0.0f;\n"
"  float x0 = cx + ((float)(tx+px)/W - 0.5f)*scale;\n"
"  float y0 = cy + ((float)(ty+py)/H - 0.5f)*scale;\n"
"  float x=0,y=0; uint i=0;\n"
"  while (x*x+y*y <= 4.0f && i < cap){ float xt=x*x-y*y+x0; y=2*x*y+y0; x=xt; i++; }\n"
"  out[base + lid] = i; }\n";

/* All cl devices on platform 0 (rusticl exposes both AMD GPUs). */
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
	size_t cap = (size_t)TILE_PX_MAX * (size_t)g->demo->ntiles;

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
	g->kern = clCreateKernel(g->prog, "mandel", &err);
	if (err != CL_SUCCESS)
		return XTC_E_IO;
	g->dbuf = clCreateBuffer(g->ctx, CL_MEM_READ_WRITE,
	    cap * sizeof(cl_uint), NULL, &err);
	if (err != CL_SUCCESS)
		return XTC_E_IO;
	g->cl_ready = 1;
	fprintf(stderr, "group %d -> cl device %d (%s)\n",
	    (int)(g - g->demo->groups), dev_index, nm);
	return XTC_OK;
}

/* Bridge argument: the detached waiter thread owns these. */
struct cl_wait {
	cl_event event;
	xtc_promise_t *prom;
};

static void *
cl_waiter(void *arg)
{
	struct cl_wait *w = arg;

	clWaitForEvents(1, &w->event);
	/* Resolve from this plain OS thread; the engine's FUTURE await wakes
	 * the driver on its own loop (dual-mode park/wake). */
	xtc_promise_set(w->prom, 0, XTC_OK);
	free(w);
	return NULL;
}

/*
 * OPENCL submit: for each ready tile set the per-tile kernel args and enqueue
 * its NDRange on the group's queue (one buffer region per tile, keyed by the
 * tile's slot). The ops chain on one in-order queue; we take the LAST tile's
 * cl_event as the completion token for the whole vector and bridge it to a
 * FUTURE via a detached waiter thread.
 */
static int
cl_submit(struct group *g, const xtc_batch_item_t *items, size_t n,
    xtc_batch_exec_t *exec)
{
	xtc_promise_t *prom;
	xtc_future_t *fut;
	struct cl_wait *w;
	cl_event ev = NULL;
	cl_int err;
	size_t i;

	if (!g->cl_ready)
		return XTC_E_IO;
	g->op_n = 0;
	for (i = 0; i < n; i++) {
		struct tile *t = items[i].request;
		cl_uint W = (cl_uint)t->img_w, H = (cl_uint)t->img_h;
		cl_uint tw = (cl_uint)t->tw, th = (cl_uint)t->th;
		cl_int tx = t->tx, ty = t->ty;
		cl_uint base = (cl_uint)((size_t)t->slot * TILE_PX_MAX);
		cl_uint cap = (cl_uint)((t->pass + 1) * PASS_ITERS);
		size_t gws = (size_t)tw * th;

		if (cap > (cl_uint)t->maxiter)
			cap = (cl_uint)t->maxiter;
		clSetKernelArg(g->kern, 0, sizeof g->dbuf, &g->dbuf);
		clSetKernelArg(g->kern, 1, sizeof W, &W);
		clSetKernelArg(g->kern, 2, sizeof H, &H);
		clSetKernelArg(g->kern, 3, sizeof tx, &tx);
		clSetKernelArg(g->kern, 4, sizeof ty, &ty);
		clSetKernelArg(g->kern, 5, sizeof tw, &tw);
		clSetKernelArg(g->kern, 6, sizeof th, &th);
		clSetKernelArg(g->kern, 7, sizeof base, &base);
		clSetKernelArg(g->kern, 8, sizeof cap, &cap);
		if (ev != NULL)
			clReleaseEvent(ev);
		err = clEnqueueNDRangeKernel(g->q, g->kern, 1, NULL, &gws, NULL,
		    0, NULL, &ev);
		if (err != CL_SUCCESS)
			return XTC_E_IO;
		g->op_tiles[g->op_n++] = t;
		t->pass++;
	}
	clFlush(g->q);
	if (xtc_future_new_pair(&prom, &fut) != XTC_OK) {
		if (ev != NULL)
			clReleaseEvent(ev);
		return XTC_E_NOMEM;
	}
	w = malloc(sizeof *w);
	if (w == NULL) {
		xtc_promise_drop(prom);
		if (ev != NULL)
			clReleaseEvent(ev);
		return XTC_E_NOMEM;
	}
	w->event = ev; /* waiter releases it after the wait */
	w->prom = prom;
	g->last_event = ev;
	exec->kind = XTC_BATCH_EXEC_FUTURE;
	exec->future = fut;
	{
		pthread_t th;
		if (pthread_create(&th, NULL, cl_waiter, w) != 0) {
			free(w);
			xtc_promise_drop(prom);
			if (ev != NULL)
				clReleaseEvent(ev);
			return XTC_E_IO;
		}
		pthread_detach(th);
	}
	return XTC_OK;
}

static void
cl_collect(struct group *g, const xtc_batch_item_t *items,
    xtc_batch_result_t *results, size_t n, int exec_status)
{
	size_t i;

	/* The waiter released the event already after resolving the promise;
	 * read back each tile's region from the shared device buffer. */
	if (g->last_event != NULL)
		g->last_event = NULL;
	for (i = 0; i < n; i++) {
		struct tile *t = items[i].request;
		size_t base = (size_t)t->slot * TILE_PX_MAX;
		size_t count = (size_t)t->tw * t->th;
		cl_int err;

		if (exec_status != XTC_BATCH_EXEC_OK) {
			results[i].state = XTC_BATCH_ERROR;
			results[i].error = XTC_E_IO;
			continue;
		}
		err = clEnqueueReadBuffer(g->q, g->dbuf, CL_TRUE,
		    base * sizeof(cl_uint), count * sizeof(cl_uint), t->iters,
		    0, NULL, NULL);
		if (err != CL_SUCCESS) {
			results[i].state = XTC_BATCH_ERROR;
			results[i].error = XTC_E_IO;
			continue;
		}
		if (tile_converged(t)) {
			t->done = 1;
			g->tiles_done++;
			results[i].state = XTC_BATCH_DONE;
		} else
			results[i].state = XTC_BATCH_CONTINUE;
	}
}

static void
cl_group_fini(struct group *g)
{
	if (!g->cl_ready)
		return;
	clReleaseMemObject(g->dbuf);
	clReleaseKernel(g->kern);
	clReleaseProgram(g->prog);
	clReleaseCommandQueue(g->q);
	clReleaseContext(g->ctx);
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
/* Batch glue: route the engine's submit/collect to the active backend.   */
/* ---------------------------------------------------------------------- */

static int
batch_submit(const xtc_batch_item_t *items, size_t n, void *user,
    xtc_batch_exec_t *exec)
{
	struct group *g = user;
	int rc;

	g->submits++;
	g->in_flight_sum += n;
	if ((unsigned)n > g->max_in_flight)
		g->max_in_flight = (unsigned)n;
	rc = g->demo->dev->submit(g, items, n, exec);
	return rc;
}

static int
batch_collect(const xtc_batch_item_t *items, xtc_batch_result_t *results,
    size_t n, int exec_status, void *user)
{
	struct group *g = user;

	g->collects++;
	g->demo->dev->collect(g, items, results, n, exec_status);
	return XTC_OK;
}

static void
batch_complete(uint64_t handle, void *request, int outcome, void *user)
{
	struct group *g = user;
	struct tile *t = request;

	if (handle != t->handle || ++t->completions != 1)
		die("exactly-once terminal completion");
	t->outcome = outcome;
	if (outcome == XTC_OK)
		g->demo->completed++;
	else if (outcome == XTC_E_ABORTED)
		g->demo->cancelled++;
	else
		g->demo->failed++;
}

/* ---------------------------------------------------------------------- */
/* Tile setup: deterministic, varying-depth tiles.                        */
/* ---------------------------------------------------------------------- */

static void
build_tiles(struct demo *d)
{
	int tw = d->img_w / d->tiles_x;
	int th = d->img_h / d->tiles_y;
	int i = 0, ty, tx;

	if (tw * th > TILE_PX_MAX)
		die("tile too large (raise TILE_PX_MAX or lower tiles)");
	for (ty = 0; ty < d->tiles_y; ty++) {
		for (tx = 0; tx < d->tiles_x; tx++) {
			struct tile *t = &d->tiles[i];
			t->id = i;
			t->slot = i; /* mock KV-slot id = device buffer region */
			t->tx = tx * tw;
			t->ty = ty * th;
			t->tw = tw;
			t->th = th;
			t->img_w = d->img_w;
			t->img_h = d->img_h;
			t->maxiter = d->maxiter;
			i++;
		}
	}
	d->ntiles = i;
}

/* ---------------------------------------------------------------------- */
/* Driver proc: feeds a group, continuous or static.                      */
/* ---------------------------------------------------------------------- */

struct feed_arg {
	struct demo *d;
	struct group *g;
	int first_tile, last_tile; /* this group's slice of tiles [first,last) */
};

/*
 * Continuous feeder: submit up to `capacity` tiles, then as each completes
 * (a free slot opens) submit the next pending tile, keeping the batch full.
 * Static feeder: submit one cohort of `max_active` tiles, wait for ALL to
 * finish, then submit the next cohort -- the control that under-fills.
 */
static int
group_pending_done(struct group *g, int first, int last)
{
	struct demo *d = g->demo;
	int i, done = 0;

	for (i = first; i < last; i++)
		if (d->tiles[i].completions == 1)
			done++;
	return done == (last - first);
}

#define GROUP_CAP 8   /* admitted live tiles per group */
#define GROUP_ACTIVE 4 /* max tiles per device op (vector width) */

static void
feeder(void *arg)
{
	struct feed_arg *fa = arg;
	struct demo *d = fa->d;
	struct group *g = fa->g;
	int next = fa->first_tile, admitted = 0;

	if (d->continuous) {
		/* Prime the batch to capacity, then refill as tiles complete. */
		while (next < fa->last_tile && admitted < GROUP_CAP) {
			must(xtc_batch_submit(g->batch, &d->tiles[next],
			    1, &d->tiles[next].handle), "submit");
			next++;
			admitted++;
		}
		while (!group_pending_done(g, fa->first_tile, fa->last_tile)) {
			/* Refill freed slots with pending tiles. */
			while (next < fa->last_tile) {
				int rc = xtc_batch_submit(g->batch,
				    &d->tiles[next], 1, &d->tiles[next].handle);
				if (rc == XTC_E_AGAIN)
					break; /* batch full, wait for drain */
				must(rc, "submit refill");
				next++;
			}
			xtc_yield();
		}
	} else {
		/* Static: cohort of GROUP_ACTIVE at a time, drain fully. */
		while (next < fa->last_tile) {
			int cohort = 0;
			int cohort_start = next;
			while (next < fa->last_tile && cohort < GROUP_ACTIVE) {
				must(xtc_batch_submit(g->batch,
				    &d->tiles[next], 1,
				    &d->tiles[next].handle), "submit cohort");
				next++;
				cohort++;
			}
			while (!group_pending_done(g, cohort_start, next))
				xtc_yield();
		}
	}
	must(xtc_batch_close(g->batch, XTC_BATCH_DRAIN), "close");
}

/* ---------------------------------------------------------------------- */
/* Run.                                                                   */
/* ---------------------------------------------------------------------- */

struct run_stats {
	unsigned total_submits;    /* device ops over all groups */
	uint64_t total_members;    /* sum of per-op member counts */
	unsigned tiles_done;
	double avg_occupancy;      /* members per op (continuous keeps high) */
	unsigned per_group_done[MAX_GROUPS];
	unsigned per_group_submits[MAX_GROUPS];
};

static struct feed_arg feed_args[MAX_GROUPS];

static struct run_stats
run(struct demo *d)
{
	xtc_exec_t *exec;
	xtc_exec_class_t cls;
	xtc_pid_t pid;
	struct run_stats rs = {0};
	xtc_batch_async_opts_t opts;
	int i, slice, per;

	must(xtc_exec_init(&exec, d->ngroups), "exec_init");
	build_tiles(d);
	/* Partition tiles across groups (round-robin would interleave depth;
	 * contiguous slices keep each group's workload reproducible). */
	per = (d->ntiles + d->ngroups - 1) / d->ngroups;
	for (i = 0; i < d->ngroups; i++) {
		struct group *g = &d->groups[i];
		g->demo = d;
		g->loop = xtc_exec_loop(exec, i);
		must(g->demo->dev->group_init(g, i), "group_init");
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
		must(xtc_batch_create_async(g->loop, &opts, &g->batch),
		    "create_async");
	}
	/* Spawn one feeder per group on its own loop. */
	for (i = 0; i < d->ngroups; i++) {
		slice = i * per;
		feed_args[i].d = d;
		feed_args[i].g = &d->groups[i];
		feed_args[i].first_tile = slice;
		feed_args[i].last_tile = slice + per < d->ntiles ?
		    slice + per : d->ntiles;
		must(xtc_proc_spawn(xtc_exec_loop(exec, i), feeder,
		    &feed_args[i], NULL, &pid), "spawn feeder");
	}
	must(xtc_exec_run(exec), "exec_run");

	/* Collect stats. */
	for (i = 0; i < d->ngroups; i++) {
		struct group *g = &d->groups[i];
		rs.total_submits += g->submits;
		rs.total_members += g->in_flight_sum;
		rs.tiles_done += g->tiles_done;
		rs.per_group_done[i] = g->tiles_done;
		rs.per_group_submits[i] = g->submits;
		must(xtc_batch_destroy(g->batch), "destroy");
		g->demo->dev->group_fini(g);
	}
	if (rs.total_submits > 0)
		rs.avg_occupancy = (double)rs.total_members /
		    (double)rs.total_submits;
	must(xtc_exec_fini(exec), "exec_fini");
	return rs;
}

/* ---------------------------------------------------------------------- */
/* Checksum verification.                                                 */
/* ---------------------------------------------------------------------- */

/*
 * Exact integer checksum: sum of final escape-iteration counts over every
 * pixel of every tile. Integer and deterministic on the CPU; the GPU float
 * path may differ by a handful of boundary pixels (documented tolerance).
 */
static uint64_t
checksum_tiles(const struct demo *d)
{
	uint64_t sum = 0;
	int i, k, n;

	for (i = 0; i < d->ntiles; i++) {
		n = d->tiles[i].tw * d->tiles[i].th;
		for (k = 0; k < n; k++)
			sum += d->tiles[i].iters[k];
	}
	return sum;
}

/* Scalar reference checksum: independent full-escape computation. */
static uint64_t
checksum_reference(const struct demo *d)
{
	uint64_t sum = 0;
	int i, px, py;

	for (i = 0; i < d->ntiles; i++) {
		const struct tile *t = &d->tiles[i];
		for (py = 0; py < t->th; py++)
			for (px = 0; px < t->tw; px++)
				sum += mandel_pixel(t->tx + px, t->ty + py,
				    t->img_w, t->img_h, t->maxiter);
	}
	return sum;
}

/* ---------------------------------------------------------------------- */
/* Self-test (FAKE backend, no GPU needed, deterministic).                */
/* ---------------------------------------------------------------------- */

static struct demo *
demo_new(int continuous, int devices, int w, int h, int tiles, int maxiter)
{
	struct demo *d = calloc(1, sizeof *d);

	if (d == NULL)
		die("oom");
	d->dev = &fake_device;
	d->ngroups = devices;
	d->continuous = continuous;
	d->img_w = w;
	d->img_h = h;
	d->maxiter = maxiter;
	d->tiles_x = tiles;
	d->tiles_y = tiles;
	return d;
}

static void
self_test(void)
{
	struct demo *dc, *ds;
	struct run_stats rc, rsc;
	uint64_t sum_c, sum_s, ref;
	int i, shallow_order, deep_order;

	/* 1. Determinism + exactly-once + checksum (continuous, FUTURE). */
	fake_use_sync = 0;
	dc = demo_new(1, 1, 128, 128, 4, 256); /* 16 tiles, 1 group */
	rc = run(dc);
	if (dc->completed != (unsigned)dc->ntiles || dc->cancelled != 0 ||
	    dc->failed != 0)
		die("every tile completes exactly once");
	for (i = 0; i < dc->ntiles; i++)
		if (dc->tiles[i].completions != 1 || !dc->tiles[i].done)
			die("tile done exactly once");
	sum_c = checksum_tiles(dc);
	ref = checksum_reference(dc);
	if (sum_c != ref)
		die("fake checksum != scalar reference");

	/* 2. Variable-length: deep tiles need more passes than shallow ones.
	 *    The centre tiles (inside the set) run all maxiter/PASS_ITERS
	 *    passes; corner tiles escape in one pass. Assert the spread. */
	{
		int min_pass = 1 << 30, max_pass = 0;
		for (i = 0; i < dc->ntiles; i++) {
			if (dc->tiles[i].pass < min_pass)
				min_pass = dc->tiles[i].pass;
			if (dc->tiles[i].pass > max_pass)
				max_pass = dc->tiles[i].pass;
		}
		if (!(min_pass == 1 && max_pass > min_pass))
			die("variable-length tiles (shallow=1 pass, deep>1)");
	}

	/* 3. Continuous vs static occupancy: same work, continuous keeps the
	 *    device vector fuller (higher members-per-op). */
	fake_use_sync = 0;
	ds = demo_new(0, 1, 128, 128, 4, 256);
	rsc = run(ds);
	sum_s = checksum_tiles(ds);
	if (sum_s != ref)
		die("static checksum != reference");
	if (!(rc.avg_occupancy > rsc.avg_occupancy))
		die("continuous occupancy must exceed static");

	/* 4. SYNC exec kind also works and matches. */
	fake_use_sync = 1;
	{
		struct demo *dy = demo_new(1, 1, 128, 128, 4, 256);
		uint64_t s;
		(void)run(dy);
		s = checksum_tiles(dy);
		if (s != ref)
			die("SYNC-kind checksum != reference");
		free(dy);
	}
	fake_use_sync = 0;

	/* 5. Shallow-before-deep completion order (vLLM shape): the shallowest
	 *    tile terminates at an earlier ordinal than the deepest. */
	shallow_order = deep_order = 0;
	{
		int min_pass = 1 << 30, max_pass = 0, shallow = 0, deep = 0;
		for (i = 0; i < dc->ntiles; i++) {
			if (dc->tiles[i].pass < min_pass) {
				min_pass = dc->tiles[i].pass;
				shallow = i;
			}
			if (dc->tiles[i].pass > max_pass) {
				max_pass = dc->tiles[i].pass;
				deep = i;
			}
		}
		shallow_order = dc->tiles[shallow].pass;
		deep_order = dc->tiles[deep].pass;
		if (!(shallow_order < deep_order))
			die("shallow tile finishes in fewer passes than deep");
	}

	printf("self-test checksum=%llu (reference=%llu)\n",
	    (unsigned long long)sum_c, (unsigned long long)ref);
	printf("continuous occupancy=%.2f members/op, static=%.2f members/op\n",
	    rc.avg_occupancy, rsc.avg_occupancy);
	printf("variable passes: shallow=%d deep=%d\n", shallow_order,
	    deep_order);
	puts("gpu batching self-test: PASS");
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
	int i, devices = 1, w = 256, h = 256, tiles = 4, maxiter = 256;
	int continuous = 1, test = 0, use_opencl = 0;
	const char *backend = "fake";
	struct demo *d;
	struct run_stats rs;

	for (i = 1; i < argc; i++) {
		char *a = argv[i];
		if (strcmp(a, "--help") == 0) {
			puts(
"usage: mandel [options]\n"
"  --backend=fake|opencl   device backend (default fake; opencl needs a GPU)\n"
"  --devices=N             number of groups/GPUs (default 1, max 2)\n"
"  --mode=continuous|static  batching mode (default continuous)\n"
"  --width=N --height=N    image size in pixels (default 256x256)\n"
"  --tiles=N               tiles per axis (NxN tiles total; default 4)\n"
"  --maxiter=N             escape-iteration cap (default 256)\n"
"  --self-test             deterministic fake-backend checks, no GPU\n"
"  --help                  this message\n"
"Build the opencl backend with XTC_WITH_OPENCL=1 (links -lOpenCL).\n"
"Requires libxtc configured with --enable-sched-policy.");
			return 0;
		} else if (strcmp(a, "--self-test") == 0)
			test = 1;
		else if (strcmp(a, "--backend=fake") == 0)
			backend = "fake";
		else if (strcmp(a, "--backend=opencl") == 0) {
			backend = "opencl";
			use_opencl = 1;
		} else if (strcmp(a, "--mode=continuous") == 0)
			continuous = 1;
		else if (strcmp(a, "--mode=static") == 0)
			continuous = 0;
		else if (arg_int(a, "--devices", &devices)) {
		} else if (arg_int(a, "--width", &w)) {
		} else if (arg_int(a, "--height", &h)) {
		} else if (arg_int(a, "--tiles", &tiles)) {
		} else if (arg_int(a, "--maxiter", &maxiter)) {
		} else {
			fprintf(stderr, "unknown argument: %s (try --help)\n", a);
			return 2;
		}
	}
	if (test) {
		self_test();
		return 0;
	}
	if (devices < 1 || devices > MAX_GROUPS) {
		fprintf(stderr, "--devices must be 1..%d\n", MAX_GROUPS);
		return 2;
	}
	d = demo_new(continuous, devices, w, h, tiles, maxiter);
	if (use_opencl) {
#ifdef XTC_WITH_OPENCL
		d->dev = &opencl_device;
#else
		fprintf(stderr, "opencl backend not built "
		    "(rebuild with XTC_WITH_OPENCL=1)\n");
		return 2;
#endif
	}
	rs = run(d);
	printf("backend=%s mode=%s devices=%d tiles=%d maxiter=%d\n",
	    backend, continuous ? "continuous" : "static", devices,
	    d->ntiles, maxiter);
	printf("tiles_done=%u device_ops=%u avg_occupancy=%.2f members/op\n",
	    rs.tiles_done, rs.total_submits, rs.avg_occupancy);
	for (i = 0; i < devices; i++)
		printf("  group %d: tiles_done=%u device_ops=%u\n",
		    i, rs.per_group_done[i], rs.per_group_submits[i]);
	if (!use_opencl) {
		uint64_t sum = checksum_tiles(d), ref = checksum_reference(d);
		printf("checksum=%llu reference=%llu %s\n",
		    (unsigned long long)sum, (unsigned long long)ref,
		    sum == ref ? "OK" : "MISMATCH");
	} else {
		printf("checksum=%llu (GPU float path; compare to fake-backend "
		    "reference, small boundary differences expected)\n",
		    (unsigned long long)checksum_tiles(d));
	}
	free(d);
	return 0;
}

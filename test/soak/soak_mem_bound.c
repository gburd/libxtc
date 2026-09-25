/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/soak/soak_mem_bound.c -- PLAN 19.27.20: a memory-BOUND soak.
 *
 *	N generations of short-lived work on ONE long-lived loop.  Every
 *	generation spawns K workers, lets them all finish, and returns the
 *	loop to idle -- so the CONCURRENT footprint is identical in every
 *	generation, and a bounded runtime must plateau.  The soak measures,
 *	after each generation,
 *
 *	  heap  -- live bytes allocated through the libxtc allocator hook
 *	           (malloc_usable_size of every block; exact, no sampling);
 *	  map   -- VmSize, which is where mmap'd fiber stacks show up;
 *	  rss   -- VmRSS,
 *
 *	fits growth from a warm-up generation to the last one, prints the
 *	bytes retained PER GENERATION and PER UNIT OF WORK for each, and
 *	fails if the heap or the mapping grows by more than a small slack.
 *
 *	Modes (argv[1]):
 *	  procs  -- each worker is an xtc_proc that calls xtc_proc_sleep once
 *	            (the long-lived-service shape PLAN 19.26 describes).
 *	            STILL EXPECTED TO FAIL: a completed coro-backed task keeps
 *	            its fiber stack + coro until xtc_loop_fini (PLAN 19.26).
 *	            The park-timer half of this arm's old growth is fixed --
 *	            park timers are now reclaimed when they fire or cancel --
 *	            so the residual growth here is the fiber stack alone.
 *	  tasks  -- plain xtc_task workers that park once on a timer and
 *	            finish.  Plain tasks recycle, so only the park timers
 *	            could grow here.  Now PLATEAUS: the ~80 B/park retention
 *	            was fixed (park timers are refcounted and freed on fire or
 *	            cancel instead of living on all_timers until loop_fini).
 *	  plain  -- the CONTROL: plain xtc_task workers that finish without
 *	            parking.  Must plateau; if it does not, the instrument is
 *	            wrong or there is a third leak.
 *
 *	NOT part of `make check` (it is a slow soak and it is red by design
 *	until 19.26 lands).  CI runs it nightly as a non-gating job; build
 *	and run it by hand with
 *
 *	    make soak-mem          (from a configured build dir)
 *	    ./soak_mem_bound procs|tasks|plain [generations] [workers/gen]
 *
 *	Exit 0 = bounded, 1 = growth above the slack, 2 = setup error.
 */

#define _GNU_SOURCE

#include <malloc.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_slab.h"
#include "os_alloc.h"

/* ---- exact heap accounting through the allocator hook --------------- */

static struct __os_alloc_hook g_real;
static _Atomic long long g_heap;

static void *
c_malloc(size_t n)
{
	void *p = g_real.malloc(n);
	if (p) atomic_fetch_add(&g_heap, (long long)malloc_usable_size(p));
	return p;
}
static void *
c_calloc(size_t n, size_t sz)
{
	void *p = g_real.calloc(n, sz);
	if (p) atomic_fetch_add(&g_heap, (long long)malloc_usable_size(p));
	return p;
}
static void *
c_realloc(void *o, size_t sz)
{
	long long before = o ? (long long)malloc_usable_size(o) : 0;
	void *p = g_real.realloc(o, sz);
	if (p) atomic_fetch_add(&g_heap,
	    (long long)malloc_usable_size(p) - before);
	return p;
}
static void
c_free(void *p)
{
	if (p) atomic_fetch_sub(&g_heap, (long long)malloc_usable_size(p));
	g_real.free(p);
}
static void *
c_aligned(size_t a, size_t sz)
{
	void *p = g_real.aligned(a, sz);
	if (p) atomic_fetch_add(&g_heap, (long long)malloc_usable_size(p));
	return p;
}
static void
c_aligned_free(void *p)
{
	if (p) atomic_fetch_sub(&g_heap, (long long)malloc_usable_size(p));
	g_real.aligned_free(p);
}

static long long
status_kb(const char *key)
{
	char line[256];
	long long v = -1;
	size_t kl = strlen(key);
	FILE *f = fopen("/proc/self/status", "r");
	if (f == NULL) return -1;
	while (fgets(line, sizeof line, f) != NULL)
		if (strncmp(line, key, kl) == 0 && line[kl] == ':') {
			v = strtoll(line + kl + 1, NULL, 10);
			break;
		}
	fclose(f);
	return v;
}

/* ---- workloads ------------------------------------------------------- */

static void
proc_worker(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(1000);   /* one park timer, then exit */
}

static int
plain_worker(xtc_task_t *self, void *u)
{
	(void)self; (void)u;
	return XTC_TASK_DONE;
}

static int
task_worker(xtc_task_t *self, void *u)
{
	int *parked = u;
	if (*parked == 0) {
		*parked = 1;
		if (xtc_task_park_on_timer(self, 1000) == XTC_OK)
			return XTC_TASK_PENDING;
	}
	return XTC_TASK_DONE;
}

enum { M_PROCS, M_TASKS, M_PLAIN };

static int
generation(xtc_loop_t *loop, int mode, int k)
{
	static int *parked;
	int i;
	if (parked == NULL && (parked = calloc((size_t)k, sizeof *parked)) == NULL)
		return -1;
	memset(parked, 0, (size_t)k * sizeof *parked);
	for (i = 0; i < k; i++) {
		int rc = mode == M_PROCS
		    ? xtc_proc_spawn(loop, proc_worker, NULL, NULL, NULL)
		    : xtc_task_spawn(loop, mode == M_TASKS ? task_worker :
		        plain_worker, &parked[i], NULL);
		if (rc != XTC_OK) return -1;
	}
	return xtc_loop_run(loop) == XTC_OK ? 0 : -1;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "procs";
	int gens = argc > 2 ? atoi(argv[2]) : 200;
	int k = argc > 3 ? atoi(argv[3]) : 100;
	int m, g, warm;
	struct __os_alloc_hook h;
	xtc_loop_t *loop = NULL;
	long long heap0 = 0, map0 = 0, rss0 = 0, heap1, map1, rss1;
	double dh, dm, dr, per;
	/* Slack: bytes of growth per generation tolerated as noise (malloc
	 * arena shape, slab chunk rounding).  A retention of even one
	 * 64-byte node per unit of work at k=100 is 6400 B/gen, well above. */
	const double slack_per_gen = 512.0;
	int fail = 0;

	if (strcmp(mode, "procs") == 0) m = M_PROCS;
	else if (strcmp(mode, "tasks") == 0) m = M_TASKS;
	else if (strcmp(mode, "plain") == 0) m = M_PLAIN;
	else {
		fprintf(stderr, "usage: %s procs|tasks|plain [generations] [k]\n",
		    argv[0]);
		return 2;
	}
	if (gens < 20 || k < 1) {
		fprintf(stderr, "need >= 20 generations and k >= 1\n");
		return 2;
	}
	/* Baseline at the midpoint, after every bounded cache has filled:
	 * a plain task spawned from OFF the loop thread never pops the
	 * loop's task free list, so the list fills to its cap
	 * (XTC_TASK_FREELIST_MAX = 4096 structs, ~550 KB) before it
	 * plateaus.  The defaults (200 x 100) put 10000 units of work
	 * before the baseline. */
	warm = gens / 2;

	if (__os_alloc_get_hook(&g_real) != XTC_OK) return 2;
	h.malloc = c_malloc; h.calloc = c_calloc; h.realloc = c_realloc;
	h.free = c_free; h.aligned = c_aligned; h.aligned_free = c_aligned_free;
	if (__os_alloc_set_hook(&h) != XTC_OK) return 2;
	if (xtc_loop_init(&loop) != XTC_OK) return 2;

	printf("soak_mem_bound: mode=%s generations=%d workers/gen=%d "
	    "(backend %s)\n", mode, gens, k, xtc_io_backend_name());
	for (g = 1; g <= gens; g++) {
		if (generation(loop, m, k) != 0) {
			fprintf(stderr, "generation %d failed\n", g);
			return 2;
		}
		(void)xtc_slab_reap_all();
		if (g == warm) {
			heap0 = atomic_load(&g_heap);
			map0 = status_kb("VmSize");
			rss0 = status_kb("VmRSS");
		}
		if (g % (gens / 10) == 0)
			printf("  gen %4d: heap %10lld B  VmSize %8lld kB  "
			    "VmRSS %8lld kB\n", g, (long long)atomic_load(&g_heap),
			    status_kb("VmSize"), status_kb("VmRSS"));
	}
	heap1 = atomic_load(&g_heap);
	map1 = status_kb("VmSize");
	rss1 = status_kb("VmRSS");
	per = (double)(gens - warm);
	dh = (double)(heap1 - heap0) / per;
	dm = (double)(map1 - map0) * 1024.0 / per;
	dr = (double)(rss1 - rss0) * 1024.0 / per;

	printf("RESULT mode=%s: retained per generation (gen %d..%d):\n",
	    mode, warm, gens);
	printf("  heap   %+10.0f B/gen  (%+8.1f B per %s)\n", dh, dh / k,
	    m == M_PROCS ? "proc" : "task");
	printf("  VmSize %+10.0f B/gen  (%+8.1f B per %s)\n", dm, dm / k,
	    m == M_PROCS ? "proc" : "task");
	printf("  VmRSS  %+10.0f B/gen  (%+8.1f B per %s)\n", dr, dr / k,
	    m == M_PROCS ? "proc" : "task");

	if (dh > slack_per_gen) {
		printf("FAIL: heap grows %.0f B per generation under a constant "
		    "concurrent load -- %s\n", dh, m == M_PROCS ?
		    "park timers stay on all_timers until loop_fini, and "
		    "completed procs keep their coro (PLAN 19.26)" :
		    m == M_TASKS ? "each park timer stays on the loop's "
		    "all_timers until loop_fini" :
		    "the CONTROL grew: an instrument error or a new leak");
		fail = 1;
	}
	if (dm > slack_per_gen) {
		printf("FAIL: address space grows %.0f B per generation -- %s\n",
		    dm, m == M_PROCS ? "completed procs keep their fiber stack "
		    "until loop_fini (PLAN 19.26)" :
		    "heap growth above (no fiber stacks in this mode)");
		fail = 1;
	}
	if (!fail)
		printf("OK: memory plateaued (heap and mapping within %.0f "
		    "B/gen)\n", slack_per_gen);

	(void)xtc_loop_fini(loop);
	(void)__os_alloc_set_hook(&g_real);
	return fail;
}

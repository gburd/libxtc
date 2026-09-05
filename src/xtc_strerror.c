/*-
 * Copyright (c) 2026, The XTC Project
 *
 * Use of this source code is governed by the ISC License.
 *
 * src/xtc_strerror.c
 *	Stable English descriptions of XTC_E_* codes.
 *	The text is part of the API contract for log-message
 *	stability (PLAN.md (S)18.7); changes follow the same
 *	deprecation lifecycle as function signatures.
 */

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "os_alloc.h"
#include "os_time.h"
#include "os_atomic.h"
#include "os_thread.h"   /* XTC_THREAD_LOCAL */
#include "os_sharp.h"
#include "os_cpu.h"      /* CPU/NUMA topology, exposed as xtc_* below */

/*
 * PUBLIC: const char *xtc_strerror __P((int));
 */
const char *
xtc_strerror(int e)
{
	switch ((xtc_err_t)e) {
	case XTC_OK:		return "ok";
	case XTC_E_INVAL:	return "invalid argument";
	case XTC_E_NOMEM:	return "out of memory";
	case XTC_E_NOSYS:	return "not implemented on this platform";
	case XTC_E_RANGE:	return "numeric out of range";
	case XTC_E_AGAIN:	return "resource temporarily unavailable";
	case XTC_E_INTERNAL:	return "internal invariant violation";
	case XTC_E_RESOURCE:	return "resource cap reached";
	case XTC_E_DEADLK:	return "deadlock victim";
	case XTC_E_VERSION:	return "version mismatch";
	case XTC_E_ABORTED:	return "operation aborted";
	case XTC_E_NOTFOUND:	return "requested item does not exist";
	case XTC_E_IO:		return "I/O error";
	}
	return "unknown";
}

/*
 * Public deallocation entry point for buffers libxtc hands the caller
 * to own (xtc_recv / xtc_net_recv_frame / xtc_osproc_call / xtc_svr_call
 * results, etc.).  A thin wrapper over the library allocator so the
 * caller need not (and must not) reach the internal __os_free directly.
 * NULL is a no-op.  Declared in xtc.h (the umbrella public header).
 */
void
xtc_free(void *p)
{
	__os_free(p);
}

void *
xtc_malloc(size_t size)
{
	void *p = NULL;
	if (__os_malloc(size, &p) != XTC_OK)
		return NULL;
	return p;
}

void *
xtc_calloc(size_t n, size_t size)
{
	void *p = NULL;
	if (__os_calloc(n, size, &p) != XTC_OK)
		return NULL;
	return p;
}

void *
xtc_realloc(void *p, size_t size)
{
	void *q = NULL;
	if (__os_realloc(p, size, &q) != XTC_OK)
		return NULL;
	return q;
}

void *
xtc_aligned_alloc(size_t align, size_t size)
{
	void *p = NULL;
	if (__os_aligned_alloc(align, size, &p) != XTC_OK)
		return NULL;
	return p;
}

void
xtc_aligned_free(void *p)
{
	__os_aligned_free(p);
}

int64_t
xtc_clock_mono(void)
{
	int64_t ns = 0;
	(void)__os_clock_mono(&ns);
	return ns;
}

int64_t
xtc_clock_real(void)
{
	int64_t ns = 0;
	(void)__os_clock_real(&ns);
	return ns;
}

int
xtc_sleep_ns(int64_t ns)
{
	return __os_sleep_ns(ns);
}

int64_t
xtc_atomic_i64_load(const int64_t *p)
{
	return __os_atomic_load_i64((int64_t *)p);
}

int64_t
xtc_atomic_i64_add(int64_t *p, int64_t delta)
{
	return __os_atomic_fetch_add_i64(p, delta);
}

/*
 * Per-thread return buffer for xtc_env_get: the value is copied here
 * under the environment lock so the pointer cannot be invalidated by a
 * setenv from another thread.  Valid until the next xtc_env_get on the
 * same thread.  1024 bytes covers PATH-sized values; longer values are
 * truncated (still NUL-terminated).
 */
static XTC_THREAD_LOCAL char xtc_env_buf[1024];

const char *
xtc_env_get(const char *name)
{
	if (__os_env_get(name, xtc_env_buf, sizeof xtc_env_buf) != XTC_OK)
		return NULL;
	return xtc_env_buf;
}

int
xtc_env_set(const char *name, const char *value, int overwrite)
{
	return __os_env_set(name, value, overwrite);
}

uint64_t
xtc_rand_u64(void)
{
	return __os_rand_u64();
}

void
xtc_rand_seed(uint64_t seed)
{
	__os_rand_seed(seed);
}

size_t
xtc_strlcpy(char *dst, const char *src, size_t dstsize)
{
	return __os_strlcpy(dst, src, dstsize);
}

size_t
xtc_strlcat(char *dst, const char *src, size_t dstsize)
{
	return __os_strlcat(dst, src, dstsize);
}

/*
 * CPU / NUMA topology.
 *
 * Public names for the topology queries a consumer needs when it wants to
 * shard state per core or per NUMA node (a buffer pool, a partitioned
 * index, a per-core free list).  These exist because AGENTS.md rule 3
 * says a consumer must use only the public xtc_* surface: before this,
 * examples/06_sqlxtc reached into the internal __os_ncpus /
 * __os_numa_node_of_cpu, which is exactly the "if a consumer needs a
 * primitive, add the public xtc_* for it" case.
 *
 * All are cheap, cached queries and safe to call from any thread.
 * xtc_numa_nnodes() returns 1 on a non-NUMA (or unqueryable) system, and
 * xtc_numa_node_of_cpu() returns 0 there -- so a caller can always shard
 * by the returned node without special-casing.
 */
int
xtc_ncpus(void)
{
	return __os_ncpus();
}

int
xtc_numa_nnodes(void)
{
	return __os_numa_nnodes();
}

int
xtc_numa_node_of_cpu(int cpu)
{
	return __os_numa_node_of_cpu(cpu);
}

int
xtc_numa_current_node(void)
{
	return __os_numa_current_node();
}

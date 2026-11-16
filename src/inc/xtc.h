/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 *
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc.h
 *	The base public header: version, error codes, xtc_strerror, the
 *	allocator, clocks, environment, random numbers, strings and CPU
 *	topology.  Every other public header includes it.  Each subsystem
 *	has its own header (xtc_loop.h, xtc_proc.h, ...).
 */

#ifndef XTC_H
#define XTC_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h> /* int64_t for the public clock / atomic helpers */

/* Portable compiler directives and atomics on plain storage
 * (xtc_atomic(3)): static inline functions and macros, C only (a C++
 * translation unit sees neither). */
#include "xtc_compiler.h"
#include "xtc_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile-time version macros, and what the build selected: its TLS and
 * I/O backends and ucontext.  An installed libxtc carries them in
 * xtc_version.h, which configure (or meson) generates from
 * dist/xtc_version.h.in; the library's own build also has its private
 * xtc_config.h, which defines the same names with the same values.  A
 * header used without either (parsed on its own, or a hand-made build)
 * falls back to known-bad sentinels, so a missing build is detected
 * loudly.
 */
#if defined(__has_include)
# if __has_include("xtc_version.h")
#  include "xtc_version.h"
# endif
# if __has_include("xtc_config.h")
#  include "xtc_config.h"
# endif
#endif

#ifndef XTC_VERSION_MAJOR
# define XTC_VERSION_MAJOR	0
#endif
#ifndef XTC_VERSION_MINOR
# define XTC_VERSION_MINOR	0
#endif
#ifndef XTC_VERSION_PATCH
# define XTC_VERSION_PATCH	0
#endif
#ifndef XTC_VERSION_STRING
# define XTC_VERSION_STRING	"0.0.0-unconfigured"
#endif

/*
 * Error codes.
 * 0 == OK; negative values are stable XTC_E_* codes.
 * Codes are added at the end of the enumeration in minor releases and
 * never renumbered.  See docs/abi-stability.md.
 */
typedef enum xtc_err {
	XTC_OK			=  0,	/* success */
	XTC_E_INVAL		= -1,	/* invalid argument */
	XTC_E_NOMEM		= -2,	/* out of memory */
	XTC_E_NOSYS		= -3,	/* not implemented on this platform */
	XTC_E_RANGE		= -4,	/* numeric out of range */
	XTC_E_AGAIN		= -5,	/* try again later */
	XTC_E_INTERNAL		= -6,	/* invariant violation; bug */
	XTC_E_RESOURCE		= -7,	/* resource cap reached (xtc_res) */
	XTC_E_DEADLK		= -8,	/* lock-manager: deadlock victim */
	XTC_E_VERSION		= -9,	/* version mismatch (shm) */
	XTC_E_ABORTED		= -10,	/* operation cancelled via abort token */
	XTC_E_NOTFOUND		= -11,	/* requested item does not exist */
	XTC_E_IO		= -12,	/* I/O error (read/write/fsync failed) */
	XTC_E_EXIST		= -13,	/* already exists (a name is taken); 2.2 */
	XTC_E_STATE		= -14	/* not allowed in this state or calling
					 * context; 2.2 */
} xtc_err_t;

/*
 * The system-error band (2.2).  A call that fails with an operating
 * system error the XTC_E_* codes do not name returns XTC_E_SYS(errno):
 * XTC_E_SYS_BASE minus the errno, so the band lies below every XTC_E_*
 * code and a raw negative errno (which collides with them: -EIO is
 * XTC_E_AGAIN, -EBADF is XTC_E_VERSION) never leaves the library.
 * xtc_err_errno(rc) recovers the errno; xtc_strerror renders the band as
 * "system error: <the C library's text>".
 */
#define XTC_E_SYS_BASE (-4096)
#define XTC_E_SYS(e) (XTC_E_SYS_BASE - (e))

/*
 * xtc_err_errno --
 *	The errno inside the system-error band: e for rc == XTC_E_SYS(e),
 *	e > 0, and 0 for any other rc (XTC_OK, an XTC_E_* code, a count).
 *	The one-call migration for a caller that compared an xtc_aio_*
 *	result against -EIO: `xtc_err_errno(rc) == EIO`.
 *
 * xtc_err_from_errno --
 *	XTC_E_SYS(e) for an errno e given either sign (e or -e); XTC_OK for
 *	0.  A value too large to fit the band is XTC_E_INTERNAL.  For
 *	callers that wrap their own system calls in the same space.
 */
XTC_API int xtc_err_errno(int rc);
XTC_API int xtc_err_from_errno(int neg_or_pos_errno);

/*
 * Size-first structs (docs/abi-stability.md, "Caller-allocated structs").
 * Every public option / info / stats struct the caller allocates starts
 * with `size_t size`, which the caller sets to sizeof its view of the
 * struct -- the struct's XTC_*_INIT / *_DEFAULT initializer does it.  The
 * library reads an input struct only up to `size` (fields past it take
 * their defaults) and writes an output struct only up to `size`; size 0 is
 * XTC_E_INVAL.  Fields are only ever APPENDED in a minor release.
 *
 * XTC_FIELD_OK(p, field) -- nonzero when `field` lies inside p's declared
 * size.  A consumer reading a struct the LIBRARY sized (one handed to a
 * callback) checks it before using a field newer than its oldest supported
 * library: `if (XTC_FIELD_OK(info, mask_depth)) use(info->mask_depth);`.
 */
#define XTC_FIELD_OK(p, field) \
	((size_t)((const char *)&(p)->field - (const char *)(p)) + \
	 sizeof((p)->field) <= (p)->size)

/*
 * xtc_version_string --
 *	Return the library version as a NUL-terminated SemVer 2.0 string.
 *	The pointer is to static storage; the caller must not free it.
 */
XTC_API const char *xtc_version_string(void);

/*
 * xtc_version_components --
 *	Decompose the version into three integers.
 *	On success returns XTC_OK and writes *major, *minor, *patch.
 *	On NULL out-pointers returns XTC_E_INVAL.
 */
XTC_API int xtc_version_components(int *major, int *minor, int *patch);

/*
 * xtc_strerror --
 *	Return a stable English description of an xtc error code.
 *	The pointer is to static storage; the caller must not free it.
 *	Returns a pointer to "unknown" for codes outside the known set
 *	rather than NULL, so callers can chain into log lines safely.
 *	A code in the system-error band (XTC_E_SYS(e)) reads
 *	"system error: " and the C library's text for e; that string is
 *	per thread and valid until the thread's next xtc_strerror call.
 *	xtc_strerror(3) lists every code and its text.
 */
XTC_API const char *xtc_strerror(int xtc_err);

/*
 * xtc_free --
 *	Free a heap buffer that a libxtc call handed to the caller to own
 *	-- notably the message buffer from xtc_recv / xtc_recv_match /
 *	xtc_recv_correlate, the frame from xtc_net_recv_frame, the reply
 *	from xtc_osproc_call / xtc_svr_call, and any other buffer whose
 *	documentation says "free with xtc_free".  Those buffers are
 *	allocated by libxtc's own allocator, which is not necessarily the
 *	C library malloc/free, so they MUST be released through this
 *	function rather than plain free().  Passing NULL is a no-op.
 *
 *	This is the public name for the library allocator's deallocation
 *	entry point; it is safe to call from any thread.
 */
XTC_API void xtc_free(void *p);

/*
 * xtc_malloc / xtc_calloc / xtc_realloc --
 *	Allocate through libxtc's own allocator (the same one xtc_free
 *	releases), so a consumer never needs the internal __os_* surface.
 *	Return the pointer
 *	directly (NULL on failure), matching the C idiom; the result is
 *	released with xtc_free.  xtc_calloc zero-fills; xtc_realloc grows
 *	/ shrinks an xtc_malloc/xtc_calloc/xtc_realloc block (NULL p acts
 *	like xtc_malloc).  A zero size yields a unique freeable pointer,
 *	not NULL.  Safe from any thread.  These are the public complement
 *	to xtc_free -- library CONSUMERS use only the xtc_* API, never the
 *	internal __os_* wrappers.
 *
 *	NOTE: there is NO public API for replacing the allocator.  The
 *	hook setter (__os_alloc_set_hook) lives in src/inc/os_alloc.h,
 *	which is NOT an installed header, so it is not part of the public
 *	surface; an embedder that needs a custom allocator is building
 *	against the source tree, not the installed API.
 */
XTC_API void *xtc_malloc(size_t size);
XTC_API void *xtc_calloc(size_t n, size_t size);
XTC_API void *xtc_realloc(void *p, size_t size);

/*
 * xtc_aligned_alloc / xtc_aligned_free --
 *	Allocate `size` bytes aligned to `align` (a power of two, e.g.
 *	XTC_CACHE_LINE) through libxtc's allocator; release ONLY with
 *	xtc_aligned_free (never xtc_free -- an aligned block may carry
 *	header/padding a plain free would mishandle).  Returns NULL on
 *	failure, which covers a bad `align` (not a power of two, or
 *	smaller than sizeof(void *)), OOM, and a `size` so large that
 *	rounding it up to a multiple of `align` would not fit in a size_t
 *	(that last case used to WRAP and hand back a tiny block for a
 *	huge request; it is now rejected).  A zero size yields a unique
 *	freeable pointer, not NULL.  For a struct with an over-aligned
 *	member.
 */
XTC_API void *xtc_aligned_alloc(size_t align, size_t size);
XTC_API void xtc_aligned_free(void *p);

/*
 * xtc_clock_mono / xtc_clock_real --
 *	Read the monotonic (never goes backward; for intervals/timeouts)
 *	or real (wall-clock; for timestamps) clock in NANOSECONDS.  Return
 *	the time directly (0 on the rare query failure).  These are the
 *	public clocks a consumer uses instead of raw clock_gettime.
 *
 *	DETERMINISM (matters if you run under the simulator):
 *	xtc_clock_mono is VIRTUALIZED by the deterministic simulator, so a
 *	seeded run replays its timeline exactly -- it is the clock to use
 *	anywhere a simulated run can reach.  xtc_clock_real reads the host
 *	WALL clock, which the simulator does NOT virtualize; calling it on
 *	a sim-reachable path makes the run depend on when it was launched
 *	rather than on the seed, and the determinism guard will flag it
 *	(xtc_sim_exec_run then refuses XTC_OK).  Use xtc_clock_real only
 *	for human-facing timestamps outside simulated paths, or derive a
 *	physical component from xtc_clock_mono plus a single epoch offset
 *	sampled at startup.
 */
XTC_API int64_t xtc_clock_mono(void);
XTC_API int64_t xtc_clock_real(void);

/*
 * xtc_sleep_ns --
 *	Sleep the CALLING OS THREAD for at least `ns` nanoseconds.  This
 *	is a THREAD sleep (blocking); inside a fiber use xtc_proc_sleep
 *	instead, which parks the fiber and keeps the loop live.  Provided
 *	so a consumer never needs raw nanosleep.  Returns XTC_OK, or a
 *	negative XTC_E_* on interruption/error.
 */
XTC_API int xtc_sleep_ns(int64_t ns);

/*
 * xtc_atomic_i64_load / xtc_atomic_i64_add --
 *	SUPERSEDED by the inline xtc_atomic_load_i64 / xtc_atomic_fetch_add_i64
 *	(xtc_atomic.h), which take an explicit order and cost no call; kept
 *	exported for ABI.  Both are sequentially consistent: the load is
 *	xtc_atomic_load_i64(p, xtc_mo_seq_cst), the add (which returns the
 *	PRIOR value) xtc_atomic_fetch_add_i64(p, delta, xtc_mo_seq_cst).
 */
XTC_API int64_t xtc_atomic_i64_load(const int64_t *p);
XTC_API int64_t xtc_atomic_i64_add(int64_t *p, int64_t delta);

/*
 * xtc_env_get / xtc_env_set --
 *	a THREAD-SAFE wrapper over getenv/setenv, which are NOT safe
 *	against each other (a setenv from one thread can reallocate the
 *	environ block a getenv on another thread is reading).  Both
 *	serialize on a process-wide lock.
 *
 *	xtc_env_get returns a pointer to the variable's value, or NULL if
 *	it is unset.  The value is copied into a small PER-THREAD buffer,
 *	so the pointer is valid until the NEXT xtc_env_get call ON THE
 *	SAME THREAD (and never invalidated by a setenv from another
 *	thread); copy it if you need to keep it.  A value longer than the
 *	thread buffer is truncated (but always NUL-terminated).
 *
 *	xtc_env_set sets `name` to `value`; when overwrite == 0 an existing
 *	variable is left unchanged.  Returns XTC_OK, or a negative XTC_E_*
 *	(XTC_E_INVAL for a NULL/empty/'='-containing name, XTC_E_NOMEM if
 *	the environment cannot grow).
 */
XTC_API const char *xtc_env_get(const char *name);
XTC_API int xtc_env_set(const char *name, const char *value, int overwrite);

/*
 * xtc_rand_u64 / xtc_rand_seed --
 *	a THREAD-SAFE, seedable pseudo-random source.  rand(3)/random(3)
 *	share process-global state and are not thread-safe; this is a
 *	PER-THREAD splitmix64 stream, so every thread draws from its own
 *	state with no shared-state race and no contention.
 *
 *	xtc_rand_u64 returns the next 64-bit value for the calling thread.
 *	xtc_rand_seed makes the calling thread's stream reproducible: the
 *	same seed yields the same sequence.  An un-seeded thread auto-seeds
 *	from the clock, so distinct threads get distinct default streams.
 *	Under the deterministic simulator an un-seeded thread draws from
 *	the run's own stream instead, so its values replay from the run's
 *	seed (a seeded thread keeps its own sequence).
 *	This is NOT a cryptographic generator; do not use it for keys or
 *	security tokens.
 */
XTC_API uint64_t xtc_rand_u64(void);
XTC_API void xtc_rand_seed(uint64_t seed);

/*
 * xtc_strlcpy / xtc_strlcat --
 *	bounded string copy/cat with BSD (OpenBSD) semantics, avoiding the
 *	strncpy footgun (it does not NUL-terminate when the source fills
 *	the buffer).  These ALWAYS NUL-terminate when dstsize > 0, and
 *	return the total length they TRIED to create -- strlen(src) for
 *	xtc_strlcpy, and the initial strlen(dst) plus strlen(src) for
 *	xtc_strlcat.  A return value >= dstsize means the result was
 *	truncated.  Pure; no allocation.
 */
XTC_API size_t xtc_strlcpy(char *dst, const char *src, size_t dstsize);
XTC_API size_t xtc_strlcat(char *dst, const char *src, size_t dstsize);

/*
 * xtc_ncpus / xtc_numa_nnodes / xtc_numa_node_of_cpu /
 * xtc_numa_current_node --
 *	CPU and NUMA topology.  Use these when sharding state per core or
 *	per NUMA node (a buffer pool, a partitioned index, a per-core free
 *	list) instead of hard-coding a width or reading sysfs directly.
 *
 *	xtc_ncpus() is the number of usable CPUs (honoring the cgroup quota
 *	and affinity mask where the platform reports them), always >= 1.
 *	xtc_numa_nnodes() returns 1 on a non-NUMA or unqueryable system, and
 *	xtc_numa_node_of_cpu() then returns 0 -- so a caller can shard by
 *	the returned node with no special case.  xtc_numa_current_node() is
 *	the node the CALLING thread is running on right now, which can
 *	change if the thread migrates; treat it as a placement hint.
 *
 *	All are cheap cached queries, safe from any thread.
 */
XTC_API int xtc_ncpus(void);
XTC_API int xtc_numa_nnodes(void);
XTC_API int xtc_numa_node_of_cpu(int cpu);
XTC_API int xtc_numa_current_node(void);

#ifdef __cplusplus
}
#endif

#endif /* XTC_H */

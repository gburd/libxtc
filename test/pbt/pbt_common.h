/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_common.h
 *	Shared scaffolding for hegel property tests.  Each pbt_*.c file
 *	includes this, defines its properties, and registers them in a
 *	pbt_entry_t table consumed by PBT_MAIN.
 *
 *	When --with-hegel was NOT used at configure time this header
 *	provides a stub that prints SKIP and exits 0, so the default build
 *	stays green for contributors without hegel.
 *
 * ---------------------------------------------------------------------
 * WHY THIS FILE WAS REWRITTEN (2026-09)
 *
 *	It previously targeted gburd/hegel-c: a hegel_session_new() that
 *	FORKED A SERVER SUBPROCESS (HEGEL_SERVER_COMMAND, or `hegel` on
 *	PATH) and spoke to it over pipes.  That project is deprecated and
 *	read-only, and its socket protocol was removed upstream -- the
 *	client and the server never actually spoke to each other, so every
 *	property SKIPPED at runtime and had done so for the tier's whole
 *	life.  The README counted 23 of them as delivered coverage.
 *
 *	Upstream is now hegeldev/hegel-rust's libhegel: a pure IN-PROCESS
 *	C-ABI shared library.  Generation, shrinking and the example
 *	database all live inside the library; there is no server, no
 *	subprocess, and nothing to put on PATH.  The driving model is a
 *	caller-run loop:
 *
 *	    hegel_context_new
 *	      hegel_settings_new / _set_test_cases
 *	      hegel_run_start
 *	        loop: hegel_next_test_case  -> NULL means the run is over
 *	              <run the property, drawing values>
 *	              hegel_mark_complete(status, origin)
 *	              hegel_test_case_free
 *	      hegel_run_result -> _status
 *
 *	This header adapts that loop to the property signature the 35
 *	existing properties are written against, so NONE of the pbt_*.c
 *	files had to change.  The whole surface they use is four names:
 *	hegel_test_case, hegel_integers, hegel_draw_int, hegel_assume.
 *
 * ---------------------------------------------------------------------
 * THE ONE SEMANTIC DECISION WORTH READING
 *
 *	Upstream `assume` FILTERS: a violated assumption marks the case
 *	HEGEL_STATUS_INVALID, meaning "inconclusive, draw another" -- it is
 *	NOT a failure.  But this suite's properties use hegel_assume() as
 *	an ASSERTION: 216 of its 227 call sites are comparisons like
 *
 *	    hegel_assume(counter == (int64_t)n_threads * iters);
 *
 *	which is the invariant the property exists to check.  Mapping that
 *	to INVALID would make every property vacuously pass forever -- the
 *	worst possible outcome, because the tier would look green while
 *	verifying nothing.  That is precisely the failure mode this tier
 *	already had for its whole life, and reproducing it in a new form
 *	would be worse than leaving it skipped.
 *
 *	So this harness splits the two meanings explicitly:
 *
 *	  PBT_ASSERT(cond)  -> INTERESTING (a real counterexample)
 *	  PBT_ASSUME(cond)  -> INVALID     (filter, draw another case)
 *
 *	and hegel_assume() is mapped to PBT_ASSERT, matching how the
 *	existing properties actually use it.  A property that genuinely
 *	wants a filter should be edited to say PBT_ASSUME.  The handful of
 *	allocation/pthread guards (`... != NULL`, `pthread_create(...) == 0`)
 *	are arguably filters, but treating them as assertions is the safe
 *	direction: a failed malloc in a test IS a problem worth surfacing,
 *	whereas silently discarding the case hides it.
 */

#ifndef XTC_PBT_COMMON_H
#define XTC_PBT_COMMON_H

#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"

#if defined(XTC_HAVE_HEGEL)

#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "hegel.h"

/*
 * Per-test-case state.  A property has no return value, so a violated
 * assertion has to unwind: PBT_ASSERT/PBT_ASSUME longjmp back to the
 * driver, which then reports the right status for the case.  The buffer
 * and flags are thread-local because libhegel invokes the property on
 * whichever thread calls hegel_next_test_case, and a property may itself
 * spawn threads (several of these do) -- only the property's OWN thread
 * may longjmp.
 */
typedef struct hegel_test_case pbt_tc_wrapper_t;  /* opaque to properties */

struct pbt_state {
	hegel_context_t   *ctx;
	hegel_test_case_t *tc;
	jmp_buf            jmp;
	int                jmp_armed;
	int                status;      /* hegel_status_t for this case */
	const char        *origin;      /* file:line of the failing check */
};

#if defined(__GNUC__) || defined(__clang__)
#define PBT_TLS __thread
#else
#define PBT_TLS
#endif

static PBT_TLS struct pbt_state pbt_cur;

/*
 * The type the properties see.  Historically an opaque `hegel_test_case`
 * pointer; keep that spelling so no property file changes.  It carries
 * the per-case state rather than libhegel's handle directly, because the
 * shim needs the context and the jmp_buf too.
 */
typedef struct pbt_state hegel_test_case;

/* Bounds for the next hegel_draw_int, set by hegel_integers().  The old
 * API returned an allocated generator object; libhegel takes the bounds
 * inline at draw time, so this records them without allocating.  Not
 * thread-safe by design: a property draws on its own thread, before it
 * spawns any workers (true of all 35). */
struct pbt_int_bounds { int64_t lo, hi; };
static PBT_TLS struct pbt_int_bounds pbt_bounds;

static inline struct pbt_int_bounds *
hegel_integers(int64_t lo, int64_t hi)
{
	pbt_bounds.lo = lo;
	pbt_bounds.hi = hi;
	return &pbt_bounds;
}

/* Kept for source compatibility; libhegel allocates nothing here. */
static inline void
hegel_generator_free(struct pbt_int_bounds *g)
{
	(void)g;
}

/*
 * Draw an integer in [lo, hi].  A draw can legitimately fail with
 * HEGEL_E_STOP_TEST when libhegel runs out of choice budget; that is an
 * OVERRUN, not a property failure, so unwind with that status rather
 * than reporting a bug that does not exist.
 */
static inline int64_t
hegel_draw_int(hegel_test_case *tc, struct pbt_int_bounds *g)
{
	int64_t v = 0;
	hegel_result_t rc;

	rc = hegel_generate_integer(tc->ctx, tc->tc, g->lo, g->hi, &v);
	if (rc != HEGEL_OK) {
		tc->status = HEGEL_STATUS_OVERRUN;
		tc->origin = "draw-overrun";
		if (tc->jmp_armed)
			longjmp(tc->jmp, 1);
	}
	return v;
}

/* Unwind helpers.  __FILE__ ":" __LINE__ is the origin string, which is
 * what libhegel groups failures by -- two failures with the same origin
 * are the same bug and shrink together, so it must be stable and
 * specific to the failing check. */
#define PBT_STR2(x) #x
#define PBT_STR(x)  PBT_STR2(x)

#define PBT_UNWIND(st)							\
	do {								\
		hegel_test_case *_tc = &pbt_cur;			\
		_tc->status = (st);					\
		_tc->origin = __FILE__ ":" PBT_STR(__LINE__);		\
		if (_tc->jmp_armed)					\
			longjmp(_tc->jmp, 1);				\
	} while (0)

/* A violated invariant: this case is a counterexample. */
#define PBT_ASSERT(cond)						\
	do { if (!(cond)) PBT_UNWIND(HEGEL_STATUS_INTERESTING); } while (0)

/* A precondition that does not hold: discard and draw another. */
#define PBT_ASSUME(cond)						\
	do { if (!(cond)) PBT_UNWIND(HEGEL_STATUS_INVALID); } while (0)

/*
 * hegel_assume maps to PBT_ASSERT, not PBT_ASSUME -- see the header
 * comment.  This suite uses it as an assertion at 216 of 227 call sites,
 * and mapping it to a filter would make every property vacuously pass.
 */
#define hegel_assume(cond) PBT_ASSERT(cond)

typedef void (*hegel_test_fn)(hegel_test_case *tc, void *user_data);

typedef struct pbt_entry {
	const char     *name;
	hegel_test_fn   fn;
	int             max_examples;
} pbt_entry_t;

/*
 * Run one property: drive libhegel's caller-run loop, invoking fn once
 * per generated test case and reporting the outcome of each.
 */
static int
pbt_run_one(hegel_context_t *ctx, const char *suite, const pbt_entry_t *e)
{
	hegel_settings_t  *settings = NULL;
	hegel_run_t       *run = NULL;
	hegel_run_result_t *res = NULL;
	hegel_run_status_t  st = HEGEL_RUN_STATUS_PASSED;
	/* volatile: these are live across the setjmp below, and a local
	 * modified between setjmp and longjmp is indeterminate unless
	 * volatile (C11 7.13.2.1p3).  gcc -Wclobbered catches it. */
	volatile unsigned long n_valid = 0, n_invalid = 0;
	volatile int           failed = 0;

	if (hegel_settings_new(ctx, &settings) != HEGEL_OK) {
		printf("  [PBT] %s/%s ERROR: settings_new failed\n",
		    suite, e->name);
		return 1;
	}
	(void)hegel_settings_set_test_cases(ctx, settings,
	    (uint64_t)(e->max_examples > 0 ? e->max_examples : 100));

	if (hegel_run_start(ctx, settings, NULL, NULL, &run) != HEGEL_OK) {
		printf("  [PBT] %s/%s ERROR: run_start failed\n",
		    suite, e->name);
		(void)hegel_settings_free(ctx, settings);
		return 1;
	}

	for (;;) {
		hegel_test_case_t *tc = NULL;

		if (hegel_next_test_case(ctx, run, &tc) != HEGEL_OK) {
			printf("  [PBT] %s/%s ERROR: next_test_case failed\n",
			    suite, e->name);
			failed = 1;
			break;
		}
		if (tc == NULL)
			break;                  /* run complete */

		memset(&pbt_cur, 0, sizeof pbt_cur);
		pbt_cur.ctx    = ctx;
		pbt_cur.tc     = tc;
		pbt_cur.status = HEGEL_STATUS_VALID;
		pbt_cur.origin = NULL;

		pbt_cur.jmp_armed = 1;
		if (setjmp(pbt_cur.jmp) == 0)
			e->fn(&pbt_cur, NULL);
		pbt_cur.jmp_armed = 0;

		if (pbt_cur.status == HEGEL_STATUS_VALID)
			n_valid++;
		else if (pbt_cur.status == HEGEL_STATUS_INVALID)
			n_invalid++;

		(void)hegel_mark_complete(ctx, tc, (uint32_t)pbt_cur.status,
		    pbt_cur.origin != NULL ? pbt_cur.origin : e->name);
		(void)hegel_test_case_free(ctx, tc);
	}

	if (!failed && hegel_run_result(ctx, run, &res) == HEGEL_OK) {
		(void)hegel_run_result_status(ctx, res, &st);
		if (st == HEGEL_RUN_STATUS_PASSED) {
			printf("  [PBT] %s/%s OK (%lu valid, %lu filtered)\n",
			    suite, e->name, (unsigned long)n_valid,
			    (unsigned long)n_invalid);
		} else {
			const char *err = NULL;
			size_t nf = 0, fi;

			/* run_result_error covers ENGINE errors (health check,
			 * nondeterminism); a property counterexample is NOT one,
			 * so it is NULL for an ordinary FAIL.  The useful detail
			 * is the per-failure origin -- which is why PBT_ASSERT
			 * sets origin to __FILE__:__LINE__ of the failing check:
			 * libhegel groups and shrinks by origin, so it both
			 * names the bug and keeps distinct bugs apart. */
			(void)hegel_run_result_error(ctx, res, &err);
			(void)hegel_run_result_failure_count(ctx, res, &nf);
			printf("  [PBT] %s/%s FAIL (status=%d, %lu distinct "
			    "failure(s))%s%s\n", suite, e->name, (int)st,
			    (unsigned long)nf,
			    err != NULL ? ": " : "", err != NULL ? err : "");
			for (fi = 0; fi < nf; fi++) {
				hegel_failure_t *f = NULL;
				const char *origin = NULL;
				if (hegel_run_result_failure(ctx, res, fi, &f)
				    != HEGEL_OK)
					continue;
				(void)hegel_failure_origin(ctx, f, &origin);
				printf("      at %s\n",
				    origin != NULL ? origin : "<unknown>");
				(void)hegel_failure_free(ctx, f);
			}
			failed = 1;
		}
		(void)hegel_run_result_free(ctx, res);
	} else if (!failed) {
		printf("  [PBT] %s/%s ERROR: run_result failed\n",
		    suite, e->name);
		failed = 1;
	}

	(void)hegel_run_free(ctx, run);
	(void)hegel_settings_free(ctx, settings);
	return failed;
}

static inline int
pbt_run_all(const char *suite_name, const pbt_entry_t *tests)
{
	hegel_context_t *ctx = NULL;
	const pbt_entry_t *e;
	int failures = 0;

	/* hegel_context_new takes no arguments, returns the context, and
	 * never returns NULL (documented).  Check anyway rather than trust
	 * a doc comment. */
	ctx = hegel_context_new();
	if (ctx == NULL) {
		fprintf(stderr, "[%s] FAIL: hegel_context_new returned NULL\n",
		    suite_name);
		return 1;
	}
	for (e = tests; e->name != NULL; e++)
		failures += pbt_run_one(ctx, suite_name, e);
	(void)hegel_context_free(ctx);
	return failures == 0 ? 0 : 1;
}

#define PBT_MAIN(SUITE, TESTS)						\
	int main(int argc, char *argv[]) {				\
		(void)argc; (void)argv;					\
		return pbt_run_all((SUITE), (TESTS));			\
	}

#else  /* !XTC_HAVE_HEGEL */

/*
 * Stub mode: print SKIP with the property count and exit 0, so a
 * contributor without hegel still gets a green tree -- but LOUDLY, so a
 * green `make check` cannot be mistaken for verified properties.
 */
#include <stdint.h>

typedef int hegel_test_case;
struct pbt_int_bounds { int64_t lo, hi; };
typedef void (*hegel_test_fn)(hegel_test_case *tc, void *user_data);
typedef struct pbt_entry {
	const char     *name;
	hegel_test_fn   fn;
	int             max_examples;
} pbt_entry_t;

static inline int
pbt_run_all(const char *suite_name, const pbt_entry_t *tests)
{
	const pbt_entry_t *e;
	int n = 0;
	for (e = tests; e->name != NULL; e++)
		n++;
	printf("  [PBT] %s SKIP (--with-hegel was not configured); "
	    "%d properties unverified\n", suite_name, n);
	return 0;
}

#define PBT_MAIN(SUITE, TESTS)						\
	int main(int argc, char *argv[]) {				\
		(void)argc; (void)argv;					\
		return pbt_run_all((SUITE), (TESTS));			\
	}

static inline int64_t hegel_draw_int(hegel_test_case *tc,
    struct pbt_int_bounds *g) { (void)tc; (void)g; return 0; }
static inline struct pbt_int_bounds *hegel_integers(int64_t lo, int64_t hi)
	{ (void)lo; (void)hi; return NULL; }
static inline void hegel_assume(int cond) { (void)cond; }
static inline void hegel_generator_free(struct pbt_int_bounds *g) { (void)g; }
#define PBT_ASSERT(cond) do { (void)(cond); } while (0)
#define PBT_ASSUME(cond) do { (void)(cond); } while (0)

#endif /* XTC_HAVE_HEGEL */

#endif /* XTC_PBT_COMMON_H */

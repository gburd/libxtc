/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_common.h
 *	Shared scaffolding for hegel-c property tests.  Each pbt_*.c
 *	file includes this and defines its property tests as
 *	pbt_property_<name> functions, then registers them in main().
 *
 *	When --with-hegel was NOT used at configure time, this header
 *	provides a stub that prints SKIP and exits 0 so the build still
 *	succeeds.  When hegel IS enabled, we pull in the real headers.
 */

#ifndef XTC_PBT_COMMON_H
#define XTC_PBT_COMMON_H

#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"

#if defined(XTC_HAVE_HEGEL)

#include "hegel/hegel.h"
#include "hegel/generators.h"

/*
 * A property-based test entry.  name appears in the SKIP / OK output;
 * fn is the hegel-c test function.
 */
typedef struct pbt_entry {
	const char     *name;
	hegel_test_fn   fn;
	int             max_examples;
} pbt_entry_t;

/*
 * pbt_run_all -- run every entry in `tests` against a single hegel
 * session.  Returns 0 if all pass, 1 if any failed.
 */
static inline int
pbt_run_all(const char *suite_name, const pbt_entry_t *tests)
{
	hegel_session *s;
	int failures = 0;
	const pbt_entry_t *e;

	s = hegel_session_new();
	if (s == NULL) {
		fprintf(stderr, "[%s] FAIL: cannot start hegel session\n",
		    suite_name);
		fprintf(stderr, "  hint: set HEGEL_SERVER_COMMAND or "
		    "ensure `hegel` is on PATH\n");
		return 1;
	}

	for (e = tests; e->name != NULL; e++) {
		hegel_settings settings = HEGEL_DEFAULT_SETTINGS;
		hegel_results r;
		settings.max_examples =
		    e->max_examples > 0 ? e->max_examples : 100;
		r = hegel_run_test(s, e->fn, NULL, &settings);
		if (r.passed) {
			printf("  [PBT] %s/%s OK (%d valid examples)\n",
			    suite_name, e->name, r.valid_test_cases);
		} else {
			printf("  [PBT] %s/%s FAIL (seed=%s): %s\n",
			    suite_name, e->name,
			    r.seed ? r.seed : "?",
			    r.error ? r.error : "?");
			failures++;
		}
		hegel_results_free(&r);
	}
	hegel_session_free(s);
	return failures == 0 ? 0 : 1;
}

#define PBT_MAIN(SUITE, TESTS)						\
	int main(int argc, char *argv[]) {				\
		(void)argc; (void)argv;					\
		return pbt_run_all((SUITE), (TESTS));			\
	}

#else  /* !XTC_HAVE_HEGEL */

/*
 * Stub mode: tests print SKIP and exit 0.  This is the default build
 * so contributors without hegel installed still see a green tree.
 *
 * We define minimal types so the test files compile without
 * conditional ifdefs everywhere.
 */
typedef int hegel_test_case;
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

/* Stub generators so files compile.  Bodies are unreachable. */
static inline int hegel_draw_int(hegel_test_case *tc, void *gen)
	{ (void)tc; (void)gen; return 0; }
static inline void *hegel_integers(long lo, long hi)
	{ (void)lo; (void)hi; return NULL; }
static inline void *hegel_booleans(void)
	{ return NULL; }
static inline int hegel_draw_bool(hegel_test_case *tc, void *gen)
	{ (void)tc; (void)gen; return 0; }
static inline void hegel_assume(int cond)
	{ (void)cond; }

#endif /* XTC_HAVE_HEGEL */

#endif /* XTC_PBT_COMMON_H */

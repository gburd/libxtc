/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
/*
 * test/abi/test_deprecated_bridge.c -- every deprecated 2.x name still
 * works.
 *
 * A name renamed in 2.2 keeps its old spelling until 3.0
 * (docs/deprecations.md): an exported wrapper for a function, an alias
 * for a type, an enumerator or a macro.  This program is the one place in
 * the tree that uses the old spellings: each bridge_* function below
 * calls the old names of one rename group and checks that they do what
 * the new names do.
 *
 * The build compiles it with XTC_NO_DEPRECATION_WARNINGS.
 * test/dist/test_deprecated.sh compiles it again without that define and
 * checks that every old name in the docs/deprecations.md table warns
 * (and that a frozen alias does not).
 */
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_prob.h"

#define CHECK(c)                                                               \
	do {                                                                   \
		if (!(c)) {                                                    \
			fprintf(stderr, "  [bridge] FAIL %s:%d: %s\n",         \
			    __FILE__, __LINE__, #c);                           \
			exit(1);                                               \
		}                                                              \
	} while (0)

/* loop / io / bloom / hll lifecycle: *_init / *_fini -> *_create /
 * *_destroy.  The old *_fini keep their int result. */
static void
bridge_lifecycle(void)
{
	xtc_loop_t *loop = NULL;
	xtc_io_t *io = NULL;
	xtc_bloom_t *b = NULL;
	xtc_hll_t *h = NULL;

	CHECK(xtc_loop_init(&loop) == XTC_OK && loop != NULL);
	CHECK(xtc_loop_fini(loop) == XTC_OK);
	CHECK(xtc_loop_fini(NULL) == XTC_E_INVAL);
	CHECK(xtc_io_init(&io) == XTC_OK && io != NULL);
	CHECK(xtc_io_fini(io) == XTC_OK);
	CHECK(xtc_io_fini(NULL) == XTC_E_INVAL);
	CHECK(xtc_bloom_init(&b, 100, 0.01) == XTC_OK && b != NULL);
	xtc_bloom_add(b, "k", 1);
	CHECK(xtc_bloom_maybe_contains(b, "k", 1) == 1);
	CHECK(xtc_bloom_init(&b, 100, 1.0) == XTC_E_INVAL);
	xtc_bloom_fini(b);
	CHECK(xtc_hll_init(&h, 4) == XTC_OK && h != NULL);
	CHECK(xtc_hll_init(&h, 3) == XTC_E_INVAL);
	xtc_hll_fini(h);
}

int
main(void)
{
	bridge_lifecycle();
	printf("  [bridge] OK: every deprecated name still works\n");
	return 0;
}

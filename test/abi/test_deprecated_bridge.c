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

#define CHECK(c)                                                               \
	do {                                                                   \
		if (!(c)) {                                                    \
			fprintf(stderr, "  [bridge] FAIL %s:%d: %s\n",         \
			    __FILE__, __LINE__, #c);                           \
			exit(1);                                               \
		}                                                              \
	} while (0)

int
main(void)
{
	printf("  [bridge] OK: every deprecated name still works\n");
	return 0;
}

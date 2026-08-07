/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/07_resource_scope.c -- a resource that is
 * released on EVERY exit path, and a cancellation-masked acquire.
 *
 * xtc_scope makes "this WILL be released" a runtime mechanism, not a
 * convention: finalizers deferred into an open scope run in LIFO order
 * whether the fiber returns normally, exits, is aborted, or a contained
 * fault unwinds it.  xtc_bracket is the acquire/use/release sugar over
 * it, with the acquire run cancellation-masked so the release is always
 * registered before an abort can be observed.
 */

/* !region full */
#include <stdio.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

/* !region scope */
static void
close_file(void *arg)
{
	printf("finalizer: closing %s\n", (const char *)arg);
}

static void
using_a_scope(void *arg)
{
	xtc_scope_t *s;
	(void)arg;

	s = xtc_scope_open();          /* opens on the calling process */
	if (s == NULL)
		return;

	/* Defer finalizers; they run LIFO on close OR on any unwind. */
	(void)xtc_scope_defer(s, close_file, (void *)"data.db");
	(void)xtc_scope_defer(s, close_file, (void *)"index.db");

	/* ... work with the resources ... */

	xtc_scope_close(s);            /* runs: index.db, then data.db */
}
/* !endregion scope */

/* !region bracket */
static int
acquire_buf(void **res, void *ud)
{
	(void)ud;
	*res = xtc_malloc(64);
	return (*res != NULL) ? XTC_OK : XTC_E_RESOURCE;
}
static int
use_buf(void *res, void *ud)
{
	(void)ud;
	((char *)res)[0] = 'x';        /* use the resource */
	return XTC_OK;
}
static void
release_buf(void *res, void *ud)
{
	(void)ud;
	printf("bracket: releasing buffer\n");
	xtc_free(res);                 /* runs on every exit path of use */
}

static void
using_bracket(void *arg)
{
	(void)arg;
	/* acquire runs abort-masked, so release is always registered. */
	(void)xtc_bracket(acquire_buf, use_buf, release_buf, NULL);
}
/* !endregion bracket */

int
main(void)
{
	xtc_loop_t *loop;

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, using_a_scope, NULL, NULL, NULL) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, using_bracket, NULL, NULL, NULL) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	return 0;
}
/* !endregion full */

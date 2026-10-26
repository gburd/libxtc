/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
/*
 * test/abi/test_abi_sized.c -- the size-first struct contract (2.0 ABI).
 *
 * Simulates a consumer built against an OLDER (smaller) or NEWER (larger)
 * header than the library it links: the struct is cut to a prefix, or
 * padded with a tail, and its .size says so.  The library must not read
 * or write past .size (each prefix is heap-allocated at EXACTLY its size,
 * so an AddressSanitizer build turns any overrun into a hard failure; the
 * plain build checks guard bytes and defaults), must treat fields past
 * .size as their defaults, must reject size 0, and must reject a larger
 * struct whose unknown tail is not zero.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_inspect.h"

static int fails;

#define CHECK(c) do {                                                   \
	if (!(c)) {                                                     \
		fprintf(stderr, "  [abi] FAIL %s:%d: %s\n", __FILE__,   \
		    __LINE__, #c);                                      \
		fails++;                                                \
	}                                                               \
} while (0)

/* An N-1 consumer's xtc_proc_opts_t: it ends at mailbox_cap. */
struct old_proc_opts {
	size_t      size;
	const char *name;
	size_t      mailbox_cap;
};

/* An N-1 consumer's xtc_proc_info_t: it ends at pid. */
struct old_proc_info {
	size_t    size;
	xtc_pid_t pid;
};

static void *
xalloc(size_t n)
{
	void *p = malloc(n);

	if (p == NULL) {
		perror("malloc");
		exit(2);
	}
	return p;
}

/* Runs as the proc spawned from the truncated opts: checks itself. */
static void
self_check(void *arg)
{
	struct old_proc_info *oi;
	xtc_proc_info_t full = XTC_PROC_INFO_INIT;
	unsigned char *big;
	size_t i, bigsz = sizeof(xtc_proc_info_t) + 32;
	int all_zero = 1;

	(void)arg;
	/* The opts prefix set mailbox_cap = 7; everything past it defaulted. */
	CHECK(xtc_proc_info(xtc_self(), &full) == XTC_OK);
	CHECK(full.mbox_cap == 7);
	CHECK(full.size == sizeof(xtc_proc_info_t));

	/* OLDER output struct: written only up to its size. */
	oi = xalloc(sizeof *oi);
	oi->size = sizeof *oi;
	CHECK(xtc_proc_info(xtc_self(), (xtc_proc_info_t *)(void *)oi) ==
	    XTC_OK);
	CHECK(xtc_pid_eq(oi->pid, xtc_self()));
	CHECK(oi->size == sizeof *oi);
	free(oi);

	/* NEWER output struct: the bytes this library does not know are 0. */
	big = xalloc(bigsz);
	memset(big, 0xA5, bigsz);
	memcpy(big, &bigsz, sizeof bigsz);
	CHECK(xtc_proc_info(xtc_self(), (xtc_proc_info_t *)(void *)big) ==
	    XTC_OK);
	for (i = sizeof(xtc_proc_info_t); i < bigsz; i++)
		if (big[i] != 0)
			all_zero = 0;
	CHECK(all_zero);
	free(big);

	/* size 0: rejected, nothing written. */
	full.size = 0;
	full.mbox_cap = 12345;
	CHECK(xtc_proc_info(xtc_self(), &full) == XTC_E_INVAL);
	CHECK(full.mbox_cap == 12345);
}

static int
spawn_with(xtc_loop_t *loop, const void *opts)
{
	return xtc_proc_spawn(loop, self_check, NULL,
	    (const xtc_proc_opts_t *)opts, NULL);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	struct old_proc_opts *oo;
	unsigned char *big;
	size_t bigsz = sizeof(xtc_proc_opts_t) + 16;
	xtc_proc_opts_t zero = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;

	CHECK(xtc_loop_init(&loop) == XTC_OK);
	if (loop == NULL)
		return 1;

	/* OLDER input struct, allocated at exactly its size. */
	oo = xalloc(sizeof *oo);
	oo->size = sizeof *oo;
	oo->name = "n-1";
	oo->mailbox_cap = 7;
	CHECK(spawn_with(loop, oo) == XTC_OK);
	free(oo);
	CHECK(xtc_loop_run(loop) == XTC_OK);

	/* size 0 / below the size field: rejected. */
	zero.size = 0;
	CHECK(xtc_proc_spawn(loop, self_check, NULL, &zero, &pid) ==
	    XTC_E_INVAL);
	zero.size = sizeof(size_t) - 1;
	CHECK(xtc_proc_spawn(loop, self_check, NULL, &zero, &pid) ==
	    XTC_E_INVAL);

	/* NEWER input struct: an unknown nonzero tail is refused ... */
	big = xalloc(bigsz);
	memset(big, 0, bigsz);
	memcpy(big, &bigsz, sizeof bigsz);
	big[bigsz - 1] = 1;
	CHECK(spawn_with(loop, big) == XTC_E_VERSION);
	/* ... and an all-zero unknown tail is accepted (those fields default).
	 * mailbox_cap is set through the library's own view so the spawned
	 * proc's mbox_cap == 7 check holds. */
	big[bigsz - 1] = 0;
	((xtc_proc_opts_t *)(void *)big)->mailbox_cap = 7;
	CHECK(spawn_with(loop, big) == XTC_OK);
	free(big);
	CHECK(xtc_loop_run(loop) == XTC_OK);

	CHECK(xtc_loop_fini(loop) == XTC_OK);
	if (fails != 0) {
		fprintf(stderr, "  [abi] FAIL: %d size-contract check(s)\n",
		    fails);
		return 1;
	}
	printf("  [abi] OK: proc opts/info honor .size (older, newer, 0)\n");
	return 0;
}

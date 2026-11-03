/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/real/down_decode_harness.c
 *
 *	REAL CODE: links src/ptc/proc.c unchanged (no transcription).
 *
 *	PROPERTY: xtc_down_decode and xtc_down_decode_ex are memory-safe
 *	on ANY input -- every message length 0..MAXLEN and every byte
 *	value -- and on a caller struct of any `size` down to the
 *	smallest accepted.  A DOWN/EXIT message is attacker-shaped data
 *	once it crosses a node boundary (orc/node.c forwards it), so the
 *	decoder must never read past `len` or write past out->size.
 *	Also: on XTC_OK the classified kind is one of the five the header
 *	documents, and EXIT carries exit_code == reason.
 */
#include "xtc.h"
#include "xtc_proc.h"
#include <stdlib.h>

#define MAXLEN 40

int
main(void)
{
	size_t len, osz;
	unsigned char *msg;
	xtc_down_info_t *info;
	xtc_pid_t pid;
	int reason, rc;

	__CPROVER_assume(len <= MAXLEN);
	msg = malloc(len);          /* bytes are nondeterministic */
	__CPROVER_assume(msg != NULL || len == 0);

	rc = xtc_down_decode(msg, len, &pid, &reason);
	(void)rc;

	/* A caller compiled against an older, shorter xtc_down_info_t. */
	__CPROVER_assume(osz >= sizeof(size_t) &&
	    osz <= sizeof(xtc_down_info_t));
	info = malloc(osz);
	__CPROVER_assume(info != NULL);
	info->size = osz;
	rc = xtc_down_decode_ex(msg, len, info);
	if (rc == XTC_OK && osz == sizeof(xtc_down_info_t)) {
		__CPROVER_assert(info->kind == XTC_DOWN_KIND_CLEAN ||
		    info->kind == XTC_DOWN_KIND_EXIT ||
		    info->kind == XTC_DOWN_KIND_SIGNAL ||
		    info->kind == XTC_DOWN_KIND_NOPROC ||
		    info->kind == XTC_DOWN_KIND_NOCONNECTION,
		    "decoded kind is one of the five documented kinds");
		if (info->kind == XTC_DOWN_KIND_EXIT)
			__CPROVER_assert(info->exit_code == info->reason,
			    "EXIT carries exit_code == reason");
	}
	return 0;
}

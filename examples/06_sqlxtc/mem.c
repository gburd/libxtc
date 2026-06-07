/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/mem.c
 *	An xsql_mem_methods implementation backed by xtc's allocator
 *	(__os_malloc / __os_realloc / __os_free).
 *
 *	This is one of the engine's "use xtc where possible" seams.  By
 *	installing it (sx_config_mem, before sx_init) every allocation
 *	the SQL engine makes -- parse trees, the page cache spill, VDBE
 *	register stacks, schema objects -- flows through xtc's allocator
 *	rather than bare malloc(3).  xtc's allocator is itself a hookable
 *	vtable (__os_alloc_set_hook), so a host that supplies its own
 *	primitives -- a host plugging in its own arena/slab allocator,
 *	say -- transparently captures the engine's
 *	allocations too, and the xtc_alloc_audit machinery can attribute
 *	them to the owning proc.  This is exactly the platform malloc
 *	shim that the hard-fork plan lists as redundant once xtc supplies
 *	the primitive (see docs/M_SQLXTC_HARDFORK.md).
 *
 *	SQLite requires xSize(p) to report the usable size of a prior
 *	allocation.  Each block carries a 16-byte header recording its
 *	size; the recorded size is the allocator's ACTUAL usable size
 *	(__os_msize, minus the header) when the backend reports it, so
 *	SQLite may use the slack the allocator handed out -- falling back
 *	to the requested size for a custom backend that cannot report it.
 *	The 16-byte header also keeps the pointer handed back to SQLite
 *	16-byte aligned (SQLite needs 8).
 */

#include "mem.h"
#include "sqlite3.h"

#include "os_alloc.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Header carried in front of every block.  16 bytes: large enough to
 * hold the size and to preserve 16-byte alignment of the returned
 * pointer (malloc(3) returns max_align_t-aligned memory, so base + 16
 * is still 16-aligned).
 */
#define MEM_HDR 16u

/* Record the block's usable size in its header: the allocator's actual
 * usable bytes (minus the header) when known, else the request. */
static void
mem_set_size(void *base, size_t requested)
{
	size_t usable = __os_msize(base);
	size_t avail = (usable > MEM_HDR) ? usable - MEM_HDR : requested;
	if (avail < requested)
		avail = requested;
	*(size_t *)base = avail;
}

static void *
mem_malloc(int n)
{
	void *base;
	size_t sz;

	if (n <= 0)
		return NULL;
	sz = (size_t)n;
	if (__os_malloc(sz + MEM_HDR, &base) != 0)
		return NULL;
	mem_set_size(base, sz);
	return (char *)base + MEM_HDR;
}

static void
mem_free(void *p)
{
	if (p == NULL)
		return;
	__os_free((char *)p - MEM_HDR);
}

static void *
mem_realloc(void *p, int n)
{
	void *base, *nb;
	size_t sz;

	if (p == NULL)
		return mem_malloc(n);
	if (n <= 0) {
		mem_free(p);
		return NULL;
	}
	sz = (size_t)n;
	base = (char *)p - MEM_HDR;
	if (__os_realloc(base, sz + MEM_HDR, &nb) != 0)
		return NULL;
	mem_set_size(nb, sz);
	return (char *)nb + MEM_HDR;
}

static int
mem_size(void *p)
{
	if (p == NULL)
		return 0;
	return (int)*(size_t *)((char *)p - MEM_HDR);
}

static int
mem_roundup(int n)
{
	/* SQLite asks how big an allocation it will actually get for `n`.
	 * We hand back the allocator's usable size, which is at least the
	 * 8-byte-rounded request, so reporting that lower bound is safe. */
	if (n <= 0)
		return 0;
	return (n + 7) & ~7;
}

static int
mem_init(void *unused)
{
	(void)unused;
	return SQLITE_OK;
}

static void
mem_shutdown(void *unused)
{
	(void)unused;
}

static const xsql_mem_methods mem_table = {
	mem_malloc,
	mem_free,
	mem_realloc,
	mem_size,
	mem_roundup,
	mem_init,
	mem_shutdown,
	NULL,
};

const void *
mem_methods(void)
{
	return &mem_table;
}

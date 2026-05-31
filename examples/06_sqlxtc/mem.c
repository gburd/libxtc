/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/mem.c
 *	An sqlite3_mem_methods implementation backed by xtc's allocator
 *	(__os_malloc / __os_realloc / __os_free).
 *
 *	This is one of the engine's "use xtc where possible" seams.  By
 *	installing it (sx_config_mem, before sx_init) every allocation
 *	the SQL engine makes -- parse trees, the page cache spill, VDBE
 *	register stacks, schema objects -- flows through xtc's allocator
 *	rather than bare malloc(3).  xtc's allocator is itself a hookable
 *	vtable (__os_alloc_set_hook), so a host that supplies its own
 *	primitives -- the PostgreSQL-on-xtc port substituting an
 *	arena/slab allocator, say -- transparently captures the engine's
 *	allocations too, and the xtc_alloc_audit machinery can attribute
 *	them to the owning proc.  This is exactly the platform malloc
 *	shim that the hard-fork plan lists as redundant once xtc supplies
 *	the primitive (see docs/M_SQLXTC_HARDFORK.md).
 *
 *	SQLite requires xSize(p) to report the usable size of a prior
 *	allocation, which the xtc allocator does not track, so each block
 *	carries a small header that records its size.  The header is one
 *	cache-friendly 16-byte slot, which also keeps the pointer handed
 *	back to SQLite 16-byte aligned (SQLite needs 8).
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
	*(size_t *)base = sz;
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
	*(size_t *)nb = sz;
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
	/* Round to 8 bytes; SQLite asks for the size it will actually
	 * get so it can pack structures.  We allocate exactly what is
	 * requested, so reporting the 8-byte-rounded request is exact. */
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

static const sqlite3_mem_methods mem_table = {
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

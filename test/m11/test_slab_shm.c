/*-
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 2026, The XTC Project
 *
 * test/m11/test_slab_shm.c
 *	Cross-process shared-memory tests for xtc_slab.
 *
 *	Verifies that XTC_SLAB_SHARED_MEMORY mode actually works
 *	across fork(2) with POSIX shm_open.  Unlike test_slab.c's
 *	test_shm_offset_resolve_single_process (which uses MAP_PRIVATE),
 *	these tests exercise real inter-process sharing:
 *
 *	  - Parent allocates, child reads via offset
 *	  - Child allocates, parent reads via offset
 *	  - Concurrent alloc from both processes (no collision)
 *	  - Region too small for one chunk
 *	  - Invalid offset resolution
 */

#ifndef _WIN32

/* Feature-test macros for usleep, shm_open. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_slab.h"
#include "xtc_int.h"

/* Test payload written to shared memory. */
struct test_payload {
	uint32_t magic;
	int32_t  value;
};

#define TEST_MAGIC  0xCAFEBABE
#define SHM_SIZE    (1024 * 1024)   /* 1 MiB */

/* Generate a unique shm name for this test run. */
static void
make_shm_name(char *buf, size_t bufsz, const char *tag)
{
	snprintf(buf, bufsz, "/xtc-test-shm-%d-%s", (int)getpid(), tag);
}

/* Helper: create shm region, returns fd and mapped address. */
static int
create_shm_region(const char *name, size_t size, void **out_addr)
{
	int fd;
	void *addr;

	/* Remove any stale segment. */
	(void)shm_unlink(name);

	fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
	if (fd < 0) return -1;

	if (ftruncate(fd, (off_t)size) != 0) {
		(void)close(fd);
		(void)shm_unlink(name);
		return -1;
	}

	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		(void)close(fd);
		(void)shm_unlink(name);
		return -1;
	}

	/* Zero the region to ensure clean state. */
	memset(addr, 0, size);

	*out_addr = addr;
	return fd;
}

/* Helper: attach to existing shm region. */
static int
attach_shm_region(const char *name, size_t size, void **out_addr)
{
	int fd;
	void *addr;

	fd = shm_open(name, O_RDWR, 0600);
	if (fd < 0) return -1;

	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		(void)close(fd);
		return -1;
	}

	*out_addr = addr;
	return fd;
}

/* Helper: cleanup shm region. */
static void
cleanup_shm(const char *name, int fd, void *addr, size_t size)
{
	if (addr != NULL && addr != MAP_FAILED)
		(void)munmap(addr, size);
	if (fd >= 0)
		(void)close(fd);
	if (name != NULL)
		(void)shm_unlink(name);
}

/* ----- test_shm_basic_fork -------------------------------------- */
/*
 * Parent creates shm + slab, allocates an object, writes a pattern,
 * forks.  Child attaches to same shm, creates its own slab, resolves
 * the parent's offset, reads the pattern, modifies it.  Parent waits,
 * reads back the modification.
 */
static MunitResult
test_shm_basic_fork(const MunitParameter p[], void *d)
{
	char shm_name[64];
	void *shm_addr = NULL;
	int shm_fd = -1;
	xtc_slab_t *slab = NULL;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	struct test_payload *obj;
	xtc_slab_off_t off;
	pid_t pid;
	int status;
	/* We'll store the offset in the shm region itself (at a known location). */
	volatile xtc_slab_off_t *offset_slot;
	(void)p; (void)d;

	make_shm_name(shm_name, sizeof(shm_name), "basic");
	shm_fd = create_shm_region(shm_name, SHM_SIZE, &shm_addr);
	munit_assert_int(shm_fd, >=, 0);

	/* Reserve first 8 bytes of usable area for offset passing. */
	/* (After the slab header, we have usable space starting at offset 64) */

	opts.name = "shm_basic";
	opts.obj_size = sizeof(struct test_payload);
	opts.mode = XTC_SLAB_SHARED_MEMORY;
	opts.shm_base = shm_addr;
	opts.shm_size = SHM_SIZE;
	munit_assert_int(xtc_slab_create(&opts, &slab), ==, XTC_OK);

	/* Allocate and write pattern. */
	obj = xtc_slab_alloc(slab);
	munit_assert_not_null(obj);
	obj->magic = TEST_MAGIC;
	obj->value = 42;

	off = xtc_slab_offset(slab, obj);
	munit_assert_int64(off, !=, XTC_SLAB_OFF_NONE);

	/* Store offset in shm for child to find.
	 * Use bytes 32-39 in the header's reserved area (safe from slot data). */
	offset_slot = (volatile xtc_slab_off_t *)((uint8_t *)shm_addr + 32);
	*offset_slot = off;

	/* Memory barrier to ensure writes visible to child. */
	atomic_thread_fence(memory_order_release);

	pid = fork();
	munit_assert_int(pid, >=, 0);

	if (pid == 0) {
		/* Child process. */
		xtc_slab_t *child_slab = NULL;
		xtc_slab_opts_t child_opts = XTC_SLAB_OPTS_DEFAULT;
		struct test_payload *child_obj;
		xtc_slab_off_t child_off;
		void *child_shm_addr = NULL;
		int child_fd;

		/* Attach to same shm region. */
		child_fd = attach_shm_region(shm_name, SHM_SIZE, &child_shm_addr);
		if (child_fd < 0) _exit(1);

		/* Read offset from shm header's reserved area. */
		volatile xtc_slab_off_t *child_offset_slot =
		    (volatile xtc_slab_off_t *)((uint8_t *)child_shm_addr + 32);
		atomic_thread_fence(memory_order_acquire);
		child_off = *child_offset_slot;

		/* Create slab over same region. */
		child_opts.name = "shm_basic_child";
		child_opts.obj_size = sizeof(struct test_payload);
		child_opts.mode = XTC_SLAB_SHARED_MEMORY;
		child_opts.shm_base = child_shm_addr;
		child_opts.shm_size = SHM_SIZE;
		if (xtc_slab_create(&child_opts, &child_slab) != XTC_OK)
			_exit(2);

		/* Resolve parent's offset. */
		child_obj = xtc_slab_resolve(child_slab, child_off);
		if (child_obj == NULL) _exit(3);

		/* Verify pattern. */
		if (child_obj->magic != TEST_MAGIC) _exit(4);
		if (child_obj->value != 42) _exit(5);

		/* Modify value. */
		child_obj->value = 99;
		atomic_thread_fence(memory_order_release);

		xtc_slab_destroy(child_slab);
		(void)munmap(child_shm_addr, SHM_SIZE);
		(void)close(child_fd);
		_exit(0);
	}

	/* Parent: wait for child. */
	(void)waitpid(pid, &status, 0);
	munit_assert_true(WIFEXITED(status));
	munit_assert_int(WEXITSTATUS(status), ==, 0);

	/* Verify child's modification. */
	atomic_thread_fence(memory_order_acquire);
	munit_assert_uint32(obj->magic, ==, TEST_MAGIC);
	munit_assert_int32(obj->value, ==, 99);

	xtc_slab_free(slab, obj);
	xtc_slab_destroy(slab);
	cleanup_shm(shm_name, shm_fd, shm_addr, SHM_SIZE);

	/* Verify shm is gone. */
	munit_assert_int(shm_open(shm_name, O_RDONLY, 0), ==, -1);
	munit_assert_int(errno, ==, ENOENT);

	return MUNIT_OK;
}

/* ----- test_shm_alloc_in_child ---------------------------------- */
/*
 * Child allocates, parent reads back via offset.
 */
static MunitResult
test_shm_alloc_in_child(const MunitParameter p[], void *d)
{
	char shm_name[64];
	void *shm_addr = NULL;
	int shm_fd = -1;
	xtc_slab_t *slab = NULL;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	pid_t pid;
	int status;
	volatile xtc_slab_off_t *offset_slot;
	volatile int *ready_flag;
	(void)p; (void)d;

	make_shm_name(shm_name, sizeof(shm_name), "child_alloc");
	shm_fd = create_shm_region(shm_name, SHM_SIZE, &shm_addr);
	munit_assert_int(shm_fd, >=, 0);

	/* Communication slots in shm header's reserved area. */
	offset_slot = (volatile xtc_slab_off_t *)((uint8_t *)shm_addr + 32);
	ready_flag = (volatile int *)((uint8_t *)shm_addr + 40);
	*ready_flag = 0;

	opts.name = "shm_child_alloc";
	opts.obj_size = sizeof(struct test_payload);
	opts.mode = XTC_SLAB_SHARED_MEMORY;
	opts.shm_base = shm_addr;
	opts.shm_size = SHM_SIZE;
	munit_assert_int(xtc_slab_create(&opts, &slab), ==, XTC_OK);

	pid = fork();
	munit_assert_int(pid, >=, 0);

	if (pid == 0) {
		/* Child: allocate and write. */
		xtc_slab_t *child_slab = NULL;
		xtc_slab_opts_t child_opts = XTC_SLAB_OPTS_DEFAULT;
		struct test_payload *child_obj;
		void *child_shm_addr = NULL;
		int child_fd;

		child_fd = attach_shm_region(shm_name, SHM_SIZE, &child_shm_addr);
		if (child_fd < 0) _exit(1);

		volatile xtc_slab_off_t *child_offset_slot =
		    (volatile xtc_slab_off_t *)((uint8_t *)child_shm_addr + 32);
		volatile int *child_ready =
		    (volatile int *)((uint8_t *)child_shm_addr + 40);

		child_opts.name = "shm_child_alloc_child";
		child_opts.obj_size = sizeof(struct test_payload);
		child_opts.mode = XTC_SLAB_SHARED_MEMORY;
		child_opts.shm_base = child_shm_addr;
		child_opts.shm_size = SHM_SIZE;
		if (xtc_slab_create(&child_opts, &child_slab) != XTC_OK)
			_exit(2);

		/* Allocate in child. */
		child_obj = xtc_slab_alloc(child_slab);
		if (child_obj == NULL) _exit(3);

		child_obj->magic = TEST_MAGIC;
		child_obj->value = 777;

		*child_offset_slot = xtc_slab_offset(child_slab, child_obj);
		atomic_thread_fence(memory_order_release);
		*child_ready = 1;

		/* Wait a bit for parent to read, then exit. */
		usleep(50000);

		xtc_slab_destroy(child_slab);
		(void)munmap(child_shm_addr, SHM_SIZE);
		(void)close(child_fd);
		_exit(0);
	}

	/* Parent: wait for child's allocation. */
	while (atomic_load_explicit((volatile _Atomic int *)ready_flag,
	    memory_order_acquire) == 0) {
		usleep(1000);
	}

	xtc_slab_off_t off = *offset_slot;
	munit_assert_int64(off, !=, XTC_SLAB_OFF_NONE);

	struct test_payload *obj = xtc_slab_resolve(slab, off);
	munit_assert_not_null(obj);
	munit_assert_uint32(obj->magic, ==, TEST_MAGIC);
	munit_assert_int32(obj->value, ==, 777);

	(void)waitpid(pid, &status, 0);
	munit_assert_true(WIFEXITED(status));
	munit_assert_int(WEXITSTATUS(status), ==, 0);

	xtc_slab_destroy(slab);
	cleanup_shm(shm_name, shm_fd, shm_addr, SHM_SIZE);
	return MUNIT_OK;
}

/* ----- test_shm_concurrent_alloc -------------------------------- */
/*
 * Both parent and child allocate concurrently.  Verify no overlap,
 * no double-allocation.  This is the key test that would fail with
 * per-process cursor.
 */
#define CONC_ALLOCS  50

static MunitResult
test_shm_concurrent_alloc(const MunitParameter p[], void *d)
{
	char shm_name[64];
	void *shm_addr = NULL;
	int shm_fd = -1;
	xtc_slab_t *slab = NULL;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	pid_t pid;
	int status;
	struct test_payload *parent_objs[CONC_ALLOCS];
	xtc_slab_off_t parent_offs[CONC_ALLOCS];
	/* Child stores its offsets in shm. */
	volatile xtc_slab_off_t *child_offs_slot;
	volatile int *child_count_slot;
	int i, j;
	(void)p; (void)d;

	make_shm_name(shm_name, sizeof(shm_name), "concurrent");
	shm_fd = create_shm_region(shm_name, SHM_SIZE, &shm_addr);
	munit_assert_int(shm_fd, >=, 0);

	/* Reserve space for child's count and offsets.
	 * Use a separate area after header but before chunk data.
	 * We'll use a dedicated communication page at the end of the region. */
	child_count_slot = (volatile int *)((uint8_t *)shm_addr + SHM_SIZE - 4096);
	child_offs_slot = (volatile xtc_slab_off_t *)((uint8_t *)shm_addr + SHM_SIZE - 4096 + 8);
	*child_count_slot = 0;

	opts.name = "shm_conc";
	opts.obj_size = sizeof(struct test_payload);
	opts.mode = XTC_SLAB_SHARED_MEMORY;
	opts.shm_base = shm_addr;
	opts.shm_size = SHM_SIZE;
	munit_assert_int(xtc_slab_create(&opts, &slab), ==, XTC_OK);

	pid = fork();
	munit_assert_int(pid, >=, 0);

	if (pid == 0) {
		/* Child: allocate CONC_ALLOCS objects concurrently with parent. */
		xtc_slab_t *child_slab = NULL;
		xtc_slab_opts_t child_opts = XTC_SLAB_OPTS_DEFAULT;
		struct test_payload *child_objs[CONC_ALLOCS];
		void *child_shm_addr = NULL;
		int child_fd;
		int ci;

		child_fd = attach_shm_region(shm_name, SHM_SIZE, &child_shm_addr);
		if (child_fd < 0) _exit(1);

		volatile int *c_count =
		    (volatile int *)((uint8_t *)child_shm_addr + SHM_SIZE - 4096);
		volatile xtc_slab_off_t *c_offs =
		    (volatile xtc_slab_off_t *)((uint8_t *)child_shm_addr + SHM_SIZE - 4096 + 8);

		child_opts.name = "shm_conc_child";
		child_opts.obj_size = sizeof(struct test_payload);
		child_opts.mode = XTC_SLAB_SHARED_MEMORY;
		child_opts.shm_base = child_shm_addr;
		child_opts.shm_size = SHM_SIZE;
		if (xtc_slab_create(&child_opts, &child_slab) != XTC_OK)
			_exit(2);

		for (ci = 0; ci < CONC_ALLOCS; ci++) {
			child_objs[ci] = xtc_slab_alloc(child_slab);
			if (child_objs[ci] == NULL) _exit(3);
			child_objs[ci]->magic = TEST_MAGIC;
			child_objs[ci]->value = 1000 + ci;
			c_offs[ci] = xtc_slab_offset(child_slab, child_objs[ci]);
		}
		atomic_thread_fence(memory_order_release);
		*c_count = CONC_ALLOCS;

		/* Wait for parent to finish checking. */
		usleep(100000);

		xtc_slab_destroy(child_slab);
		(void)munmap(child_shm_addr, SHM_SIZE);
		(void)close(child_fd);
		_exit(0);
	}

	/* Parent: allocate concurrently. */
	for (i = 0; i < CONC_ALLOCS; i++) {
		parent_objs[i] = xtc_slab_alloc(slab);
		munit_assert_not_null(parent_objs[i]);
		parent_objs[i]->magic = TEST_MAGIC;
		parent_objs[i]->value = i;
		parent_offs[i] = xtc_slab_offset(slab, parent_objs[i]);
	}

	/* Wait for child to finish allocating. */
	while (atomic_load_explicit((volatile _Atomic int *)child_count_slot,
	    memory_order_acquire) < CONC_ALLOCS) {
		usleep(1000);
	}

	/* Verify no overlap between parent and child offsets. */
	for (i = 0; i < CONC_ALLOCS; i++) {
		for (j = 0; j < CONC_ALLOCS; j++) {
			munit_assert_int64(parent_offs[i], !=, child_offs_slot[j]);
		}
	}

	/* Verify all parent objects are distinct. */
	for (i = 0; i < CONC_ALLOCS; i++) {
		for (j = i + 1; j < CONC_ALLOCS; j++) {
			munit_assert_int64(parent_offs[i], !=, parent_offs[j]);
		}
	}

	/* Verify we can resolve child's allocations and read their values. */
	for (i = 0; i < CONC_ALLOCS; i++) {
		struct test_payload *cobj = xtc_slab_resolve(slab, child_offs_slot[i]);
		munit_assert_not_null(cobj);
		munit_assert_uint32(cobj->magic, ==, TEST_MAGIC);
		munit_assert_int32(cobj->value, ==, 1000 + i);
	}

	(void)waitpid(pid, &status, 0);
	munit_assert_true(WIFEXITED(status));
	munit_assert_int(WEXITSTATUS(status), ==, 0);

	for (i = 0; i < CONC_ALLOCS; i++)
		xtc_slab_free(slab, parent_objs[i]);
	xtc_slab_destroy(slab);
	cleanup_shm(shm_name, shm_fd, shm_addr, SHM_SIZE);
	return MUNIT_OK;
}

/* ----- test_shm_size_too_small ---------------------------------- */
/*
 * Region too small for header + one chunk; xtc_slab_create should
 * fail with XTC_E_RESOURCE.
 */
static MunitResult
test_shm_size_too_small(const MunitParameter p[], void *d)
{
	char shm_name[64];
	void *shm_addr = NULL;
	int shm_fd = -1;
	xtc_slab_t *slab = NULL;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	size_t tiny_size = 128;  /* Too small for header + default 64K chunk. */
	(void)p; (void)d;

	make_shm_name(shm_name, sizeof(shm_name), "toosmall");
	shm_fd = create_shm_region(shm_name, tiny_size, &shm_addr);
	munit_assert_int(shm_fd, >=, 0);

	opts.name = "shm_tiny";
	opts.obj_size = 64;
	opts.chunk_size = 64 * 1024;  /* 64 KiB chunk. */
	opts.mode = XTC_SLAB_SHARED_MEMORY;
	opts.shm_base = shm_addr;
	opts.shm_size = tiny_size;

	int rc = xtc_slab_create(&opts, &slab);
	munit_assert_int(rc, ==, XTC_E_RESOURCE);
	munit_assert_null(slab);

	cleanup_shm(shm_name, shm_fd, shm_addr, tiny_size);
	return MUNIT_OK;
}

/* ----- test_shm_resolve_invalid_offset -------------------------- */
/*
 * Resolve with a junk offset returns NULL.
 */
static MunitResult
test_shm_resolve_invalid_offset(const MunitParameter p[], void *d)
{
	char shm_name[64];
	void *shm_addr = NULL;
	int shm_fd = -1;
	xtc_slab_t *slab = NULL;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	(void)p; (void)d;

	make_shm_name(shm_name, sizeof(shm_name), "invalid");
	shm_fd = create_shm_region(shm_name, SHM_SIZE, &shm_addr);
	munit_assert_int(shm_fd, >=, 0);

	opts.name = "shm_invalid";
	opts.obj_size = sizeof(struct test_payload);
	opts.mode = XTC_SLAB_SHARED_MEMORY;
	opts.shm_base = shm_addr;
	opts.shm_size = SHM_SIZE;
	munit_assert_int(xtc_slab_create(&opts, &slab), ==, XTC_OK);

	/* XTC_SLAB_OFF_NONE should return NULL. */
	munit_assert_null(xtc_slab_resolve(slab, XTC_SLAB_OFF_NONE));

	/* Offset beyond region size should return NULL. */
	munit_assert_null(xtc_slab_resolve(slab, (xtc_slab_off_t)(SHM_SIZE + 1000)));

	/* Negative offset (other than -1) should be invalid. */
	munit_assert_null(xtc_slab_resolve(slab, (xtc_slab_off_t)(-42)));

	xtc_slab_destroy(slab);
	cleanup_shm(shm_name, shm_fd, shm_addr, SHM_SIZE);
	return MUNIT_OK;
}

/* ----- Test suite ---------------------------------------------- */

static MunitTest tests[] = {
	{ "/basic_fork",        test_shm_basic_fork,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alloc_in_child",    test_shm_alloc_in_child,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent_alloc",  test_shm_concurrent_alloc,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/size_too_small",    test_shm_size_too_small,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/invalid_offset",    test_shm_resolve_invalid_offset, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m11/slab_shm", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}

#else  /* _WIN32 */

#include <stdio.h>

int main(void)
{
	printf("SKIP: cross-process shm tests require POSIX (fork + shm_open)\n");
	return 0;
}

#endif /* _WIN32 */

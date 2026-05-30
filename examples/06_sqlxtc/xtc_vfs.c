/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xtc_vfs.c
 *	An xtc-native SQLite VFS (shim over the platform default).
 *
 *	The VFS owns every byte of database I/O.  Per-file state is
 *	allocated with the xtc allocator; reads, writes, and syncs are
 *	counted and timed with xtc_stats.  Path operations and the
 *	byte-range file locks delegate to the base VFS, so locking
 *	stays POSIX-correct while the data path becomes ours.
 *
 *	The shim layout places our bookkeeping first, then the base
 *	VFS's file object inline:
 *
 *	    [ struct xtc_file | <base sqlite3_file, szOsFile bytes> ]
 *
 *	szOsFile is sized to cover both, so SQLite allocates the whole
 *	thing in one shot and the base VFS sees a properly aligned
 *	sqlite3_file at the tail.
 */

#include "xtc_vfs.h"
#include "sqlite/sqlite3.h"

#include <string.h>

#include "xtc_int.h"        /* __os_malloc/__os_free, __os_clock_mono */
#include "xtc_stats.h"
#include "xtc_blocking.h"   /* offload blocking disk I/O off the loop */

/* ---- blocking-I/O offload helpers ----
 *
 * The reactor thread must not stall on a disk read/write/fsync.  Each
 * blocking base-VFS call is packaged into a closure and run via
 * xtc_blocking_run, which parks the calling process on a loop (a pool
 * thread does the syscall, contending processes keep running) and
 * falls back to a synchronous call off a loop (startup, the
 * standalone tests).  buf and the arg struct live on the parked
 * fiber's stack, which persists across the park. */
struct io_rw_args {
	sqlite3_file *real;
	void         *buf;
	int           n;
	sqlite3_int64 off;
};
struct io_sync_args {
	sqlite3_file *real;
	int           flags;
};

static int
io_read_fn(void *a)
{
	struct io_rw_args *r = a;
	return r->real->pMethods->xRead(r->real, r->buf, r->n, r->off);
}
static int
io_write_fn(void *a)
{
	struct io_rw_args *r = a;
	return r->real->pMethods->xWrite(r->real, r->buf, r->n, r->off);
}
static int
io_sync_fn(void *a)
{
	struct io_sync_args *s = a;
	return s->real->pMethods->xSync(s->real, s->flags);
}

/* ---- per-file shim object ---- */
struct xtc_file {
	sqlite3_file   base;     /* methods table; must be first */
	sqlite3_file  *real;     /* base VFS file, inline after this struct */
};

/* ---- instrumentation ---- */
static xtc_counter_t *g_c_reads;
static xtc_counter_t *g_c_writes;
static xtc_counter_t *g_c_bytes_read;
static xtc_counter_t *g_c_bytes_written;
static xtc_counter_t *g_c_syncs;
static xtc_hist_t    *g_h_read_us;
static xtc_hist_t    *g_h_write_us;

static sqlite3_vfs   *g_base;          /* underlying default VFS */
static int            g_registered;    /* register-once guard */

/* now() in nanoseconds; 0 if the clock is unavailable. */
static int64_t
xtc_vfs_now_ns(void)
{
	int64_t t = 0;
	(void)__os_clock_mono(&t);
	return t;
}

/* ---- io_methods: forward to the base file, instrument I/O ---- */

static int
xv_close(sqlite3_file *pf)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	int rc = SQLITE_OK;

	if (p->real != NULL && p->real->pMethods != NULL)
		rc = p->real->pMethods->xClose(p->real);
	p->real = NULL;
	return rc;
}

static int
xv_read(sqlite3_file *pf, void *buf, int n, sqlite3_int64 off)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	struct io_rw_args a = { p->real, buf, n, off };
	int64_t t0 = xtc_vfs_now_ns();
	int rc;

	if (xtc_blocking_run(io_read_fn, &a, &rc) != XTC_OK)
		rc = io_read_fn(&a);        /* offload unavailable: do it inline */

	xtc_counter_inc(g_c_reads);
	if (rc == SQLITE_OK)
		xtc_counter_add(g_c_bytes_read, n);
	xtc_hist_record(g_h_read_us, (xtc_vfs_now_ns() - t0) / 1000);
	return rc;
}

static int
xv_write(sqlite3_file *pf, const void *buf, int n, sqlite3_int64 off)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	struct io_rw_args a = { p->real, (void *)buf, n, off };
	int64_t t0 = xtc_vfs_now_ns();
	int rc;

	if (xtc_blocking_run(io_write_fn, &a, &rc) != XTC_OK)
		rc = io_write_fn(&a);

	xtc_counter_inc(g_c_writes);
	if (rc == SQLITE_OK)
		xtc_counter_add(g_c_bytes_written, n);
	xtc_hist_record(g_h_write_us, (xtc_vfs_now_ns() - t0) / 1000);
	return rc;
}

static int
xv_truncate(sqlite3_file *pf, sqlite3_int64 size)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xTruncate(p->real, size);
}

static int
xv_sync(sqlite3_file *pf, int flags)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	struct io_sync_args a = { p->real, flags };
	int rc;
	xtc_counter_inc(g_c_syncs);
	if (xtc_blocking_run(io_sync_fn, &a, &rc) != XTC_OK)
		rc = io_sync_fn(&a);
	return rc;
}

static int
xv_file_size(sqlite3_file *pf, sqlite3_int64 *out)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xFileSize(p->real, out);
}

static int
xv_lock(sqlite3_file *pf, int level)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xLock(p->real, level);
}

static int
xv_unlock(sqlite3_file *pf, int level)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xUnlock(p->real, level);
}

static int
xv_check_reserved_lock(sqlite3_file *pf, int *out)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xCheckReservedLock(p->real, out);
}

static int
xv_file_control(sqlite3_file *pf, int op, void *arg)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xFileControl(p->real, op, arg);
}

static int
xv_sector_size(sqlite3_file *pf)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xSectorSize(p->real);
}

static int
xv_device_characteristics(sqlite3_file *pf)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xDeviceCharacteristics(p->real);
}

/* Shared-memory and memory-map methods (WAL): forward when present. */
static int
xv_shm_map(sqlite3_file *pf, int pg, int sz, int ext, void volatile **pp)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	if (p->real->pMethods->iVersion < 2 ||
	    p->real->pMethods->xShmMap == NULL)
		return SQLITE_IOERR_SHMOPEN;
	return p->real->pMethods->xShmMap(p->real, pg, sz, ext, pp);
}

static int
xv_shm_lock(sqlite3_file *pf, int off, int n, int flags)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xShmLock(p->real, off, n, flags);
}

static void
xv_shm_barrier(sqlite3_file *pf)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	p->real->pMethods->xShmBarrier(p->real);
}

static int
xv_shm_unmap(sqlite3_file *pf, int del)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	return p->real->pMethods->xShmUnmap(p->real, del);
}

static const sqlite3_io_methods xtc_io_methods = {
	2,                              /* iVersion (WAL shm methods) */
	xv_close,
	xv_read,
	xv_write,
	xv_truncate,
	xv_sync,
	xv_file_size,
	xv_lock,
	xv_unlock,
	xv_check_reserved_lock,
	xv_file_control,
	xv_sector_size,
	xv_device_characteristics,
	xv_shm_map,
	xv_shm_lock,
	xv_shm_barrier,
	xv_shm_unmap,
	0,                              /* xFetch  (iVersion 3) */
	0                               /* xUnfetch */
};

/* ---- VFS methods ---- */

static int
xv_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *pf,
        int flags, int *out_flags)
{
	struct xtc_file *p = (struct xtc_file *)pf;
	int rc;

	(void)vfs;
	memset(p, 0, sizeof *p);
	p->real = (sqlite3_file *)((char *)p + sizeof(struct xtc_file));
	rc = g_base->xOpen(g_base, name, p->real, flags, out_flags);
	if (rc != SQLITE_OK) {
		p->base.pMethods = NULL;
		return rc;
	}
	/* Only install our methods if the base file actually opened. */
	p->base.pMethods = (p->real->pMethods != NULL)
	    ? &xtc_io_methods : NULL;
	return SQLITE_OK;
}

static int
xv_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
	(void)vfs;
	return g_base->xDelete(g_base, name, sync_dir);
}

static int
xv_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
	(void)vfs;
	return g_base->xAccess(g_base, name, flags, out);
}

static int
xv_full_pathname(sqlite3_vfs *vfs, const char *name, int n, char *out)
{
	(void)vfs;
	return g_base->xFullPathname(g_base, name, n, out);
}

static void *
xv_dlopen(sqlite3_vfs *vfs, const char *name)
{
	(void)vfs;
	return g_base->xDlOpen ? g_base->xDlOpen(g_base, name) : NULL;
}

static void
xv_dlerror(sqlite3_vfs *vfs, int n, char *out)
{
	(void)vfs;
	if (g_base->xDlError) g_base->xDlError(g_base, n, out);
}

static void (*xv_dlsym(sqlite3_vfs *vfs, void *h, const char *sym))(void)
{
	(void)vfs;
	return g_base->xDlSym ? g_base->xDlSym(g_base, h, sym) : NULL;
}

static void
xv_dlclose(sqlite3_vfs *vfs, void *h)
{
	(void)vfs;
	if (g_base->xDlClose) g_base->xDlClose(g_base, h);
}

static int
xv_randomness(sqlite3_vfs *vfs, int n, char *out)
{
	(void)vfs;
	return g_base->xRandomness(g_base, n, out);
}

static int
xv_sleep(sqlite3_vfs *vfs, int us)
{
	(void)vfs;
	return g_base->xSleep(g_base, us);
}

static int
xv_current_time(sqlite3_vfs *vfs, double *out)
{
	(void)vfs;
	return g_base->xCurrentTime(g_base, out);
}

static int
xv_get_last_error(sqlite3_vfs *vfs, int n, char *out)
{
	(void)vfs;
	return g_base->xGetLastError ? g_base->xGetLastError(g_base, n, out)
	                             : 0;
}

static int
xv_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
	(void)vfs;
	if (g_base->iVersion >= 2 && g_base->xCurrentTimeInt64)
		return g_base->xCurrentTimeInt64(g_base, out);
	return SQLITE_ERROR;
}

static sqlite3_vfs xtc_vfs;     /* filled in by xtc_vfs_register */

int
xtc_vfs_register(int make_default)
{
	if (g_registered)
		return SQLITE_OK;

	g_base = sqlite3_vfs_find(NULL);
	if (g_base == NULL)
		return SQLITE_ERROR;

	/* Counters and latency histograms.  Failure to create a stat
	 * is non-fatal: the VFS still functions, it just is not timed. */
	(void)xtc_counter_create("sqlxtc.vfs.reads", &g_c_reads);
	(void)xtc_counter_create("sqlxtc.vfs.writes", &g_c_writes);
	(void)xtc_counter_create("sqlxtc.vfs.bytes_read", &g_c_bytes_read);
	(void)xtc_counter_create("sqlxtc.vfs.bytes_written",
	    &g_c_bytes_written);
	(void)xtc_counter_create("sqlxtc.vfs.syncs", &g_c_syncs);
	(void)xtc_hist_create("sqlxtc.vfs.read_us", &g_h_read_us);
	(void)xtc_hist_create("sqlxtc.vfs.write_us", &g_h_write_us);

	memset(&xtc_vfs, 0, sizeof xtc_vfs);
	xtc_vfs.iVersion = 2;
	xtc_vfs.szOsFile = (int)sizeof(struct xtc_file) + g_base->szOsFile;
	xtc_vfs.mxPathname = g_base->mxPathname;
	xtc_vfs.zName = "xtc";
	xtc_vfs.xOpen = xv_open;
	xtc_vfs.xDelete = xv_delete;
	xtc_vfs.xAccess = xv_access;
	xtc_vfs.xFullPathname = xv_full_pathname;
	xtc_vfs.xDlOpen = xv_dlopen;
	xtc_vfs.xDlError = xv_dlerror;
	xtc_vfs.xDlSym = xv_dlsym;
	xtc_vfs.xDlClose = xv_dlclose;
	xtc_vfs.xRandomness = xv_randomness;
	xtc_vfs.xSleep = xv_sleep;
	xtc_vfs.xCurrentTime = xv_current_time;
	xtc_vfs.xGetLastError = xv_get_last_error;
	xtc_vfs.xCurrentTimeInt64 = xv_current_time_int64;

	if (sqlite3_vfs_register(&xtc_vfs, make_default) != SQLITE_OK)
		return SQLITE_ERROR;

	g_registered = 1;
	return SQLITE_OK;
}

void
xtc_vfs_get_stats(xtc_vfs_stats_t *out)
{
	if (out == NULL)
		return;
	memset(out, 0, sizeof *out);
	if (g_c_reads != NULL) out->reads = xtc_counter_read(g_c_reads);
	if (g_c_writes != NULL) out->writes = xtc_counter_read(g_c_writes);
	if (g_c_bytes_read != NULL)
		out->bytes_read = xtc_counter_read(g_c_bytes_read);
	if (g_c_bytes_written != NULL)
		out->bytes_written = xtc_counter_read(g_c_bytes_written);
	if (g_c_syncs != NULL) out->syncs = xtc_counter_read(g_c_syncs);
	if (g_h_read_us != NULL) {
		out->read_p50_us = xtc_hist_quantile(g_h_read_us, 0.50);
		out->read_p99_us = xtc_hist_quantile(g_h_read_us, 0.99);
	}
	if (g_h_write_us != NULL) {
		out->write_p50_us = xtc_hist_quantile(g_h_write_us, 0.50);
		out->write_p99_us = xtc_hist_quantile(g_h_write_us, 0.99);
	}
}

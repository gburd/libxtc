/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_dio.c
 *	The log on direct I/O: block-aligned writes that rewrite the partial
 *	last block, with the frames still contiguous.
 *
 *	  1. the fd is O_DIRECT (where the filesystem can do 4 KiB direct
 *	     I/O; the test says so and checks the rest buffered otherwise);
 *	  2. records of every size class -- a few bytes, a block straddler,
 *	     a big record written past the ring (wal_append_direct), enough
 *	     volume to wrap the 8 MiB ring several times -- from 8 threads:
 *	     every commit acked, and a scan returns every record with its
 *	     exact bytes, in LSN order, nothing after;
 *	  3. a reopen (append) at an end that is NOT block aligned, with a
 *	     big record and then small ones right after it: the earlier
 *	     frames of the shared block survive the rewrites;
 *	  4. the same after a checkpoint rebinds the handle to the
 *	     compacted file;
 *	  5. the file holds no padding: the log's end (wal_size) equals the
 *	     sum of the frames;
 *	  6. kill -9: a child process commits from 2 threads and reports each
 *	     acked commit's id down a pipe; the parent SIGKILLs it mid-load,
 *	     then every acked commit must replay with its bytes (4 rounds
 *	     appending to the same log, so each round reopens a log that a
 *	     killed writer left mid-block).
 *
 *	A direct write that did not rewrite the earlier bytes of its first
 *	block (or wrote unaligned) fails 2-4.
 */

#include "wal.c"

#include <signal.h>
#include <sys/wait.h>

#include "t_tmp.h"

#define NTH  8
#define PER  2500
#define BIGL ((3u << 20) + 333)      /* > WAL_RING / 4: written direct */

struct rec_id { uint32_t th, i; };

static wal_t           *g_w;
static _Atomic long     g_ok, g_bad;
static _Atomic uint64_t g_bytes;

static uint32_t
r_len(uint32_t th, uint32_t i)
{
	if (th == NTH) {                 /* the single-threaded phases */
		i -= PER;
		return i == 3 || i == 50 ? BIGL + 1 : 9 + (i * 37) % 700;
	}
	if (th == 3 && i % 997 == 500)
		return BIGL + i;
	if (i % 61 == 0)
		return 4000 + (i * 7) % 300;     /* straddles a block */
	return 9 + (i * 2654435761u + th) % 500;
}

static uint8_t
r_byte(uint32_t th, uint32_t i, uint32_t k)
{
	return (uint8_t)(th * 31 + i * 7 + k * 13 + (k >> 8));
}

static int
fill(uint8_t *buf, uint32_t th, uint32_t i)
{
	uint32_t l = r_len(th, i), k;
	memcpy(buf, &th, 4);
	memcpy(buf + 4, &i, 4);
	for (k = 8; k < l; k++)
		buf[k] = r_byte(th, i, k);
	return (int)l;
}

static void *
committer(void *arg)
{
	uint32_t th = (uint32_t)(uintptr_t)arg, i;
	uint8_t *buf = malloc(BIGL + 4096);
	if (buf == NULL)
		return NULL;
	for (i = 0; i < PER; i++) {
		int l = fill(buf, th, i);
		if (wal_commit_sync(g_w, buf, (uint32_t)l, NULL) == XTC_OK) {
			atomic_fetch_add(&g_ok, 1);
			atomic_fetch_add(&g_bytes, (uint64_t)l + WAL_REC_HDR +
			    WAL_REC_CRC);
		} else {
			atomic_fetch_add(&g_bad, 1);
		}
	}
	free(buf);
	return NULL;
}

struct seen {
	long n, bad;
	uint64_t prev;
	uint8_t *got;                    /* got[th * PER2 + i] */
};
#define PER2 (PER + 64)

static int
check_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct seen *s = user;
	const uint8_t *p = rec;
	uint32_t th, i, k;
	if (lsn <= s->prev)
		s->bad++;
	s->prev = lsn;
	s->n++;
	if (len < 8) {
		if (!(len == 4 && memcmp(p, "ckpt", 4) == 0))
			s->bad++;
		return 0;
	}
	memcpy(&th, p, 4);
	memcpy(&i, p + 4, 4);
	if (th >= NTH + 2 || i >= PER2 || len != r_len(th, i)) {
		s->bad++;
		return 0;
	}
	for (k = 8; k < len; k++)
		if (p[k] != r_byte(th, i, k)) {
			s->bad++;
			return 0;
		}
	if (s->got[th * PER2 + i]++)
		s->bad++;                    /* a duplicate */
	return 0;
}

/* Scan the log: every record of threads [0, nth) x [0, per) and of the
 * extra ids in [PER, PER + extra) for thread NTH (the single-threaded
 * phases), each exactly once, nothing else but checkpoint records. */
static int
scan_all(const char *path, int nth, uint32_t per, uint32_t extra, long ckpt)
{
	struct seen s;
	uint32_t th, i;
	long missing = 0;
	memset(&s, 0, sizeof s);
	s.got = calloc((size_t)(NTH + 2) * PER2, 1);
	if (s.got == NULL)
		return 0;
	CK(wal_scan(path, check_cb, &s) == XTC_OK);
	for (th = 0; th < (uint32_t)nth; th++)
		for (i = 0; i < per; i++)
			missing += s.got[th * PER2 + i] != 1;
	for (i = 0; i < extra; i++)
		missing += s.got[NTH * PER2 + PER + i] != 1;
	printf("    scan: %ld frames, %ld bad, %ld missing\n", s.n, s.bad,
	    missing);
	CK(s.bad == 0 && missing == 0);
	CK(s.n == (long)nth * per + extra + ckpt);
	free(s.got);
	return s.bad == 0 && missing == 0;
}

/* Single-threaded commits of ids [PER + from, PER + to) as thread NTH
 * (ids PER + 3 and PER + 50 are big records, see r_len). */
static void
more(uint32_t from, uint32_t to)
{
	uint8_t *buf = malloc(BIGL + 8192);
	uint32_t i;
	if (buf == NULL)
		return;
	for (i = from; i < to; i++) {
		int l = fill(buf, NTH, PER + i);
		CK(wal_commit_sync(g_w, buf, (uint32_t)l, NULL) == XTC_OK);
		atomic_fetch_add(&g_bytes, (uint64_t)l + WAL_REC_HDR + WAL_REC_CRC);
	}
	free(buf);
}

/* 6: the child's committers write each acked (th, i) to the pipe */
static int g_pipe = -1;

static void *
kill_committer(void *arg)
{
	uint32_t th = (uint32_t)(uintptr_t)arg, i, id[2];
	uint8_t *buf = malloc(BIGL + 4096);
	if (buf == NULL)
		return NULL;
	for (i = 0; i < PER; i++) {
		int l = fill(buf, th, i);
		if (wal_commit_sync(g_w, buf, (uint32_t)l, NULL) != XTC_OK)
			break;
		id[0] = th;
		id[1] = i;
		if (write(g_pipe, id, sizeof id) != (ssize_t)sizeof id)
			break;
	}
	free(buf);
	return NULL;
}

/* One round: a child appends as threads round * 2 and round * 2 + 1
 * (disjoint ids per round) and is killed after 300 + 150 * round acks. */
static int
kill_round(const char *path, int round, uint8_t *acked)
{
	int pfd[2], st, killed = 0;
	pid_t pid;
	uint32_t id[2];
	long n = 0;
	struct seen s;
	ssize_t r;
	if (pipe(pfd) != 0)
		return 0;
	if ((pid = fork()) < 0)
		return 0;
	if (pid == 0) {
		wal_opts_t wo;
		pthread_t t[NTH];
		long k;
		close(pfd[0]);
		g_pipe = pfd[1];
		memset(&wo, 0, sizeof wo);
		wo.path = path;
		wo.append = 1;
		if (wal_open(&wo, &g_w) != XTC_OK)
			_exit(2);
		for (k = 0; k < 2; k++)
			(void)pthread_create(&t[k], NULL, kill_committer,
			    (void *)(uintptr_t)(round * 2 + k));
		for (k = 0; k < 2; k++)
			(void)pthread_join(t[k], NULL);
		_exit(0);
	}
	close(pfd[1]);
	while ((r = read(pfd[0], id, sizeof id)) == (ssize_t)sizeof id) {
		if (id[0] < NTH && id[1] < PER2)
			acked[id[0] * PER2 + id[1]] = 1;
		if (++n == 300 + round * 150 && !killed) {
			(void)kill(pid, SIGKILL);  /* mid-load */
			killed = 1;
		}
	}
	close(pfd[0]);
	(void)waitpid(pid, &st, 0);
	memset(&s, 0, sizeof s);
	s.got = calloc((size_t)(NTH + 2) * PER2, 1);
	if (s.got == NULL)
		return 0;
	CK(wal_scan(path, check_cb, &s) == XTC_OK);
	CK(s.bad == 0);
	for (n = 0; n < (long)NTH * PER2; n++)
		if (acked[n] && s.got[n] != 1) {
			fprintf(stderr, "  round %d: acked (%ld, %ld) not replayed\n",
			    round, n / PER2, n % PER2);
			CK(0);
			break;
		}
	printf("    kill -9 round %d: child %s, %ld frames replayed, %ld bad\n",
	    round, WIFSIGNALED(st) ? "killed" : "exited", s.n, s.bad);
	free(s.got);
	return 1;
}

static void
dump1(wal_emit_fn emit, void *ctx, void *user)
{
	(void)user;
	emit(ctx, "ckpt", 4);
}

int
main(void)
{
	char path[256];
	wal_opts_t wo;
	pthread_t t[NTH];
	struct seen s;
	uint8_t *acked;
	uint64_t end;
	long k;
	int fd, fl, dio;

	t_tmpl(path, sizeof path, "sqlxtc-waldio");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	CK(wal_open(&wo, &g_w) == XTC_OK);
	if (g_w == NULL)
		return 1;
	fl = fcntl(wal_fd(g_w), F_GETFL);
	dio = fl >= 0 && (fl & O_DIRECT) != 0;
	printf("  log fd: %s\n", dio ? "O_DIRECT|O_DSYNC" :
	    "buffered (no 4 KiB direct I/O here)");
	CK(dio == g_w->dio);

	/* 2. many sizes from 8 threads, wrapping the ring */
	for (k = 0; k < NTH; k++)
		CK(pthread_create(&t[k], NULL, committer, (void *)(uintptr_t)k) == 0);
	for (k = 0; k < NTH; k++)
		(void)pthread_join(t[k], NULL);
	CK(atomic_load(&g_ok) == (long)NTH * PER && atomic_load(&g_bad) == 0);
	end = wal_size(g_w);
	CK(end == atomic_load(&g_bytes));   /* 5. no padding in the log */
	printf("  %ld commits, log %llu bytes (%.1f ring wraps)\n",
	    atomic_load(&g_ok), (unsigned long long)end,
	    (double)end / WAL_RING);
	printf("  end %% 4096 = %llu (the reopen below starts mid-block)\n",
	    (unsigned long long)(end % WAL_BLK));
	wal_close(g_w);
	CK(scan_all(path, NTH, PER, 0, 0));

	/* 3. reopen at an unaligned end; big record, then small ones */
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	wo.append = 1;
	CK(wal_open(&wo, &g_w) == XTC_OK);
	CK(wal_size(g_w) == end);
	more(0, 40);
	end = wal_size(g_w);
	wal_close(g_w);
	CK(scan_all(path, NTH, PER, 40, 0));
	CK(end == atomic_load(&g_bytes));

	/* 4. after a checkpoint rebind (the compacted file) */
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	wo.append = 1;
	CK(wal_open(&wo, &g_w) == XTC_OK);
	CK(wal_checkpoint(g_w, path, dump1, NULL) == XTC_OK);
	fl = fcntl(wal_fd(g_w), F_GETFL);
	CK(fl >= 0 && ((fl & O_DIRECT) != 0) == dio && g_w->dio == dio);
	more(40, 60);
	wal_close(g_w);
	memset(&s, 0, sizeof s);
	s.got = calloc((size_t)(NTH + 2) * PER2, 1);
	CK(s.got != NULL && wal_scan(path, check_cb, &s) == XTC_OK);
	CK(s.bad == 0 && s.n == 21);      /* ckpt + 20 */
	for (k = 40; k < 60 && s.got != NULL; k++)
		CK(s.got[NTH * PER2 + PER + (uint32_t)k] == 1);
	printf("    after the checkpoint: %ld frames, %ld bad\n", s.n, s.bad);
	free(s.got);
	unlink(path);

	/* 6. kill -9 mid-load, 4 rounds on one log (2 threads x round) */
	t_tmpl(path, sizeof path, "sqlxtc-waldio");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	acked = calloc((size_t)(NTH + 2) * PER2, 1);
	CK(acked != NULL);
	for (k = 0; acked != NULL && k < NTH / 2; k++)
		CK(kill_round(path, (int)k, acked));
	free(acked);
	unlink(path);
	if (g_fail) {
		fprintf(stderr, "FAIL: test_wal_dio\n");
		return 1;
	}
	printf("  ok   direct-I/O log: every record exact across ring wraps, "
	    "big records, an unaligned reopen and a checkpoint rebind\n");
	return 0;
}

/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_seek.c
 *	bt_seek_ge, the cursor-free "first key >= probe" read that the
 *	engine's point reads use.
 *
 *	S1  oracle: random keys with holes, a deleted run (merged leaves),
 *	    out-of-line values, probes on, between and past the keys; every
 *	    answer must be the first live key >= the probe with its whole
 *	    value.  4 KiB and 16 KiB pages.
 *	S2  concurrent: 4 OS threads; churners insert and delete dense
 *	    keys (splits and merges, a pool small enough to evict) while
 *	    readers seek.  A STABLE key (never deleted) bounds every answer:
 *	    the result must lie in [probe, first stable key >= probe] and
 *	    carry its own key's value, so a seek that followed a stale
 *	    right link or skipped a live key is caught.
 *
 *	S3  in the same churn, readers take reverse batches (bt_seek_lt_n,
 *	    the descending index walk): every batch is strictly descending,
 *	    below its bound, and skips no stable key between the bound and
 *	    its last key.
 *	S4  bt_seek_ge_n, the batched seek behind an index walk's row
 *	    fetches: in S1 every settled answer equals the oracle's and most
 *	    probes are settled by the batch; in the S2 churn every settled
 *	    answer obeys S2's bounds and carries its own key's value.
 *
 *	Built twice: test_btree_seek (the optimistic, version-validated
 *	path) and test_btree_seek_latched (-DBT_OLC_ENABLED=0: the latched
 *	cursor fallback).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "t_tmp.h"

/* The optimistic reads are compiled out of the latched build and under
 * TSan (btree.c BT_OLC_ENABLED): there the batch settles nothing. */
#if defined(__SANITIZE_THREAD__)
#define T_OLC 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define T_OLC 0
#endif
#endif
#if !defined(T_OLC) && defined(BT_OLC_ENABLED)
#define T_OLC BT_OLC_ENABLED
#endif
#ifndef T_OLC
#define T_OLC 1
#endif

static uint32_t
rnd(uint64_t *s)
{
	*s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
	return (uint32_t)*s;
}

static void
fill(unsigned char *b, size_t n, uint32_t seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		b[i] = (unsigned char)(seed * 131u + i * 7u + (i >> 8));
}

static bm_t *
mk(char *p, size_t cap, uint32_t ps, uint32_t nf)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	int fd;

	t_tmpl(p, cap, "btseek");
	fd = mkstemp(p);
	if (fd < 0) { perror("mkstemp"); exit(2); }
	(void)close(fd);
	bo.path = p;
	bo.page_size = ps;
	bo.n_frames = nf;
	bo.cool_pct = 25;
	if (bm_create(&bo, &bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); exit(2); }
	return bm;
}

static void
done(bt_t *bt, bm_t *bm, const char *p)
{
	char wal[288];

	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(p);
	snprintf(wal, sizeof wal, "%s-wal", p);
	(void)unlink(wal);
}

/* ---- S1: oracle ---------------------------------------------------- */

#define S1_N 20000

static uint16_t
s1_vlen(int i)
{
	return (uint16_t)(i % 50 == 0 ? 5000 : 20);
}

static void
t_oracle(uint32_t ps)
{
	char p[256], k[16], q[16], got[64];
	bm_t *bm = mk(p, sizeof p, ps, 2048);
	bt_t *bt = NULL;
	static unsigned char live[S1_N];
	static unsigned char v[8192], out[8192];
	uint64_t s = 7 + ps;
	uint16_t gl = 0, vl = 0;
	int i, wrong = 0, probes = 0;

	memset(live, 0, sizeof live);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(bt_seek_ge(bt, "a", 1, got, sizeof got, &gl, out, sizeof out, &vl)
	    == XTC_E_NOTFOUND);                       /* empty tree */
	for (i = 0; i < S1_N; i++) {
		if (rnd(&s) % 3 == 0) continue;           /* holes */
		snprintf(k, sizeof k, "s%07d", i);
		fill(v, s1_vlen(i), (uint32_t)i);
		if (bt_insert(bt, k, 8, v, s1_vlen(i)) == XTC_OK)
			live[i] = 1;
	}
	for (i = 5000; i < 9000; i++) {               /* empty, merged leaves */
		snprintf(k, sizeof k, "s%07d", i);
		if (live[i] && bt_delete(bt, k, 8) == XTC_OK) live[i] = 0;
	}
	for (i = 0; i < S1_N; i += 3) {
		int j, rc, form;
		for (form = 0; form < 2; form++) {
			/* form 0: the key itself; form 1: just above it (a 9-byte
			 * probe sorts after key i and before key i+1). */
			uint16_t ql = 8;
			snprintf(q, sizeof q, "s%07d", i);
			if (form == 1) { q[8] = '!'; ql = 9; }
			for (j = form ? i + 1 : i; j < S1_N && !live[j]; j++)
				;
			gl = vl = 0;
			rc = bt_seek_ge(bt, q, ql, got, sizeof got, &gl, out,
			    sizeof out, &vl);
			probes++;
			if (j >= S1_N) {
				if (rc != XTC_E_NOTFOUND) wrong++;
				continue;
			}
			snprintf(k, sizeof k, "s%07d", j);
			fill(v, s1_vlen(j), (uint32_t)j);
			if (rc != XTC_OK || gl != 8 || memcmp(got, k, 8) != 0 ||
			    vl != s1_vlen(j) || memcmp(out, v, vl) != 0) {
				if (wrong < 5)
					fprintf(stderr, "  ps=%u probe %.*s: rc=%d got %.*s "
					    "want %s\n", ps, ql, q, rc, gl, got, k);
				wrong++;
			}
		}
	}
	/* S4 (oracle half): the same probes in shuffled batches of 1..64
	 * through bt_seek_ge_n.  A settled answer must equal bt_seek_ge's;
	 * an unsettled one (XTC_E_AGAIN) is allowed but must be rare on a
	 * quiet tree. */
	{
		static unsigned char nb[64 * 8192];
		bt_probe_t pr[BT_SEEKN_MAX];
		char qk[BT_SEEKN_MAX][16];
		int nq, settled = 0, total = 0, nwrong = 0;
		for (i = 0; i < S1_N; i += nq) {
			int j2;
			nq = 1 + (int)(rnd(&s) % BT_SEEKN_MAX);
			for (j2 = 0; j2 < nq; j2++) {
				int x = (int)(rnd(&s) % (S1_N + 10));
				snprintf(qk[j2], sizeof qk[j2], "s%07d", x);
				pr[j2].key = qk[j2];
				pr[j2].klen = (uint16_t)((rnd(&s) & 1) ? 8 : 9);
				if (pr[j2].klen == 9) qk[j2][8] = '!';
			}
			CK(bt_seek_ge_n(bt, pr, nq, nb, sizeof nb) == XTC_OK);
			for (j2 = 0; j2 < nq; j2++) {
				int rc2;
				total++;
				if (pr[j2].rc == XTC_E_AGAIN) continue;
				settled++;
				rc2 = bt_seek_ge(bt, pr[j2].key, pr[j2].klen, got,
				    sizeof got, &gl, out, sizeof out, &vl);
				if (pr[j2].rc != XTC_OK || rc2 != XTC_OK ||
				    pr[j2].kolen != gl || pr[j2].vlen != vl ||
				    memcmp(nb + pr[j2].koff, got, gl) != 0 ||
				    memcmp(nb + pr[j2].voff, out, vl) != 0)
					nwrong++;
			}
		}
		/* Unsettled: the answer is in the next leaf (the deleted run,
		 * past the last key) or out of line -- a quarter of these
		 * probes; the rest must be settled where the batch can read. */
		CK(nwrong == 0);
		CK(!T_OLC || settled * 2 >= total);
		printf("  %s   S4 ps=%u: %d batched probes, %d settled by the "
		    "batch, %d wrong\n", nwrong == 0 && (!T_OLC ||
		    settled * 2 >= total) ? "ok" : "FAIL", ps, total, settled,
		    nwrong);
	}

	/* Past every key; a NULL value buffer; a key buffer too small. */
	CK(bt_seek_ge(bt, "t", 1, got, sizeof got, &gl, NULL, 0, NULL) ==
	    XTC_E_NOTFOUND);
	CK(bt_seek_ge(bt, "", 0, got, sizeof got, &gl, NULL, 0, &vl) == XTC_OK &&
	    gl == 8);
	CK(bt_seek_ge(bt, "s", 1, got, 4, &gl, NULL, 0, NULL) == XTC_E_INVAL);
	CK(wrong == 0);
	printf("  ok   S1 ps=%u: %d probes against the oracle, %d wrong\n", ps,
	    probes, wrong);
	done(bt, bm, p);
}

/* ---- S2: concurrent seeks vs splits + merges ------------------------ */

#define N_LOOPS     4
#define N_CHURNERS  4
#define N_READERS   4
#define S2_N        6001       /* key slots; slot 6000 is stable */
#define STABLE(i)   ((i) % 8 == 0)
#define ROUNDS      3
#define S2_FRAMES   32

static bm_t        *g_bm;
static bt_t        *g_bt;
static _Atomic int  g_active;
static _Atomic long g_reads, g_bad_range, g_bad_val, g_bad_rc;
static _Atomic long g_lt_reads, g_lt_bad;

static void
s2_key(int i, char *k)
{
	snprintf(k, 16, "k%07d", i);
}

static uint16_t
s2_vlen(int i)
{
	if (STABLE(i))
		return (uint16_t)(i % 512 == 0 ? 3000 : 24);   /* some out of line */
	return 16;
}

static void
s2_val(int i, unsigned char *v)
{
	fill(v, s2_vlen(i), (uint32_t)i * 2654435761u);
}

static void
churner(void *arg)
{
	long w = (long)arg;
	unsigned char v[64];
	char k[16];
	int r, i;

	for (r = 0; r < ROUNDS; r++) {
		for (i = (int)w + 1; i < S2_N; i += N_CHURNERS) {
			if (STABLE(i)) continue;
			s2_key(i, k); s2_val(i, v);
			(void)bt_insert(g_bt, k, 8, v, s2_vlen(i));
		}
		for (i = (int)w + 1; i < S2_N; i += N_CHURNERS) {
			if (STABLE(i)) continue;
			s2_key(i, k);
			(void)bt_delete(g_bt, k, 8);
		}
	}
	if (atomic_fetch_sub(&g_active, 1) == 1)
		bm_provider_stop(g_bm);
}

static void
reader(void *arg)
{
	uint64_t s = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
	/* Per reader, not per thread: a seek can park on page I/O and let
	 * another reader on the same loop run. */
	unsigned char *out = malloc(4096), *want = malloc(4096);
	char q[16], got[64], hi[16];
	long n = 0;

	if (out == NULL || want == NULL) {
		atomic_fetch_add(&g_bad_rc, 1);
		free(out); free(want);
		return;
	}
	while (atomic_load(&g_active) > 0 || n < 2000) {
		int i = (int)(rnd(&s) % S2_N), j, gi, rc;
		uint16_t ql = 8, gl = 0, vl = 0;

		s2_key(i, q);
		if (rnd(&s) & 1) { q[8] = '!'; ql = 9; }  /* between keys */
		for (j = ql == 9 ? i + 1 : i; !STABLE(j); j++)
			;                                     /* the bound */
		rc = bt_seek_ge(g_bt, q, ql, got, sizeof got, &gl, out, 4096,
		    &vl);
		n++;
		if (j >= S2_N) {
			if (rc != XTC_E_NOTFOUND) atomic_fetch_add(&g_bad_rc, 1);
			goto next;
		}
		if (rc != XTC_OK || gl != 8) {
			atomic_fetch_add(&g_bad_rc, 1);
			goto next;
		}
		s2_key(j, hi);
		if (memcmp(got, q, ql < gl ? ql : gl) < 0 ||
		    (memcmp(got, q, 8) == 0 && ql == 9) ||
		    memcmp(got, hi, 8) > 0) {
			atomic_fetch_add(&g_bad_range, 1);   /* skipped a live key */
			goto next;
		}
		gi = atoi(got + 1);
		s2_val(gi, want);
		if (vl != s2_vlen(gi) || memcmp(out, want, vl) != 0)
			atomic_fetch_add(&g_bad_val, 1);
next:
		if ((n & 31) == 0)
			xtc_yield();
	}
	atomic_fetch_add(&g_reads, n);
	free(out);
	free(want);
}

/* S4: batches of probes through bt_seek_ge_n in the churn.  Each
 * settled answer obeys S2's bound check; unsettled ones are counted. */
static _Atomic long g_n_reads, g_n_settled, g_n_bad;

static void
reader_n(void *arg)
{
	uint64_t s = (uint64_t)(uintptr_t)arg * 0xA0761D6478BD642Full + 3;
	/* Per reader: a seek can park on page I/O. */
	unsigned char *nb = malloc(64 * 4096), *want = malloc(4096);
	bt_probe_t pr[BT_SEEKN_MAX];
	char q[BT_SEEKN_MAX][16], hi[16];
	long n = 0, settled = 0, bad = 0;

	if (nb == NULL || want == NULL) {
		atomic_fetch_add(&g_n_bad, 1);
		free(nb); free(want);
		return;
	}
	while (atomic_load(&g_active) > 0 || n < 2000) {
		int nq = 1 + (int)(rnd(&s) % BT_SEEKN_MAX), k, j, gi;
		for (k = 0; k < nq; k++) {
			s2_key((int)(rnd(&s) % S2_N), q[k]);
			pr[k].key = q[k];
			pr[k].klen = 8;
			if (rnd(&s) & 1) { q[k][8] = '!'; pr[k].klen = 9; }
		}
		if (bt_seek_ge_n(g_bt, pr, nq, nb, 64 * 4096) != XTC_OK) {
			bad++;
			goto next;
		}
		for (k = 0; k < nq; k++) {
			const char *got = (const char *)nb + pr[k].koff;
			int i = atoi(q[k] + 1);
			n++;
			if (pr[k].rc == XTC_E_AGAIN) continue;
			settled++;
			for (j = pr[k].klen == 9 ? i + 1 : i; !STABLE(j); j++)
				;
			if (pr[k].rc != XTC_OK || pr[k].kolen != 8 || j >= S2_N) {
				bad++;              /* a stable key bounds every probe */
				continue;
			}
			s2_key(j, hi);
			if (memcmp(got, q[k], 8) < 0 ||
			    (memcmp(got, q[k], 8) == 0 && pr[k].klen == 9) ||
			    memcmp(got, hi, 8) > 0) {
				bad++;
				continue;
			}
			memcpy(hi, got, 8);         /* the value follows the key */
			hi[8] = '\0';
			gi = atoi(hi + 1);
			s2_val(gi, want);
			if (pr[k].vlen != s2_vlen(gi) ||
			    memcmp(nb + pr[k].voff, want, pr[k].vlen) != 0)
				bad++;
		}
next:
		if ((n & 63) < nq)
			xtc_yield();
	}
	atomic_fetch_add(&g_n_reads, n);
	atomic_fetch_add(&g_n_settled, settled);
	atomic_fetch_add(&g_n_bad, bad);
	free(nb);
	free(want);
}

/* S3: a reverse batch below a random bound.  The answer must be the
 * largest keys below the bound in descending order: strictly
 * descending, all below it, and no stable key (never deleted) may lie
 * between the bound and the batch's last key without being in it. */
static void
reader_lt(void *arg)
{
	uint64_t s = (uint64_t)(uintptr_t)arg * 0xD1B54A32D192ED03ull + 7;
	uint8_t *buf = malloc(4096);   /* per reader: a seek can park */
	uint32_t off[64];
	uint16_t lens[64];
	char q[16];
	long n = 0;

	if (buf == NULL) {
		atomic_fetch_add(&g_lt_bad, 1);
		return;
	}
	while (atomic_load(&g_active) > 0 || n < 2000) {
		int i = 1 + (int)(rnd(&s) % (S2_N - 1)), got = 0, j, lo, bad = 0;
		int want = 1 + (int)(rnd(&s) % 40);
		s2_key(i, q);
		if (bt_seek_lt_n(g_bt, q, 8, buf, 4096, off, lens, want,
		    &got) != XTC_OK || got < 1 || got > want) {
			atomic_fetch_add(&g_lt_bad, 1);    /* key 0 is stable: >= 1 */
			goto next;
		}
		for (j = 0; j < got; j++) {
			const char *k = (const char *)buf + off[j];
			if (lens[j] != 8 || memcmp(k, q, 8) >= 0 ||
			    (j > 0 && memcmp(k, (const char *)buf + off[j - 1], 8) >= 0))
				bad = 1;
		}
		if (!bad) {
			/* Every stable key in [last returned, bound) is present. */
			lo = atoi((const char *)buf + off[got - 1] + 1);
			for (j = lo; j < i && !bad; j++) {
				int t, hit = 0;
				char k[16];
				if (!STABLE(j)) continue;
				s2_key(j, k);
				for (t = 0; t < got && !hit; t++)
					hit = memcmp(buf + off[t], k, 8) == 0;
				bad = !hit;
			}
		}
		if (bad)
			atomic_fetch_add(&g_lt_bad, 1);
next:
		n++;
		if ((n & 15) == 0)
			xtc_yield();
	}
	atomic_fetch_add(&g_lt_reads, n);
	free(buf);
}

static void
t_concurrent(void)
{
	char p[256], k[16];
	unsigned char v[4096];
	xtc_exec_t *ex = NULL;
	bt_stats_t ts;
	bm_stats_t bs;
	long c, r;
	int i;

	g_bm = mk(p, sizeof p, 4096, S2_FRAMES);
	CK(bt_open(g_bm, &g_bt) == XTC_OK);
	for (i = 0; i < S2_N; i++) {                  /* the stable keys */
		if (!STABLE(i)) continue;
		s2_key(i, k); s2_val(i, v);
		CK(bt_insert(g_bt, k, 8, v, s2_vlen(i)) == XTC_OK);
	}
	atomic_store(&g_active, N_CHURNERS);
	CK(xtc_exec_init(&ex, N_LOOPS) == XTC_OK);
	CK(bm_provider_spawn(g_bm, xtc_exec_loop(ex, 0), 1LL * 1000 * 1000,
	    NULL) == XTC_OK);
	for (c = 0; c < N_CHURNERS; c++)
		CK(xtc_proc_spawn(xtc_exec_loop(ex, (int)(c % N_LOOPS)), churner,
		    (void *)c, NULL, NULL) == XTC_OK);
	for (r = 0; r < N_READERS; r++)
		CK(xtc_proc_spawn(xtc_exec_loop(ex, (int)((r + 1) % N_LOOPS)),
		    reader, (void *)(r + 1), NULL, NULL) == XTC_OK);
	for (r = 0; r < N_READERS; r++)
		CK(xtc_proc_spawn(xtc_exec_loop(ex, (int)((r + 2) % N_LOOPS)),
		    reader_lt, (void *)(r + 1), NULL, NULL) == XTC_OK);
	for (r = 0; r < N_READERS; r++)
		CK(xtc_proc_spawn(xtc_exec_loop(ex, (int)((r + 3) % N_LOOPS)),
		    reader_n, (void *)(r + 1), NULL, NULL) == XTC_OK);
	CK(xtc_exec_run(ex) == XTC_OK);
	bm_provider_stop(g_bm);
	(void)xtc_exec_fini(ex);

	bt_get_stats(g_bt, &ts);
	bm_get_stats(g_bm, &bs);
	CK(atomic_load(&g_bad_range) == 0);
	CK(atomic_load(&g_bad_val) == 0);
	CK(atomic_load(&g_bad_rc) == 0);
	CK(atomic_load(&g_lt_bad) == 0);
	CK(atomic_load(&g_n_bad) == 0);
	CK(ts.splits > 0 && ts.merges > 0);           /* both SMOs raced */
	printf("  %s   S2: %ld concurrent seeks vs %d churners on %d threads "
	    "(splits=%llu merges=%llu evicted=%llu): %ld out of range, %ld bad "
	    "values, %ld bad rc\n", g_fail ? "FAIL" : "ok",
	    atomic_load(&g_reads), N_CHURNERS, N_LOOPS,
	    (unsigned long long)ts.splits, (unsigned long long)ts.merges,
	    (unsigned long long)bs.evicted, atomic_load(&g_bad_range),
	    atomic_load(&g_bad_val), atomic_load(&g_bad_rc));
	printf("  %s   S3: %ld concurrent reverse batches (bt_seek_lt_n) in the "
	    "same churn: %ld wrong\n", atomic_load(&g_lt_bad) ? "FAIL" : "ok",
	    atomic_load(&g_lt_reads), atomic_load(&g_lt_bad));
	printf("  %s   S4: %ld batched probes (bt_seek_ge_n) in the same churn, "
	    "%ld settled by the batch: %ld wrong\n",
	    atomic_load(&g_n_bad) ? "FAIL" : "ok", atomic_load(&g_n_reads),
	    atomic_load(&g_n_settled), atomic_load(&g_n_bad));
	done(g_bt, g_bm, p);
}

int
main(void)
{
	t_oracle(4096);
	t_oracle(16384);
	t_concurrent();
	if (g_fail) {
		fprintf(stderr, "test_btree_seek: FAILED\n");
		return 1;
	}
	printf("test_btree_seek: ALL PASS\n");
	return 0;
}

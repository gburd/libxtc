/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_timerq.c
 *	Priority-queue candidates for the per-loop timer heap
 *	(src/evt/timer.c), run on timer-shaped traces.  Standalone (no
 *	libxtc link) so each structure is measured alone, behind the same
 *	interface the loop uses: push, cancel-by-handle, pop-one-due,
 *	next-deadline.
 *
 *	Nodes are 72 bytes (sizeof(xtc_timer_t)), taken from a pool in a
 *	shuffled order so neighbours in a structure are not neighbours in
 *	memory, and every arm takes a fresh node (a park timer is a fresh
 *	allocation per arm).  A structure that reads the key through the
 *	node pointer pays the cache miss the real heap pays.
 *
 *	Structures:
 *	  bin    the current heap: node pointers, key read through the
 *	         pointer, swap-based sift (a copy of timer.c)
 *	  binh   bin with a hole-based sift (one idx write per level)
 *	  bins   binary heap, keys in their own array beside the node
 *	         pointers (SoA), hole-based sift
 *	  bini   binary heap, {key, node} pairs inline (AoS), hole sift.
 *	         binh, bins and bini make the same comparisons as bin, so
 *	         they pop in the SAME order, equal deadlines included
 *	         (checked: idhash must match bin's)
 *	  d4/d8  4-ary / 8-ary heap, {key, node} inline, hole-based sift
 *	  pair   pairing heap (two-pass), cancel = cut + merge
 *	  radix  radix heap on ns keys (Ahuja et al.), doubly-linked buckets
 *	  wheel  hierarchical timing wheel: 1.024 us ticks, 9 levels of 64
 *	         slots with occupancy bitmaps; fires up to one tick late
 *	  qh     QuickHeap in the SimdQuickHeap layout (pivot array,
 *	         per-bucket arrays), scalar, handles kept in the nodes so
 *	         cancel is O(1); every partition rewrites the moved nodes'
 *	         handles
 *	  qhl    the same with lazy cancel (mark, sweep when dead > live),
 *	         no handles, AVX-512 compress partition when the CPU has it
 *	  qhls   qhl forced scalar
 *
 *	Traces (one "unit" is what the ns figure is per):
 *	  idle   N live timers, period 1 s +-1%, a drain every B*P/N ns
 *	         (B = mean timers due per drain); unit = one fire + re-arm
 *	  load   N idle timers + 50 busy connections at 125k req/s, each
 *	         request cancels its connection's timer and arms a new one;
 *	         unit = one request or one idle fire
 *	  sleep  N fibers in xtc_proc_sleep(U(10us, 10ms)); unit = one fire
 *	  cancel N live 60 s timers + arm/cancel churn (4 outstanding);
 *	         unit = one arm + one cancel
 *	  mcs    the paper's MonotoneConstantSize: pop, push last+U(0,N);
 *	         unit = one pop + one push
 *	  same   N timers with one deadline (a DST virtual-time burst);
 *	         unit = one fire
 *
 *	Usage: bench_timerq [-t trace] [-n N] [-b B] [-i impl[,impl]]
 *	                    [-r reps] [-u units] [-l]
 *	  -l adds per-operation latency (rdtsc on x86-64, else
 *	     clock_gettime): p50/p99 of push, pop (one due node) and cancel.
 *	Output: one CSV row per run (see the header row).
 */

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#include <x86intrin.h>
#define TQ_X86 1
#endif

typedef struct tnode {
	int64_t       dl;     /* deadline, ns */
	uint64_t      id;     /* arm order */
	int32_t       idx;    /* heap index / bucket id / radix bucket */
	int32_t       pos;    /* qh position / wheel level*64+slot */
	struct tnode *a, *b, *c;
	int32_t       live;
	int32_t       dead;   /* lazily cancelled (qhl) */
	char          pad[16];
} tnode_t;

/* ---- node pool, rng ------------------------------------------------ */

static tnode_t  *g_pool;
static tnode_t **g_free;
static int       g_nfree;
static uint64_t  g_id;
static uint64_t  g_rng = 0x9E3779B97F4A7C15ULL;

static inline uint64_t
rnd(void)
{
	uint64_t x = g_rng;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	g_rng = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static inline int64_t
urange(int64_t lo, int64_t hi)
{
	return lo + (int64_t)(rnd() % (uint64_t)(hi - lo + 1));
}

static void
pool_init(int n)
{
	int i, j;
	tnode_t *tmp;

	g_pool = calloc((size_t)n, sizeof *g_pool);
	g_free = malloc(sizeof *g_free * (size_t)n);
	if (g_pool == NULL || g_free == NULL) {
		fprintf(stderr, "pool: out of memory\n");
		exit(1);
	}
	for (i = 0; i < n; i++)
		g_free[i] = &g_pool[i];
	for (i = n - 1; i > 0; i--) {
		j = (int)(rnd() % (uint64_t)(i + 1));
		tmp = g_free[i];
		g_free[i] = g_free[j];
		g_free[j] = tmp;
	}
	g_nfree = n;
}

static void
pool_fini(void)
{
	free(g_pool);
	free(g_free);
	g_pool = NULL;
	g_free = NULL;
}

static inline tnode_t *
nalloc(int64_t dl)
{
	tnode_t *t;

	if (g_nfree == 0) {
		fprintf(stderr, "pool exhausted\n");
		exit(1);
	}
	t = g_free[--g_nfree];
	t->dl = dl;
	t->id = g_id++;
	t->idx = -1;
	t->pos = -1;
	t->a = t->b = t->c = NULL;
	t->live = 1;
	t->dead = 0;
	return t;
}

static inline void
nfree(tnode_t *t)
{
	t->live = 0;
	g_free[g_nfree++] = t;
}

/* ---- the interface ------------------------------------------------- */

typedef struct pq {
	const char *name;
	int         exact;   /* pops in deadline order, never late */
	void     *(*create)(int cap);
	void      (*destroy)(void *s);
	void      (*push)(void *s, tnode_t *t);
	int       (*cancel)(void *s, tnode_t *t);  /* 1: caller frees t */
	tnode_t  *(*pop_due)(void *s, int64_t now);
	int64_t   (*next)(void *s);
} pq_t;

static void *
xmalloc(size_t n)
{
	void *p = malloc(n);

	if (p == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

static void *
xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (p == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

/* ---- bin: a copy of src/evt/timer.c's heap ------------------------- */

struct bin {
	tnode_t **h;
	int       n, cap;
};

static void *
bin_create(int cap)
{
	struct bin *s = calloc(1, sizeof *s);

	s->cap = cap > 8 ? cap : 8;
	s->h = xmalloc(sizeof *s->h * (size_t)s->cap);
	return s;
}

static void
bin_destroy(void *p)
{
	struct bin *s = p;

	free(s->h);
	free(s);
}

static void
bin_swap(tnode_t **a, tnode_t **b)
{
	tnode_t *tmp = *a;
	int ia = (*a)->idx, ib = (*b)->idx;

	*a = *b;
	(*a)->idx = ia;
	*b = tmp;
	(*b)->idx = ib;
}

static void
bin_up(tnode_t **h, int i)
{
	while (i > 0) {
		int parent = (i - 1) / 2;
		if (h[parent]->dl <= h[i]->dl)
			break;
		bin_swap(&h[parent], &h[i]);
		i = parent;
	}
}

static void
bin_down(tnode_t **h, int n, int i)
{
	for (;;) {
		int l = 2 * i + 1, r = 2 * i + 2, best = i;
		if (l < n && h[l]->dl < h[best]->dl)
			best = l;
		if (r < n && h[r]->dl < h[best]->dl)
			best = r;
		if (best == i)
			break;
		bin_swap(&h[i], &h[best]);
		i = best;
	}
}

static void
bin_push(void *p, tnode_t *t)
{
	struct bin *s = p;

	if (s->n >= s->cap) {
		s->cap *= 2;
		s->h = xrealloc(s->h, sizeof *s->h * (size_t)s->cap);
	}
	t->idx = s->n;
	s->h[s->n++] = t;
	bin_up(s->h, t->idx);
}

static int
bin_cancel(void *p, tnode_t *t)
{
	struct bin *s = p;
	tnode_t **h = s->h;
	int i = t->idx, last = --s->n;

	h[i]->idx = -1;
	if (i == last)
		return 1;
	h[i] = h[last];
	h[i]->idx = i;
	if (i > 0 && h[i]->dl < h[(i - 1) / 2]->dl)
		bin_up(h, i);
	else
		bin_down(h, s->n, i);
	return 1;
}

static tnode_t *
bin_pop_due(void *p, int64_t now)
{
	struct bin *s = p;
	tnode_t *top;

	if (s->n == 0 || s->h[0]->dl > now)
		return NULL;
	top = s->h[0];
	top->idx = -1;
	if (--s->n > 0) {
		s->h[0] = s->h[s->n];
		s->h[0]->idx = 0;
		bin_down(s->h, s->n, 0);
	}
	return top;
}

static int64_t
bin_next(void *p)
{
	struct bin *s = p;

	return s->n == 0 ? -1 : s->h[0]->dl;
}

/* ---- bini / d4 / d8: d-ary, {key, node} inline, hole-based sift ---- */

struct he {
	int64_t  k;
	tnode_t *t;
};

struct dh {
	struct he *h;
	int        n, cap;
};

/* d must stay a constant in each instance (else every step is an idiv). */
#define DH_INLINE static inline __attribute__((always_inline))

DH_INLINE void
dh_up(struct he *h, int i, const int d)
{
	struct he x = h[i];

	while (i > 0) {
		int p = (i - 1) / d;
		if (h[p].k <= x.k)
			break;
		h[i] = h[p];
		h[i].t->idx = i;
		i = p;
	}
	h[i] = x;
	x.t->idx = i;
}

DH_INLINE void
dh_down(struct he *h, int n, int i, const int d)
{
	struct he x = h[i];

	for (;;) {
		int f = d * i + 1, e, c, j;
		if (f >= n)
			break;
		e = f + d < n ? f + d : n;
		c = f;
		for (j = f + 1; j < e; j++)
			if (h[j].k < h[c].k)
				c = j;
		if (!(h[c].k < x.k))
			break;
		h[i] = h[c];
		h[i].t->idx = i;
		i = c;
	}
	h[i] = x;
	x.t->idx = i;
}

static void *
dh_create(int cap)
{
	struct dh *s = calloc(1, sizeof *s);

	s->cap = cap > 8 ? cap : 8;
	s->h = xmalloc(sizeof *s->h * (size_t)s->cap);
	return s;
}

static void
dh_destroy(void *p)
{
	struct dh *s = p;

	free(s->h);
	free(s);
}

DH_INLINE void
dh_push(struct dh *s, tnode_t *t, const int d)
{
	if (s->n >= s->cap) {
		s->cap *= 2;
		s->h = xrealloc(s->h, sizeof *s->h * (size_t)s->cap);
	}
	s->h[s->n].k = t->dl;
	s->h[s->n].t = t;
	dh_up(s->h, s->n++, d);
}

DH_INLINE void
dh_remove(struct dh *s, int i, const int d)
{
	struct he *h = s->h;
	int last = --s->n;

	h[i].t->idx = -1;
	if (i == last)
		return;
	h[i] = h[last];
	if (i > 0 && h[i].k < h[(i - 1) / d].k)
		dh_up(h, i, d);
	else
		dh_down(h, s->n, i, d);
}

DH_INLINE tnode_t *
dh_pop_due(struct dh *s, int64_t now, const int d)
{
	tnode_t *top;

	if (s->n == 0 || s->h[0].k > now)
		return NULL;
	top = s->h[0].t;
	dh_remove(s, 0, d);
	return top;
}

static int64_t
dh_next(void *p)
{
	struct dh *s = p;

	return s->n == 0 ? -1 : s->h[0].k;
}

#define DH_OPS(D)                                                       \
	static void d##D##_push(void *s, tnode_t *t)                    \
	{                                                               \
		dh_push(s, t, D);                                       \
	}                                                               \
	static int d##D##_cancel(void *s, tnode_t *t)                   \
	{                                                               \
		dh_remove(s, t->idx, D);                                \
		return 1;                                               \
	}                                                               \
	static tnode_t *d##D##_pop_due(void *s, int64_t now)            \
	{                                                               \
		return dh_pop_due(s, now, D);                           \
	}

DH_OPS(2)
DH_OPS(4)
DH_OPS(8)

/* ---- binh: node pointers (key through the pointer), hole-based sift --- */

DH_INLINE void
bh_up(tnode_t **h, int i)
{
	tnode_t *x = h[i];

	while (i > 0) {
		int p = (i - 1) / 2;
		if (h[p]->dl <= x->dl)
			break;
		h[i] = h[p];
		h[i]->idx = i;
		i = p;
	}
	h[i] = x;
	x->idx = i;
}

DH_INLINE void
bh_down(tnode_t **h, int n, int i)
{
	tnode_t *x = h[i];

	for (;;) {
		int l = 2 * i + 1, r = l + 1, c;
		if (l >= n)
			break;
		c = r < n && h[r]->dl < h[l]->dl ? r : l;
		if (!(h[c]->dl < x->dl))
			break;
		h[i] = h[c];
		h[i]->idx = i;
		i = c;
	}
	h[i] = x;
	x->idx = i;
}

static void
binh_push(void *p, tnode_t *t)
{
	struct bin *s = p;

	if (s->n >= s->cap) {
		s->cap *= 2;
		s->h = xrealloc(s->h, sizeof *s->h * (size_t)s->cap);
	}
	s->h[s->n] = t;
	bh_up(s->h, s->n++);
}

DH_INLINE void
binh_remove(struct bin *s, int i)
{
	tnode_t **h = s->h;
	int last = --s->n;

	h[i]->idx = -1;
	if (i == last)
		return;
	h[i] = h[last];
	if (i > 0 && h[i]->dl < h[(i - 1) / 2]->dl)
		bh_up(h, i);
	else
		bh_down(h, s->n, i);
}

static int
binh_cancel(void *p, tnode_t *t)
{
	binh_remove(p, t->idx);
	return 1;
}

static tnode_t *
binh_pop_due(void *p, int64_t now)
{
	struct bin *s = p;
	tnode_t *top;

	if (s->n == 0 || s->h[0]->dl > now)
		return NULL;
	top = s->h[0];
	binh_remove(s, 0);
	return top;
}

/* ---- bins: SoA, keys[] beside the node pointers, hole-based sift ---- */

struct bs {
	int64_t  *k;
	tnode_t **h;
	int       n, cap;
};

static void *
bs_create(int cap)
{
	struct bs *s = calloc(1, sizeof *s);

	s->cap = cap > 8 ? cap : 8;
	s->k = xmalloc(sizeof *s->k * (size_t)s->cap);
	s->h = xmalloc(sizeof *s->h * (size_t)s->cap);
	return s;
}

static void
bs_destroy(void *p)
{
	struct bs *s = p;

	free(s->k);
	free(s->h);
	free(s);
}

DH_INLINE void
bs_up(struct bs *s, int i)
{
	int64_t xk = s->k[i];
	tnode_t *x = s->h[i];

	while (i > 0) {
		int p = (i - 1) / 2;
		if (s->k[p] <= xk)
			break;
		s->k[i] = s->k[p];
		s->h[i] = s->h[p];
		s->h[i]->idx = i;
		i = p;
	}
	s->k[i] = xk;
	s->h[i] = x;
	x->idx = i;
}

DH_INLINE void
bs_down(struct bs *s, int i)
{
	int64_t xk = s->k[i];
	tnode_t *x = s->h[i];
	int n = s->n;

	for (;;) {
		int l = 2 * i + 1, r = l + 1, c;
		if (l >= n)
			break;
		c = r < n && s->k[r] < s->k[l] ? r : l;
		if (!(s->k[c] < xk))
			break;
		s->k[i] = s->k[c];
		s->h[i] = s->h[c];
		s->h[i]->idx = i;
		i = c;
	}
	s->k[i] = xk;
	s->h[i] = x;
	x->idx = i;
}

static void
bs_push(void *p, tnode_t *t)
{
	struct bs *s = p;

	if (s->n >= s->cap) {
		s->cap *= 2;
		s->k = xrealloc(s->k, sizeof *s->k * (size_t)s->cap);
		s->h = xrealloc(s->h, sizeof *s->h * (size_t)s->cap);
	}
	s->k[s->n] = t->dl;
	s->h[s->n] = t;
	bs_up(s, s->n++);
}

DH_INLINE void
bs_remove(struct bs *s, int i)
{
	int last = --s->n;

	s->h[i]->idx = -1;
	if (i == last)
		return;
	s->k[i] = s->k[last];
	s->h[i] = s->h[last];
	if (i > 0 && s->k[i] < s->k[(i - 1) / 2])
		bs_up(s, i);
	else
		bs_down(s, i);
}

static int
bs_cancel(void *p, tnode_t *t)
{
	bs_remove(p, t->idx);
	return 1;
}

static tnode_t *
bs_pop_due(void *p, int64_t now)
{
	struct bs *s = p;
	tnode_t *top;

	if (s->n == 0 || s->k[0] > now)
		return NULL;
	top = s->h[0];
	bs_remove(s, 0);
	return top;
}

static int64_t
bs_next(void *p)
{
	struct bs *s = p;

	return s->n == 0 ? -1 : s->k[0];
}

/* ---- pair: pairing heap (a = child, b = next sibling, c = prev) ---- */

struct ph {
	tnode_t *root;
};

static inline int
ph_less(const tnode_t *x, const tnode_t *y)
{
	return x->dl < y->dl || (x->dl == y->dl && x->id < y->id);
}

/* Meld two roots (b, c NULL). */
static inline tnode_t *
ph_meld(tnode_t *x, tnode_t *y)
{
	tnode_t *tmp;

	if (x == NULL)
		return y;
	if (y == NULL)
		return x;
	if (ph_less(y, x)) {
		tmp = x;
		x = y;
		y = tmp;
	}
	y->c = x;
	y->b = x->a;
	if (x->a != NULL)
		x->a->c = y;
	x->a = y;
	return x;
}

static tnode_t *
ph_two_pass(tnode_t *f)
{
	tnode_t *list = NULL, *x, *y, *nx, *r;

	while (f != NULL) {
		x = f;
		y = x->b;
		nx = y != NULL ? y->b : NULL;
		x->b = x->c = NULL;
		if (y != NULL) {
			y->b = y->c = NULL;
			x = ph_meld(x, y);
		}
		x->b = list;
		list = x;
		f = nx;
	}
	if (list == NULL)
		return NULL;
	r = list;
	list = list->b;
	r->b = NULL;
	while (list != NULL) {
		nx = list->b;
		list->b = NULL;
		r = ph_meld(r, list);
		list = nx;
	}
	r->c = NULL;
	return r;
}

static void *
ph_create(int cap)
{
	(void)cap;
	return calloc(1, sizeof(struct ph));
}

static void
ph_destroy(void *p)
{
	free(p);
}

static void
ph_push(void *p, tnode_t *t)
{
	struct ph *s = p;

	t->a = t->b = t->c = NULL;
	s->root = ph_meld(s->root, t);
}

static int
ph_cancel(void *p, tnode_t *t)
{
	struct ph *s = p;
	tnode_t *sub;

	if (t == s->root) {
		s->root = ph_two_pass(t->a);
		return 1;
	}
	if (t->c->a == t)
		t->c->a = t->b;
	else
		t->c->b = t->b;
	if (t->b != NULL)
		t->b->c = t->c;
	sub = ph_two_pass(t->a);
	s->root = ph_meld(s->root, sub);
	return 1;
}

static tnode_t *
ph_pop_due(void *p, int64_t now)
{
	struct ph *s = p;
	tnode_t *r = s->root;

	if (r == NULL || r->dl > now)
		return NULL;
	s->root = ph_two_pass(r->a);
	return r;
}

static int64_t
ph_next(void *p)
{
	struct ph *s = p;

	return s->root == NULL ? -1 : s->root->dl;
}

/* ---- intrusive doubly-linked bucket lists (a = next, c = prev) ----- */

static inline void
dl_ins(tnode_t **head, tnode_t *t)
{
	t->c = NULL;
	t->a = *head;
	if (*head != NULL)
		(*head)->c = t;
	*head = t;
}

static inline void
dl_del(tnode_t **head, tnode_t *t)
{
	if (t->c != NULL)
		t->c->a = t->a;
	else
		*head = t->a;
	if (t->a != NULL)
		t->a->c = t->c;
}

/* ---- radix: radix heap on ns keys ---------------------------------- */

/*
 * Bucket 0 holds keys <= last, bucket i >= 1 keys whose highest bit
 * differing from last is i-1.  `last` only advances to a key <= now, and
 * a timer's deadline is >= the now it was armed at, so pushes stay >=
 * last (a key below it is due anyway and goes to bucket 0).
 */
struct rx {
	tnode_t *bk[65];
	uint64_t mask;
	int64_t  last;
	int64_t  cmin;     /* min key in buckets >= 1, when cmin_ok */
	int      cmin_ok;
};

static inline int
rx_bucket(int64_t last, int64_t k)
{
	return k <= last ? 0 :
	    64 - __builtin_clzll((uint64_t)(k ^ last));
}

static inline void
rx_ins(struct rx *s, tnode_t *t)
{
	int b = rx_bucket(s->last, t->dl);

	t->idx = b;
	dl_ins(&s->bk[b], t);
	s->mask |= 1ULL << b;
}

static void *
rx_create(int cap)
{
	(void)cap;
	return calloc(1, sizeof(struct rx));
}

static void
rx_destroy(void *p)
{
	free(p);
}

static void
rx_push(void *p, tnode_t *t)
{
	struct rx *s = p;

	rx_ins(s, t);
	if (t->idx != 0 && s->cmin_ok && t->dl < s->cmin)
		s->cmin = t->dl;
}

static int
rx_cancel(void *p, tnode_t *t)
{
	struct rx *s = p;

	dl_del(&s->bk[t->idx], t);
	if (s->bk[t->idx] == NULL)
		s->mask &= ~(1ULL << t->idx);
	if (t->idx != 0 && s->cmin_ok && t->dl == s->cmin)
		s->cmin_ok = 0;
	return 1;
}

/* The minimum key over buckets >= 1 (cached): scan the lowest of them. */
static int64_t
rx_min(struct rx *s)
{
	tnode_t *t;
	int b;

	if (!s->cmin_ok) {
		b = __builtin_ctzll(s->mask & ~1ULL);
		s->cmin = INT64_MAX;
		for (t = s->bk[b]; t != NULL; t = t->a)
			if (t->dl < s->cmin)
				s->cmin = t->dl;
		s->cmin_ok = 1;
	}
	return s->cmin;
}

static tnode_t *
rx_pop_due(void *p, int64_t now)
{
	struct rx *s = p;
	tnode_t *t, *l, *nx;
	int b;

	if (s->mask == 0)
		return NULL;
	if ((s->mask & 1) == 0) {
		if (rx_min(s) > now)
			return NULL;
		b = __builtin_ctzll(s->mask);
		s->last = s->cmin;
		l = s->bk[b];
		s->bk[b] = NULL;
		s->mask &= ~(1ULL << b);
		for (; l != NULL; l = nx) {
			nx = l->a;
			rx_ins(s, l);
		}
		s->cmin_ok = 0;
	}
	t = s->bk[0];
	if (t->dl > now)
		return NULL;
	dl_del(&s->bk[0], t);
	if (s->bk[0] == NULL)
		s->mask &= ~1ULL;
	return t;
}

static int64_t
rx_next(void *p)
{
	struct rx *s = p;

	if (s->mask == 0)
		return -1;
	if (s->mask & 1)
		return s->last;
	return rx_min(s);
}

/* ---- wheel: hierarchical timing wheel ------------------------------ */

#define WH_SHIFT 10   /* 1.024 us ticks */
#define WH_LV    9    /* 9 x 6 bits covers every tick of a 63-bit ns */

struct wh {
	tnode_t *sl[WH_LV][64];
	uint64_t bm[WH_LV];
	int64_t  cur;      /* every timer's tick is >= cur */
};

static inline void
wh_ins(struct wh *s, tnode_t *t)
{
	int64_t tk = t->dl >> WH_SHIFT;
	uint64_t x;
	int lv, sl;

	if (tk < s->cur)
		tk = s->cur;
	x = (uint64_t)(tk ^ s->cur);
	lv = x != 0 ? (63 - __builtin_clzll(x)) / 6 : 0;
	sl = (int)((tk >> (6 * lv)) & 63);
	t->pos = lv * 64 + sl;
	dl_ins(&s->sl[lv][sl], t);
	s->bm[lv] |= 1ULL << sl;
}

static void *
wh_create(int cap)
{
	(void)cap;
	return calloc(1, sizeof(struct wh));
}

static void
wh_destroy(void *p)
{
	free(p);
}

static void
wh_push(void *p, tnode_t *t)
{
	wh_ins(p, t);
}

static int
wh_cancel(void *p, tnode_t *t)
{
	struct wh *s = p;
	int lv = t->pos >> 6, sl = t->pos & 63;

	dl_del(&s->sl[lv][sl], t);
	if (s->sl[lv][sl] == NULL)
		s->bm[lv] &= ~(1ULL << sl);
	return 1;
}

/* A timer is due once its tick has passed: never early, <= 1 tick late. */
static tnode_t *
wh_pop_due(void *p, int64_t now)
{
	struct wh *s = p;
	int64_t nt = now >> WH_SHIFT, tk, start;
	tnode_t *t, *l, *nx;
	int lv, sl;

	for (;;) {
		if (s->bm[0] != 0) {
			sl = __builtin_ctzll(s->bm[0]);
			tk = (s->cur & ~(int64_t)63) | sl;
			if (tk >= nt)
				return NULL;
			s->cur = tk;
			t = s->sl[0][sl];
			dl_del(&s->sl[0][sl], t);
			if (s->sl[0][sl] == NULL)
				s->bm[0] &= ~(1ULL << sl);
			return t;
		}
		for (lv = 1; lv < WH_LV && s->bm[lv] == 0; lv++)
			;
		if (lv == WH_LV)
			return NULL;
		sl = __builtin_ctzll(s->bm[lv]);
		start = (s->cur & ~(((int64_t)1 << (6 * (lv + 1))) - 1)) |
		    ((int64_t)sl << (6 * lv));
		if (start >= nt)
			return NULL;
		/* Cascade the slot one level down (at least). */
		s->cur = start;
		l = s->sl[lv][sl];
		s->sl[lv][sl] = NULL;
		s->bm[lv] &= ~(1ULL << sl);
		for (; l != NULL; l = nx) {
			nx = l->a;
			wh_ins(s, l);
		}
	}
}

/* A lower bound for an upper level (the slot start), exact at level 0. */
static int64_t
wh_next(void *p)
{
	struct wh *s = p;
	int64_t start;
	int lv, sl;

	if (s->bm[0] != 0)
		return (((s->cur & ~(int64_t)63) |
		    __builtin_ctzll(s->bm[0])) + 1) << WH_SHIFT;
	for (lv = 1; lv < WH_LV && s->bm[lv] == 0; lv++)
		;
	if (lv == WH_LV)
		return -1;
	sl = __builtin_ctzll(s->bm[lv]);
	start = (s->cur & ~(((int64_t)1 << (6 * (lv + 1))) - 1)) |
	    ((int64_t)sl << (6 * lv));
	return (start + 1) << WH_SHIFT;
}

/* ---- qh / qhl / qhls: QuickHeap, SimdQuickHeap layout -------------- */

#define QH_SMALL 16   /* stop partitioning below this (the paper's 16) */

struct qbk {
	int64_t  *k;
	tnode_t **t;
	int       n, cap;
};

/*
 * ord[0] is the top bucket (largest keys), ord[nb-1] the bottom.
 * piv[i] separates ord[i] (keys >= piv[i]) from ord[i+1] (keys <=
 * piv[i]); pivots are non-increasing.  Buckets have stable ids (the
 * pool index) so a node's handle survives bucket removal.
 */
struct qh {
	struct qbk *pool;
	int         npool, cappool;
	int        *freeid;
	int         nfree;
	int        *ord;
	int64_t    *piv;
	int         nb, capb;
	int         lazy, simd;
	int         nlive, ndead;
	uint64_t    rng;
};

static int g_have_avx512;

static inline uint64_t
qh_rnd(struct qh *s)
{
	uint64_t x = s->rng;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	s->rng = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static void
qb_reserve(struct qbk *b, int need)
{
	if (need <= b->cap)
		return;
	while (b->cap < need)
		b->cap = b->cap == 0 ? 32 : b->cap * 2;
	/* +8: a full-vector store may run past the count. */
	b->k = xrealloc(b->k, sizeof *b->k * (size_t)(b->cap + 8));
	b->t = xrealloc(b->t, sizeof *b->t * (size_t)(b->cap + 8));
}

static int
qh_newbucket(struct qh *s)
{
	int id;

	if (s->nfree > 0)
		return s->freeid[--s->nfree];
	if (s->npool == s->cappool) {
		s->cappool = s->cappool == 0 ? 64 : s->cappool * 2;
		s->pool = xrealloc(s->pool, sizeof *s->pool *
		    (size_t)s->cappool);
		s->freeid = xrealloc(s->freeid, sizeof *s->freeid *
		    (size_t)s->cappool);
	}
	id = s->npool++;
	memset(&s->pool[id], 0, sizeof s->pool[id]);
	return id;
}

static void *
qh_create_common(int lazy, int simd)
{
	struct qh *s = calloc(1, sizeof *s);

	s->lazy = lazy;
	s->simd = simd && g_have_avx512;
	s->rng = 0xD1B54A32D192ED03ULL;
	s->capb = 64;
	s->ord = xmalloc(sizeof *s->ord * (size_t)s->capb);
	s->piv = xmalloc(sizeof *s->piv * (size_t)s->capb);
	s->ord[0] = qh_newbucket(s);
	s->nb = 1;
	return s;
}

static void *
qh_create(int cap)
{
	(void)cap;
	return qh_create_common(0, 0);
}

static void *
qhl_create(int cap)
{
	(void)cap;
	return qh_create_common(1, 1);
}

static void *
qhls_create(int cap)
{
	(void)cap;
	return qh_create_common(1, 0);
}

static void
qh_destroy(void *p)
{
	struct qh *s = p;
	int i;

	for (i = 0; i < s->npool; i++) {
		free(s->pool[i].k);
		free(s->pool[i].t);
	}
	free(s->pool);
	free(s->freeid);
	free(s->ord);
	free(s->piv);
	free(s);
}

static inline void
qb_append(struct qh *s, int id, int64_t k, tnode_t *t)
{
	struct qbk *b = &s->pool[id];

	qb_reserve(b, b->n + 1);
	b->k[b->n] = k;
	b->t[b->n] = t;
	if (!s->lazy) {
		t->idx = id;
		t->pos = b->n;
	}
	b->n++;
}

static void
qh_push(void *p, tnode_t *t)
{
	struct qh *s = p;
	int i;

	/* New deadlines are mostly the largest: scan from the top. */
	for (i = 0; i < s->nb - 1; i++)
		if (t->dl >= s->piv[i])
			break;
	qb_append(s, s->ord[i], t->dl, t);
	s->nlive++;
}

/* Bucket at order position oi is empty: drop it and a pivot. */
static void
qh_drop(struct qh *s, int oi)
{
	int pi = oi == s->nb - 1 ? oi - 1 : oi;

	s->freeid[s->nfree++] = s->ord[oi];
	memmove(&s->ord[oi], &s->ord[oi + 1], sizeof *s->ord *
	    (size_t)(s->nb - 1 - oi));
	memmove(&s->piv[pi], &s->piv[pi + 1], sizeof *s->piv *
	    (size_t)(s->nb - 2 - pi));
	s->nb--;
}

static void
qh_remove_at(struct qh *s, int id, int p)
{
	struct qbk *b = &s->pool[id];
	int last = --b->n, oi;

	if (p != last) {
		b->k[p] = b->k[last];
		b->t[p] = b->t[last];
		if (!s->lazy)
			b->t[p]->pos = p;
	}
	if (b->n == 0 && s->nb > 1) {
		for (oi = 0; s->ord[oi] != id; oi++)
			;
		qh_drop(s, oi);
	}
}

#ifdef TQ_X86
/*
 * Partition b's keys + node pointers around pivot p at index pi with
 * AVX-512 compress: left of pi go down if < p, right of it if <= p.
 * Stays are compacted in place, gos appended to nk/nt.  Returns the
 * number of stays.
 */
__attribute__((target("avx512f"))) static int
qh_part_avx512(int64_t *k, tnode_t **t, int m, int pi, int64_t p,
    int64_t *nk, tnode_t **nt, int *ng)
{
	__m512i vp = _mm512_set1_epi64(p);
	int j = 0, w = 0, g = *ng;

	for (; j + 8 <= m; j += 8) {
		__m512i v = _mm512_loadu_si512((const void *)(k + j));
		__m512i vt = _mm512_loadu_si512((const void *)(t + j));
		__mmask8 lt = _mm512_cmplt_epi64_mask(v, vp);
		__mmask8 le = _mm512_cmple_epi64_mask(v, vp);
		unsigned lm, rm, go;
		if (pi >= j + 8) {
			lm = 0xFF;
			rm = 0;
		} else if (pi < j) {
			lm = 0;
			rm = 0xFF;
		} else {
			lm = (1u << (pi - j)) - 1;
			rm = (0xFFu << (pi - j + 1)) & 0xFF;
		}
		go = ((unsigned)lt & lm) | ((unsigned)le & rm);
		_mm512_storeu_si512((void *)(nk + g),
		    _mm512_maskz_compress_epi64((__mmask8)go, v));
		_mm512_storeu_si512((void *)(nt + g),
		    _mm512_maskz_compress_epi64((__mmask8)go, vt));
		_mm512_mask_compressstoreu_epi64(k + w,
		    (__mmask8)(~go & 0xFF), v);
		_mm512_mask_compressstoreu_epi64(t + w,
		    (__mmask8)(~go & 0xFF), vt);
		g += __builtin_popcount(go);
		w += 8 - __builtin_popcount(go);
	}
	for (; j < m; j++) {
		int64_t kj = k[j];
		tnode_t *tj = t[j];
		if (j < pi ? kj < p : (j > pi && kj <= p)) {
			nk[g] = kj;
			nt[g++] = tj;
		} else {
			k[w] = kj;
			t[w++] = tj;
		}
	}
	*ng = g;
	return w;
}
#endif

/* Split the bottom bucket once.  0 if no pivot split it. */
static int
qh_partition(struct qh *s)
{
	int bid = s->ord[s->nb - 1], nid, tries, m, i0, i1, i2, pi, w, g, j;
	struct qbk *b, *nb;
	int64_t p, k0, k1, k2;

	for (tries = 0; tries < 8; tries++) {
		b = &s->pool[bid];
		m = b->n;
		i0 = (int)(qh_rnd(s) % (uint64_t)m);
		i1 = (int)(qh_rnd(s) % (uint64_t)m);
		i2 = (int)(qh_rnd(s) % (uint64_t)m);
		k0 = b->k[i0];
		k1 = b->k[i1];
		k2 = b->k[i2];
		if ((k0 <= k1) == (k1 <= k2))
			pi = i1;
		else if ((k1 <= k0) == (k0 <= k2))
			pi = i0;
		else
			pi = i2;
		p = b->k[pi];
		nid = qh_newbucket(s);   /* may move pool */
		b = &s->pool[bid];
		nb = &s->pool[nid];
		qb_reserve(nb, m);
		g = 0;
#ifdef TQ_X86
		if (s->simd) {
			w = qh_part_avx512(b->k, b->t, m, pi, p, nb->k, nb->t,
			    &g);
		} else
#endif
		{
			w = 0;
			for (j = 0; j < m; j++) {
				int64_t kj = b->k[j];
				tnode_t *tj = b->t[j];
				if (j < pi ? kj < p : (j > pi && kj <= p)) {
					nb->k[g] = kj;
					nb->t[g] = tj;
					if (!s->lazy) {
						tj->idx = nid;
						tj->pos = g;
					}
					g++;
				} else {
					b->k[w] = kj;
					b->t[w] = tj;
					if (!s->lazy)
						tj->pos = w;
					w++;
				}
			}
		}
		b->n = w;
		nb->n = g;
		if (g == 0) {
			s->freeid[s->nfree++] = nid;
			continue;
		}
		if (s->nb == s->capb) {
			s->capb *= 2;
			s->ord = xrealloc(s->ord, sizeof *s->ord *
			    (size_t)s->capb);
			s->piv = xrealloc(s->piv, sizeof *s->piv *
			    (size_t)s->capb);
		}
		s->piv[s->nb - 1] = p;
		s->ord[s->nb++] = nid;
		return 1;
	}
	return 0;
}

/* Index of the minimum of the (small) bottom bucket, -1 if empty. */
static int
qh_bottom_min(struct qh *s)
{
	struct qbk *b;
	int j, mi;

	for (;;) {
		b = &s->pool[s->ord[s->nb - 1]];
		if (b->n <= QH_SMALL || !qh_partition(s))
			break;
	}
	b = &s->pool[s->ord[s->nb - 1]];
	if (b->n == 0)
		return -1;
	for (mi = 0, j = 1; j < b->n; j++)
		if (b->k[j] < b->k[mi])
			mi = j;
	return mi;
}

/* Drop every lazily cancelled entry (qhl). */
static void
qh_sweep(struct qh *s)
{
	int oi, j, w, id;
	struct qbk *b;

	for (oi = 0; oi < s->nb; oi++) {
		id = s->ord[oi];
		b = &s->pool[id];
		for (w = 0, j = 0; j < b->n; j++) {
			if (b->t[j]->dead) {
				nfree(b->t[j]);
				continue;
			}
			b->k[w] = b->k[j];
			b->t[w++] = b->t[j];
		}
		b->n = w;
	}
	for (oi = s->nb - 1; oi >= 0 && s->nb > 1; oi--)
		if (s->pool[s->ord[oi]].n == 0)
			qh_drop(s, oi);
	s->ndead = 0;
}

static int
qh_cancel(void *p, tnode_t *t)
{
	struct qh *s = p;

	s->nlive--;
	if (!s->lazy) {
		qh_remove_at(s, t->idx, t->pos);
		return 1;
	}
	t->dead = 1;
	s->ndead++;
	if (s->ndead > s->nlive && s->ndead > 32)
		qh_sweep(s);
	return 0;
}

static tnode_t *
qh_pop_due(void *p, int64_t now)
{
	struct qh *s = p;
	struct qbk *b;
	tnode_t *t;
	int mi, bid;

	for (;;) {
		if ((mi = qh_bottom_min(s)) < 0)
			return NULL;
		bid = s->ord[s->nb - 1];
		b = &s->pool[bid];
		t = b->t[mi];
		if (!t->dead && b->k[mi] > now)
			return NULL;
		qh_remove_at(s, bid, mi);
		if (!t->dead) {
			s->nlive--;
			return t;
		}
		s->ndead--;
		nfree(t);
	}
}

static int64_t
qh_next(void *p)
{
	struct qh *s = p;
	struct qbk *b;
	tnode_t *t;
	int mi, bid;

	for (;;) {
		if ((mi = qh_bottom_min(s)) < 0)
			return -1;
		bid = s->ord[s->nb - 1];
		b = &s->pool[bid];
		t = b->t[mi];
		if (!t->dead)
			return b->k[mi];
		qh_remove_at(s, bid, mi);
		s->ndead--;
		nfree(t);
	}
}

/* ---- registry ------------------------------------------------------ */

static const pq_t g_pqs[] = {
	{ "bin", 1, bin_create, bin_destroy, bin_push, bin_cancel,
	    bin_pop_due, bin_next },
	{ "binh", 1, bin_create, bin_destroy, binh_push, binh_cancel,
	    binh_pop_due, bin_next },
	{ "bins", 1, bs_create, bs_destroy, bs_push, bs_cancel, bs_pop_due,
	    bs_next },
	{ "bini", 1, dh_create, dh_destroy, d2_push, d2_cancel, d2_pop_due,
	    dh_next },
	{ "d4", 1, dh_create, dh_destroy, d4_push, d4_cancel, d4_pop_due,
	    dh_next },
	{ "d8", 1, dh_create, dh_destroy, d8_push, d8_cancel, d8_pop_due,
	    dh_next },
	{ "pair", 1, ph_create, ph_destroy, ph_push, ph_cancel, ph_pop_due,
	    ph_next },
	{ "radix", 1, rx_create, rx_destroy, rx_push, rx_cancel, rx_pop_due,
	    rx_next },
	{ "wheel", 0, wh_create, wh_destroy, wh_push, wh_cancel, wh_pop_due,
	    wh_next },
	{ "qh", 1, qh_create, qh_destroy, qh_push, qh_cancel, qh_pop_due,
	    qh_next },
	{ "qhl", 1, qhl_create, qh_destroy, qh_push, qh_cancel, qh_pop_due,
	    qh_next },
	{ "qhls", 1, qhls_create, qh_destroy, qh_push, qh_cancel,
	    qh_pop_due, qh_next },
};
#define N_PQS ((int)(sizeof g_pqs / sizeof g_pqs[0]))

/* ---- measurement --------------------------------------------------- */

static double
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

#ifdef TQ_X86
static inline uint64_t
cyc(void)
{
	return __rdtsc();
}
#else
static inline uint64_t
cyc(void)
{
	return (uint64_t)now_ns();
}
#endif

#define LAT_CAP (1 << 21)
enum { L_PUSH, L_POP, L_CANCEL, L_N };
static const char *g_lname[L_N] = { "push", "pop", "cancel" };
static uint32_t *g_lat[L_N];
static int       g_nlat[L_N];
static int       g_latency;
static double    g_ns_per_cyc = 1.0;

static inline void
lat_rec(int kind, uint64_t c0, uint64_t c1)
{
	if (g_nlat[kind] < LAT_CAP)
		g_lat[kind][g_nlat[kind]++] = (uint32_t)(c1 - c0);
}

/* Timed wrappers: the timing is compiled in but only taken with -l. */
static inline void
op_push(const pq_t *q, void *s, tnode_t *t)
{
	uint64_t c0;

	if (!g_latency) {
		q->push(s, t);
		return;
	}
	c0 = cyc();
	q->push(s, t);
	lat_rec(L_PUSH, c0, cyc());
}

static inline void
op_cancel(const pq_t *q, void *s, tnode_t *t)
{
	uint64_t c0 = 0;
	int freed;

	if (g_latency)
		c0 = cyc();
	freed = q->cancel(s, t);
	if (g_latency)
		lat_rec(L_CANCEL, c0, cyc());
	if (freed)
		nfree(t);
}

/* The per-run checks: never early, order (exact impls), hashes. */
struct run {
	const pq_t *q;
	uint64_t    units;
	uint64_t    fires;
	uint64_t    dlhash, idhash;
	int64_t     lastkey;
	int         bad;
};

static inline tnode_t *
op_pop(struct run *r, void *s, int64_t now)
{
	uint64_t c0 = 0;
	tnode_t *t;
	int64_t key;

	if (g_latency)
		c0 = cyc();
	t = r->q->pop_due(s, now);
	if (t == NULL)
		return NULL;
	if (g_latency)
		lat_rec(L_POP, c0, cyc());
	key = r->q->exact ? t->dl : t->dl >> WH_SHIFT;
	if (!t->live || t->dead || t->dl > now || key < r->lastkey)
		r->bad++;
	r->lastkey = key;
	r->fires++;
	r->dlhash = (r->dlhash ^ (uint64_t)t->dl) * 0x100000001B3ULL;
	r->idhash = (r->idhash ^ t->id) * 0x100000001B3ULL;
	return t;
}

/* ---- traces -------------------------------------------------------- */

#define P_NS 1000000000LL
#define FIRE_CAP 65536

static tnode_t *g_fired[FIRE_CAP];

/*
 * The loop's shape: __xtc_drain_due_timers pops every due timer, THEN the
 * woken fibers run and re-arm.  Returns the number popped into g_fired
 * (a due node past FIRE_CAP is re-armed by the caller at once).
 */
static int
drain_due(struct run *r, void *s, int64_t now)
{
	tnode_t *t;
	int nf = 0;

	(void)r->q->next(s);
	while (nf < FIRE_CAP && (t = op_pop(r, s, now)) != NULL)
		g_fired[nf++] = t;
	return nf;
}

static void
tr_idle(struct run *r, void *s, int n, int batch, uint64_t units)
{
	int64_t now = 0, d = (int64_t)batch * P_NS / n;
	int i, nf;

	if (d < 1)
		d = 1;
	for (i = 0; i < n; i++)
		r->q->push(s, nalloc(urange(1, P_NS)));
	while (r->fires < units) {
		now += d;
		nf = drain_due(r, s, now);
		for (i = 0; i < nf; i++) {
			nfree(g_fired[i]);
			op_push(r->q, s, nalloc(now + P_NS +
			    urange(-P_NS / 100, P_NS / 100)));
		}
		(void)r->q->next(s);
	}
	r->units = r->fires;
}

#define LOAD_C 50

static void
tr_load(struct run *r, void *s, int n, uint64_t units)
{
	tnode_t *cc[LOAD_C];
	int64_t now = 0;
	uint64_t req = 0;
	int i, j, nf;

	for (i = 0; i < n; i++)
		r->q->push(s, nalloc(urange(1, P_NS)));
	for (i = 0; i < LOAD_C; i++) {
		cc[i] = nalloc(P_NS);
		r->q->push(s, cc[i]);
	}
	while (req + r->fires < units) {
		now += 8000;
		j = (int)(rnd() % LOAD_C);
		op_cancel(r->q, s, cc[j]);
		cc[j] = nalloc(now + P_NS);
		op_push(r->q, s, cc[j]);
		req++;
		nf = drain_due(r, s, now);
		for (i = 0; i < nf; i++) {
			nfree(g_fired[i]);
			op_push(r->q, s, nalloc(now + P_NS));
		}
		(void)r->q->next(s);
	}
	r->units = req + r->fires;
}

static void
tr_sleep(struct run *r, void *s, int n, uint64_t units)
{
	int64_t now = 0, d = 5000000LL / n;
	int i, nf;

	if (d < 1000)
		d = 1000;
	for (i = 0; i < n; i++)
		r->q->push(s, nalloc(urange(10000, 10000000)));
	while (r->fires < units) {
		now += d;
		nf = drain_due(r, s, now);
		for (i = 0; i < nf; i++) {
			nfree(g_fired[i]);
			op_push(r->q, s, nalloc(now + urange(10000, 10000000)));
		}
		(void)r->q->next(s);
	}
	r->units = r->fires;
}

#define CANCEL_K 4

static void
tr_cancel(struct run *r, void *s, int n, uint64_t units)
{
	tnode_t *ring[CANCEL_K] = { NULL };
	int64_t now = 0;
	uint64_t u;
	int i;

	for (i = 0; i < n; i++)
		r->q->push(s, nalloc(60 * P_NS + urange(0, P_NS)));
	for (u = 0; u < units; u++) {
		now += 1000;
		i = (int)(u % CANCEL_K);
		if (ring[i] != NULL)
			op_cancel(r->q, s, ring[i]);
		ring[i] = nalloc(now + P_NS);
		op_push(r->q, s, ring[i]);
		if (op_pop(r, s, now) != NULL)
			r->bad++;   /* nothing is due in this trace */
	}
	for (i = 0; i < CANCEL_K; i++)
		if (ring[i] != NULL)
			op_cancel(r->q, s, ring[i]);
	r->units = units;
}

static void
tr_mcs(struct run *r, void *s, int n, uint64_t units)
{
	tnode_t *t;
	int64_t last;
	int i;

	for (i = 0; i < n; i++)
		r->q->push(s, nalloc(urange(0, n)));
	while (r->fires < units) {
		if ((t = op_pop(r, s, INT64_MAX)) == NULL) {
			r->bad++;
			break;
		}
		last = t->dl;
		nfree(t);
		op_push(r->q, s, nalloc(last + urange(0, n)));
	}
	r->units = r->fires;
}

static void
tr_same(struct run *r, void *s, int n, uint64_t units)
{
	int64_t now = P_NS;
	int i, nf;

	for (i = 0; i < n; i++)
		r->q->push(s, nalloc(P_NS));
	while (r->fires < units) {
		do {
			nf = drain_due(r, s, now + 2048);
			for (i = 0; i < nf; i++) {
				nfree(g_fired[i]);
				op_push(r->q, s, nalloc(now + P_NS));
			}
		} while (nf == FIRE_CAP);
		now += P_NS;
		r->lastkey = 0;   /* a new round */
	}
	r->units = r->fires;
}

/* Empty the structure (live timers left at the end of a trace). */
static void
drain_all(struct run *r, void *s)
{
	tnode_t *t;

	while ((t = r->q->pop_due(s, INT64_MAX)) != NULL)
		nfree(t);
}

static int
cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

static void
calibrate(void)
{
#ifdef TQ_X86
	double t0 = now_ns(), t1;
	uint64_t c0 = cyc(), c1;

	do {
		t1 = now_ns();
	} while (t1 - t0 < 50e6);
	c1 = cyc();
	g_ns_per_cyc = (t1 - t0) / (double)(c1 - c0);
#endif
}

static int
run_one(const char *trace, int n, int batch, const pq_t *q, int rep,
    uint64_t units)
{
	struct run r;
	void *s;
	double t0, t1;
	int k;

	memset(&r, 0, sizeof r);
	r.q = q;
	r.dlhash = r.idhash = 0xCBF29CE484222325ULL;
	r.lastkey = INT64_MIN;
	g_rng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)n;
	g_id = 0;
	pool_init(2 * n + 4 * LOAD_C + 1024);
	for (k = 0; k < L_N; k++)
		g_nlat[k] = 0;
	s = q->create(n + LOAD_C + 16);
	t0 = now_ns();
	if (strcmp(trace, "idle") == 0)
		tr_idle(&r, s, n, batch, units);
	else if (strcmp(trace, "load") == 0)
		tr_load(&r, s, n, units);
	else if (strcmp(trace, "sleep") == 0)
		tr_sleep(&r, s, n, units);
	else if (strcmp(trace, "cancel") == 0)
		tr_cancel(&r, s, n, units);
	else if (strcmp(trace, "mcs") == 0)
		tr_mcs(&r, s, n, units);
	else if (strcmp(trace, "same") == 0)
		tr_same(&r, s, n, units);
	else {
		fprintf(stderr, "unknown trace %s\n", trace);
		exit(2);
	}
	t1 = now_ns();
	drain_all(&r, s);
	q->destroy(s);
	pool_fini();
	printf("%s,%d,%d,%s,%d,%.2f,%" PRIu64 ",%" PRIu64 ",%016" PRIx64
	    ",%016" PRIx64 ",%d", trace, n, batch, q->name, rep,
	    (t1 - t0) / (double)r.units, r.units, r.fires, r.dlhash,
	    r.idhash, r.bad);
	if (g_latency) {
		for (k = 0; k < L_N; k++) {
			double p50 = 0, p99 = 0;
			if (g_nlat[k] > 0) {
				qsort(g_lat[k], (size_t)g_nlat[k],
				    sizeof g_lat[k][0], cmp_u32);
				p50 = g_lat[k][g_nlat[k] / 2] * g_ns_per_cyc;
				p99 = g_lat[k][(int)((double)g_nlat[k] * 0.99)] *
				    g_ns_per_cyc;
			}
			printf(",%s,%.1f,%.1f", g_lname[k], p50, p99);
		}
	}
	printf("\n");
	fflush(stdout);
	return r.bad != 0;
}

int
main(int argc, char **argv)
{
	const char *trace = "idle", *impls = NULL;
	int n = 1000, batch = 1, reps = 1, rep, i, k, bad = 0, ai;
	uint64_t units = 0;
	char buf[256], *tok, *save;

	for (ai = 1; ai < argc; ai++) {
		if (strcmp(argv[ai], "-l") == 0) {
			g_latency = 1;
			continue;
		}
		if (ai + 1 >= argc)
			break;
		if (strcmp(argv[ai], "-t") == 0)
			trace = argv[++ai];
		else if (strcmp(argv[ai], "-n") == 0)
			n = atoi(argv[++ai]);
		else if (strcmp(argv[ai], "-b") == 0)
			batch = atoi(argv[++ai]);
		else if (strcmp(argv[ai], "-i") == 0)
			impls = argv[++ai];
		else if (strcmp(argv[ai], "-r") == 0)
			reps = atoi(argv[++ai]);
		else if (strcmp(argv[ai], "-u") == 0)
			units = strtoull(argv[++ai], NULL, 10);
	}
	if (n < 1)
		n = 1;
	if (units == 0)
		units = (uint64_t)n * 4 > 2000000 ? (uint64_t)n * 4 : 2000000;
#ifdef TQ_X86
	__builtin_cpu_init();
	g_have_avx512 = __builtin_cpu_supports("avx512f");
#endif
	calibrate();
	if (g_latency)
		for (k = 0; k < L_N; k++)
			g_lat[k] = xmalloc(sizeof(uint32_t) * LAT_CAP);
	printf("# avx512=%d ns_per_cyc=%.4f node=%zu\n", g_have_avx512,
	    g_ns_per_cyc, sizeof(tnode_t));
	printf("trace,n,batch,impl,rep,ns_per_unit,units,fires,dlhash,idhash,"
	    "bad%s\n", g_latency ? ",op,p50,p99,op,p50,p99,op,p50,p99" : "");
	/* Interleave: every impl once per rep, in rotating order. */
	for (rep = 0; rep < reps; rep++) {
		const pq_t *sel[N_PQS];
		int ns = 0;
		if (impls == NULL) {
			for (i = 0; i < N_PQS; i++)
				sel[ns++] = &g_pqs[i];
		} else {
			snprintf(buf, sizeof buf, "%s", impls);
			for (tok = strtok_r(buf, ",", &save); tok != NULL;
			    tok = strtok_r(NULL, ",", &save))
				for (i = 0; i < N_PQS; i++)
					if (strcmp(tok, g_pqs[i].name) == 0)
						sel[ns++] = &g_pqs[i];
		}
		for (i = 0; i < ns; i++)
			bad |= run_one(trace, n, batch,
			    sel[(i + rep) % ns], rep, units);
	}
	return bad;
}

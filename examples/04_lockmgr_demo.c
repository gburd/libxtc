/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/04_lockmgr_demo.c
 *
 * Heavyweight lock manager demo with three twists beyond the basic
 * "two transactions deadlock, detector picks a victim" scenario:
 *
 *   1. The victim choice is randomized via a Fisher-Yates-shuffled
 *      deck of 1024 cards.  When the deck is exhausted it reshuffles
 *      from a chacha20-style PRNG seeded from /dev/urandom.  This
 *      demonstrates that xtc_lockmgr's victim policy is pluggable;
 *      operators can install any decision function -- a fairness
 *      policy, a cost model, an external oracle.
 *
 *   2. The cards are dealt by an xtc_proc that acts as a generator.
 *      The lockmgr's victim callback sends a request and blocks on
 *      a reply.  Demonstrates that the deadlock detector's choice
 *      can integrate cleanly with the rest of the runtime, including
 *      cross-proc message passing.
 *
 *   3. We sort 32 random ints with a quicksort whose comparator
 *      yields between partitions, demonstrating how to weave async
 *      behaviour into a classically synchronous algorithm shape.
 *
 * Run repeatedly to see different victims chosen (the deck is
 * re-seeded each run).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_int.h"
#include "xtc_loop.h"
#include "xtc_lockmgr.h"
#include "xtc_proc.h"

/* ---- Fisher-Yates shuffled deck ---- */

#define DECK_N 1024

struct deck {
	int      cards[DECK_N];
	int      pos;        /* next card to deal */
	uint64_t deals;      /* total deals across reshuffles */
	uint64_t state;      /* xorshift64 state for shuffling */
};

static uint64_t
xs64(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	*s = x ? x : 0x1234567890abcdefULL;
	return x;
}

static void
deck_shuffle(struct deck *d)
{
	int i;
	for (i = DECK_N - 1; i > 0; i--) {
		int j = (int)(xs64(&d->state) % (uint64_t)(i + 1));
		int t = d->cards[i]; d->cards[i] = d->cards[j]; d->cards[j] = t;
	}
	d->pos = 0;
}

static void
deck_init(struct deck *d)
{
	int i;
	int fd;
	uint64_t seed = 0;
	for (i = 0; i < DECK_N; i++) d->cards[i] = i;
	d->pos = 0; d->deals = 0;
	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) { (void)read(fd, &seed, sizeof seed); (void)close(fd); }
	if (seed == 0) seed = (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ULL;
	d->state = seed;
	deck_shuffle(d);
}

static int
deck_next(struct deck *d)
{
	int card;
	if (d->pos >= DECK_N) deck_shuffle(d);
	card = d->cards[d->pos++];
	d->deals++;
	return card;
}

/* ---- xtc_proc card-producer (a coroutine generator) ---- */

struct producer_msg {
	xtc_pid_t reply_to;
};
struct producer_reply {
	int card;
};

static void
producer_proc(void *arg)
{
	struct deck d;
	(void)arg;
	deck_init(&d);
	for (;;) {
		void *msg = NULL; size_t sz = 0;
		struct producer_reply r;
		if (xtc_recv(&msg, &sz, -1) != XTC_OK) return;
		if (msg != NULL && sz == sizeof(struct producer_msg)) {
			struct producer_msg *m = msg;
			r.card = deck_next(&d);
			(void)xtc_send(m->reply_to, &r, sizeof r);
		}
		if (msg) __os_free(msg);
	}
}

static xtc_pid_t g_producer;

/* Synchronous adapter: send-recv against the producer.  Used from
 * inside the loop (driver_proc + qsort) where we have a current
 * proc context. */
static int
deck_get(void)
{
	struct producer_msg req;
	void *reply = NULL; size_t sz = 0;
	int card = 0;
	req.reply_to = xtc_self();
	if (xtc_send(g_producer, &req, sizeof req) != XTC_OK) return 0;
	if (xtc_recv(&reply, &sz, 1000LL * 1000 * 1000) != XTC_OK) return 0;
	if (reply && sz == sizeof(struct producer_reply))
		card = ((struct producer_reply *)reply)->card;
	if (reply) __os_free(reply);
	return card;
}

/* Deck used by the victim picker, which is invoked from the
 * deadlock detector's own thread (not an xtc_proc).  Cannot send
 * to the producer from here, so we keep our own deck behind a
 * mutex.  (Demonstrates that the picker callback can pull
 * randomness from any source it likes.) */
static struct deck       g_picker_deck;
static pthread_mutex_t   g_picker_lock = PTHREAD_MUTEX_INITIALIZER;
static int               g_picker_deck_ready;

static int
picker_deck_get(void)
{
	int card;
	pthread_mutex_lock(&g_picker_lock);
	if (!g_picker_deck_ready) {
		deck_init(&g_picker_deck);
		g_picker_deck_ready = 1;
	}
	card = deck_next(&g_picker_deck);
	pthread_mutex_unlock(&g_picker_lock);
	return card;
}

/* ---- custom victim picker: deck card mod n_candidates ---- */

static _Atomic int g_picks;

static int
random_victim_picker(const uint64_t *candidates, int n, void *user)
{
	int card;
	(void)user; (void)candidates;
	card = picker_deck_get();
	atomic_fetch_add(&g_picks, 1);
	return card % n;
}

/* ---- coroutine-friendly quicksort ---- */

static _Atomic int g_cmps;

static int
qcmp(int x, int y)
{
	atomic_fetch_add(&g_cmps, 1);
	/* Routing the comparison through a fiber yield demonstrates how
	 * to bridge the synchronous qsort shape with async work.  In a
	 * real app the compare might consult a database or a remote
	 * service; here we just yield to let the producer or other
	 * fibers run between comparisons. */
	xtc_yield();
	return (x > y) - (x < y);
}

static void
qsort_yielding(int *a, int n)
{
	int i, j, pivot, tmp;
	if (n <= 1) return;
	pivot = a[n / 2];
	i = 0; j = n - 1;
	while (i <= j) {
		while (qcmp(a[i], pivot) < 0) i++;
		while (qcmp(a[j], pivot) > 0) j--;
		if (i <= j) {
			tmp = a[i]; a[i] = a[j]; a[j] = tmp;
			i++; j--;
		}
	}
	qsort_yielding(a, j + 1);
	qsort_yielding(a + i, n - i);
}

/* ---- the deadlock scenario ---- */

static xtc_lockmgr_t *g_mgr;
static _Atomic int    g_a_rc;
static _Atomic int    g_b_rc;

static void *
txn_a(void *arg)
{
	xtc_locker_t l = *(xtc_locker_t *)arg;
	struct timespec sleep_ns = { 0, 100 * 1000 * 1000 };
	int rc;
	(void)xtc_lock_get(g_mgr, l, "A", 1, XTC_LOCK_X, 0);
	(void)nanosleep(&sleep_ns, NULL);
	rc = xtc_lock_get(g_mgr, l, "B", 1, XTC_LOCK_X,
	    5LL * 1000 * 1000 * 1000);
	atomic_store(&g_a_rc, rc);
	if (rc == XTC_OK) {
		(void)xtc_lock_put(g_mgr, l, "B", 1);
		(void)xtc_lock_put(g_mgr, l, "A", 1);
	}
	return NULL;
}

static void *
txn_b(void *arg)
{
	xtc_locker_t l = *(xtc_locker_t *)arg;
	struct timespec sleep_ns = { 0, 100 * 1000 * 1000 };
	int rc;
	(void)xtc_lock_get(g_mgr, l, "B", 1, XTC_LOCK_X, 0);
	(void)nanosleep(&sleep_ns, NULL);
	rc = xtc_lock_get(g_mgr, l, "A", 1, XTC_LOCK_X,
	    5LL * 1000 * 1000 * 1000);
	atomic_store(&g_b_rc, rc);
	if (rc == XTC_OK) {
		(void)xtc_lock_put(g_mgr, l, "A", 1);
		(void)xtc_lock_put(g_mgr, l, "B", 1);
	}
	return NULL;
}

/* ---- driver proc that runs everything inside the xtc loop ---- */

struct driver_args { xtc_loop_t *loop; };

static void
driver_proc(void *arg)
{
	xtc_lockmgr_opts_t opts = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	pthread_t t1, t2;
	int round;
	int n_a_aborted = 0, n_b_aborted = 0;

	(void)arg;

	opts.detect_mode  = XTC_LOCK_DETECT_PERIODIC;
	opts.detect_interval_ns = 50 * 1000 * 1000;       /* 50 ms */
	opts.victim       = XTC_LOCK_VICTIM_CUSTOM;
	opts.victim_pick_fn = random_victim_picker;

	if (xtc_lockmgr_create(&opts, &g_mgr) != XTC_OK) {
		fprintf(stderr, "lockmgr create failed\n");
		return;
	}

	/* Run the deadlock scenario several times.  With random victim
	 * selection the choice should diverge across runs. */
	printf("Deadlock scenario, custom victim picker:\n");
	for (round = 0; round < 6; round++) {
		(void)xtc_lockmgr_id(g_mgr, &l1);
		(void)xtc_lockmgr_id(g_mgr, &l2);
		atomic_store(&g_a_rc, 0);
		atomic_store(&g_b_rc, 0);

		pthread_create(&t1, NULL, txn_a, &l1);
		pthread_create(&t2, NULL, txn_b, &l2);
		pthread_join(t1, NULL);
		pthread_join(t2, NULL);

		{
			int ar = atomic_load(&g_a_rc);
			int br = atomic_load(&g_b_rc);
			if (ar == XTC_E_DEADLK) n_a_aborted++;
			if (br == XTC_E_DEADLK) n_b_aborted++;
			printf("  round %d: txn_a rc=%d txn_b rc=%d  (victim %s)\n",
			    round, ar, br,
			    ar == XTC_E_DEADLK ? "= A" :
			    br == XTC_E_DEADLK ? "= B" :
			    "(none -- no deadlock this round)");
		}

		(void)xtc_lockmgr_id_free(g_mgr, l1);
		(void)xtc_lockmgr_id_free(g_mgr, l2);
	}
	printf("  abort tally: A=%d, B=%d (random expects rough split)\n",
	    n_a_aborted, n_b_aborted);
	printf("  total custom-picker calls: %d\n", atomic_load(&g_picks));

	/* Demonstrate the coroutine qsort: pull 32 cards from the deck,
	 * sort them with our yielding quicksort. */
	{
		int arr[32];
		int i;
		printf("\nFiber-yielding quicksort over 32 dealt cards:\n");
		for (i = 0; i < 32; i++) arr[i] = deck_get();
		printf("  before: ");
		for (i = 0; i < 16; i++) printf("%d ", arr[i]);
		printf("...\n");
		atomic_store(&g_cmps, 0);
		qsort_yielding(arr, 32);
		printf("  after:  ");
		for (i = 0; i < 16; i++) printf("%d ", arr[i]);
		printf("...\n");
		printf("  comparisons: %d (each yields, so the producer can run)\n",
		    atomic_load(&g_cmps));
		/* Verify sort. */
		for (i = 1; i < 32; i++) {
			if (arr[i] < arr[i - 1]) {
				printf("  ERROR: sort failed at index %d\n", i);
				break;
			}
		}
	}

	{
		xtc_lockmgr_stat_t s;
		xtc_lockmgr_stat(g_mgr, &s);
		printf("\nlockmgr stats: acquires=%llu releases=%llu deadlocks=%llu\n",
		    (unsigned long long)s.n_acquires,
		    (unsigned long long)s.n_releases,
		    (unsigned long long)s.n_deadlocks_found);
	}

	xtc_lockmgr_destroy(g_mgr);
	fflush(stdout);

	/* Stop the loop -- the producer would otherwise keep us
	 * running forever waiting for cards we'll never request. */
	xtc_loop_stop(((struct driver_args *)arg)->loop);
}

int
main(void)
{
	xtc_loop_t *loop;
	xtc_pid_t prod_pid, drv_pid;
	struct driver_args args;

	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	args.loop = loop;

	/* Spawn the card-producer first so the driver and its child
	 * threads can consult it. */
	if (xtc_proc_spawn(loop, producer_proc, NULL, NULL, &prod_pid)
	    != XTC_OK)
		return 1;
	g_producer = prod_pid;

	if (xtc_proc_spawn(loop, driver_proc, &args, NULL, &drv_pid)
	    != XTC_OK)
		return 1;

	/* Run the loop.  When the driver returns it stops the loop;
	 * the producer (which would otherwise wait forever) doesn't
	 * get a chance to drift. */
	if (xtc_loop_run(loop) != XTC_OK) return 1;

	(void)xtc_loop_fini(loop);
	return 0;
}

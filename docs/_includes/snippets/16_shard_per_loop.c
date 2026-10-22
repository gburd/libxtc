/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/16_shard_per_loop.c -- DragonFly's
 * shared-nothing shape on libxtc: each loop of an xtc_exec owns one
 * slice of the keyspace (an "owner" proc per loop), a key's owner is
 * hash(key) % nloops, and a client runs a command INLINE when it is
 * already on the owner's loop and sends it there (a "hop") otherwise.
 * No lock guards the data: only the owner ever touches its slice.
 *
 * Prints how many commands ran inline vs hopped and checks the final
 * counters, so the pattern is shown working, not just compiling.
 */

/* !region full */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define NLOOPS   4
#define NKEYS    64
#define NCLIENTS 8
#define NOPS     1000

/* !region shard */
/* One slice of the keyspace.  Only the proc running on loop `id`
 * reads or writes it -- no lock. */
struct shard {
	int64_t counter[NKEYS];
};
static struct shard g_shard[NLOOPS];
static xtc_pid_t g_owner[NLOOPS];

static int
owner_of(int key)
{
	uint32_t h = (uint32_t)key * 2654435761u;   /* any good hash */
	return (int)(h % NLOOPS);
}

/* The operation itself: runs on the owner's loop, always. */
static int64_t
incr(int key)
{
	return ++g_shard[owner_of(key)].counter[key];
}
/* !endregion shard */

/* !region hop */
struct req {
	xtc_pid_t from;
	int       key;
};

/* The owner proc: serves hops for its slice until told to stop
 * (an empty message). */
static void
owner_proc(void *arg)
{
	void *m;
	size_t sz;
	int64_t v;

	(void)arg;
	for (;;) {
		if (xtc_recv(&m, &sz, -1) != XTC_OK)
			continue;
		if (sz != sizeof(struct req)) {
			xtc_free(m);
			return;
		}
		v = incr(((struct req *)m)->key);
		(void)xtc_send(((struct req *)m)->from, &v, sizeof v);
		xtc_free(m);
	}
}

/* A client: inline when it already runs on the owner's loop (DragonFly's
 * CanRunInlined), otherwise one request/reply round trip to the owner. */
static _Atomic int g_inline, g_hops, g_done;

static void
client_proc(void *arg)
{
	struct req r;
	void *m;
	size_t sz;
	int i, key, seed = (int)(intptr_t)arg;

	r.from = xtc_self();
	for (i = 0; i < NOPS; i++) {
		key = (seed * 31 + i * 7) % NKEYS;
		if (owner_of(key) == xtc_shard_id()) {
			(void)incr(key);
			g_inline++;
			continue;
		}
		r.key = key;
		(void)xtc_send(g_owner[owner_of(key)], &r, sizeof r);
		while (xtc_recv(&m, &sz, -1) != XTC_OK)
			;
		xtc_free(m);
		g_hops++;
	}
	if (++g_done == NCLIENTS)
		for (i = 0; i < NLOOPS; i++)
			(void)xtc_send(g_owner[i], NULL, 0);
}
/* !endregion hop */

int
main(void)
{
	xtc_exec_t *ex;
	int64_t total = 0;
	int i, k;

	if (xtc_exec_init(&ex, NLOOPS) != XTC_OK)
		return 1;
	for (i = 0; i < NLOOPS; i++)
		if (xtc_proc_spawn(xtc_exec_loop(ex, i), owner_proc, NULL,
		    NULL, &g_owner[i]) != XTC_OK)
			return 1;
	for (i = 0; i < NCLIENTS; i++)
		if (xtc_proc_spawn(xtc_exec_loop(ex, i % NLOOPS), client_proc,
		    (void *)(intptr_t)i, NULL, NULL) != XTC_OK)
			return 1;
	(void)xtc_exec_run(ex);     /* returns when every proc has exited */
	(void)xtc_exec_fini(ex);

	for (i = 0; i < NLOOPS; i++)
		for (k = 0; k < NKEYS; k++)
			total += g_shard[i].counter[k];
	printf("%d commands: %d inline on the owner's loop, %d hopped\n",
	    NCLIENTS * NOPS, g_inline, g_hops);
	printf("sum of all counters = %lld (expect %d)\n", (long long)total,
	    NCLIENTS * NOPS);
	return total == NCLIENTS * NOPS && g_inline > 0 && g_hops > 0 ? 0 : 1;
}
/* !endregion full */

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/group.c -- consumer-group offset coordinator.
 *
 *	One coordinator proc owns the whole committed-offset table.
 *	All commits and fetches arrive as mailbox messages and are
 *	handled one at a time, so the table is touched by a single
 *	thread of control and needs no lock.  COMMIT upserts the offset
 *	for a (group, topic, partition) key; FETCH returns it (or
 *	reports that none is committed yet).
 */

#include "group.h"

#include <stdlib.h>
#include <string.h>

#include "xtc_int.h"          /* __os_free */
#include "xtc_proc.h"

#define GRP_MAX_ENTRIES  1024

struct grp_entry {
	char      group[KAKA_GROUP_NAME_MAX];
	char      topic[KAKA_TOPIC_NAME_MAX];
	uint32_t  partition;
	uint64_t  offset;
	int       used;
};

static int
grp_key_eq(const struct grp_entry *e, const struct grp_req *r)
{
	return e->used &&
	    e->partition == r->partition &&
	    strncmp(e->group, r->group, KAKA_GROUP_NAME_MAX) == 0 &&
	    strncmp(e->topic, r->topic, KAKA_TOPIC_NAME_MAX) == 0;
}

static void
coordinator_proc(void *arg)
{
	struct grp_entry *tab;
	int n_used = 0;
	void *msg;
	size_t mlen;

	(void)arg;
	tab = calloc(GRP_MAX_ENTRIES, sizeof *tab);
	if (tab == NULL)
		return;

	for (;;) {
		struct grp_req req;
		struct grp_reply rep;
		int i, slot;

		if (xtc_recv(&msg, &mlen, -1) != XTC_OK || msg == NULL)
			break;
		if (mlen < sizeof req) { __os_free(msg); continue; }
		memcpy(&req, msg, sizeof req);
		__os_free(msg);

		if (req.op == GRP_SHUTDOWN) {
			memset(&rep, 0, sizeof rep);
			rep.tag = req.tag; rep.ok = 1;
			(void)xtc_send(req.reply, &rep, sizeof rep);
			break;
		}

		memset(&rep, 0, sizeof rep);
		rep.tag = req.tag;

		/* Locate an existing entry for this key. */
		slot = -1;
		for (i = 0; i < GRP_MAX_ENTRIES; i++) {
			if (grp_key_eq(&tab[i], &req)) { slot = i; break; }
		}

		if (req.op == GRP_COMMIT) {
			if (slot < 0) {
				/* Insert into the first free slot. */
				for (i = 0; i < GRP_MAX_ENTRIES; i++)
					if (!tab[i].used) { slot = i; break; }
				if (slot < 0) {
					rep.ok = 0;     /* table full */
					(void)xtc_send(req.reply, &rep,
					    sizeof rep);
					continue;
				}
				tab[slot].used = 1;
				n_used++;
				memcpy(tab[slot].group, req.group,
				    KAKA_GROUP_NAME_MAX);
				memcpy(tab[slot].topic, req.topic,
				    KAKA_TOPIC_NAME_MAX);
				tab[slot].partition = req.partition;
			}
			tab[slot].offset = req.offset;  /* last write wins */
			rep.ok = 1;
			rep.offset = req.offset;
			(void)xtc_send(req.reply, &rep, sizeof rep);
		} else if (req.op == GRP_FETCH) {
			rep.ok = 1;
			if (slot >= 0) {
				rep.found = 1;
				rep.offset = tab[slot].offset;
			} else {
				rep.found = 0;
				rep.offset = 0;
			}
			(void)xtc_send(req.reply, &rep, sizeof rep);
		} else {
			rep.ok = 0;
			(void)xtc_send(req.reply, &rep, sizeof rep);
		}
	}

	(void)n_used;
	free(tab);
}

int
group_coordinator_spawn(xtc_loop_t *loop, xtc_pid_t *out)
{
	xtc_proc_opts_t opts = { 0 };
	opts.name = "group-coordinator";
	return xtc_proc_spawn(loop, coordinator_proc, NULL, &opts, out);
}

/* ---- in-process self-test ---- */

struct grp_test_state {
	int        result;
	xtc_pid_t  coord;
};

static int
grp_call(xtc_pid_t coord, const struct grp_req *req, struct grp_reply *rep)
{
	void *msg;
	size_t mlen;

	if (xtc_send(coord, req, sizeof *req) != XTC_OK)
		return -1;
	if (xtc_recv(&msg, &mlen, 2000LL * 1000000) != XTC_OK || msg == NULL)
		return -1;
	if (mlen >= sizeof *rep)
		memcpy(rep, msg, sizeof *rep);
	__os_free(msg);
	return 0;
}

static void
grp_test_client(void *arg)
{
	struct grp_test_state *st = arg;
	struct grp_req req;
	struct grp_reply rep;

	/* 1. FETCH before any commit: not found. */
	memset(&req, 0, sizeof req);
	req.op = GRP_FETCH; req.reply = xtc_self(); req.tag = 1;
	strcpy(req.group, "g1"); strcpy(req.topic, "t"); req.partition = 0;
	if (grp_call(st->coord, &req, &rep) != 0 || !rep.ok || rep.found) {
		st->result = 1; goto done;
	}

	/* 2. COMMIT g1/t/0 = 50, then FETCH returns 50. */
	req.op = GRP_COMMIT; req.tag = 2; req.offset = 50;
	if (grp_call(st->coord, &req, &rep) != 0 || !rep.ok) {
		st->result = 2; goto done;
	}
	req.op = GRP_FETCH; req.tag = 3; req.offset = 0;
	if (grp_call(st->coord, &req, &rep) != 0 || !rep.found ||
	    rep.offset != 50) {
		st->result = 3; goto done;
	}

	/* 3. Last write wins: COMMIT 100, FETCH returns 100. */
	req.op = GRP_COMMIT; req.tag = 4; req.offset = 100;
	if (grp_call(st->coord, &req, &rep) != 0 || !rep.ok) {
		st->result = 4; goto done;
	}
	req.op = GRP_FETCH; req.tag = 5; req.offset = 0;
	if (grp_call(st->coord, &req, &rep) != 0 || rep.offset != 100) {
		st->result = 5; goto done;
	}

	/* 4. Group isolation: a different group sees no offset. */
	memset(req.group, 0, sizeof req.group); strcpy(req.group, "g2");
	req.op = GRP_FETCH; req.tag = 6;
	if (grp_call(st->coord, &req, &rep) != 0 || rep.found) {
		st->result = 6; goto done;
	}

	/* 5. Partition isolation: same group, partition 1, no offset. */
	strcpy(req.group, "g1"); req.partition = 1;
	req.op = GRP_FETCH; req.tag = 7;
	if (grp_call(st->coord, &req, &rep) != 0 || rep.found) {
		st->result = 7; goto done;
	}

	st->result = 0;
done:
	memset(&req, 0, sizeof req);
	req.op = GRP_SHUTDOWN; req.reply = xtc_self(); req.tag = 999;
	(void)grp_call(st->coord, &req, &rep);
}

int
group_selftest(void)
{
	xtc_loop_t *loop = NULL;
	struct grp_test_state st;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t client;

	memset(&st, 0, sizeof st);
	if (xtc_loop_init(&loop) != XTC_OK)
		return -1;
	if (group_coordinator_spawn(loop, &st.coord) != XTC_OK)
		return -1;
	opts.name = "group-test-client";
	if (xtc_proc_spawn(loop, grp_test_client, &st, &opts, &client)
	    != XTC_OK)
		return -1;
	if (xtc_loop_run(loop) != XTC_OK)
		return -1;
	return st.result;
}

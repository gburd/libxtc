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
#include "partition.h"        /* plog_* for durable offset commits */

#define GRP_MAX_ENTRIES  1024

/* On-disk key layout for a committed offset: fixed group/topic name
 * fields plus the partition, so replay reconstructs the entry exactly
 * (no ambiguity from a delimiter inside a name).  Value is the 8-byte
 * offset, little-endian. */
#define GRP_KEY_LEN  (KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX + 4)
#define GRP_VAL_LEN  8

struct grp_entry {
	char      group[KAKA_GROUP_NAME_MAX];
	char      topic[KAKA_TOPIC_NAME_MAX];
	uint32_t  partition;
	uint64_t  offset;
	int       used;
};

struct coord_arg {
	int   has_dir;
	char  dir[200];
};

static int
grp_match(const struct grp_entry *e, const char *group, const char *topic,
    uint32_t partition)
{
	return e->used && e->partition == partition &&
	    strncmp(e->group, group, KAKA_GROUP_NAME_MAX) == 0 &&
	    strncmp(e->topic, topic, KAKA_TOPIC_NAME_MAX) == 0;
}

/* Upsert (group, topic, partition) -> offset.  Returns 0 on success,
 * -1 if the table is full. */
static int
grp_upsert(struct grp_entry *tab, const char *group, const char *topic,
    uint32_t partition, uint64_t offset)
{
	int i, slot = -1;

	for (i = 0; i < GRP_MAX_ENTRIES; i++)
		if (grp_match(&tab[i], group, topic, partition)) {
			slot = i; break;
		}
	if (slot < 0) {
		for (i = 0; i < GRP_MAX_ENTRIES; i++)
			if (!tab[i].used) { slot = i; break; }
		if (slot < 0)
			return -1;
		tab[slot].used = 1;
		memset(tab[slot].group, 0, KAKA_GROUP_NAME_MAX);
		memset(tab[slot].topic, 0, KAKA_TOPIC_NAME_MAX);
		strncpy(tab[slot].group, group, KAKA_GROUP_NAME_MAX - 1);
		strncpy(tab[slot].topic, topic, KAKA_TOPIC_NAME_MAX - 1);
		tab[slot].partition = partition;
	}
	tab[slot].offset = offset;       /* last write wins */
	return 0;
}

static int
grp_find(const struct grp_entry *tab, const char *group, const char *topic,
    uint32_t partition, uint64_t *offset)
{
	int i;
	for (i = 0; i < GRP_MAX_ENTRIES; i++)
		if (grp_match(&tab[i], group, topic, partition)) {
			if (offset != NULL) *offset = tab[i].offset;
			return 1;
		}
	return 0;
}

/* Encode/append one committed offset to the durable log. */
static void
grp_persist(plog_t *log, const struct grp_req *req)
{
	uint8_t key[GRP_KEY_LEN];
	uint8_t val[GRP_VAL_LEN];
	kaka_record_t rec;
	uint64_t o = req->offset;
	int i;

	if (log == NULL)
		return;
	memset(key, 0, sizeof key);
	memcpy(key, req->group, KAKA_GROUP_NAME_MAX);
	memcpy(key + KAKA_GROUP_NAME_MAX, req->topic, KAKA_TOPIC_NAME_MAX);
	key[KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX + 0] =
	    (uint8_t)(req->partition & 0xff);
	key[KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX + 1] =
	    (uint8_t)((req->partition >> 8) & 0xff);
	key[KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX + 2] =
	    (uint8_t)((req->partition >> 16) & 0xff);
	key[KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX + 3] =
	    (uint8_t)((req->partition >> 24) & 0xff);
	for (i = 0; i < GRP_VAL_LEN; i++)
		val[i] = (uint8_t)((o >> (8 * i)) & 0xff);
	rec.key = key; rec.key_len = GRP_KEY_LEN;
	rec.value = val; rec.value_len = GRP_VAL_LEN;
	(void)plog_append(log, &rec);
}

/* Replay the durable log into the in-memory table (commit order =
 * append order, so last write wins naturally). */
static void
grp_replay(plog_t *log, struct grp_entry *tab)
{
	uint64_t n, i;

	if (log == NULL)
		return;
	n = plog_count(log);
	for (i = 0; i < n; i++) {
		kaka_record_t rec;
		char group[KAKA_GROUP_NAME_MAX];
		char topic[KAKA_TOPIC_NAME_MAX];
		uint32_t partition;
		uint64_t offset = 0;
		int b;

		if (plog_read(log, i, &rec) != 1)
			continue;       /* plog_read returns 1 on success */
		if (rec.key_len != GRP_KEY_LEN || rec.value_len != GRP_VAL_LEN)
			continue;
		memset(group, 0, sizeof group);
		memset(topic, 0, sizeof topic);
		memcpy(group, rec.key, KAKA_GROUP_NAME_MAX);
		memcpy(topic, rec.key + KAKA_GROUP_NAME_MAX,
		    KAKA_TOPIC_NAME_MAX);
		group[KAKA_GROUP_NAME_MAX - 1] = '\0';
		topic[KAKA_TOPIC_NAME_MAX - 1] = '\0';
		partition =
		    (uint32_t)rec.key[KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX] |
		    ((uint32_t)rec.key[KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX + 1] << 8) |
		    ((uint32_t)rec.key[KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX + 2] << 16) |
		    ((uint32_t)rec.key[KAKA_GROUP_NAME_MAX + KAKA_TOPIC_NAME_MAX + 3] << 24);
		for (b = 0; b < GRP_VAL_LEN; b++)
			offset |= (uint64_t)rec.value[b] << (8 * b);
		(void)grp_upsert(tab, group, topic, partition, offset);
	}
}

static void
coordinator_proc(void *arg)
{
	struct coord_arg *ca = arg;
	struct grp_entry *tab;
	plog_t *log = NULL;
	void *msg;
	size_t mlen;

	tab = calloc(GRP_MAX_ENTRIES, sizeof *tab);
	if (tab == NULL) {
		free(ca);
		return;
	}
	if (ca != NULL && ca->has_dir) {
		if (plog_create_ex(ca->dir, 0, &log) != 0)
			log = NULL;
		else
			grp_replay(log, tab);
	}
	free(ca);

	for (;;) {
		struct grp_req req;
		struct grp_reply rep;

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

		if (req.op == GRP_COMMIT) {
			if (grp_upsert(tab, req.group, req.topic,
			    req.partition, req.offset) != 0) {
				rep.ok = 0;          /* table full */
				(void)xtc_send(req.reply, &rep, sizeof rep);
				continue;
			}
			grp_persist(log, &req);  /* durable, after in-memory */
			rep.ok = 1;
			rep.offset = req.offset;
			(void)xtc_send(req.reply, &rep, sizeof rep);
		} else if (req.op == GRP_FETCH) {
			uint64_t off = 0;
			rep.ok = 1;
			rep.found = grp_find(tab, req.group, req.topic,
			    req.partition, &off);
			rep.offset = rep.found ? off : 0;
			(void)xtc_send(req.reply, &rep, sizeof rep);
		} else {
			rep.ok = 0;
			(void)xtc_send(req.reply, &rep, sizeof rep);
		}
	}

	if (log != NULL)
		plog_destroy(log);
	free(tab);
}

int
group_coordinator_spawn(xtc_loop_t *loop, xtc_pid_t *out)
{
	return group_coordinator_spawn_ex(loop, NULL, out);
}

int
group_coordinator_spawn_ex(xtc_loop_t *loop, const char *dir, xtc_pid_t *out)
{
	xtc_proc_opts_t opts = { 0 };
	struct coord_arg *ca;

	ca = calloc(1, sizeof *ca);
	if (ca == NULL)
		return XTC_E_NOMEM;
	if (dir != NULL && dir[0] != '\0') {
		ca->has_dir = 1;
		strncpy(ca->dir, dir, sizeof ca->dir - 1);
	}
	opts.name = "group-coordinator";
	if (xtc_proc_spawn(loop, coordinator_proc, ca, &opts, out) != XTC_OK) {
		free(ca);
		return XTC_E_INVAL;
	}
	return XTC_OK;
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

/* ---- durability self-test (two coordinator lifetimes) ---- */

struct grp_persist_state {
	int        result;
	int        mode;     /* 0 = commit phase, 1 = verify phase */
	xtc_pid_t  coord;
};

static void
grp_persist_client(void *arg)
{
	struct grp_persist_state *st = arg;
	struct grp_req req;
	struct grp_reply rep;

	memset(&req, 0, sizeof req);
	req.reply = xtc_self();
	strcpy(req.topic, "t");

	if (st->mode == 0) {
		/* Commit three offsets across two groups/partitions. */
		strcpy(req.group, "g1"); req.partition = 0;
		req.op = GRP_COMMIT; req.tag = 1; req.offset = 111;
		if (grp_call(st->coord, &req, &rep) != 0 || !rep.ok)
			{ st->result = 1; goto done; }
		req.partition = 1; req.tag = 2; req.offset = 222;
		if (grp_call(st->coord, &req, &rep) != 0 || !rep.ok)
			{ st->result = 2; goto done; }
		memset(req.group, 0, sizeof req.group); strcpy(req.group, "g2");
		req.partition = 0; req.tag = 3; req.offset = 333;
		if (grp_call(st->coord, &req, &rep) != 0 || !rep.ok)
			{ st->result = 3; goto done; }
		/* Re-commit g1/t/0 to a higher offset; replay must keep this. */
		strcpy(req.group, "g1"); req.partition = 0;
		req.tag = 4; req.offset = 150;
		if (grp_call(st->coord, &req, &rep) != 0 || !rep.ok)
			{ st->result = 4; goto done; }
		st->result = 0;
	} else {
		/* After replay, the committed offsets must be present. */
		strcpy(req.group, "g1"); req.partition = 0;
		req.op = GRP_FETCH; req.tag = 10;
		if (grp_call(st->coord, &req, &rep) != 0 || !rep.found ||
		    rep.offset != 150)            /* last write wins survived */
			{ st->result = 10; goto done; }
		req.partition = 1; req.tag = 11;
		if (grp_call(st->coord, &req, &rep) != 0 || !rep.found ||
		    rep.offset != 222)
			{ st->result = 11; goto done; }
		memset(req.group, 0, sizeof req.group); strcpy(req.group, "g2");
		req.partition = 0; req.tag = 12;
		if (grp_call(st->coord, &req, &rep) != 0 || !rep.found ||
		    rep.offset != 333)
			{ st->result = 12; goto done; }
		st->result = 0;
	}
done:
	memset(&req, 0, sizeof req);
	req.op = GRP_SHUTDOWN; req.reply = xtc_self(); req.tag = 999;
	(void)grp_call(st->coord, &req, &rep);
}

static int
grp_persist_phase(const char *dir, int mode)
{
	xtc_loop_t *loop = NULL;
	struct grp_persist_state st;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t client;

	memset(&st, 0, sizeof st);
	st.mode = mode;
	if (xtc_loop_init(&loop) != XTC_OK)
		return -1;
	if (group_coordinator_spawn_ex(loop, dir, &st.coord) != XTC_OK)
		return -1;
	opts.name = "group-persist-client";
	if (xtc_proc_spawn(loop, grp_persist_client, &st, &opts, &client)
	    != XTC_OK)
		return -1;
	if (xtc_loop_run(loop) != XTC_OK)
		return -1;
	return st.result;
}

int
group_persist_selftest(const char *dir)
{
	int rc;

	rc = grp_persist_phase(dir, 0);   /* commit, then coordinator exits */
	if (rc != 0)
		return rc;
	/* A fresh coordinator over the same dir must replay the offsets. */
	return grp_persist_phase(dir, 1);
}

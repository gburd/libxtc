/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/group.h -- consumer-group offset coordinator.
 *
 *	A single coordinator process owns every consumer group's
 *	committed offsets.  Because exactly one process touches that
 *	state, the offset table needs no locks: commits and fetches are
 *	serialized by the coordinator's mailbox.  This is the actor /
 *	single-owner pattern -- the same one the partition proc uses for
 *	log ordering -- applied to group metadata.
 */

#ifndef KAKA_GROUP_H
#define KAKA_GROUP_H

#include <stdint.h>
#include "xtc_loop.h"
#include "xtc_proc.h"

/* Coordinator message opcodes. */
enum {
	GRP_COMMIT    = 1,   /* store a committed offset */
	GRP_FETCH     = 2,   /* read a committed offset */
	GRP_SHUTDOWN  = 3
};

#define KAKA_GROUP_NAME_MAX  32
#define KAKA_TOPIC_NAME_MAX  32

/* Request sent to the coordinator. */
struct grp_req {
	uint8_t    op;
	xtc_pid_t  reply;
	uint32_t   tag;
	char       group[KAKA_GROUP_NAME_MAX];
	char       topic[KAKA_TOPIC_NAME_MAX];
	uint32_t   partition;
	uint64_t   offset;       /* COMMIT: offset to store */
};

/* Reply from the coordinator. */
struct grp_reply {
	uint32_t   tag;
	int        ok;
	int        found;        /* FETCH: 1 if a committed offset exists */
	uint64_t   offset;       /* FETCH: the committed offset, else 0 */
};

/* Spawn the coordinator proc on `loop`; returns its pid via *out. */
int group_coordinator_spawn(xtc_loop_t *loop, xtc_pid_t *out);

/* In-process self-test: commit and fetch offsets across two groups
 * and assert isolation and last-write-wins.  Returns 0 on pass. */
int group_selftest(void);

#endif /* KAKA_GROUP_H */

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/pg.c
 *	Process groups over the duplicate-key registry (xtc_reg_register_dup
 *	et al).  See src/inc/xtc_pg.h.  This is a thin, single-node layer:
 *	join/leave are registry ops, members walks the key, and send
 *	broadcasts to each member via xtc_send.
 */

#include "xtc_int.h"
#include "xtc_pg.h"

int
xtc_pg_join(xtc_reg_t *reg, const char *group, xtc_pid_t pid)
{
	if (reg == NULL || group == NULL) return XTC_E_INVAL;
	return xtc_reg_register_dup(reg, group, pid);
}

int
xtc_pg_leave(xtc_reg_t *reg, const char *group, xtc_pid_t pid)
{
	if (reg == NULL || group == NULL) return XTC_E_INVAL;
	return xtc_reg_unregister_pid(reg, group, pid);
}

int
xtc_pg_members(xtc_reg_t *reg, const char *group,
               int (*fn)(xtc_pid_t pid, void *user), void *user)
{
	if (reg == NULL || group == NULL || fn == NULL) return 0;
	return xtc_reg_members(reg, group, fn, user);
}

/* Broadcast: collect the member pids under the registry lock (via
 * xtc_reg_members' callback), then send OUTSIDE the lock.  Sending
 * inside the members callback would xtc_send while holding the registry
 * lock, which can park the sender (a bounded mailbox) with the lock
 * held -- a latent stall.  So the callback only copies pids into a small
 * heap array, and we send after the walk returns. */
struct pg_collect {
	xtc_pid_t *pids;
	int        n;
	int        cap;
	int        oom;
};

static int
pg_collect_cb(xtc_pid_t pid, void *user)
{
	struct pg_collect *c = user;
	if (c->n == c->cap) {
		int ncap = c->cap == 0 ? 8 : c->cap * 2;
		void *np = NULL;
		if (__os_realloc(c->pids, (size_t)ncap * sizeof(xtc_pid_t), &np)
		    != XTC_OK) {
			c->oom = 1;
			return 1;   /* stop the walk */
		}
		c->pids = np;
		c->cap = ncap;
	}
	c->pids[c->n++] = pid;
	return 0;
}

int
xtc_pg_send(xtc_reg_t *reg, const char *group, const void *msg, size_t size)
{
	struct pg_collect c;
	int i, sent = 0;

	if (reg == NULL || group == NULL) return 0;
	if (size > 0 && msg == NULL) return 0;

	c.pids = NULL;
	c.n = 0;
	c.cap = 0;
	c.oom = 0;
	(void)xtc_reg_members(reg, group, pg_collect_cb, &c);

	for (i = 0; i < c.n; i++) {
		if (xtc_send(c.pids[i], msg, size) == XTC_OK)
			sent++;
		/* a dead member's send fails harmlessly; skip it */
	}
	if (c.pids != NULL)
		__os_free(c.pids);
	return sent;
}

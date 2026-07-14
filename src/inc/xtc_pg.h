/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_pg.h
 *	Process groups: named, membership sets of pids for pub/sub and
 *	fan-out (the Erlang :pg / Phoenix.PubSub / Discord-presence
 *	pattern).  A group is a duplicate-key registry key; join adds the
 *	caller's pid, leave removes it, and xtc_pg_send broadcasts a
 *	message to every current member.
 *
 *	Single-node only.  Cross-node process groups need the (unbuilt)
 *	distributed module; this is the local substrate.
 *
 *	A group set is backed by an xtc_reg_t (see xtc_reg_register_dup),
 *	so it composes with the app registry: pass xtc_app_registry(app)
 *	to share one namespace, or a dedicated xtc_reg_t for isolation.
 */

#ifndef XTC_PG_H
#define XTC_PG_H

#include "xtc_export.h"

#include <stddef.h>

#include "xtc.h"
#include "xtc_proc.h"
#include "xtc_reg.h"

/*
 * PUBLIC: int xtc_pg_join __P((xtc_reg_t *, const char *, xtc_pid_t));
 * PUBLIC: int xtc_pg_leave __P((xtc_reg_t *, const char *, xtc_pid_t));
 * PUBLIC: int xtc_pg_members __P((xtc_reg_t *, const char *, int (*)(xtc_pid_t, void *), void *));
 * PUBLIC: int xtc_pg_send __P((xtc_reg_t *, const char *, const void *, size_t));
 */

/* Add `pid` to group `group` (idempotent).  Typically the caller joins
 * itself with xtc_self(). */
XTC_API int xtc_pg_join(xtc_reg_t *reg, const char *group, xtc_pid_t pid);

/* Remove `pid` from `group`.  XTC_E_INVAL if not a member. */
XTC_API int xtc_pg_leave(xtc_reg_t *reg, const char *group, xtc_pid_t pid);

/* Visit every member pid of `group` (callback runs under the registry
 * lock -- keep it brief; do not re-enter the registry).  A nonzero
 * callback return stops the walk.  Returns the member count. */
XTC_API int xtc_pg_members(xtc_reg_t *reg, const char *group,
                           int (*fn)(xtc_pid_t pid, void *user), void *user);

/* Broadcast `msg` (a copy) to every current member of `group`.  Returns
 * the number of members the message was sent to.  A send to an
 * already-dead member is a harmless no-op (its DOWN reaping is the
 * embedder's responsibility until the registry monitor lands). */
XTC_API int xtc_pg_send(xtc_reg_t *reg, const char *group,
                        const void *msg, size_t size);

#endif /* XTC_PG_H */

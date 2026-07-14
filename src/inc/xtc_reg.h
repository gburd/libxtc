/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_reg.h
 *	Process registry: name -> xtc_pid_t lookup.  M10.5.
 *
 *	Erlang's gen_server-via-name pattern: spawn a service, register
 *	it under a stable name, and the rest of the system finds it by
 *	name rather than by passing pids around.  We provide this as a
 *	per-application table guarded by a mutex; entries are
 *	registered/unregistered explicitly.
 */

#ifndef XTC_REG_H
#define XTC_REG_H

#include "xtc_export.h"

#include <stddef.h>

#include "xtc.h"
#include "xtc_proc.h"

typedef struct xtc_reg xtc_reg_t;

/*
 * PUBLIC: int       xtc_reg_create __P((xtc_reg_t **));
 * PUBLIC: void      xtc_reg_destroy __P((xtc_reg_t *));
 * PUBLIC: int       xtc_reg_register __P((xtc_reg_t *, const char *, xtc_pid_t));
 * PUBLIC: int       xtc_reg_unregister __P((xtc_reg_t *, const char *));
 * PUBLIC: int       xtc_reg_whereis __P((xtc_reg_t *, const char *, xtc_pid_t *));
 * PUBLIC: int       xtc_reg_count __P((const xtc_reg_t *));
 * PUBLIC: int       xtc_reg_register_dup __P((xtc_reg_t *, const char *, xtc_pid_t));
 * PUBLIC: int       xtc_reg_unregister_pid __P((xtc_reg_t *, const char *, xtc_pid_t));
 * PUBLIC: int       xtc_reg_drop_pid __P((xtc_reg_t *, xtc_pid_t));
 * PUBLIC: void      xtc_reg_reaper __P((void *));
 * PUBLIC: int       xtc_reg_register_mon __P((xtc_reg_t *, const char *, xtc_pid_t));
 * PUBLIC: int       xtc_svr_call_name __P((xtc_reg_t *, const char *, const void *, size_t, void **, size_t *, int64_t));
 * PUBLIC: int       xtc_reg_members __P((xtc_reg_t *, const char *, int (*)(xtc_pid_t, void *), void *));
 */
XTC_API int  xtc_reg_create(xtc_reg_t **out);
XTC_API void xtc_reg_destroy(xtc_reg_t *r);

/* Register name -> pid.  Fails with XTC_E_INVAL if name already taken. */
XTC_API int  xtc_reg_register(xtc_reg_t *r, const char *name, xtc_pid_t pid);

/* Remove a name.  Returns XTC_E_INVAL if not registered. */
XTC_API int  xtc_reg_unregister(xtc_reg_t *r, const char *name);

/* Look up a pid by name.  Writes to *out_pid on success. */
XTC_API int  xtc_reg_whereis(xtc_reg_t *r, const char *name, xtc_pid_t *out_pid);

XTC_API int  xtc_reg_count(const xtc_reg_t *r);

/* Duplicate-key (pub/sub, group-membership) registration: many pids may
 * share one key.  The substrate for process groups.  Registering the
 * same (key, pid) twice is idempotent. */
XTC_API int  xtc_reg_register_dup(xtc_reg_t *r, const char *key, xtc_pid_t pid);

/* Remove one (key, pid) duplicate-key entry (a group leave). */
XTC_API int  xtc_reg_unregister_pid(xtc_reg_t *r, const char *key, xtc_pid_t pid);

/* Remove `pid` from EVERY key it is registered under (unique names and
 * all duplicate-key groups).  The "process left everything" cleanup an
 * embedder calls when a process exits or a connection closes, until the
 * registry gains an automatic monitor-on-DOWN.  Returns the number of
 * entries removed. */
XTC_API int  xtc_reg_drop_pid(xtc_reg_t *r, xtc_pid_t pid);

/* The crash-aware registry.  Spawn ONE proc with this body and the
 * registry as its argument (xtc_proc_spawn(loop, xtc_reg_reaper, reg,
 * ...)).  It registers itself, then auto-drops any pid registered via
 * xtc_reg_register_mon when that pid goes DOWN -- the automatic form of
 * xtc_reg_drop_pid.  Runs until its loop is torn down. */
XTC_API void xtc_reg_reaper(void *reg);

/* Register `name` -> `pid` (like xtc_reg_register) AND, if a reaper proc
 * is running, arrange for the entry to be auto-dropped when `pid` goes
 * DOWN.  With no reaper it is exactly xtc_reg_register (the caller may
 * still xtc_reg_drop_pid manually). */
XTC_API int  xtc_reg_register_mon(xtc_reg_t *r, const char *name, xtc_pid_t pid);

/* Via-dispatch: look up `name` -> pid, then xtc_svr_call it.  Returns
 * XTC_E_NOTFOUND if the name is not registered, otherwise the result of
 * xtc_svr_call.  Lets a client address a gen_server by registered name
 * instead of holding its pid (the Erlang {via, ...} / global name
 * pattern). */
XTC_API int  xtc_svr_call_name(xtc_reg_t *r, const char *name,
                               const void *req, size_t req_size,
                               void **out_reply, size_t *out_size,
                               int64_t timeout_ns);

/* Visit every pid registered under `key`.  The callback runs under the
 * registry lock (keep it brief; do not re-enter the registry); a nonzero
 * return stops the walk.  Returns the number of members visited. */
XTC_API int  xtc_reg_members(xtc_reg_t *r, const char *key,
                             int (*fn)(xtc_pid_t pid, void *user), void *user);

#endif /* XTC_REG_H */

/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_reg.h
 *	Process registry: name -> xtc_pid_t lookup.
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
#include <stdint.h>

#include "xtc.h"
#include "xtc_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xtc_reg xtc_reg_t;

/* xtc_reg_visit's callback: one entry's pid and value.  Nonzero stops
 * the walk. */
typedef int (*xtc_reg_visit_fn)(xtc_pid_t pid, void *value, void *user);

/*
 * PUBLIC: int       xtc_reg_create __P((xtc_reg_t **));
 * PUBLIC: void      xtc_reg_destroy __P((xtc_reg_t *));
 * PUBLIC: int       xtc_reg_get __P((xtc_reg_t *, const char *, xtc_pid_t *));
 * PUBLIC: int       xtc_reg_del __P((xtc_reg_t *, const char *, xtc_pid_t));
 * PUBLIC: int       xtc_reg_count __P((const xtc_reg_t *));
 * PUBLIC: int       xtc_reg_drop_pid __P((xtc_reg_t *, xtc_pid_t));
 * PUBLIC: void      xtc_reg_reaper __P((void *));
 * PUBLIC: int       xtc_svr_call_name __P((xtc_reg_t *, const char *, const void *, size_t, void **, size_t *, int64_t));
 * PUBLIC: int       xtc_reg_members __P((xtc_reg_t *, const char *, int (*)(xtc_pid_t, void *), void *));
 * PUBLIC: int       xtc_reg_put __P((xtc_reg_t *, const char *, xtc_pid_t, void *, unsigned));
 * PUBLIC: int       xtc_reg_visit __P((xtc_reg_t *, const char *, xtc_reg_visit_fn, void *, int *));
 * PUBLIC: int       xtc_reg_whereis_wait __P((xtc_reg_t *, const char *, xtc_pid_t, int64_t, xtc_pid_t *));
 * PUBLIC: int       xtc_reg_send_wait __P((xtc_reg_t *, const char *, const void *, size_t, int64_t));
 */
XTC_API int  xtc_reg_create(xtc_reg_t **out);
XTC_API void xtc_reg_destroy(xtc_reg_t *r);

/* Look up a pid by name (2.2): for a unique name its holder, for a group
 * key its most recent member (walk a group with xtc_reg_visit).  Writes
 * *out_pid on XTC_OK; XTC_E_NOTFOUND if `name` has no entry, XTC_E_INVAL
 * for a NULL argument.  Under the simulator a lookup may report a
 * transient miss (buggify), as a racing xtc_reg_del could. */
XTC_API int  xtc_reg_get(xtc_reg_t *r, const char *name, xtc_pid_t *out_pid);

/* Remove one entry under `name` (2.2): with pid XTC_PID_NONE its most
 * recent entry (for a unique name, the only one), otherwise the entry
 * for (name, pid), such as one group member.  XTC_E_NOTFOUND if there is
 * none. */
XTC_API int  xtc_reg_del(xtc_reg_t *r, const char *name, xtc_pid_t pid);

XTC_API int  xtc_reg_count(const xtc_reg_t *r);

/* Remove `pid` from EVERY key it is registered under (unique names and
 * all duplicate-key groups).  The "process left everything" cleanup an
 * embedder calls when a process exits or a connection closes, until the
 * registry gains an automatic monitor-on-DOWN.  Returns the number of
 * entries removed. */
XTC_API int  xtc_reg_drop_pid(xtc_reg_t *r, xtc_pid_t pid);

/* The crash-aware registry.  A proc with this body and the registry as
 * its argument registers itself, then auto-drops any pid put with
 * XTC_REG_MONITOR when that pid goes DOWN -- the automatic form of
 * xtc_reg_drop_pid.  xtc_app_start runs one for xtc_app_registry(app)
 * and stops it with the root supervisor.  For a registry of your own,
 * spawn one (xtc_proc_spawn(loop, xtc_reg_reaper, reg, ...)) or make it
 * a supervised child.  It runs until it is killed or receives
 * XTC_APP_SHUTDOWN_MSG; it parks meanwhile, so a plain xtc_loop_run of
 * its loop does not return until one of those happens. */
XTC_API void xtc_reg_reaper(void *reg);

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

/*
 * Adding entries (2.2).
 *
 * xtc_reg_put registers name -> (pid, value).  `value` is opaque and
 * owned by the caller: the registry stores it, hands it to
 * xtc_reg_visit, and never dereferences or frees it.  Flags:
 *   0                  a unique name; XTC_E_EXIST if it is taken
 *   XTC_REG_DUP        a duplicate-key (group) member: many pids may share
 *                      one key; putting the same (key, pid) twice is a
 *                      no-op that keeps the first value
 *   XTC_REG_MONITOR    also drop the entry when pid goes DOWN (needs the
 *                      reaper; with none running it is a plain put).  If
 *                      the enrollment cannot reach the reaper the entry
 *                      is rolled back (XTC_E_AGAIN or XTC_E_RESOURCE).
 *
 * xtc_reg_visit calls fn(pid, value, user) for each entry under `name`
 * WITH THE REGISTRY LOCK HELD.  Remove an entry (xtc_reg_del,
 * xtc_reg_drop_pid) before freeing its value, and
 * the value cannot be freed while fn uses it.  fn must not park and must
 * not call into the registry; xtc_proc_wake and atomics are fine.  A
 * nonzero return stops the walk.  Returns XTC_OK if at least one entry
 * was visited, XTC_E_NOTFOUND if none exists (a miss is authoritative);
 * *out_n, if not NULL, gets the count.  There is deliberately no getter
 * that returns the value: once the lock is dropped the value may be freed.
 */
#define XTC_REG_DUP     0x1u
#define XTC_REG_MONITOR 0x2u

XTC_API int  xtc_reg_put(xtc_reg_t *r, const char *name, xtc_pid_t pid,
                         void *value, unsigned flags);
XTC_API int  xtc_reg_visit(xtc_reg_t *r, const char *name,
                           xtc_reg_visit_fn fn, void *user, int *out_n);

/*
 * Wait for a name (2.2): park until `name` names a LIVE pid other than
 * `not_pid`, then store it in *out_pid.  Woken by the registration (any
 * insert under the name, and a supervisor's replacement of a child's
 * name; see xtc_child_spec_t.reg_name), not by a timer.  The use: a
 * client whose server died waits for the next incarnation, passing the
 * dead pid as not_pid (XTC_PID_NONE: any live holder).  The lookup is
 * exact; a miss is not retried by the caller.  From a proc it parks the
 * fiber (kill-safe); from a plain thread it blocks the thread.
 * timeout_ns: < 0 forever, 0 look once, > 0 bound.  Returns XTC_OK,
 * XTC_E_AGAIN when the time ran out, XTC_E_INVAL for a NULL argument,
 * XTC_E_NOMEM / XTC_E_RESOURCE if the wait could not be set up.
 */
XTC_API int xtc_reg_whereis_wait(xtc_reg_t *r, const char *name,
    xtc_pid_t not_pid, int64_t timeout_ns, xtc_pid_t *out_pid);

/*
 * Send to a name (2.2): xtc_reg_whereis_wait for a live holder, then
 * xtc_send_wait (xtc_proc.h) for room in its mailbox.  A holder that dies
 * before or during the send is skipped and the next one waited for.
 * XTC_OK: the message is in a live incarnation's mailbox (if that one
 * dies before taking it, the message is lost with the mailbox).  One
 * deadline for the whole call.  For a device or I/O thread that reports
 * to a supervised service which may be restarting.  Returns XTC_OK,
 * XTC_E_AGAIN when the time ran out, XTC_E_INVAL for a NULL argument,
 * XTC_E_NOMEM.
 */
XTC_API int xtc_reg_send_wait(xtc_reg_t *r, const char *name,
    const void *data, size_t size, int64_t timeout_ns);

/*
 * Deprecated names (docs/deprecations.md): removed in 3.0.
 */
XTC_DEPRECATED("use xtc_reg_put")
XTC_API int xtc_reg_register(xtc_reg_t *r, const char *name, xtc_pid_t pid);
XTC_DEPRECATED("use xtc_reg_put")
XTC_API int xtc_reg_register_dup(xtc_reg_t *r, const char *key, xtc_pid_t pid);
XTC_DEPRECATED("use xtc_reg_put")
XTC_API int xtc_reg_register_mon(xtc_reg_t *r, const char *name, xtc_pid_t pid);
XTC_DEPRECATED("use xtc_reg_del")
XTC_API int xtc_reg_unregister(xtc_reg_t *r, const char *name);
XTC_DEPRECATED("use xtc_reg_del")
XTC_API int xtc_reg_unregister_pid(xtc_reg_t *r, const char *key,
    xtc_pid_t pid);
XTC_DEPRECATED("use xtc_reg_get")
XTC_API int xtc_reg_whereis(xtc_reg_t *r, const char *name, xtc_pid_t *out_pid);

#ifdef __cplusplus
}
#endif

#endif /* XTC_REG_H */

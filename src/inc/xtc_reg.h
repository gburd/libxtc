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
 * PUBLIC: int       xtc_reg_members __P((xtc_reg_t *, const char *, int (*)(xtc_pid_t, void *), void *));
 */
int  xtc_reg_create(xtc_reg_t **out);
void xtc_reg_destroy(xtc_reg_t *r);

/* Register name -> pid.  Fails with XTC_E_INVAL if name already taken. */
int  xtc_reg_register(xtc_reg_t *r, const char *name, xtc_pid_t pid);

/* Remove a name.  Returns XTC_E_INVAL if not registered. */
int  xtc_reg_unregister(xtc_reg_t *r, const char *name);

/* Look up a pid by name.  Writes to *out_pid on success. */
int  xtc_reg_whereis(xtc_reg_t *r, const char *name, xtc_pid_t *out_pid);

int  xtc_reg_count(const xtc_reg_t *r);

/* Duplicate-key (pub/sub, group-membership) registration: many pids may
 * share one key.  The substrate for process groups.  Registering the
 * same (key, pid) twice is idempotent. */
int  xtc_reg_register_dup(xtc_reg_t *r, const char *key, xtc_pid_t pid);

/* Remove one (key, pid) duplicate-key entry (a group leave). */
int  xtc_reg_unregister_pid(xtc_reg_t *r, const char *key, xtc_pid_t pid);

/* Visit every pid registered under `key`.  The callback runs under the
 * registry lock (keep it brief; do not re-enter the registry); a nonzero
 * return stops the walk.  Returns the number of members visited. */
int  xtc_reg_members(xtc_reg_t *r, const char *key,
                     int (*fn)(xtc_pid_t pid, void *user), void *user);

#endif /* XTC_REG_H */

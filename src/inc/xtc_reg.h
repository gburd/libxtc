/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
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

#endif /* XTC_REG_H */

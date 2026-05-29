/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/broker.h -- broker process entry points.
 */

#ifndef KAKA_BROKER_H
#define KAKA_BROKER_H

#include "xtc_loop.h"

/* Record the loop the partition procs are spawned on.  Call once at
 * startup before any connection is accepted. */
void broker_set_loop(xtc_loop_t *loop);

/* Spawn a connection proc to service an accepted socket fd.  The proc
 * owns the fd and closes it on exit. */
int  broker_spawn_conn(xtc_loop_t *loop, int fd);

/* In-process PRODUCE/FETCH self-test; returns 0 on pass. */
int  broker_selftest(void);

#endif /* KAKA_BROKER_H */

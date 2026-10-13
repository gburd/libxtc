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

#include <stddef.h>
#include <stdint.h>

#include "xtc_loop.h"

/* ---- configuration (call before broker_start) ---- */

/* Loop the partition and coordinator procs are spawned on. */
void broker_set_loop(xtc_loop_t *loop);

/* Log directory.  Non-NULL: each partition persists under
 * <dir>/<topic>-<partition>/ and consumer-group offsets under
 * <dir>/__offsets/.  NULL: everything is in memory. */
void broker_set_log_dir(const char *dir);

/* Durability.  0 (the default): a PRODUCE is acked only after its
 * records are fdatasync'ed (group commit -- one sync covers every
 * request queued behind it).  > 0: ack after write(2) and sync at most
 * every `ns`, on segment roll, and at shutdown; a power failure or
 * kernel crash can lose up to that window of acked records (a broker
 * crash loses none). */
void broker_set_fsync_interval(int64_t ns);

/* Segment roll size (0 = 1 MiB) and size-based retention per partition
 * (0 = keep everything). */
void broker_set_segment_bytes(size_t bytes);
void broker_set_retention_bytes(uint64_t bytes);

/* How long a connection waits for a partition or the offset store
 * before answering KAKA_ERR_TIMEOUT (default 30 s). */
void broker_set_request_timeout(int64_t ns);

/* Spawn the consumer-group coordinator and size the connection table
 * (max_conns connections; further ones are refused).  Returns 0. */
int  broker_start(xtc_loop_t *loop, int max_conns);

/* Spawn a connection proc for an accepted socket (from xtc_net_accept);
 * the proc owns the fd and closes it with xtc_net_close.  -1 (and the
 * caller closes fd) when the connection table is full. */
int  broker_spawn_conn(xtc_loop_t *loop, int fd);

/*
 * Graceful shutdown; call from a proc on the broker loop after the
 * listener stopped accepting.  Stops reading new requests (in-flight
 * ones finish and are answered), waits up to grace_ns for connections
 * to close, then cuts the rest, then shuts down every partition (each
 * syncs its log) and the offset store.  Returns once they are gone or
 * after a bounded wait.
 */
void broker_shutdown(int64_t grace_ns);

/* ---- in-process self-tests (no socket); each returns 0 on pass ---- */

int  broker_selftest(void);
int  broker_credit_selftest(int *max_in_flight, uint64_t *hwm);
int  broker_metrics_selftest(void);
/* Acks wait for fdatasync; a failed sync fails the produce and every
 * later one (fail-stop) while synced data stays readable. */
int  broker_durable_selftest(const char *dir);
/* The memory budget tracks live RAM, not lifetime bytes. */
int  broker_budget_selftest(const char *dir);
/* A request that outlives the connection's timeout owns its payload. */
int  broker_timeout_selftest(const char *dir);

#endif /* KAKA_BROKER_H */

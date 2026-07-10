/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_credit.h
 *	A sliding-window credit regulator: cap the number of operations a
 *	producer keeps IN FLIGHT (outstanding, un-acknowledged) at once.
 *	Take a credit before issuing an operation, return it when the
 *	operation's acknowledgement comes back; when credits are exhausted
 *	the producer blocks (as a fiber) until an ack frees one.
 *
 *	This is the credit-based flow-control pattern, transport-agnostic:
 *	it works over a request/reply RPC to another process, over socket
 *	writes, over disk operations -- anywhere a fast issuer can outrun a
 *	slower acknowledger.  The demand channel (xtc_chan_demand) is its
 *	producer/consumer-channel sibling; this regulator is for the
 *	request-issue/ack-return sliding window.
 *
 *	Precedent: Erlang's :jobs regulator, RabbitMQ credit_flow, and
 *	GenStage's max_demand/min_demand all express exactly this window.
 *	OTP has no single named behaviour for it -- it is a gen_server plus
 *	an integer counter -- which is what this regulator packages, with
 *	peak-in-flight observability the hand-rolled version drops.
 *
 *	Built on xtc_sem (the credits ARE semaphore units), so a producer
 *	running inside a fiber parks its fiber (not the OS thread) while
 *	waiting for a credit.
 */

#ifndef XTC_CREDIT_H
#define XTC_CREDIT_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_credit xtc_credit_t;

/*
 * PUBLIC: int    xtc_credit_create __P((unsigned, xtc_credit_t **));
 * PUBLIC: void   xtc_credit_destroy __P((xtc_credit_t *));
 * PUBLIC: int    xtc_credit_acquire __P((xtc_credit_t *, int64_t));
 * PUBLIC: int    xtc_credit_try_acquire __P((xtc_credit_t *));
 * PUBLIC: int    xtc_credit_release __P((xtc_credit_t *));
 * PUBLIC: unsigned xtc_credit_in_flight __P((const xtc_credit_t *));
 * PUBLIC: unsigned xtc_credit_peak __P((const xtc_credit_t *));
 * PUBLIC: unsigned xtc_credit_window __P((const xtc_credit_t *));
 */

/* Create a regulator with a window of `window` credits (max operations
 * in flight).  window must be > 0. */
int  xtc_credit_create(unsigned window, xtc_credit_t **out);
void xtc_credit_destroy(xtc_credit_t *c);

/* Take one credit before issuing an operation, blocking the calling
 * fiber until one is free or `timeout_ns` elapses (XTC_E_AGAIN).
 * timeout_ns == 0 tries once; a negative timeout blocks indefinitely. */
int  xtc_credit_acquire(xtc_credit_t *c, int64_t timeout_ns);

/* Take one credit if immediately available (XTC_E_AGAIN otherwise). */
int  xtc_credit_try_acquire(xtc_credit_t *c);

/* Return one credit when an operation's acknowledgement arrives,
 * unblocking one waiter.  XTC_E_INVAL if it would exceed the window
 * (a release without a matching acquire is a bug in the caller). */
int  xtc_credit_release(xtc_credit_t *c);

/* Operations currently in flight (acquired but not yet released). */
unsigned xtc_credit_in_flight(const xtc_credit_t *c);

/* Peak in-flight observed since creation (the high-water mark; useful
 * for tuning the window). */
unsigned xtc_credit_peak(const xtc_credit_t *c);

/* The configured window size. */
unsigned xtc_credit_window(const xtc_credit_t *c);

#endif /* XTC_CREDIT_H */

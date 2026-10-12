/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/future_int.h
 *	Internal (NOT public) future helper used by the async batch engine
 *	to own the completion token's lifetime across teardown. See
 *	src/ptc/future.c for the contract and src/ptc/batch.c for the one
 *	consumer.
 */
#ifndef XTC_FUTURE_INT_H
#define XTC_FUTURE_INT_H

#include "xtc_future.h"

/*
 * Consume a future WITHOUT waiting: release the read half's reference
 * and free the handle, exactly as a completed xtc_future_wait would,
 * but without blocking. Safe whether the paired promise is already set,
 * never set (still outstanding), or dropped -- the cell is reference
 * counted, so a later xtc_promise_set / xtc_promise_drop is a safe
 * refcount operation on the (possibly last-ref-freed) cell, never a
 * UAF. The handle must not be used after this (it is freed). NULL is a
 * no-op.
 */
void __xtc_future_discard(xtc_future_t *fut);

#endif /* XTC_FUTURE_INT_H */

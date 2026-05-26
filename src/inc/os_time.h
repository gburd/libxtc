/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/os_time.h
 *	Monotonic and wall-clock time, plus a sleep primitive.
 *	Times are nanoseconds in int64_t.  See M1_CLAIMS.md, Tm1–Tm4.
 */

#ifndef XTC_OS_TIME_H
#define XTC_OS_TIME_H

#include <stdint.h>

#define XTC_NS_PER_SEC  1000000000LL
#define XTC_NS_PER_MS   1000000LL
#define XTC_NS_PER_US   1000LL

/*
 * PUBLIC: int __os_clock_mono __P((int64_t *));
 * PUBLIC: int __os_clock_real __P((int64_t *));
 * PUBLIC: int __os_sleep_ns __P((int64_t));
 */
int __os_clock_mono(int64_t *out_ns);
int __os_clock_real(int64_t *out_ns);
int __os_sleep_ns(int64_t ns);

#endif /* XTC_OS_TIME_H */

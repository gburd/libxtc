/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_time.c
 *	POSIX clock_gettime + nanosleep.  Windows path lands later
 *	using QueryPerformanceCounter / GetSystemTimeAsFileTime.
 */

#define _POSIX_C_SOURCE 200809L

#include "xtc_int.h"

#include <errno.h>
#include <time.h>

static int
__ts_to_ns(const struct timespec *ts, int64_t *out)
{
	int64_t ns;

	/* Reject obviously bogus timestamps. */
	if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
		return XTC_E_RANGE;

	/* Detect overflow on 64-bit ns conversion. */
	if (ts->tv_sec > (int64_t)((INT64_MAX - ts->tv_nsec) / XTC_NS_PER_SEC))
		return XTC_E_RANGE;

	ns = (int64_t)ts->tv_sec * XTC_NS_PER_SEC + (int64_t)ts->tv_nsec;
	*out = ns;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_clock_mono __P((int64_t *));
 */
int
__os_clock_mono(int64_t *out)
{
	struct timespec ts;
	if (out == NULL)
		return XTC_E_INVAL;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return XTC_E_INTERNAL;
	return __ts_to_ns(&ts, out);
}

/*
 * PUBLIC: int __os_clock_real __P((int64_t *));
 */
int
__os_clock_real(int64_t *out)
{
	struct timespec ts;
	if (out == NULL)
		return XTC_E_INVAL;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return XTC_E_INTERNAL;
	return __ts_to_ns(&ts, out);
}

/*
 * PUBLIC: int __os_sleep_ns __P((int64_t));
 */
int
__os_sleep_ns(int64_t ns)
{
	struct timespec ts, rem;
	if (ns < 0)
		return XTC_E_INVAL;
	ts.tv_sec  = (time_t)(ns / XTC_NS_PER_SEC);
	ts.tv_nsec = (long)(ns % XTC_NS_PER_SEC);
	while (nanosleep(&ts, &rem) == -1) {
		if (errno == EINTR) {
			ts = rem;
			continue;
		}
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

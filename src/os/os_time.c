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

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/*
 * PUBLIC: int __os_clock_mono __P((int64_t *));
 *
 * Monotonic clock from QueryPerformanceCounter, scaled to ns.
 */
int
__os_clock_mono(int64_t *out)
{
	LARGE_INTEGER c, f;
	if (out == NULL)
		return XTC_E_INVAL;
	if (!QueryPerformanceCounter(&c) || !QueryPerformanceFrequency(&f)
	    || f.QuadPart == 0)
		return XTC_E_INTERNAL;
	/* ns = counter * 1e9 / freq, computed to avoid overflow:
	 * split into whole seconds plus fractional ns. */
	{
		int64_t sec  = c.QuadPart / f.QuadPart;
		int64_t rem  = c.QuadPart % f.QuadPart;
		*out = sec * XTC_NS_PER_SEC
		     + (rem * XTC_NS_PER_SEC) / f.QuadPart;
	}
	return XTC_OK;
}

/*
 * PUBLIC: int __os_clock_real __P((int64_t *));
 *
 * Wall clock from GetSystemTimePreciseAsFileTime.  FILETIME is
 * 100-ns ticks since 1601-01-01; convert to ns since the Unix epoch.
 */
int
__os_clock_real(int64_t *out)
{
	FILETIME ft;
	ULARGE_INTEGER u;
	/* 100-ns intervals between 1601-01-01 and 1970-01-01. */
	const int64_t EPOCH_DELTA_100NS = 116444736000000000LL;
	if (out == NULL)
		return XTC_E_INVAL;
	GetSystemTimePreciseAsFileTime(&ft);
	u.LowPart  = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	*out = (int64_t)((u.QuadPart - EPOCH_DELTA_100NS) * 100);
	return XTC_OK;
}

/*
 * PUBLIC: int __os_sleep_ns __P((int64_t));
 *
 * Sleep with millisecond granularity (Sleep) for the bulk, then a
 * brief spin for the sub-millisecond remainder so short sleeps are
 * not rounded up to a full tick.
 */
int
__os_sleep_ns(int64_t ns)
{
	int64_t ms;
	if (ns < 0)
		return XTC_E_INVAL;
	ms = ns / 1000000;
	if (ms > 0)
		Sleep((DWORD)ms);
	else if (ns > 0)
		SwitchToThread();   /* sub-ms: yield rather than busy-burn */
	return XTC_OK;
}

#else  /* POSIX */

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

#endif  /* _WIN32 vs POSIX */

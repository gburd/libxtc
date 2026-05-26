/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 *
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/xtc_strerror.c
 *	Stable English descriptions of XTC_E_* codes.
 *	The text is part of the API contract for log-message
 *	stability (PLAN.md §18.7); changes follow the same
 *	deprecation lifecycle as function signatures.
 */

#include <stddef.h>

#include "xtc.h"

/*
 * PUBLIC: const char *xtc_strerror __P((int));
 */
const char *
xtc_strerror(int e)
{
	switch ((xtc_err_t)e) {
	case XTC_OK:		return "ok";
	case XTC_E_INVAL:	return "invalid argument";
	case XTC_E_NOMEM:	return "out of memory";
	case XTC_E_NOSYS:	return "not implemented on this platform";
	case XTC_E_RANGE:	return "numeric out of range";
	case XTC_E_AGAIN:	return "resource temporarily unavailable";
	case XTC_E_INTERNAL:	return "internal invariant violation";
	case XTC_E_RESOURCE:	return "resource cap reached";
	case XTC_E_DEADLK:	return "deadlock victim";
	}
	return "unknown";
}

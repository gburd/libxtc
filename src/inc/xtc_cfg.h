/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_cfg.h
 *	Typed runtime-configurable settings registry — the xtc
 *	equivalent of PostgreSQL's GUC framework.  Each variable has:
 *	  - a name (string key)
 *	  - a type (int, int64, double, bool, string, enum)
 *	  - a default value
 *	  - optional min/max bounds
 *	  - an optional validator callback
 *	  - an optional change callback
 *
 *	Use cases:
 *	  - Tunable knobs that ops can change at runtime without
 *	    restart (e.g. log level, backpressure thresholds).
 *	  - Configuration discovery (a "show all" surface).
 *	  - Test-time fault injection (set a knob in a test, restore
 *	    in cleanup).
 *
 *	Storage model:
 *	  - Single global registry keyed by name.
 *	  - Mutex-protected list (linear scan; suitable for ~hundreds
 *	    of vars; M11.5 swaps in xtc_chash for thousands).
 *	  - Each var holds its declared type + current value via union.
 *
 *	v1 omits: per-session/per-database scoping (PG-specific),
 *	configuration-file parsing (postgresql.conf reader is M16
 *	work), SIGHUP-driven reload (signal integration is separate).
 */

#ifndef XTC_CFG_H
#define XTC_CFG_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef enum xtc_cfg_kind {
	XTC_CFG_BOOL   = 1,
	XTC_CFG_INT    = 2,
	XTC_CFG_INT64  = 3,
	XTC_CFG_DOUBLE = 4,
	XTC_CFG_STRING = 5,
	XTC_CFG_ENUM   = 6
} xtc_cfg_kind_t;

typedef int (*xtc_cfg_validator_fn)(const void *new_val, void *user);
typedef void (*xtc_cfg_changed_fn)(const char *name, const void *old_val,
                                   const void *new_val, void *user);

/* Spec used at registration time. */
typedef struct xtc_cfg_spec {
	const char         *name;
	const char         *short_desc;       /* ops-friendly description */
	xtc_cfg_kind_t      kind;

	/* Default value (interpreted per kind). */
	union {
		int       d_bool;             /* 0/1 */
		int       d_int;
		int64_t   d_int64;
		double    d_double;
		const char *d_string;
		int        d_enum;
	} dflt;

	/* Bounds for numeric types (inclusive); 0/0 means unbounded. */
	int64_t  min_int;
	int64_t  max_int;
	double   min_double;
	double   max_double;

	/* For ENUM: NULL-terminated array of allowed string labels;
	 * the int value is the index into this array. */
	const char *const *enum_labels;
	int                n_enum_labels;

	/* Optional callbacks. */
	xtc_cfg_validator_fn  validator;
	xtc_cfg_changed_fn    on_change;
	void                 *cb_user;
} xtc_cfg_spec_t;

/*
 * PUBLIC: int  xtc_cfg_register __P((const xtc_cfg_spec_t *));
 * PUBLIC: int  xtc_cfg_unregister __P((const char *));
 *
 * PUBLIC: int  xtc_cfg_get_bool __P((const char *, int *));
 * PUBLIC: int  xtc_cfg_get_int __P((const char *, int *));
 * PUBLIC: int  xtc_cfg_get_int64 __P((const char *, int64_t *));
 * PUBLIC: int  xtc_cfg_get_double __P((const char *, double *));
 * PUBLIC: int  xtc_cfg_get_string __P((const char *, const char **));
 * PUBLIC: int  xtc_cfg_get_enum __P((const char *, int *));
 *
 * PUBLIC: int  xtc_cfg_set_bool __P((const char *, int));
 * PUBLIC: int  xtc_cfg_set_int __P((const char *, int));
 * PUBLIC: int  xtc_cfg_set_int64 __P((const char *, int64_t));
 * PUBLIC: int  xtc_cfg_set_double __P((const char *, double));
 * PUBLIC: int  xtc_cfg_set_string __P((const char *, const char *));
 * PUBLIC: int  xtc_cfg_set_enum __P((const char *, int));
 *
 * PUBLIC: int  xtc_cfg_count __P((void));
 * PUBLIC: int  xtc_cfg_kind __P((const char *, xtc_cfg_kind_t *));
 */

int  xtc_cfg_register(const xtc_cfg_spec_t *spec);
int  xtc_cfg_unregister(const char *name);

int  xtc_cfg_get_bool(const char *name, int *out);
int  xtc_cfg_get_int(const char *name, int *out);
int  xtc_cfg_get_int64(const char *name, int64_t *out);
int  xtc_cfg_get_double(const char *name, double *out);
int  xtc_cfg_get_string(const char *name, const char **out);
int  xtc_cfg_get_enum(const char *name, int *out);

int  xtc_cfg_set_bool(const char *name, int v);
int  xtc_cfg_set_int(const char *name, int v);
int  xtc_cfg_set_int64(const char *name, int64_t v);
int  xtc_cfg_set_double(const char *name, double v);
int  xtc_cfg_set_string(const char *name, const char *v);
int  xtc_cfg_set_enum(const char *name, int v);

int  xtc_cfg_count(void);
int  xtc_cfg_kind(const char *name, xtc_cfg_kind_t *out);

#endif /* XTC_CFG_H */

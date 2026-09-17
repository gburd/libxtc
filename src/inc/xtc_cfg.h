/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_cfg.h
 *	Typed runtime-configurable settings registry -- named, typed,
 *	bounds-checked tunables resolved at run time.  Each variable has:
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
 *	Not yet implemented: per-session/per-database scoping (an
 *	override-stack model that needs the M16 session layer).
 *	Configuration-file parsing (xtc_cfg_load_file) and SIGHUP-driven
 *	reload (xtc_cfg_reload) are done.  See docs/KNOWN_ISSUES.md for
 *	tracking.
 *
 *	Hot-path reads:
 *	  - The name-keyed xtc_cfg_get_* do a registry lookup per call;
 *	    that is fine for cold/occasional reads but too slow for a knob
 *	    read on every operation.  For hot paths, resolve the variable
 *	    ONCE to an opaque handle with xtc_cfg_ref() and then read
 *	    through xtc_cfg_ref_get_* -- no name lookup, no scan.  The
 *	    handle stays valid until the variable is unregistered (a
 *	    registry entry is never relocated), so it can be cached for
 *	    the process lifetime like a compiled-in pointer.
 */

#ifndef XTC_CFG_H
#define XTC_CFG_H

#include "xtc_export.h"

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

/* Opaque, pointer-stable handle to a registered variable, for hot-path
 * reads that must avoid a per-read name lookup.  Obtain with
 * xtc_cfg_ref(); valid until the variable is unregistered. */
typedef struct xtc_cfg_var *xtc_cfg_ref_t;

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
 * PUBLIC: int  xtc_cfg_load_file __P((const char *));
 * PUBLIC: int  xtc_cfg_reload __P((void));
 *
 * PUBLIC: int  xtc_cfg_ref __P((const char *, xtc_cfg_ref_t *));
 * PUBLIC: int  xtc_cfg_ref_get_bool __P((xtc_cfg_ref_t, int *));
 * PUBLIC: int  xtc_cfg_ref_get_int __P((xtc_cfg_ref_t, int *));
 * PUBLIC: int  xtc_cfg_ref_get_int64 __P((xtc_cfg_ref_t, int64_t *));
 * PUBLIC: int  xtc_cfg_ref_get_double __P((xtc_cfg_ref_t, double *));
 * PUBLIC: int  xtc_cfg_ref_get_string __P((xtc_cfg_ref_t, const char **));
 * PUBLIC: int  xtc_cfg_ref_get_enum __P((xtc_cfg_ref_t, int *));
 */

XTC_API int  xtc_cfg_register(const xtc_cfg_spec_t *spec);
XTC_API int  xtc_cfg_unregister(const char *name);

XTC_API int  xtc_cfg_get_bool(const char *name, int *out);
XTC_API int  xtc_cfg_get_int(const char *name, int *out);
XTC_API int  xtc_cfg_get_int64(const char *name, int64_t *out);
XTC_API int  xtc_cfg_get_double(const char *name, double *out);
XTC_API int  xtc_cfg_get_string(const char *name, const char **out);
XTC_API int  xtc_cfg_get_enum(const char *name, int *out);

XTC_API int  xtc_cfg_set_bool(const char *name, int v);
XTC_API int  xtc_cfg_set_int(const char *name, int v);
XTC_API int  xtc_cfg_set_int64(const char *name, int64_t v);
XTC_API int  xtc_cfg_set_double(const char *name, double v);
XTC_API int  xtc_cfg_set_string(const char *name, const char *v);
XTC_API int  xtc_cfg_set_enum(const char *name, int v);

XTC_API int  xtc_cfg_count(void);
XTC_API int  xtc_cfg_kind(const char *name, xtc_cfg_kind_t *out);

/* Load a postgresql.conf-style `name = value` file: per-line, with `#`
 * comments and optional quotes; each value parsed per the variable's
 * registered kind and applied via xtc_cfg_set_* (bounds/validators
 * apply).  Unknown names and bad values are skipped.  Returns the
 * count applied (>= 0), XTC_E_INVAL (NULL path), or XTC_E_IO (open
 * failed).  Remembers the path for xtc_cfg_reload. */
XTC_API int  xtc_cfg_load_file(const char *path);

/* Re-read the file last loaded by xtc_cfg_load_file.  Meant to back a
 * SIGHUP handler, but is NOT async-signal-safe (uses stdio and the
 * registry lock): set a flag in the handler and call this from the
 * event loop.  Returns the applied count, XTC_E_INVAL (no file loaded),
 * or XTC_E_IO. */
XTC_API int  xtc_cfg_reload(void);

/*
 * Hot-path read handles.  xtc_cfg_ref() resolves a name to an opaque,
 * pointer-stable handle ONCE (the only name lookup); thereafter
 * xtc_cfg_ref_get_* read the current value with no lookup and no scan.
 * The handle stays valid until the variable is unregistered.  Reads
 * through a handle still observe live xtc_cfg_set_* updates.  The
 * kind must match (a _get_int on a non-INT handle returns XTC_E_INVAL),
 * exactly like the name-keyed getters.  xtc_cfg_ref returns XTC_E_INVAL
 * on NULL args and XTC_E_NOTFOUND for an unregistered name.
 */
XTC_API int  xtc_cfg_ref(const char *name, xtc_cfg_ref_t *out);
XTC_API int  xtc_cfg_ref_get_bool(xtc_cfg_ref_t ref, int *out);
XTC_API int  xtc_cfg_ref_get_int(xtc_cfg_ref_t ref, int *out);
XTC_API int  xtc_cfg_ref_get_int64(xtc_cfg_ref_t ref, int64_t *out);
XTC_API int  xtc_cfg_ref_get_double(xtc_cfg_ref_t ref, double *out);
XTC_API int  xtc_cfg_ref_get_string(xtc_cfg_ref_t ref, const char **out);
XTC_API int  xtc_cfg_ref_get_enum(xtc_cfg_ref_t ref, int *out);

/* ---- per-session scoping + a transactional override stack ----
 *
 * A single global value per name is what a database server CANNOT use
 * for most settings: `SET work_mem` must affect one session and no
 * other.  A session holds per-variable OVERRIDES layered over the
 * global registry, so a read resolves
 *
 *     session override (newest wins)  ->  global current value
 *
 * and the global value is never touched by a session set.  The name-
 * keyed and ref getters above resolve through the CURRENT session
 * (xtc_cfg_session_bind) when one is bound, and fall straight through
 * to the global value when none is -- so existing single-config code is
 * unchanged, and a bare `xtc_cfg_get_int("work_mem", &v)` in a bound
 * session automatically sees that session's value.  This is the shape
 * PostgreSQL reads GUCs with (bare names in thousands of places), with
 * an explicit-scope variant for admin paths that must reach across
 * sessions.
 *
 * TRANSACTIONAL OVERRIDE STACK.  Within a session each variable's
 * overrides form a stack of LEVELS, opened by xtc_cfg_session_push and
 * closed by _commit (collapse into the parent -- the values set at this
 * level survive) or _abort (discard -- the values revert to what the
 * parent saw).  Nestable to any depth.  This backs SET LOCAL (a value
 * scoped to the current transaction), SET inside a transaction that
 * later ROLLBACKs, function-local SET (proconfig), and subtransactions.
 *
 * SOURCE PRECEDENCE.  Each set carries a source rank (xtc_cfg_source_t).
 * A set is applied only if its rank is >= the rank that last set the
 * value at the current level, and the winning source is reported by
 * xtc_cfg_session_source -- mirroring PostgreSQL's
 * PGC_S_DEFAULT < file < database < user < client < session <
 * PGC_S_OVERRIDE ordering and pg_settings.source.
 *
 * Thread/fiber model: a session is owned by one fiber at a time (bind
 * it on entry, unbind on exit).  The overrides are not shared, so no
 * lock guards them; only the fallback read of the global value takes
 * the registry lock, exactly as the unscoped getters do.
 */
typedef struct xtc_cfg_session xtc_cfg_session_t;

/* Source ranks, low to high precedence.  A higher rank overrides a
 * lower one at the same stack level; an equal-or-higher rank is
 * required to replace an existing value. */
typedef enum xtc_cfg_source {
	XTC_CFG_SRC_DEFAULT  = 0,   /* compiled-in default */
	XTC_CFG_SRC_FILE     = 1,   /* config file */
	XTC_CFG_SRC_DATABASE = 2,   /* per-database default */
	XTC_CFG_SRC_USER     = 3,   /* per-role default */
	XTC_CFG_SRC_CLIENT   = 4,   /* connection string / startup packet */
	XTC_CFG_SRC_SESSION  = 5,   /* interactive SET */
	XTC_CFG_SRC_OVERRIDE = 6    /* forced; beats everything */
} xtc_cfg_source_t;

/*
 * PUBLIC: int  xtc_cfg_session_create __P((xtc_cfg_session_t **));
 * PUBLIC: void xtc_cfg_session_destroy __P((xtc_cfg_session_t *));
 * PUBLIC: int  xtc_cfg_session_bind __P((xtc_cfg_session_t *));
 * PUBLIC: xtc_cfg_session_t *xtc_cfg_session_current __P((void));
 * PUBLIC: int  xtc_cfg_session_push __P((xtc_cfg_session_t *));
 * PUBLIC: int  xtc_cfg_session_commit __P((xtc_cfg_session_t *));
 * PUBLIC: int  xtc_cfg_session_abort __P((xtc_cfg_session_t *));
 * PUBLIC: int  xtc_cfg_session_source __P((xtc_cfg_session_t *, const char *, xtc_cfg_source_t *));
 * PUBLIC: int  xtc_cfg_session_reset __P((xtc_cfg_session_t *, const char *));
 * PUBLIC: int  xtc_cfg_ssn_set_bool __P((xtc_cfg_session_t *, const char *, int, xtc_cfg_source_t));
 * PUBLIC: int  xtc_cfg_ssn_set_int __P((xtc_cfg_session_t *, const char *, int, xtc_cfg_source_t));
 * PUBLIC: int  xtc_cfg_ssn_set_int64 __P((xtc_cfg_session_t *, const char *, int64_t, xtc_cfg_source_t));
 * PUBLIC: int  xtc_cfg_ssn_set_double __P((xtc_cfg_session_t *, const char *, double, xtc_cfg_source_t));
 * PUBLIC: int  xtc_cfg_ssn_set_string __P((xtc_cfg_session_t *, const char *, const char *, xtc_cfg_source_t));
 * PUBLIC: int  xtc_cfg_ssn_set_enum __P((xtc_cfg_session_t *, const char *, int, xtc_cfg_source_t));
 */

/* Lifecycle.  A fresh session has no overrides and one (base) level.
 * destroy frees all levels and any string overrides; NULL-safe. */
XTC_API int  xtc_cfg_session_create(xtc_cfg_session_t **out);
XTC_API void xtc_cfg_session_destroy(xtc_cfg_session_t *s);

/* Bind `s` (or NULL to unbind) as the current session for the calling
 * fiber, so the unscoped xtc_cfg_get_* / ref getters resolve through
 * it.  Returns the previously bound session (or NULL) so a caller can
 * save/restore.  A session is bound to one fiber; do not share a bound
 * session across fibers concurrently. */
XTC_API xtc_cfg_session_t *xtc_cfg_session_bind(xtc_cfg_session_t *s);
XTC_API xtc_cfg_session_t *xtc_cfg_session_current(void);

/* Transactional levels.  push opens a new (innermost) level; set within
 * it, then commit to fold those values into the parent, or abort to
 * discard them.  commit/abort of the base level is XTC_E_INVAL -- there
 * is always one level.  Nestable. */
XTC_API int  xtc_cfg_session_push(xtc_cfg_session_t *s);
XTC_API int  xtc_cfg_session_commit(xtc_cfg_session_t *s);
XTC_API int  xtc_cfg_session_abort(xtc_cfg_session_t *s);

/* The source that set the effective session value for `name`, or
 * XTC_CFG_SRC_DEFAULT when the session has no override (the global
 * value wins).  XTC_E_NOTFOUND for an unregistered name. */
XTC_API int  xtc_cfg_session_source(xtc_cfg_session_t *s, const char *name,
                                    xtc_cfg_source_t *out);

/* Drop `name`'s override at the current level (RESET): the value
 * reverts to what the parent level / the global value provides.
 * XTC_OK whether or not an override existed. */
XTC_API int  xtc_cfg_session_reset(xtc_cfg_session_t *s, const char *name);

/* Set a per-session override at the current level.  The value is
 * validated against the variable's registered kind/bounds/validator
 * exactly like the global setters, and applied only if `src` outranks
 * the source that last set it at this level.  A NULL session targets
 * the fiber's currently-bound session; XTC_E_INVAL if none is bound. */
XTC_API int  xtc_cfg_ssn_set_bool(xtc_cfg_session_t *s, const char *name,
                                  int v, xtc_cfg_source_t src);
XTC_API int  xtc_cfg_ssn_set_int(xtc_cfg_session_t *s, const char *name,
                                 int v, xtc_cfg_source_t src);
XTC_API int  xtc_cfg_ssn_set_int64(xtc_cfg_session_t *s, const char *name,
                                   int64_t v, xtc_cfg_source_t src);
XTC_API int  xtc_cfg_ssn_set_double(xtc_cfg_session_t *s, const char *name,
                                    double v, xtc_cfg_source_t src);
XTC_API int  xtc_cfg_ssn_set_string(xtc_cfg_session_t *s, const char *name,
                                    const char *v, xtc_cfg_source_t src);
XTC_API int  xtc_cfg_ssn_set_enum(xtc_cfg_session_t *s, const char *name,
                                  int v, xtc_cfg_source_t src);

#endif /* XTC_CFG_H */

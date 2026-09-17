/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/cfg.c
 *	GUC-style typed configuration registry.
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_cfg.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif
#include <stdio.h>
#include <errno.h>

struct xtc_cfg_var {
	char           *name;
	char           *desc;
	xtc_cfg_kind_t  kind;
	union {
		int      v_bool;
		int      v_int;
		int64_t  v_int64;
		double   v_double;
		char    *v_string;
		int      v_enum;
	} cur;
	int64_t  min_int;
	int64_t  max_int;
	double   min_double;
	double   max_double;
	const char *const *enum_labels;
	int                n_enum_labels;
	xtc_cfg_validator_fn  validator;
	xtc_cfg_changed_fn    on_change;
	void                 *cb_user;
	struct xtc_cfg_var *next;
};
#define cfg_var xtc_cfg_var   /* keep the terse internal spelling below */

/* Value carrier for session overrides (same shape as cfg_var.cur);
 * declared here because the session-aware getters below reference it. */
union cfg_val {
	int      v_bool;
	int      v_int;
	int64_t  v_int64;
	double   v_double;
	char    *v_string;   /* owned */
	int      v_enum;
};

/* Resolve a var through the bound session (defined with the session
 * layer below); 1 if an override supplied the value, 0 to fall through
 * to the global.  Called with the registry lock held. */
static int __cfg_ssn_resolve(struct xtc_cfg_var *var, union cfg_val *out);

static pthread_mutex_t __cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cfg_var *__cfg_head;
static int             __cfg_count;

static struct cfg_var *
__cfg_find_locked(const char *name)
{
	struct cfg_var *v;
	for (v = __cfg_head; v != NULL; v = v->next)
		if (strcmp(v->name, name) == 0) return v;
	return NULL;
}

static int
__bounds_int_ok(struct cfg_var *v, int64_t n)
{
	if (v->min_int == 0 && v->max_int == 0) return 1;
	return n >= v->min_int && n <= v->max_int;
}

static int
__bounds_dbl_ok(struct cfg_var *v, double n)
{
	if (v->min_double == 0 && v->max_double == 0) return 1;
	return n >= v->min_double && n <= v->max_double;
}

int
xtc_cfg_register(const xtc_cfg_spec_t *spec)
{
	struct cfg_var *v;
	int rc;
	if (spec == NULL || spec->name == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *v, (void **)&v)) != XTC_OK) return rc;
	if ((rc = __os_strdup(spec->name, &v->name)) != XTC_OK) {
		__os_free(v); return rc;
	}
	if (spec->short_desc != NULL)
		(void)__os_strdup(spec->short_desc, &v->desc);
	v->kind = spec->kind;
	v->min_int = spec->min_int;
	v->max_int = spec->max_int;
	v->min_double = spec->min_double;
	v->max_double = spec->max_double;
	v->enum_labels = spec->enum_labels;
	v->n_enum_labels = spec->n_enum_labels;
	v->validator = spec->validator;
	v->on_change = spec->on_change;
	v->cb_user   = spec->cb_user;
	switch (spec->kind) {
	case XTC_CFG_BOOL:   v->cur.v_bool   = spec->dflt.d_bool;   break;
	case XTC_CFG_INT:    v->cur.v_int    = spec->dflt.d_int;    break;
	case XTC_CFG_INT64:  v->cur.v_int64  = spec->dflt.d_int64;  break;
	case XTC_CFG_DOUBLE: v->cur.v_double = spec->dflt.d_double; break;
	case XTC_CFG_STRING:
		if (spec->dflt.d_string)
			(void)__os_strdup(spec->dflt.d_string, &v->cur.v_string);
		break;
	case XTC_CFG_ENUM:   v->cur.v_enum   = spec->dflt.d_enum;   break;
	}

	(void)__xtc_mtx_lock(&__cfg_lock);
	if (__cfg_find_locked(spec->name) != NULL) {
		(void)__xtc_mtx_unlock(&__cfg_lock);
		__os_free(v->name); __os_free(v->desc);
		if (v->kind == XTC_CFG_STRING && v->cur.v_string)
			__os_free(v->cur.v_string);
		__os_free(v);
		return XTC_E_INVAL;
	}
	v->next = __cfg_head;
	__cfg_head = v;
	__cfg_count++;
	(void)__xtc_mtx_unlock(&__cfg_lock);
	return XTC_OK;
}

int
xtc_cfg_unregister(const char *name)
{
	struct cfg_var *v, **link;
	int rc = XTC_E_INVAL;
	if (name == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&__cfg_lock);
	for (link = &__cfg_head; (v = *link) != NULL; link = &v->next) {
		if (strcmp(v->name, name) == 0) {
			*link = v->next;
			__cfg_count--;
			(void)__xtc_mtx_unlock(&__cfg_lock);
			__os_free(v->name); __os_free(v->desc);
			if (v->kind == XTC_CFG_STRING && v->cur.v_string)
				__os_free(v->cur.v_string);
			__os_free(v);
			return XTC_OK;
		}
	}
	(void)__xtc_mtx_unlock(&__cfg_lock);
	return rc;
}

#define DEF_GET(name_suffix, K, field, type) \
int xtc_cfg_get_##name_suffix(const char *name, type *out) { \
	struct cfg_var *v; int rc = XTC_E_INVAL; union cfg_val sv; \
	if (name == NULL || out == NULL) return XTC_E_INVAL; \
	(void)__xtc_mtx_lock(&__cfg_lock); \
	v = __cfg_find_locked(name); \
	if (v && v->kind == K) { \
		/* Resolve through the bound session first; a session \
		 * override shadows the global value.  Falls straight \
		 * through to the global when none is bound / no override. */ \
		if (__cfg_ssn_resolve(v, &sv)) *out = sv.field; \
		else *out = v->cur.field; \
		rc = XTC_OK; \
	} \
	(void)__xtc_mtx_unlock(&__cfg_lock); \
	return rc; \
}

DEF_GET(bool,   XTC_CFG_BOOL,   v_bool,   int)
DEF_GET(int,    XTC_CFG_INT,    v_int,    int)
DEF_GET(int64,  XTC_CFG_INT64,  v_int64,  int64_t)
DEF_GET(double, XTC_CFG_DOUBLE, v_double, double)
DEF_GET(enum,   XTC_CFG_ENUM,   v_enum,   int)

int
xtc_cfg_get_string(const char *name, const char **out)
{
	struct cfg_var *v;
	int rc = XTC_E_INVAL;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&__cfg_lock);
	v = __cfg_find_locked(name);
	if (v && v->kind == XTC_CFG_STRING) {
		union cfg_val sv;
		*out = __cfg_ssn_resolve(v, &sv) ? sv.v_string : v->cur.v_string;
		rc = XTC_OK;
	}
	(void)__xtc_mtx_unlock(&__cfg_lock);
	return rc;
}

#define DEF_SET_NUM(name_suffix, K, field, ctype, bounds_check) \
int xtc_cfg_set_##name_suffix(const char *name, ctype v) { \
	struct cfg_var *cv; int rc = XTC_E_INVAL; \
	ctype old = 0, new_v = v; \
	xtc_cfg_changed_fn cb = NULL; void *cb_u = NULL; \
	if (name == NULL) return XTC_E_INVAL; \
	(void)__xtc_mtx_lock(&__cfg_lock); \
	cv = __cfg_find_locked(name); \
	if (cv && cv->kind == K) { \
		if (!bounds_check) { rc = XTC_E_RANGE; goto done; } \
		if (cv->validator && cv->validator(&new_v, cv->cb_user) != XTC_OK) { \
			rc = XTC_E_INVAL; goto done; \
		} \
		old = cv->cur.field; cv->cur.field = new_v; \
		cb = cv->on_change; cb_u = cv->cb_user; \
		rc = XTC_OK; \
	} \
done:	(void)__xtc_mtx_unlock(&__cfg_lock); \
	if (rc == XTC_OK && cb != NULL) cb(name, &old, &new_v, cb_u); \
	return rc; \
}

DEF_SET_NUM(bool,   XTC_CFG_BOOL,   v_bool,   int,
            (new_v == 0 || new_v == 1))
DEF_SET_NUM(int,    XTC_CFG_INT,    v_int,    int,
            __bounds_int_ok(cv, (int64_t)new_v))
DEF_SET_NUM(int64,  XTC_CFG_INT64,  v_int64,  int64_t,
            __bounds_int_ok(cv, new_v))
DEF_SET_NUM(double, XTC_CFG_DOUBLE, v_double, double,
            __bounds_dbl_ok(cv, new_v))
DEF_SET_NUM(enum,   XTC_CFG_ENUM,   v_enum,   int,
            (new_v >= 0 && new_v < cv->n_enum_labels))

int
xtc_cfg_set_string(const char *name, const char *v)
{
	struct cfg_var *cv;
	char *new_copy = NULL;
	int rc = XTC_E_INVAL;
	xtc_cfg_changed_fn cb = NULL;
	void *cb_u = NULL;
	if (name == NULL || v == NULL) return XTC_E_INVAL;
	if ((rc = __os_strdup(v, &new_copy)) != XTC_OK) return rc;
	(void)__xtc_mtx_lock(&__cfg_lock);
	cv = __cfg_find_locked(name);
	if (cv && cv->kind == XTC_CFG_STRING) {
		if (cv->validator && cv->validator(new_copy, cv->cb_user) != XTC_OK) {
			rc = XTC_E_INVAL;
			goto done_str;
		}
		if (cv->cur.v_string) __os_free(cv->cur.v_string);
		cv->cur.v_string = new_copy;
		new_copy = NULL;
		cb = cv->on_change;
		cb_u = cv->cb_user;
		rc = XTC_OK;
	} else {
		rc = XTC_E_INVAL;
	}
done_str:
	(void)__xtc_mtx_unlock(&__cfg_lock);
	if (new_copy != NULL) __os_free(new_copy);
	if (rc == XTC_OK && cb != NULL) cb(name, NULL, v, cb_u);
	return rc;
}

int
xtc_cfg_count(void)
{
	int n;
	(void)__xtc_mtx_lock(&__cfg_lock);
	n = __cfg_count;
	(void)__xtc_mtx_unlock(&__cfg_lock);
	return n;
}

int
xtc_cfg_kind(const char *name, xtc_cfg_kind_t *out)
{
	struct cfg_var *v;
	int rc = XTC_E_INVAL;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&__cfg_lock);
	v = __cfg_find_locked(name);
	if (v != NULL) { *out = v->kind; rc = XTC_OK; }
	(void)__xtc_mtx_unlock(&__cfg_lock);
	return rc;
}

/* ---- hot-path read handles ---- */

int
xtc_cfg_ref(const char *name, xtc_cfg_ref_t *out)
{
	struct cfg_var *v;
	int rc = XTC_E_NOTFOUND;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&__cfg_lock);
	v = __cfg_find_locked(name);
	if (v != NULL) { *out = v; rc = XTC_OK; }
	(void)__xtc_mtx_unlock(&__cfg_lock);
	return rc;
}

/* A handle read is a direct field load under the registry lock: O(1),
 * no name scan.  It still takes the lock so a concurrent set_string's
 * free-then-replace is observed atomically, matching the name-keyed
 * getters exactly. */
#define DEF_REF_GET(name_suffix, K, field, type) \
int xtc_cfg_ref_get_##name_suffix(xtc_cfg_ref_t ref, type *out) { \
	int rc = XTC_E_INVAL; union cfg_val sv; \
	if (ref == NULL || out == NULL) return XTC_E_INVAL; \
	(void)__xtc_mtx_lock(&__cfg_lock); \
	if (ref->kind == K) { \
		/* Handles resolve through the bound session too, so a hot- \
		 * path ref read still observes the session's value. */ \
		if (__cfg_ssn_resolve(ref, &sv)) *out = sv.field; \
		else *out = ref->cur.field; \
		rc = XTC_OK; \
	} \
	(void)__xtc_mtx_unlock(&__cfg_lock); \
	return rc; \
}

DEF_REF_GET(bool,   XTC_CFG_BOOL,   v_bool,   int)
DEF_REF_GET(int,    XTC_CFG_INT,    v_int,    int)
DEF_REF_GET(int64,  XTC_CFG_INT64,  v_int64,  int64_t)
DEF_REF_GET(double, XTC_CFG_DOUBLE, v_double, double)
DEF_REF_GET(string, XTC_CFG_STRING, v_string, const char *)
DEF_REF_GET(enum,   XTC_CFG_ENUM,   v_enum,   int)

/* ---- config-file loading (postgresql.conf-style key = value) ---- */

static char *__cfg_file;   /* last path loaded; for xtc_cfg_reload */

static int
__cfg_parse_bool(const char *s, int *out)
{
	if (strcasecmp(s, "on") == 0 || strcasecmp(s, "true") == 0 ||
	    strcasecmp(s, "yes") == 0 || strcmp(s, "1") == 0) { *out = 1; return 0; }
	if (strcasecmp(s, "off") == 0 || strcasecmp(s, "false") == 0 ||
	    strcasecmp(s, "no") == 0 || strcmp(s, "0") == 0) { *out = 0; return 0; }
	return -1;
}

/* Resolve an enum label (or numeric index) to its index for `name`. */
static int
__cfg_enum_index(const char *name, const char *sval, int *out)
{
	struct cfg_var *v;
	int rc = XTC_E_NOTFOUND, i;
	(void)__xtc_mtx_lock(&__cfg_lock);
	v = __cfg_find_locked(name);
	if (v != NULL && v->kind == XTC_CFG_ENUM) {
		rc = XTC_E_INVAL;
		for (i = 0; i < v->n_enum_labels; i++) {
			if (v->enum_labels[i] != NULL &&
			    strcasecmp(v->enum_labels[i], sval) == 0) {
				*out = i; rc = XTC_OK; break;
			}
		}
	}
	(void)__xtc_mtx_unlock(&__cfg_lock);
	return rc;
}

/* Apply one name = string-value pair, parsing per the registered kind.
 * Returns XTC_OK, XTC_E_NOTFOUND (unknown name), or a set_* error. */
static int
__cfg_apply_string(const char *name, const char *sval)
{
	xtc_cfg_kind_t k;
	if (xtc_cfg_kind(name, &k) != XTC_OK)
		return XTC_E_NOTFOUND;
	switch (k) {
	case XTC_CFG_BOOL: {
		int b;
		if (__cfg_parse_bool(sval, &b) != 0) return XTC_E_INVAL;
		return xtc_cfg_set_bool(name, b);
	}
	case XTC_CFG_INT: {
		char *end; long v;
		errno = 0; v = strtol(sval, &end, 0);
		if (*sval == '\0' || *end != '\0' || errno != 0) return XTC_E_INVAL;
		return xtc_cfg_set_int(name, (int)v);
	}
	case XTC_CFG_INT64: {
		char *end; long long v;
		errno = 0; v = strtoll(sval, &end, 0);
		if (*sval == '\0' || *end != '\0' || errno != 0) return XTC_E_INVAL;
		return xtc_cfg_set_int64(name, (int64_t)v);
	}
	case XTC_CFG_DOUBLE: {
		char *end; double v;
		errno = 0; v = strtod(sval, &end);
		if (*sval == '\0' || *end != '\0') return XTC_E_INVAL;
		return xtc_cfg_set_double(name, v);
	}
	case XTC_CFG_STRING:
		return xtc_cfg_set_string(name, sval);
	case XTC_CFG_ENUM: {
		int idx;
		int rc = __cfg_enum_index(name, sval, &idx);
		if (rc != XTC_OK) {
			/* Accept a numeric index too. */
			char *end; long n = strtol(sval, &end, 10);
			if (*sval == '\0' || *end != '\0') return rc;
			idx = (int)n;
		}
		return xtc_cfg_set_enum(name, idx);
	}
	}
	return XTC_E_INVAL;
}

static char *
__cfg_trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
	if (*s == '\0') return s;
	e = s + strlen(s) - 1;
	while (e > s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n'))
		*e-- = '\0';
	return s;
}

/*
 * PUBLIC: int  xtc_cfg_load_file __P((const char *));
 *
 * Read a postgresql.conf-style file: one `name = value` per line, `#`
 * comments, optional single/double quotes around the value, blank
 * lines ignored.  Each value is parsed per the variable's registered
 * kind and applied via the matching xtc_cfg_set_*; bounds/validators
 * still apply.  Unknown names and unparseable/rejected values are
 * skipped (a reload must not abort on one bad line).  Returns the
 * number of settings successfully applied (>= 0), or XTC_E_INVAL on a
 * NULL path or XTC_E_IO if the file cannot be opened.  The path is
 * remembered for xtc_cfg_reload.
 */
int
xtc_cfg_load_file(const char *path)
{
	FILE *f;
	char  line[2048];
	int   applied = 0;
	char *dup = NULL;

	if (path == NULL) return XTC_E_INVAL;
	f = fopen(path, "r");        /* XTC_BLOCKING_OK: config read, startup/SIGHUP */
	if (f == NULL) return XTC_E_IO;

	while (fgets(line, sizeof line, f) != NULL) {  /* XTC_BLOCKING_OK: config read */
		char *p = __cfg_trim(line);
		char *eq, *key, *val;
		if (*p == '\0' || *p == '#') continue;
		eq = strchr(p, '=');
		if (eq == NULL) continue;       /* not a key = value line */
		*eq = '\0';
		key = __cfg_trim(p);
		val = __cfg_trim(eq + 1);
		if (*val == '\'' || *val == '"') {
			char q = *val++;
			char *close = strchr(val, q);
			if (close != NULL) *close = '\0';
		} else {
			/* Strip an unquoted trailing `# comment`. */
			char *h = strchr(val, '#');
			if (h != NULL) { *h = '\0'; val = __cfg_trim(val); }
		}
		if (*key == '\0') continue;
		if (__cfg_apply_string(key, val) == XTC_OK) applied++;
	}
	(void)fclose(f);            /* XTC_BLOCKING_OK: config read */

	/* Remember the path so xtc_cfg_reload can re-read it. */
	if (__os_strdup(path, &dup) == XTC_OK) {
		(void)__xtc_mtx_lock(&__cfg_lock);
		if (__cfg_file != NULL) __os_free(__cfg_file);
		__cfg_file = dup;
		(void)__xtc_mtx_unlock(&__cfg_lock);
	}
	return applied;
}

/*
 * PUBLIC: int  xtc_cfg_reload __P((void));
 *
 * Re-read the file last passed to xtc_cfg_load_file, re-applying every
 * value (on_change callbacks fire for any that changed).  Intended to
 * back a SIGHUP handler -- but it calls stdio and may take the registry
 * lock, so it is NOT async-signal-safe: set a flag in the signal
 * handler and call xtc_cfg_reload from the event loop, not from the
 * handler itself.  Returns the applied count, XTC_E_INVAL if no file
 * has been loaded, or XTC_E_IO if the file cannot be reopened.
 */
int
xtc_cfg_reload(void)
{
	char *path = NULL;
	int rc;
	(void)__xtc_mtx_lock(&__cfg_lock);
	if (__cfg_file != NULL) (void)__os_strdup(__cfg_file, &path);
	(void)__xtc_mtx_unlock(&__cfg_lock);
	if (path == NULL) return XTC_E_INVAL;
	rc = xtc_cfg_load_file(path);
	__os_free(path);
	return rc;
}

/* ================= per-session scoping + override stack =================
 *
 * A session layers per-variable overrides over the global registry.  An
 * override is a stack of LEVELS (transactional: push/commit/abort); each
 * level holds at most one value per variable, tagged with the source
 * that set it.  A read resolves newest level down to the base, then
 * falls through to the global registry value.  See xtc_cfg.h.
 */

/* One override entry: this variable has a value at this level. */
struct cfg_ovr {
	struct cfg_var   *var;    /* which variable (identity, no name lookup) */
	union cfg_val     val;
	xtc_cfg_source_t  src;
	struct cfg_ovr   *next;   /* next entry at the same level */
};

/* One transactional level: a list of overrides set at this level. */
struct cfg_level {
	struct cfg_ovr   *head;
	struct cfg_level *parent;
};

struct xtc_cfg_session {
	struct cfg_level *top;    /* innermost level (never NULL: base level) */
};

/* Bound session for the running fiber.  A carrier runs one fiber at a
 * time and the consumer binds on fiber entry / unbinds on exit, so a
 * thread-local is fiber-local in practice and keeps cfg self-contained
 * (no field on struct xtc_proc, no dependency on the proc layer). */
static _Thread_local xtc_cfg_session_t *__cfg_ssn_current;

static void
__cfg_ovr_free(struct cfg_ovr *o)
{
	if (o->var->kind == XTC_CFG_STRING && o->val.v_string != NULL)
		__os_free(o->val.v_string);
	__os_free(o);
}

static void
__cfg_level_free(struct cfg_level *lv)
{
	struct cfg_ovr *o, *n;
	for (o = lv->head; o != NULL; o = n) { n = o->next; __cfg_ovr_free(o); }
	__os_free(lv);
}

int
xtc_cfg_session_create(xtc_cfg_session_t **out)
{
	xtc_cfg_session_t *s;
	struct cfg_level *base;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc(1, sizeof *base, (void **)&base)) != XTC_OK) {
		__os_free(s);
		return rc;
	}
	base->parent = NULL;
	s->top = base;
	*out = s;
	return XTC_OK;
}

void
xtc_cfg_session_destroy(xtc_cfg_session_t *s)
{
	struct cfg_level *lv, *p;
	if (s == NULL) return;
	if (__cfg_ssn_current == s) __cfg_ssn_current = NULL;
	for (lv = s->top; lv != NULL; lv = p) { p = lv->parent; __cfg_level_free(lv); }
	__os_free(s);
}

xtc_cfg_session_t *
xtc_cfg_session_bind(xtc_cfg_session_t *s)
{
	xtc_cfg_session_t *prev = __cfg_ssn_current;
	__cfg_ssn_current = s;
	return prev;
}

xtc_cfg_session_t *
xtc_cfg_session_current(void)
{
	return __cfg_ssn_current;
}

int
xtc_cfg_session_push(xtc_cfg_session_t *s)
{
	struct cfg_level *lv;
	int rc;
	if (s == NULL) s = __cfg_ssn_current;
	if (s == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *lv, (void **)&lv)) != XTC_OK)
		return rc;
	lv->parent = s->top;
	s->top = lv;
	return XTC_OK;
}

int
xtc_cfg_session_commit(xtc_cfg_session_t *s)
{
	struct cfg_level *top, *parent;
	struct cfg_ovr *o, *n;
	if (s == NULL) s = __cfg_ssn_current;
	if (s == NULL) return XTC_E_INVAL;
	top = s->top;
	parent = top->parent;
	if (parent == NULL) return XTC_E_INVAL;   /* base level: nothing to commit */
	/*
	 * Fold this level's overrides into the parent.  A value set here
	 * survives; it replaces the parent's entry for the same variable
	 * (this level is newer), reusing the parent slot so a string is not
	 * leaked.  Order does not matter -- one entry per (var, level).
	 */
	for (o = top->head; o != NULL; o = n) {
		struct cfg_ovr *pe;
		n = o->next;
		for (pe = parent->head; pe != NULL; pe = pe->next)
			if (pe->var == o->var) break;
		if (pe != NULL) {
			if (pe->var->kind == XTC_CFG_STRING &&
			    pe->val.v_string != NULL)
				__os_free(pe->val.v_string);
			pe->val = o->val;
			pe->src = o->src;
			/* o's string (if any) now owned by pe; do not free it. */
			__os_free(o);
		} else {
			o->next = parent->head;
			parent->head = o;   /* moved wholesale, string included */
		}
	}
	s->top = parent;
	__os_free(top);
	return XTC_OK;
}

int
xtc_cfg_session_abort(xtc_cfg_session_t *s)
{
	struct cfg_level *top;
	if (s == NULL) s = __cfg_ssn_current;
	if (s == NULL) return XTC_E_INVAL;
	top = s->top;
	if (top->parent == NULL) return XTC_E_INVAL;   /* base level */
	s->top = top->parent;
	__cfg_level_free(top);   /* discard this level's overrides + strings */
	return XTC_OK;
}

/* Find the newest override for `var` across the session's levels, and
 * the level it lives on (for source/reset).  NULL if none. */
static struct cfg_ovr *
__cfg_ssn_find(xtc_cfg_session_t *s, struct cfg_var *var)
{
	struct cfg_level *lv;
	for (lv = s->top; lv != NULL; lv = lv->parent) {
		struct cfg_ovr *o;
		for (o = lv->head; o != NULL; o = o->next)
			if (o->var == var) return o;   /* newest level first */
	}
	return NULL;
}

int
xtc_cfg_session_source(xtc_cfg_session_t *s, const char *name,
                       xtc_cfg_source_t *out)
{
	struct cfg_var *v;
	struct cfg_ovr *o;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	if (s == NULL) s = __cfg_ssn_current;
	if (s == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&__cfg_lock);
	v = __cfg_find_locked(name);
	(void)__xtc_mtx_unlock(&__cfg_lock);
	if (v == NULL) return XTC_E_NOTFOUND;
	o = __cfg_ssn_find(s, v);
	*out = (o != NULL) ? o->src : XTC_CFG_SRC_DEFAULT;
	return XTC_OK;
}

int
xtc_cfg_session_reset(xtc_cfg_session_t *s, const char *name)
{
	struct cfg_var *v;
	struct cfg_ovr **link, *o;
	if (name == NULL) return XTC_E_INVAL;
	if (s == NULL) s = __cfg_ssn_current;
	if (s == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&__cfg_lock);
	v = __cfg_find_locked(name);
	(void)__xtc_mtx_unlock(&__cfg_lock);
	if (v == NULL) return XTC_E_NOTFOUND;
	/* Drop the override at the CURRENT (top) level only, so RESET undoes
	 * this level's SET and the parent value (or global) shows through. */
	for (link = &s->top->head; (o = *link) != NULL; link = &o->next) {
		if (o->var == v) { *link = o->next; __cfg_ovr_free(o); break; }
	}
	return XTC_OK;
}

/* Set/replace `var`'s override at the current level.  Precedence: apply
 * only if src outranks whatever set it at THIS level (a lower-ranked
 * source cannot clobber a higher one, mirroring PG's source ordering).
 * `sval` is the pre-copied string for STRING kinds (ownership taken on
 * success), NULL otherwise. */
static int
__cfg_ssn_apply(xtc_cfg_session_t *s, struct cfg_var *var,
                union cfg_val val, char *sval, xtc_cfg_source_t src)
{
	struct cfg_ovr *o;
	int rc;
	/* Existing override at the TOP level for this var? */
	for (o = s->top->head; o != NULL; o = o->next)
		if (o->var == var) break;
	if (o != NULL) {
		if (src < o->src) {          /* lower-ranked: refuse */
			if (sval != NULL) __os_free(sval);
			return XTC_OK;           /* not an error: PG semantics */
		}
		if (var->kind == XTC_CFG_STRING && o->val.v_string != NULL)
			__os_free(o->val.v_string);
		o->val = val;
		if (var->kind == XTC_CFG_STRING) o->val.v_string = sval;
		o->src = src;
		return XTC_OK;
	}
	if ((rc = __os_calloc(1, sizeof *o, (void **)&o)) != XTC_OK) {
		if (sval != NULL) __os_free(sval);
		return rc;
	}
	o->var = var;
	o->val = val;
	if (var->kind == XTC_CFG_STRING) o->val.v_string = sval;
	o->src = src;
	o->next = s->top->head;
	s->top->head = o;
	return XTC_OK;
}

#define DEF_SSN_SET_NUM(sfx, K, field, ctype, okexpr)                     \
int xtc_cfg_ssn_set_##sfx(xtc_cfg_session_t *s, const char *name,         \
                          ctype v, xtc_cfg_source_t src) {                \
	struct cfg_var *cv; union cfg_val val; int ok = 0;                \
	if (name == NULL) return XTC_E_INVAL;                             \
	if (s == NULL) s = __cfg_ssn_current;                             \
	if (s == NULL) return XTC_E_INVAL;                                \
	(void)__xtc_mtx_lock(&__cfg_lock);                                \
	cv = __cfg_find_locked(name);                                     \
	if (cv != NULL && cv->kind == K && (okexpr) &&                    \
	    (cv->validator == NULL ||                                     \
	     cv->validator(&v, cv->cb_user) == XTC_OK))                   \
		ok = 1;                                                   \
	(void)__xtc_mtx_unlock(&__cfg_lock);                              \
	if (!ok) return XTC_E_INVAL;                                      \
	val.field = v;                                                    \
	return __cfg_ssn_apply(s, cv, val, NULL, src);                    \
}

DEF_SSN_SET_NUM(bool,   XTC_CFG_BOOL,   v_bool,   int,
                (v == 0 || v == 1))
DEF_SSN_SET_NUM(int,    XTC_CFG_INT,    v_int,    int,
                __bounds_int_ok(cv, (int64_t)v))
DEF_SSN_SET_NUM(int64,  XTC_CFG_INT64,  v_int64,  int64_t,
                __bounds_int_ok(cv, v))
DEF_SSN_SET_NUM(double, XTC_CFG_DOUBLE, v_double, double,
                __bounds_dbl_ok(cv, v))
DEF_SSN_SET_NUM(enum,   XTC_CFG_ENUM,   v_enum,   int,
                (v >= 0 && v < cv->n_enum_labels))

int
xtc_cfg_ssn_set_string(xtc_cfg_session_t *s, const char *name,
                       const char *v, xtc_cfg_source_t src)
{
	struct cfg_var *cv;
	union cfg_val val;
	char *copy = NULL;
	int ok = 0, rc;
	if (name == NULL || v == NULL) return XTC_E_INVAL;
	if (s == NULL) s = __cfg_ssn_current;
	if (s == NULL) return XTC_E_INVAL;
	if ((rc = __os_strdup(v, &copy)) != XTC_OK) return rc;
	(void)__xtc_mtx_lock(&__cfg_lock);
	cv = __cfg_find_locked(name);
	if (cv != NULL && cv->kind == XTC_CFG_STRING &&
	    (cv->validator == NULL ||
	     cv->validator(copy, cv->cb_user) == XTC_OK))
		ok = 1;
	(void)__xtc_mtx_unlock(&__cfg_lock);
	if (!ok) { __os_free(copy); return XTC_E_INVAL; }
	memset(&val, 0, sizeof val);
	return __cfg_ssn_apply(s, cv, val, copy, src);
}

/* Read `var`'s effective value for the CURRENTLY BOUND session into the
 * caller's slot; returns 1 if a session override supplied it, 0 if the
 * caller should fall through to the global value.  Called with the
 * registry lock HELD (so var cannot be unregistered under us). */
static int
__cfg_ssn_resolve(struct cfg_var *var, union cfg_val *out)
{
	xtc_cfg_session_t *s = __cfg_ssn_current;
	struct cfg_ovr *o;
	if (s == NULL) return 0;
	o = __cfg_ssn_find(s, var);
	if (o == NULL) return 0;
	*out = o->val;
	return 1;
}

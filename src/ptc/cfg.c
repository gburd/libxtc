/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/cfg.c
 *	GUC-style typed configuration registry.
 */

#include "xtc_int.h"
#include "xtc_cfg.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct cfg_var {
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
	struct cfg_var *next;
};

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

	(void)pthread_mutex_lock(&__cfg_lock);
	if (__cfg_find_locked(spec->name) != NULL) {
		(void)pthread_mutex_unlock(&__cfg_lock);
		__os_free(v->name); __os_free(v->desc);
		if (v->cur.v_string) __os_free(v->cur.v_string);
		__os_free(v);
		return XTC_E_INVAL;
	}
	v->next = __cfg_head;
	__cfg_head = v;
	__cfg_count++;
	(void)pthread_mutex_unlock(&__cfg_lock);
	return XTC_OK;
}

int
xtc_cfg_unregister(const char *name)
{
	struct cfg_var *v, **link;
	int rc = XTC_E_INVAL;
	if (name == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&__cfg_lock);
	for (link = &__cfg_head; (v = *link) != NULL; link = &v->next) {
		if (strcmp(v->name, name) == 0) {
			*link = v->next;
			__cfg_count--;
			(void)pthread_mutex_unlock(&__cfg_lock);
			__os_free(v->name); __os_free(v->desc);
			if (v->kind == XTC_CFG_STRING && v->cur.v_string)
				__os_free(v->cur.v_string);
			__os_free(v);
			return XTC_OK;
		}
	}
	(void)pthread_mutex_unlock(&__cfg_lock);
	return rc;
}

#define DEF_GET(name_suffix, K, field, type) \
int xtc_cfg_get_##name_suffix(const char *name, type *out) { \
	struct cfg_var *v; int rc = XTC_E_INVAL; \
	if (name == NULL || out == NULL) return XTC_E_INVAL; \
	(void)pthread_mutex_lock(&__cfg_lock); \
	v = __cfg_find_locked(name); \
	if (v && v->kind == K) { *out = v->cur.field; rc = XTC_OK; } \
	(void)pthread_mutex_unlock(&__cfg_lock); \
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
	(void)pthread_mutex_lock(&__cfg_lock);
	v = __cfg_find_locked(name);
	if (v && v->kind == XTC_CFG_STRING) {
		*out = v->cur.v_string;
		rc = XTC_OK;
	}
	(void)pthread_mutex_unlock(&__cfg_lock);
	return rc;
}

#define DEF_SET_NUM(name_suffix, K, field, ctype, bounds_check) \
int xtc_cfg_set_##name_suffix(const char *name, ctype v) { \
	struct cfg_var *cv; int rc = XTC_E_INVAL; \
	ctype old = 0, new_v = v; \
	xtc_cfg_changed_fn cb = NULL; void *cb_u = NULL; \
	if (name == NULL) return XTC_E_INVAL; \
	(void)pthread_mutex_lock(&__cfg_lock); \
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
done:	(void)pthread_mutex_unlock(&__cfg_lock); \
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
	(void)pthread_mutex_lock(&__cfg_lock);
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
	(void)pthread_mutex_unlock(&__cfg_lock);
	if (new_copy != NULL) __os_free(new_copy);
	if (rc == XTC_OK && cb != NULL) cb(name, NULL, v, cb_u);
	return rc;
}

int
xtc_cfg_count(void)
{
	int n;
	(void)pthread_mutex_lock(&__cfg_lock);
	n = __cfg_count;
	(void)pthread_mutex_unlock(&__cfg_lock);
	return n;
}

int
xtc_cfg_kind(const char *name, xtc_cfg_kind_t *out)
{
	struct cfg_var *v;
	int rc = XTC_E_INVAL;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&__cfg_lock);
	v = __cfg_find_locked(name);
	if (v != NULL) { *out = v->kind; rc = XTC_OK; }
	(void)pthread_mutex_unlock(&__cfg_lock);
	return rc;
}

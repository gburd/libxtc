/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/reg.c
 *	Process registry implementation.  M10.5: simple linear table
 *	under a mutex.  M11+ swaps in xtc_chash (RCU hash table) for
 *	wait-free reads.
 */

#include "xtc_int.h"
#include "xtc_reg.h"

#include <pthread.h>
#include <string.h>

struct entry {
	char       *name;
	xtc_pid_t   pid;
};

struct xtc_reg {
	pthread_mutex_t lock;
	struct entry   *items;
	int             n;
	int             cap;
};

int
xtc_reg_create(xtc_reg_t **out)
{
	xtc_reg_t *r;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *r, (void **)&r)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&r->lock, NULL);
	*out = r;
	return XTC_OK;
}

void
xtc_reg_destroy(xtc_reg_t *r)
{
	int i;
	if (r == NULL) return;
	for (i = 0; i < r->n; i++) __os_free(r->items[i].name);
	__os_free(r->items);
	(void)pthread_mutex_destroy(&r->lock);
	__os_free(r);
}

static int
__find_locked(struct xtc_reg *r, const char *name)
{
	int i;
	for (i = 0; i < r->n; i++)
		if (strcmp(r->items[i].name, name) == 0) return i;
	return -1;
}

static int
__grow_locked(struct xtc_reg *r)
{
	int new_cap = r->cap == 0 ? 16 : r->cap * 2;
	void *p = NULL;
	int rc = __os_realloc(r->items, sizeof(*r->items) * (size_t)new_cap, &p);
	if (rc != XTC_OK) return rc;
	r->items = p;
	r->cap = new_cap;
	return XTC_OK;
}

int
xtc_reg_register(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	int rc = XTC_OK;
	if (r == NULL || name == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&r->lock);
	if (__find_locked(r, name) >= 0) { rc = XTC_E_INVAL; goto out; }
	if (r->n >= r->cap) {
		if ((rc = __grow_locked(r)) != XTC_OK) goto out;
	}
	if ((rc = __os_strdup(name, &r->items[r->n].name)) != XTC_OK) goto out;
	r->items[r->n].pid = pid;
	r->n++;
out:
	(void)pthread_mutex_unlock(&r->lock);
	return rc;
}

int
xtc_reg_unregister(xtc_reg_t *r, const char *name)
{
	int rc = XTC_E_INVAL;
	int idx;
	if (r == NULL || name == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&r->lock);
	idx = __find_locked(r, name);
	if (idx >= 0) {
		__os_free(r->items[idx].name);
		r->n--;
		if (idx != r->n) r->items[idx] = r->items[r->n];
		rc = XTC_OK;
	}
	(void)pthread_mutex_unlock(&r->lock);
	return rc;
}

int
xtc_reg_whereis(xtc_reg_t *r, const char *name, xtc_pid_t *out_pid)
{
	int rc = XTC_E_INVAL;
	int idx;
	if (r == NULL || name == NULL || out_pid == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&r->lock);
	idx = __find_locked(r, name);
	if (idx >= 0) {
		*out_pid = r->items[idx].pid;
		rc = XTC_OK;
	}
	(void)pthread_mutex_unlock(&r->lock);
	return rc;
}

int
xtc_reg_count(const xtc_reg_t *r)
{
	int n;
	if (r == NULL) return 0;
	(void)pthread_mutex_lock((pthread_mutex_t *)&r->lock);
	n = r->n;
	(void)pthread_mutex_unlock((pthread_mutex_t *)&r->lock);
	return n;
}

/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/pdict.c
 *	Per-process dictionary backed by a thread-local list pointer
 *	keyed on the calling proc's identity.
 *
 *	Storage shape:
 *	  - We don't add a field to xtc_proc directly to avoid touching
 *	    proc.c's struct layout from another file.  Instead we keep
 *	    a tiny side-table mapping (xtc_pid -> dict head pointer)
 *	    protected by a mutex.  Linear scan is fine; at most ~hundreds
 *	    of procs hold dict state at once in typical workloads.
 *	  - Cleanup-on-exit is the responsibility of M16+ (when xtc_proc
 *	    grows a destructor hook).  For now, callers should call
 *	    xtc_pdict_clear() before their proc returns.
 */

#include "xtc_int.h"
#include "xtc_pdict.h"
#include "xtc_proc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct pdict_entry {
	char               *key;
	void               *value;
	xtc_pdict_dtor_fn   dtor;
	struct pdict_entry *next;
};

struct pdict_table_entry {
	xtc_pid_t           pid;
	struct pdict_entry *head;
	int                 n;
	struct pdict_table_entry *next;
};

static pthread_mutex_t __pd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pdict_table_entry *__pd_head;

static struct pdict_table_entry *
__find_locked(xtc_pid_t pid)
{
	struct pdict_table_entry *te;
	for (te = __pd_head; te != NULL; te = te->next)
		if (xtc_pid_eq(te->pid, pid)) return te;
	return NULL;
}

static struct pdict_table_entry *
__get_or_create_locked(xtc_pid_t pid)
{
	struct pdict_table_entry *te = __find_locked(pid);
	if (te != NULL) return te;
	if (__os_calloc(1, sizeof *te, (void **)&te) != XTC_OK) return NULL;
	te->pid = pid;
	te->next = __pd_head;
	__pd_head = te;
	return te;
}

static void
__remove_table_locked(xtc_pid_t pid)
{
	struct pdict_table_entry *te, **link;
	for (link = &__pd_head; (te = *link) != NULL; link = &te->next) {
		if (xtc_pid_eq(te->pid, pid)) {
			*link = te->next;
			__os_free(te);
			return;
		}
	}
}

int
xtc_pdict_put_with_dtor(const char *key, void *value, xtc_pdict_dtor_fn dtor)
{
	xtc_pid_t self = xtc_self();
	struct pdict_table_entry *te;
	struct pdict_entry *e;
	int rc = XTC_E_NOMEM;
	if (key == NULL) return XTC_E_INVAL;
	if (xtc_pid_is_none(self)) return XTC_E_INVAL;

	(void)pthread_mutex_lock(&__pd_lock);
	te = __get_or_create_locked(self);
	if (te == NULL) goto out;

	/* Replace if key already exists. */
	for (e = te->head; e != NULL; e = e->next) {
		if (strcmp(e->key, key) == 0) {
			if (e->dtor) e->dtor(e->value);
			e->value = value;
			e->dtor  = dtor;
			rc = XTC_OK;
			goto out;
		}
	}
	if (__os_calloc(1, sizeof *e, (void **)&e) != XTC_OK) goto out;
	if (__os_strdup(key, &e->key) != XTC_OK) {
		__os_free(e);
		goto out;
	}
	e->value = value;
	e->dtor  = dtor;
	e->next  = te->head;
	te->head = e;
	te->n++;
	rc = XTC_OK;
out:
	(void)pthread_mutex_unlock(&__pd_lock);
	return rc;
}

int
xtc_pdict_put(const char *key, void *value)
{
	return xtc_pdict_put_with_dtor(key, value, NULL);
}

int
xtc_pdict_get(const char *key, void **value)
{
	xtc_pid_t self = xtc_self();
	struct pdict_table_entry *te;
	struct pdict_entry *e;
	int rc = XTC_E_INVAL;
	if (key == NULL || value == NULL) return XTC_E_INVAL;
	if (xtc_pid_is_none(self)) return XTC_E_INVAL;

	(void)pthread_mutex_lock(&__pd_lock);
	te = __find_locked(self);
	if (te) {
		for (e = te->head; e != NULL; e = e->next) {
			if (strcmp(e->key, key) == 0) {
				*value = e->value;
				rc = XTC_OK;
				break;
			}
		}
	}
	(void)pthread_mutex_unlock(&__pd_lock);
	return rc;
}

int
xtc_pdict_erase(const char *key)
{
	xtc_pid_t self = xtc_self();
	struct pdict_table_entry *te;
	struct pdict_entry *e, **link;
	int rc = XTC_E_INVAL;
	if (key == NULL) return XTC_E_INVAL;
	if (xtc_pid_is_none(self)) return XTC_E_INVAL;

	(void)pthread_mutex_lock(&__pd_lock);
	te = __find_locked(self);
	if (te) {
		for (link = &te->head; (e = *link) != NULL; link = &e->next) {
			if (strcmp(e->key, key) == 0) {
				*link = e->next;
				if (e->dtor) e->dtor(e->value);
				__os_free(e->key);
				__os_free(e);
				te->n--;
				rc = XTC_OK;
				break;
			}
		}
	}
	(void)pthread_mutex_unlock(&__pd_lock);
	return rc;
}

int
xtc_pdict_count(void)
{
	xtc_pid_t self = xtc_self();
	struct pdict_table_entry *te;
	int n = 0;
	if (xtc_pid_is_none(self)) return 0;
	(void)pthread_mutex_lock(&__pd_lock);
	te = __find_locked(self);
	if (te) n = te->n;
	(void)pthread_mutex_unlock(&__pd_lock);
	return n;
}

int
xtc_pdict_clear(void)
{
	xtc_pid_t self = xtc_self();
	struct pdict_table_entry *te;
	struct pdict_entry *e, *next;
	if (xtc_pid_is_none(self)) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&__pd_lock);
	te = __find_locked(self);
	if (te) {
		for (e = te->head; e != NULL; e = next) {
			next = e->next;
			if (e->dtor) e->dtor(e->value);
			__os_free(e->key);
			__os_free(e);
		}
		__remove_table_locked(self);
	}
	(void)pthread_mutex_unlock(&__pd_lock);
	return XTC_OK;
}

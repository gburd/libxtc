/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/xtc_pdict.h
 *	Per-process dictionary — string-keyed kv store local to each
 *	xtc_proc.  Models Erlang's `put/2`, `get/1`, `erase/1`,
 *	`get_keys/0`.  Used for:
 *	  - per-proc tracing / debug names
 *	  - request context (correlation ids, span ids) carried with
 *	    a proc as it processes work
 *	  - test-time per-proc state injection
 *
 *	Implementation: tiny linked list inside `struct xtc_proc`.
 *	Linear lookup; entries usually <16 per proc.  M11.5 swaps in
 *	a small hash table once we have one.
 *
 *	All entries live in process memory and are freed at proc exit.
 *	Values are caller-owned `void *`; the store stores the pointer
 *	verbatim (no deep copy).  If a value's lifetime should match
 *	the proc, register a destructor via `xtc_pdict_put_with_dtor`.
 */

#ifndef XTC_PDICT_H
#define XTC_PDICT_H

#include <stddef.h>

#include "xtc.h"

typedef void (*xtc_pdict_dtor_fn)(void *value);

/*
 * PUBLIC: int  xtc_pdict_put __P((const char *, void *));
 * PUBLIC: int  xtc_pdict_put_with_dtor __P((const char *, void *, xtc_pdict_dtor_fn));
 * PUBLIC: int  xtc_pdict_get __P((const char *, void **));
 * PUBLIC: int  xtc_pdict_erase __P((const char *));
 * PUBLIC: int  xtc_pdict_count __P((void));
 * PUBLIC: int  xtc_pdict_clear __P((void));
 */

/* All operations apply to the calling proc's dict.  When called
 * outside any proc, all return XTC_E_INVAL. */

int  xtc_pdict_put(const char *key, void *value);
int  xtc_pdict_put_with_dtor(const char *key, void *value,
                             xtc_pdict_dtor_fn dtor);

/* Retrieve.  Sets *value if found; returns XTC_E_INVAL if no entry. */
int  xtc_pdict_get(const char *key, void **value);

/* Remove (and run destructor if any).  Returns XTC_OK if erased,
 * XTC_E_INVAL if absent. */
int  xtc_pdict_erase(const char *key);

int  xtc_pdict_count(void);
int  xtc_pdict_clear(void);

#endif /* XTC_PDICT_H */

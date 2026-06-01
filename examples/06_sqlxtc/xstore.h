/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xstore.h
 *	SQLite virtual-table module backed by the libxtc-native B-tree
 *	storage engine.  See xstore.c.
 */

#ifndef SQLXTC_XSTORE_H
#define SQLXTC_XSTORE_H

struct sqlite3;
typedef struct bt bt_t;

/* Register the "xstore" virtual-table module on `db`, backed by the
 * shared engine B-tree `bt`.  Thereafter:
 *	CREATE VIRTUAL TABLE t USING xstore;
 * creates a t(k INTEGER PRIMARY KEY, v) table whose rows live in `bt`
 * (the cooling buffer pool, larger-than-RAM capable) rather than in
 * SQLite's built-in B-tree. */
int xstore_register(struct sqlite3 *db, bt_t *bt);

#endif /* SQLXTC_XSTORE_H */

/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/admin.h
 *	Server-side admin commands.  These are recognised in the
 *	QUACK_MSG_QUERY path before the SQL engine sees the text, and
 *	stream their results through the same Quack result-set shape a
 *	normal query uses (cols header, data rows, done status).
 */

#ifndef SQLXTC_ADMIN_H
#define SQLXTC_ADMIN_H

#include "quack.h"

/* Emit a "SHOW PROCESSES" result set into *out: one row per live xtc
 * proc, with columns pid ("L.l.g"), state, mbox depth, mbox peak, and
 * alive.  Mirrors db_exec_params' Quack emission.  Returns 0 on
 * success, -1 on encode failure. */
int admin_show_processes(quack_buf_t *out);

#endif /* SQLXTC_ADMIN_H */

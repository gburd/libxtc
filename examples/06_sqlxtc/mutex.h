/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/mutex.h
 *	The xtc_amutex-backed engine mutex methods.  Returned as an
 *	opaque pointer (the engine's mutex-methods table) so callers
 *	need not name the vendored-engine type; pass it to
 *	sx_config_mutex().
 */

#ifndef SQLXTC_MUTEX_H
#define SQLXTC_MUTEX_H

const void *mutex_methods(void);

#endif /* SQLXTC_MUTEX_H */

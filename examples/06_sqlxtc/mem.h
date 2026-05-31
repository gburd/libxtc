/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/mem.h
 *	The xtc-backed engine memory allocator.  Returned as an opaque
 *	pointer (the engine's mem-methods table) so callers need not
 *	name the vendored-engine type; pass it to sx_config_mem().
 */

#ifndef SQLXTC_MEM_H
#define SQLXTC_MEM_H

const void *mem_methods(void);

#endif /* SQLXTC_MEM_H */

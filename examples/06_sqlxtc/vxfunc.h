/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/vxfunc.h
 *	Scalar SQL functions for vexec that need more than a few lines:
 *	date/time (date, time, datetime, julianday, unixepoch, strftime),
 *	printf/format, quote, hex, char, unicode, concat, concat_ws, iif,
 *	sign and the math set.  Semantics follow SQLite 3.46 (the
 *	differential oracle).  vexec compiles a call to one of these into a
 *	VXF_EXT node and calls vxf_eval() per row.
 */

#ifndef SQLXTC_VXFUNC_H
#define SQLXTC_VXFUNC_H

#include <stddef.h>
#include <stdint.h>

#include "vexec.h"   /* vx_cell_t */

/* Result bytes are allocated through this (vexec passes its arena). */
typedef void *(*vxf_alloc_fn)(void *ctx, size_t n);

/* Static result class of a function (for vexec's comparison gates). */
enum vxf_class { VXF_C_DYN = 0, VXF_C_TEXT, VXF_C_NUM };

/* The function id (> 0) for name/nargs; 0 if unknown; -1 if the name is
 * known but nargs is wrong (SQLite: "wrong number of arguments"). */
int vxf_lookup(const char *name, size_t len, int nargs);
enum vxf_class vxf_class_of(int id);

/* 1 if the function reads the current time when given these literal
 * arguments: no argument, or a first argument that is not a constant
 * other than 'now' / 'subsec' (args[i] == NULL = not a literal). */
int vxf_wants_now(int id, const vx_cell_t *const *args, int nargs);

/* 1 if argument argi of function id can be evaluated exactly: `lit` is
 * its value when it is a constant (else NULL), `maybe_real` is 1 when a
 * non-constant argument could be a REAL.  (quote() of a REAL is exact
 * only for values that round-trip in 15 significant digits.) */
int vxf_arg_ok(int id, int argi, const vx_cell_t *lit, int maybe_real);

/* 1 if a literal argument is a 'localtime' / 'utc' modifier, which this
 * port does not implement (it would read the TZ environment). */
int vxf_unsupported_mod(int id, const vx_cell_t *const *args, int nargs);

void vxf_eval(int id, const vx_cell_t *args, int nargs, int64_t now_ms,
              vxf_alloc_fn alloc, void *actx, vx_cell_t *out);

/* REAL -> TEXT exactly as SQLite does it ("%!.15g").  cap >= 32. */
int vxf_real_text(double r, char *buf, size_t cap);

/* Unix milliseconds for 'now'.  xtc_clock_mono plus a one-shot epoch
 * offset (the xstore.c hlc_phys_ms pattern), so under the simulator it
 * advances with the virtual clock. */
int64_t vxf_now_ms(void);

#endif /* SQLXTC_VXFUNC_H */

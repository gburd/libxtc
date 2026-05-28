/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/quack.h
 *	The sqlxtc wire protocol (Quack): line-delimited JSON.  See
 *	PROTOCOL.md for the spec.  This header exposes a hand-rolled
 *	JSON encoder/decoder limited to the small set of fields the
 *	protocol uses.  The full RFC 8259 is not implemented; we
 *	accept what well-formed clients send and reject everything
 *	else with a clear error.
 */

#ifndef SQLXTC_QUACK_H
#define SQLXTC_QUACK_H

#include <stddef.h>
#include <stdint.h>

/* ---- decoder ---- */

typedef enum quack_msg_kind {
	QUACK_MSG_NONE = 0,
	QUACK_MSG_QUERY,        /* {"q":...,"limit":N} */
	QUACK_MSG_PING,         /* {"ping":1} */
	QUACK_MSG_QUIT,         /* {"quit":1} */
	QUACK_MSG_UNKNOWN       /* well-formed JSON, unknown top-level key */
} quack_msg_kind_t;

typedef struct quack_msg {
	quack_msg_kind_t kind;
	const char      *q;          /* QUERY: pointer into caller buffer */
	size_t           q_len;
	int64_t          limit;      /* QUERY: 0 if absent */
	const char      *err;        /* parse error message; NULL on OK */
} quack_msg_t;

/* Parse one line.  `line` need NOT be NUL-terminated; `len` is the
 * length up to but not including the terminating '\n' (which the
 * caller has already stripped).  On success returns 0 and fills
 * *msg.  On JSON-level failure returns -1 with msg->err set to a
 * static string describing the failure. */
int quack_parse(const char *line, size_t len, quack_msg_t *msg);

/* ---- encoder ---- */

/* A grow-on-demand line buffer used by the connection's write side.
 * Backed by realloc; capacity is a soft hint. */
typedef struct quack_buf {
	char  *p;
	size_t len;
	size_t cap;
} quack_buf_t;

int  quack_buf_init(quack_buf_t *b, size_t initial_cap);
void quack_buf_free(quack_buf_t *b);
void quack_buf_reset(quack_buf_t *b);

/* Append helpers: each ends the message with a newline.  Returns 0
 * on success or -1 on allocation failure. */

int quack_emit_hello(quack_buf_t *b, const char *version);
int quack_emit_pong(quack_buf_t *b);
int quack_emit_err(quack_buf_t *b, const char *msg);

int quack_emit_cols_begin(quack_buf_t *b);
int quack_emit_cols_name(quack_buf_t *b, int i, const char *name);
int quack_emit_cols_end(quack_buf_t *b);

int quack_emit_row_begin(quack_buf_t *b);
int quack_emit_row_int(quack_buf_t *b, int i, int64_t v);
int quack_emit_row_double(quack_buf_t *b, int i, double v);
int quack_emit_row_text(quack_buf_t *b, int i, const char *s, size_t n);
int quack_emit_row_null(quack_buf_t *b, int i);
int quack_emit_row_blob(quack_buf_t *b, int i, const void *p, size_t n);
int quack_emit_row_end(quack_buf_t *b);

int quack_emit_done(quack_buf_t *b, int64_t n_rows);

/* ---- escape helpers (visible for testing) ---- */

/* Append an escaped JSON string ("...") for input s/n. */
int quack_append_jstring(quack_buf_t *b, const char *s, size_t n);

/* Append a raw byte sequence. */
int quack_append_raw(quack_buf_t *b, const char *s, size_t n);

#endif /* SQLXTC_QUACK_H */

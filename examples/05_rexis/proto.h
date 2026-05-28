/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/proto.h
 *	RESP2/RESP3 protocol parser and response builder.
 *	No external dependencies; xtc + libc only.
 */

#ifndef REXIS_PROTO_H
#define REXIS_PROTO_H

#include <stddef.h>
#include <stdint.h>

/*
 * RESP data types (prefix bytes).
 */
#define RESP_SIMPLE_STRING  '+'
#define RESP_ERROR          '-'
#define RESP_INTEGER        ':'
#define RESP_BULK_STRING    '$'
#define RESP_ARRAY          '*'
#define RESP_NULL           '_'    /* RESP3 */
#define RESP_BOOLEAN        '#'    /* RESP3 */
#define RESP_MAP            '%'    /* RESP3 */

/*
 * Parser error codes.
 */
typedef enum resp_err {
	RESP_OK             =  0,
	RESP_NEED_MORE      =  1,   /* incomplete frame; wait for data */
	RESP_ERR_PROTO      = -1,   /* malformed RESP */
	RESP_ERR_TOOLARGE   = -2,   /* buffer limit exceeded */
	RESP_ERR_INVAL      = -3    /* NULL input or invalid state */
} resp_err_t;

/*
 * Parsed value representation.  For arrays/maps, the children follow
 * inline (resp_value[0] is the array header, resp_value[1..N] are elements).
 */
typedef enum resp_type {
	RESP_TYPE_SIMPLE = 1,
	RESP_TYPE_ERROR  = 2,
	RESP_TYPE_INT    = 3,
	RESP_TYPE_BULK   = 4,
	RESP_TYPE_ARRAY  = 5,
	RESP_TYPE_NULL   = 6,
	RESP_TYPE_BOOL   = 7,
	RESP_TYPE_MAP    = 8
} resp_type_t;

typedef struct resp_value {
	resp_type_t  type;
	union {
		struct {
			const char *data;   /* points into input buffer */
			size_t      len;
		} str;
		int64_t ival;
		int     bval;           /* boolean */
		size_t  count;          /* array/map element count */
	} u;
} resp_value_t;

/*
 * Parser state for incremental parsing.
 */
typedef struct resp_parser {
	const char  *buf;
	size_t       len;
	size_t       pos;
	size_t       max_depth;     /* recursion limit; default 8 */
	size_t       max_bulk;      /* max bulk string size; default 512 MB */
} resp_parser_t;

/*
 * Initialize a parser with a buffer.  buf must remain valid until
 * parsing completes.
 */
void resp_parser_init(resp_parser_t *p, const char *buf, size_t len);

/*
 * Parse one RESP value from the current position.  On success:
 *   - Returns RESP_OK
 *   - *out is filled with the parsed value
 *   - *consumed is the number of bytes consumed from buf
 *
 * On incomplete data: returns RESP_NEED_MORE.
 * On error: returns a negative RESP_ERR_* code.
 *
 * For arrays, out->u.count holds the element count; caller must
 * call resp_parse() again out->u.count times to retrieve elements.
 */
resp_err_t resp_parse(resp_parser_t *p, resp_value_t *out, size_t *consumed);

/*
 * Parse a complete command (array of bulk strings) into argv/argc.
 * Returns RESP_OK on success, with *argc set and argv[] filled.
 * argv[i].data points into the original buffer.
 * max_args limits the argv array size.
 */
resp_err_t resp_parse_command(resp_parser_t *p,
                              resp_value_t *argv, int max_args,
                              int *argc, size_t *consumed);

/* ----- Response builder --------------------------------------------- */

/*
 * Response buffer.  Caller-owned; typically stack-allocated or from slab.
 */
typedef struct resp_buf {
	char   *data;
	size_t  cap;
	size_t  len;
} resp_buf_t;

void resp_buf_init(resp_buf_t *b, char *data, size_t cap);
void resp_buf_reset(resp_buf_t *b);

/* Append simple string: +msg\r\n */
int resp_write_simple(resp_buf_t *b, const char *msg);
int resp_write_simple_n(resp_buf_t *b, const char *msg, size_t len);

/* Append error: -ERR msg\r\n */
int resp_write_error(resp_buf_t *b, const char *msg);
int resp_write_error_n(resp_buf_t *b, const char *prefix, const char *msg);

/* Append integer: :val\r\n */
int resp_write_int(resp_buf_t *b, int64_t val);

/* Append bulk string: $len\r\ndata\r\n  (or $-1\r\n for NULL) */
int resp_write_bulk(resp_buf_t *b, const char *data, size_t len);
int resp_write_bulk_null(resp_buf_t *b);

/* Append array header: *count\r\n */
int resp_write_array(resp_buf_t *b, int count);
int resp_write_array_null(resp_buf_t *b);

/* Helpers */
int resp_write_ok(resp_buf_t *b);
int resp_write_pong(resp_buf_t *b);
int resp_write_queued(resp_buf_t *b);

/* Check remaining capacity */
size_t resp_buf_avail(const resp_buf_t *b);

#endif /* REXIS_PROTO_H */

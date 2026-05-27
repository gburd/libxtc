/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_redis/proto.c
 *	RESP2/RESP3 protocol parser and response builder.
 *	~300 LOC, no external dependencies.
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "proto.h"

/* Default limits */
#define DEFAULT_MAX_DEPTH   8
#define DEFAULT_MAX_BULK    (512UL * 1024 * 1024)

void
resp_parser_init(resp_parser_t *p, const char *buf, size_t len)
{
	p->buf = buf;
	p->len = len;
	p->pos = 0;
	p->max_depth = DEFAULT_MAX_DEPTH;
	p->max_bulk = DEFAULT_MAX_BULK;
}

/*
 * Find CRLF from current position.  Returns offset from p->pos or -1.
 */
static int
find_crlf(const resp_parser_t *p)
{
	size_t i;
	for (i = p->pos; i + 1 < p->len; i++) {
		if (p->buf[i] == '\r' && p->buf[i + 1] == '\n')
			return (int)(i - p->pos);
	}
	return -1;
}

/*
 * Parse an integer from the line (after the type prefix).
 */
static resp_err_t
parse_int_line(const char *start, size_t len, int64_t *out)
{
	int64_t val = 0;
	int neg = 0;
	size_t i = 0;

	if (len == 0)
		return RESP_ERR_PROTO;

	if (start[0] == '-') {
		neg = 1;
		i = 1;
	} else if (start[0] == '+') {
		i = 1;
	}

	if (i >= len)
		return RESP_ERR_PROTO;

	for (; i < len; i++) {
		if (!isdigit((unsigned char)start[i]))
			return RESP_ERR_PROTO;
		val = val * 10 + (start[i] - '0');
		if (val < 0)   /* overflow */
			return RESP_ERR_PROTO;
	}

	*out = neg ? -val : val;
	return RESP_OK;
}

resp_err_t
resp_parse(resp_parser_t *p, resp_value_t *out, size_t *consumed)
{
	int crlf_off;
	int64_t ival;
	resp_err_t rc;
	size_t start_pos;

	if (!p || !out || !consumed)
		return RESP_ERR_INVAL;

	if (p->pos >= p->len)
		return RESP_NEED_MORE;

	start_pos = p->pos;
	crlf_off = find_crlf(p);
	if (crlf_off < 0)
		return RESP_NEED_MORE;

	switch (p->buf[p->pos]) {
	case RESP_SIMPLE_STRING:
		out->type = RESP_TYPE_SIMPLE;
		out->u.str.data = p->buf + p->pos + 1;
		out->u.str.len = (size_t)crlf_off - 1;
		p->pos += (size_t)crlf_off + 2;
		break;

	case RESP_ERROR:
		out->type = RESP_TYPE_ERROR;
		out->u.str.data = p->buf + p->pos + 1;
		out->u.str.len = (size_t)crlf_off - 1;
		p->pos += (size_t)crlf_off + 2;
		break;

	case RESP_INTEGER:
		rc = parse_int_line(p->buf + p->pos + 1,
		                    (size_t)crlf_off - 1, &ival);
		if (rc != RESP_OK)
			return rc;
		out->type = RESP_TYPE_INT;
		out->u.ival = ival;
		p->pos += (size_t)crlf_off + 2;
		break;

	case RESP_BULK_STRING:
		rc = parse_int_line(p->buf + p->pos + 1,
		                    (size_t)crlf_off - 1, &ival);
		if (rc != RESP_OK)
			return rc;
		if (ival == -1) {
			out->type = RESP_TYPE_NULL;
			p->pos += (size_t)crlf_off + 2;
		} else {
			if (ival < 0 || (size_t)ival > p->max_bulk)
				return RESP_ERR_TOOLARGE;
			/* Need len bytes + \r\n */
			p->pos += (size_t)crlf_off + 2;
			if (p->pos + (size_t)ival + 2 > p->len) {
				p->pos = start_pos;
				return RESP_NEED_MORE;
			}
			if (p->buf[p->pos + (size_t)ival] != '\r' ||
			    p->buf[p->pos + (size_t)ival + 1] != '\n')
				return RESP_ERR_PROTO;
			out->type = RESP_TYPE_BULK;
			out->u.str.data = p->buf + p->pos;
			out->u.str.len = (size_t)ival;
			p->pos += (size_t)ival + 2;
		}
		break;

	case RESP_ARRAY:
		rc = parse_int_line(p->buf + p->pos + 1,
		                    (size_t)crlf_off - 1, &ival);
		if (rc != RESP_OK)
			return rc;
		if (ival == -1) {
			out->type = RESP_TYPE_NULL;
		} else {
			if (ival < 0)
				return RESP_ERR_PROTO;
			out->type = RESP_TYPE_ARRAY;
			out->u.count = (size_t)ival;
		}
		p->pos += (size_t)crlf_off + 2;
		break;

	case RESP_NULL:   /* RESP3 null */
		out->type = RESP_TYPE_NULL;
		p->pos += (size_t)crlf_off + 2;
		break;

	case RESP_BOOLEAN:   /* RESP3 #t or #f */
		if (crlf_off < 2)
			return RESP_ERR_PROTO;
		out->type = RESP_TYPE_BOOL;
		out->u.bval = (p->buf[p->pos + 1] == 't') ? 1 : 0;
		p->pos += (size_t)crlf_off + 2;
		break;

	case RESP_MAP:   /* RESP3 map */
		rc = parse_int_line(p->buf + p->pos + 1,
		                    (size_t)crlf_off - 1, &ival);
		if (rc != RESP_OK)
			return rc;
		if (ival < 0)
			return RESP_ERR_PROTO;
		out->type = RESP_TYPE_MAP;
		out->u.count = (size_t)ival;
		p->pos += (size_t)crlf_off + 2;
		break;

	default:
		return RESP_ERR_PROTO;
	}

	*consumed = p->pos - start_pos;
	return RESP_OK;
}

resp_err_t
resp_parse_command(resp_parser_t *p, resp_value_t *argv, int max_args,
                   int *argc, size_t *consumed)
{
	resp_value_t header;
	size_t bytes = 0, elem_bytes;
	resp_err_t rc;
	int i;

	if (!p || !argv || !argc || !consumed)
		return RESP_ERR_INVAL;

	/* Parse array header */
	rc = resp_parse(p, &header, &elem_bytes);
	if (rc != RESP_OK)
		return rc;
	bytes += elem_bytes;

	if (header.type == RESP_TYPE_NULL) {
		*argc = 0;
		*consumed = bytes;
		return RESP_OK;
	}
	if (header.type != RESP_TYPE_ARRAY)
		return RESP_ERR_PROTO;
	if ((int)header.u.count > max_args)
		return RESP_ERR_TOOLARGE;

	/* Parse each element */
	for (i = 0; i < (int)header.u.count; i++) {
		rc = resp_parse(p, &argv[i], &elem_bytes);
		if (rc != RESP_OK)
			return rc;
		bytes += elem_bytes;
		/* Commands must be bulk strings */
		if (argv[i].type != RESP_TYPE_BULK &&
		    argv[i].type != RESP_TYPE_SIMPLE)
			return RESP_ERR_PROTO;
	}

	*argc = (int)header.u.count;
	*consumed = bytes;
	return RESP_OK;
}

/* ----- Response builder --------------------------------------------- */

void
resp_buf_init(resp_buf_t *b, char *data, size_t cap)
{
	b->data = data;
	b->cap = cap;
	b->len = 0;
}

void
resp_buf_reset(resp_buf_t *b)
{
	b->len = 0;
}

size_t
resp_buf_avail(const resp_buf_t *b)
{
	return b->cap - b->len;
}

static int
buf_append(resp_buf_t *b, const char *s, size_t n)
{
	if (b->len + n > b->cap)
		return -1;
	memcpy(b->data + b->len, s, n);
	b->len += n;
	return 0;
}

static int
buf_append_crlf(resp_buf_t *b)
{
	return buf_append(b, "\r\n", 2);
}

int
resp_write_simple(resp_buf_t *b, const char *msg)
{
	return resp_write_simple_n(b, msg, strlen(msg));
}

int
resp_write_simple_n(resp_buf_t *b, const char *msg, size_t len)
{
	if (buf_append(b, "+", 1) < 0)
		return -1;
	if (buf_append(b, msg, len) < 0)
		return -1;
	return buf_append_crlf(b);
}

int
resp_write_error(resp_buf_t *b, const char *msg)
{
	return resp_write_error_n(b, "ERR", msg);
}

int
resp_write_error_n(resp_buf_t *b, const char *prefix, const char *msg)
{
	if (buf_append(b, "-", 1) < 0)
		return -1;
	if (buf_append(b, prefix, strlen(prefix)) < 0)
		return -1;
	if (buf_append(b, " ", 1) < 0)
		return -1;
	if (buf_append(b, msg, strlen(msg)) < 0)
		return -1;
	return buf_append_crlf(b);
}

int
resp_write_int(resp_buf_t *b, int64_t val)
{
	char tmp[32];
	int n;

	n = snprintf(tmp, sizeof tmp, ":%lld\r\n", (long long)val);
	if (n <= 0 || (size_t)n >= sizeof tmp)
		return -1;
	return buf_append(b, tmp, (size_t)n);
}

int
resp_write_bulk(resp_buf_t *b, const char *data, size_t len)
{
	char hdr[32];
	int n;

	n = snprintf(hdr, sizeof hdr, "$%zu\r\n", len);
	if (n <= 0 || (size_t)n >= sizeof hdr)
		return -1;
	if (buf_append(b, hdr, (size_t)n) < 0)
		return -1;
	if (buf_append(b, data, len) < 0)
		return -1;
	return buf_append_crlf(b);
}

int
resp_write_bulk_null(resp_buf_t *b)
{
	return buf_append(b, "$-1\r\n", 5);
}

int
resp_write_array(resp_buf_t *b, int count)
{
	char hdr[32];
	int n;

	n = snprintf(hdr, sizeof hdr, "*%d\r\n", count);
	if (n <= 0 || (size_t)n >= sizeof hdr)
		return -1;
	return buf_append(b, hdr, (size_t)n);
}

int
resp_write_array_null(resp_buf_t *b)
{
	return buf_append(b, "*-1\r\n", 5);
}

int
resp_write_ok(resp_buf_t *b)
{
	return buf_append(b, "+OK\r\n", 5);
}

int
resp_write_pong(resp_buf_t *b)
{
	return buf_append(b, "+PONG\r\n", 7);
}

int
resp_write_queued(resp_buf_t *b)
{
	return buf_append(b, "+QUEUED\r\n", 9);
}

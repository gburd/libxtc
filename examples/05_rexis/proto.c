/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/proto.c
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
#define DEFAULT_MAX_INLINE  (64UL * 1024)          /* Redis PROTO_INLINE_MAX_SIZE */

void
resp_parser_init(resp_parser_t *p, const char *buf, size_t len)
{
	p->buf = buf;
	p->len = len;
	p->pos = 0;
	p->max_depth = DEFAULT_MAX_DEPTH;
	p->max_bulk = DEFAULT_MAX_BULK;
	p->max_inline = DEFAULT_MAX_INLINE;
}

/*
 * Find CRLF from current position.  Returns offset from p->pos, -1 if
 * it is not buffered yet, or -2 if no line can end within max_inline
 * bytes.  The bound matters: a type line ("*3", "$5", "+OK") is short,
 * and without it a client that never sends the CRLF made every read
 * rescan the whole (growing) buffer -- quadratic CPU.
 */
static int
find_crlf(const resp_parser_t *p)
{
	size_t i, end = p->len;

	if (end - p->pos > p->max_inline + 2)
		end = p->pos + p->max_inline + 2;
	for (i = p->pos; i + 1 < end; i++) {
		if (p->buf[i] == '\r' && p->buf[i + 1] == '\n')
			return (int)(i - p->pos);
	}
	return end < p->len ? -2 : -1;
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
		int d;

		if (!isdigit((unsigned char)start[i]))
			return RESP_ERR_PROTO;
		d = start[i] - '0';
		/* Check BEFORE multiplying: val * 10 + d past INT64_MAX is
		 * signed overflow (UB), and in practice it wrapped to a
		 * positive garbage value that a "val < 0" test after the fact
		 * did not catch (":57386900000057386930" parsed as
		 * 2046667778928732082). */
		if (val > (INT64_MAX - d) / 10)
			return RESP_ERR_PROTO;
		val = val * 10 + d;
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
	if (crlf_off == -2)
		return RESP_ERR_TOOLARGE;
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
			if (ival < 0)          /* only -1 (null) is legal */
				return RESP_ERR_PROTO;
			if ((size_t)ival > p->max_bulk)
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

/* One inline command line at p->pos (see resp_parse_command). */
static resp_err_t
parse_inline(resp_parser_t *p, resp_value_t *argv, int max_args,
             int *argc, size_t *consumed)
{
	const char *s = p->buf + p->pos, *nl;
	size_t avail = p->len - p->pos, line, i, start;
	int n = 0;

	if ((nl = memchr(s, '\n', avail)) == NULL)
		return avail > p->max_inline ? RESP_ERR_TOOLARGE : RESP_NEED_MORE;
	line = (size_t)(nl - s);
	if (line > p->max_inline)
		return RESP_ERR_TOOLARGE;
	*consumed = line + 1;
	if (line > 0 && s[line - 1] == '\r')
		line--;
	for (i = 0; i < line;) {
		while (i < line && (s[i] == ' ' || s[i] == '\t'))
			i++;
		if (i == line)
			break;
		start = i;
		while (i < line && s[i] != ' ' && s[i] != '\t')
			i++;
		if (n == max_args)
			return RESP_ERR_TOOLARGE;
		argv[n].type = RESP_TYPE_BULK;
		argv[n].u.str.data = s + start;
		argv[n].u.str.len = i - start;
		n++;
	}
	p->pos += *consumed;
	*argc = n;
	return RESP_OK;
}

resp_err_t
resp_parse_command(resp_parser_t *p, resp_value_t *argv, int max_args,
                   int *argc, size_t *consumed)
{
	resp_value_t header;
	size_t bytes = 0, elem_bytes, start;
	resp_err_t rc;
	size_t i;

	if (!p || !argv || !argc || !consumed || max_args < 0)
		return RESP_ERR_INVAL;
	if (p->pos >= p->len)
		return RESP_NEED_MORE;
	if (p->buf[p->pos] != RESP_ARRAY)
		return parse_inline(p, argv, max_args, argc, consumed);

	/* On anything but RESP_OK, p->pos is left at the start of the
	 * command, so a caller can keep [pos, len) as the unconsumed tail
	 * and retry when more bytes arrive.  (It used to be left wherever
	 * the parse stopped -- after the array header or a complete
	 * element -- which is only safe for a caller that re-parses from
	 * the beginning of its buffer every time.) */
	start = p->pos;

	/* Parse array header */
	rc = resp_parse(p, &header, &elem_bytes);
	if (rc != RESP_OK)
		goto fail;
	bytes += elem_bytes;

	if (header.type == RESP_TYPE_NULL) {
		*argc = 0;
		*consumed = bytes;
		return RESP_OK;
	}
	if (header.type != RESP_TYPE_ARRAY) {
		rc = RESP_ERR_PROTO;
		goto fail;
	}
	if (header.u.count > (size_t)max_args) {
		rc = RESP_ERR_TOOLARGE;
		goto fail;
	}

	/* Parse each element */
	for (i = 0; i < header.u.count; i++) {
		rc = resp_parse(p, &argv[i], &elem_bytes);
		if (rc != RESP_OK)
			goto fail;
		bytes += elem_bytes;
		/* Commands must be bulk strings */
		if (argv[i].type != RESP_TYPE_BULK &&
		    argv[i].type != RESP_TYPE_SIMPLE) {
			rc = RESP_ERR_PROTO;
			goto fail;
		}
	}

	*argc = (int)header.u.count;
	*consumed = bytes;
	return RESP_OK;
fail:
	p->pos = start;
	return rc;
}

/* ----- Response builder --------------------------------------------- */

void
resp_buf_init(resp_buf_t *b, char *data, size_t cap)
{
	b->data = data;
	b->cap = cap;
	b->len = 0;
	b->err = 0;
	b->grow = NULL;
	b->grow_arg = NULL;
}

void
resp_buf_reset(resp_buf_t *b)
{
	b->len = 0;
	b->err = 0;
}

size_t
resp_buf_avail(const resp_buf_t *b)
{
	return b->cap - b->len;
}

static int
buf_append(resp_buf_t *b, const char *s, size_t n)
{
	if (b->err)
		return -1;
	if (n > b->cap - b->len &&
	    (b->grow == NULL || n > SIZE_MAX - b->len ||
	     b->grow(b, b->len + n) != 0 || n > b->cap - b->len)) {
		b->err = 1;
		return -1;
	}
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
resp_write_raw(resp_buf_t *b, const void *data, size_t len)
{
	return buf_append(b, data, len);
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
resp_write_bulk_hdr(resp_buf_t *b, size_t len)
{
	char hdr[32];
	int n;

	n = snprintf(hdr, sizeof hdr, "$%zu\r\n", len);
	if (n <= 0 || (size_t)n >= sizeof hdr)
		return -1;
	return buf_append(b, hdr, (size_t)n);
}

int
resp_write_crlf(resp_buf_t *b)
{
	return buf_append_crlf(b);
}

int
resp_write_bulk_null(resp_buf_t *b)
{
	return buf_append(b, "$-1\r\n", 5);
}

int
resp_write_array(resp_buf_t *b, size_t count)
{
	char hdr[32];
	int n;

	n = snprintf(hdr, sizeof hdr, "*%zu\r\n", count);
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

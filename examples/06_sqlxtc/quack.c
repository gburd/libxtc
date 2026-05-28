/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/quack.c
 *	Hand-rolled JSON parser/encoder for the Quack wire protocol.
 *	~400 LOC, no external deps.  Handles only the fixed set of
 *	objects the protocol uses; unknown shapes are rejected.
 */

#include "quack.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ */
/*  buffer                                                       */
/* ============================================================ */

int
quack_buf_init(quack_buf_t *b, size_t initial_cap)
{
	if (initial_cap < 64) initial_cap = 64;
	b->p = (char *)malloc(initial_cap);
	if (!b->p) return -1;
	b->len = 0;
	b->cap = initial_cap;
	return 0;
}

void
quack_buf_free(quack_buf_t *b)
{
	if (!b) return;
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

void
quack_buf_reset(quack_buf_t *b)
{
	b->len = 0;
}

static int
buf_grow(quack_buf_t *b, size_t need)
{
	size_t newcap;
	char  *np;
	if (b->len + need <= b->cap)
		return 0;
	newcap = b->cap ? b->cap : 64;
	while (newcap < b->len + need) newcap *= 2;
	np = (char *)realloc(b->p, newcap);
	if (!np) return -1;
	b->p = np;
	b->cap = newcap;
	return 0;
}

int
quack_append_raw(quack_buf_t *b, const char *s, size_t n)
{
	if (buf_grow(b, n) < 0) return -1;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	return 0;
}

static int
buf_putc(quack_buf_t *b, char c)
{
	if (buf_grow(b, 1) < 0) return -1;
	b->p[b->len++] = c;
	return 0;
}

static int
buf_putcstr(quack_buf_t *b, const char *s)
{
	return quack_append_raw(b, s, strlen(s));
}

/* ============================================================ */
/*  JSON string append                                           */
/* ============================================================ */

int
quack_append_jstring(quack_buf_t *b, const char *s, size_t n)
{
	size_t i;
	if (buf_putc(b, '"') < 0) return -1;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		char esc[8];
		switch (c) {
		case '"':  if (quack_append_raw(b, "\\\"", 2) < 0) return -1; break;
		case '\\': if (quack_append_raw(b, "\\\\", 2) < 0) return -1; break;
		case '\b': if (quack_append_raw(b, "\\b",  2) < 0) return -1; break;
		case '\f': if (quack_append_raw(b, "\\f",  2) < 0) return -1; break;
		case '\n': if (quack_append_raw(b, "\\n",  2) < 0) return -1; break;
		case '\r': if (quack_append_raw(b, "\\r",  2) < 0) return -1; break;
		case '\t': if (quack_append_raw(b, "\\t",  2) < 0) return -1; break;
		default:
			if (c < 0x20) {
				int k = snprintf(esc, sizeof esc, "\\u%04x", c);
				if (quack_append_raw(b, esc, (size_t)k) < 0)
					return -1;
			} else {
				if (buf_putc(b, (char)c) < 0) return -1;
			}
		}
	}
	if (buf_putc(b, '"') < 0) return -1;
	return 0;
}

/* ============================================================ */
/*  emit                                                         */
/* ============================================================ */

int
quack_emit_hello(quack_buf_t *b, const char *version)
{
	int rc = 0;
	rc |= buf_putcstr(b, "{\"hello\":\"sqlxtc\",\"version\":\"");
	rc |= buf_putcstr(b, version);
	rc |= buf_putcstr(b, "\",\"quack\":1}\n");
	return rc < 0 ? -1 : 0;
}

int
quack_emit_pong(quack_buf_t *b)
{
	return buf_putcstr(b, "{\"pong\":1}\n");
}

int
quack_emit_err(quack_buf_t *b, const char *msg)
{
	int rc = 0;
	rc |= buf_putcstr(b, "{\"err\":");
	rc |= quack_append_jstring(b, msg, strlen(msg));
	rc |= buf_putcstr(b, "}\n");
	return rc < 0 ? -1 : 0;
}

int
quack_emit_cols_begin(quack_buf_t *b)
{
	return buf_putcstr(b, "{\"cols\":[");
}

int
quack_emit_cols_name(quack_buf_t *b, int i, const char *name)
{
	int rc = 0;
	if (i > 0) rc |= buf_putc(b, ',');
	rc |= quack_append_jstring(b, name ? name : "",
	                           name ? strlen(name) : 0);
	return rc < 0 ? -1 : 0;
}

int
quack_emit_cols_end(quack_buf_t *b)
{
	return buf_putcstr(b, "]}\n");
}

int
quack_emit_row_begin(quack_buf_t *b)
{
	return buf_putcstr(b, "{\"row\":[");
}

int
quack_emit_row_int(quack_buf_t *b, int i, int64_t v)
{
	char tmp[32];
	int  rc = 0;
	int  k = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
	if (i > 0) rc |= buf_putc(b, ',');
	rc |= quack_append_raw(b, tmp, (size_t)k);
	return rc < 0 ? -1 : 0;
}

int
quack_emit_row_double(quack_buf_t *b, int i, double v)
{
	char tmp[64];
	int  rc = 0;
	int  k;
	if (i > 0) rc |= buf_putc(b, ',');
	if (v != v || v == 1.0/0.0 || v == -1.0/0.0) {
		rc |= buf_putcstr(b, "null");
	} else {
		k = snprintf(tmp, sizeof tmp, "%.17g", v);
		rc |= quack_append_raw(b, tmp, (size_t)k);
	}
	return rc < 0 ? -1 : 0;
}

int
quack_emit_row_text(quack_buf_t *b, int i, const char *s, size_t n)
{
	int rc = 0;
	if (i > 0) rc |= buf_putc(b, ',');
	rc |= quack_append_jstring(b, s, n);
	return rc < 0 ? -1 : 0;
}

int
quack_emit_row_null(quack_buf_t *b, int i)
{
	int rc = 0;
	if (i > 0) rc |= buf_putc(b, ',');
	rc |= buf_putcstr(b, "null");
	return rc < 0 ? -1 : 0;
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int
quack_emit_row_blob(quack_buf_t *b, int i, const void *p, size_t n)
{
	const unsigned char *bytes = (const unsigned char *)p;
	size_t out_len = 4 * ((n + 2) / 3);
	size_t j;
	int    rc = 0;
	if (i > 0) rc |= buf_putc(b, ',');
	rc |= buf_putc(b, '"');
	if (buf_grow(b, out_len) < 0) return -1;
	for (j = 0; j + 3 <= n; j += 3) {
		uint32_t w = ((uint32_t)bytes[j] << 16) |
		             ((uint32_t)bytes[j+1] << 8) |
		              (uint32_t)bytes[j+2];
		b->p[b->len++] = base64_alphabet[(w >> 18) & 0x3f];
		b->p[b->len++] = base64_alphabet[(w >> 12) & 0x3f];
		b->p[b->len++] = base64_alphabet[(w >>  6) & 0x3f];
		b->p[b->len++] = base64_alphabet[ w        & 0x3f];
	}
	if (j < n) {
		uint32_t w = (uint32_t)bytes[j] << 16;
		if (j + 1 < n) w |= (uint32_t)bytes[j+1] << 8;
		b->p[b->len++] = base64_alphabet[(w >> 18) & 0x3f];
		b->p[b->len++] = base64_alphabet[(w >> 12) & 0x3f];
		b->p[b->len++] = (j + 1 < n) ?
		    base64_alphabet[(w >> 6) & 0x3f] : '=';
		b->p[b->len++] = '=';
	}
	rc |= buf_putc(b, '"');
	return rc < 0 ? -1 : 0;
}

int
quack_emit_row_end(quack_buf_t *b)
{
	return buf_putcstr(b, "]}\n");
}

int
quack_emit_done(quack_buf_t *b, int64_t n_rows)
{
	char tmp[64];
	int  k = snprintf(tmp, sizeof tmp,
	                  "{\"done\":%lld}\n", (long long)n_rows);
	return quack_append_raw(b, tmp, (size_t)k);
}

/* ============================================================ */
/*  decoder -- a tiny one-pass scanner                           */
/* ============================================================ */

typedef struct {
	const char *p;
	size_t      len;
	size_t      pos;
	const char *err;
} jp_t;

static void
jp_skip(jp_t *j)
{
	while (j->pos < j->len) {
		char c = j->p[j->pos];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			j->pos++;
		else break;
	}
}

static int
jp_peek(jp_t *j, char *out)
{
	jp_skip(j);
	if (j->pos >= j->len) return -1;
	*out = j->p[j->pos];
	return 0;
}

static int
jp_eat(jp_t *j, char c)
{
	jp_skip(j);
	if (j->pos < j->len && j->p[j->pos] == c) { j->pos++; return 0; }
	j->err = "expected punctuation";
	return -1;
}

/* Parse a JSON string from j into [out_p, out_len) (pointer into the
 * source buffer; we don't unescape -- the protocol fields we care
 * about are SQL strings where escapes are rare).  We handle escapes
 * by recording the un-escaped span via an out-buffer if requested,
 * but for this server we only consume, validate, and remember the
 * span and unescape lazily in the caller.  Here: parse and return
 * the [start,end) span between the quotes, EXCLUDING the quotes.
 *
 * Note: this does NOT unescape.  For the SQL field we want the raw
 * un-escaped contents; if the client embedded \n in JSON we have to
 * undo that.  We unescape into the same buffer in-place when needed
 * via a separate pass: see jp_string_unesc below. */
static int
jp_string_span(jp_t *j, const char **out_p, size_t *out_len)
{
	if (jp_eat(j, '"') < 0) return -1;
	*out_p = j->p + j->pos;
	while (j->pos < j->len) {
		char c = j->p[j->pos];
		if (c == '"') {
			*out_len = (size_t)((j->p + j->pos) - *out_p);
			j->pos++;
			return 0;
		}
		if (c == '\\') {
			if (j->pos + 1 >= j->len) break;
			j->pos += 2;
			continue;
		}
		j->pos++;
	}
	j->err = "unterminated string";
	return -1;
}

/* Unescape a JSON string span (which we may have copied verbatim)
 * into a freshly allocated NUL-terminated buffer.  Returns malloc'd
 * memory on success; NULL on alloc failure or invalid escape. */
static char *
jp_string_dup_unesc(const char *p, size_t n, size_t *out_len)
{
	char  *out;
	size_t i, j = 0;
	out = (char *)malloc(n + 1);
	if (!out) return NULL;
	for (i = 0; i < n; i++) {
		if (p[i] != '\\') { out[j++] = p[i]; continue; }
		if (i + 1 >= n) { free(out); return NULL; }
		switch (p[++i]) {
		case '"':  out[j++] = '"';  break;
		case '\\': out[j++] = '\\'; break;
		case '/':  out[j++] = '/';  break;
		case 'b':  out[j++] = '\b'; break;
		case 'f':  out[j++] = '\f'; break;
		case 'n':  out[j++] = '\n'; break;
		case 'r':  out[j++] = '\r'; break;
		case 't':  out[j++] = '\t'; break;
		case 'u': {
			unsigned v = 0;
			int k;
			if (i + 4 >= n) { free(out); return NULL; }
			for (k = 0; k < 4; k++) {
				char c = p[++i];
				v <<= 4;
				if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
				else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
				else { free(out); return NULL; }
			}
			/* Encode as UTF-8.  We don't handle surrogate pairs;
			 * good enough for the SQL we expect. */
			if (v < 0x80) {
				out[j++] = (char)v;
			} else if (v < 0x800) {
				out[j++] = (char)(0xC0 | (v >> 6));
				out[j++] = (char)(0x80 | (v & 0x3F));
			} else {
				out[j++] = (char)(0xE0 | (v >> 12));
				out[j++] = (char)(0x80 | ((v >> 6) & 0x3F));
				out[j++] = (char)(0x80 | (v & 0x3F));
			}
			break;
		}
		default:
			free(out);
			return NULL;
		}
	}
	out[j] = '\0';
	if (out_len) *out_len = j;
	return out;
}

static int
jp_int(jp_t *j, int64_t *out)
{
	int64_t v = 0;
	int     neg = 0;
	int     have = 0;
	jp_skip(j);
	if (j->pos < j->len && j->p[j->pos] == '-') { neg = 1; j->pos++; }
	while (j->pos < j->len) {
		char c = j->p[j->pos];
		if (c < '0' || c > '9') break;
		v = v * 10 + (c - '0');
		j->pos++;
		have = 1;
	}
	if (!have) { j->err = "expected integer"; return -1; }
	*out = neg ? -v : v;
	return 0;
}

/* Skip a JSON value (any type) without keeping it. */
static int jp_skip_value(jp_t *j);

static int
jp_skip_string(jp_t *j)
{
	const char *p; size_t n;
	return jp_string_span(j, &p, &n);
}

static int
jp_skip_value(jp_t *j)
{
	char c;
	if (jp_peek(j, &c) < 0) { j->err = "unexpected eof"; return -1; }
	if (c == '"') return jp_skip_string(j);
	if (c == '{') {
		j->pos++;
		jp_skip(j);
		if (jp_peek(j, &c) == 0 && c == '}') { j->pos++; return 0; }
		for (;;) {
			if (jp_skip_string(j) < 0) return -1;
			if (jp_eat(j, ':') < 0) return -1;
			if (jp_skip_value(j) < 0) return -1;
			jp_skip(j);
			if (j->pos < j->len && j->p[j->pos] == ',') {
				j->pos++; continue;
			}
			break;
		}
		return jp_eat(j, '}');
	}
	if (c == '[') {
		j->pos++;
		jp_skip(j);
		if (jp_peek(j, &c) == 0 && c == ']') { j->pos++; return 0; }
		for (;;) {
			if (jp_skip_value(j) < 0) return -1;
			jp_skip(j);
			if (j->pos < j->len && j->p[j->pos] == ',') {
				j->pos++; continue;
			}
			break;
		}
		return jp_eat(j, ']');
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		while (j->pos < j->len) {
			char k = j->p[j->pos];
			if ((k >= '0' && k <= '9') || k == '-' || k == '+' ||
			    k == '.' || k == 'e' || k == 'E') {
				j->pos++;
			} else break;
		}
		return 0;
	}
	if (c == 't' && j->pos + 4 <= j->len &&
	    memcmp(j->p + j->pos, "true", 4) == 0) {
		j->pos += 4; return 0;
	}
	if (c == 'f' && j->pos + 5 <= j->len &&
	    memcmp(j->p + j->pos, "false", 5) == 0) {
		j->pos += 5; return 0;
	}
	if (c == 'n' && j->pos + 4 <= j->len &&
	    memcmp(j->p + j->pos, "null", 4) == 0) {
		j->pos += 4; return 0;
	}
	j->err = "unrecognized value";
	return -1;
}

int
quack_parse(const char *line, size_t len, quack_msg_t *msg)
{
	jp_t j;
	char c;
	int  saw_q = 0, saw_ping = 0, saw_quit = 0;
	const char *q_span_p = NULL;
	size_t q_span_n = 0;
	int64_t limit = 0;
	int     q_has_escape = 0;

	memset(msg, 0, sizeof *msg);
	msg->kind = QUACK_MSG_NONE;

	j.p = line; j.len = len; j.pos = 0; j.err = NULL;

	if (jp_eat(&j, '{') < 0) {
		msg->err = "json: expected object";
		return -1;
	}
	jp_skip(&j);
	if (jp_peek(&j, &c) == 0 && c == '}') { j.pos++; goto done; }

	for (;;) {
		const char *kp; size_t kn;
		if (jp_string_span(&j, &kp, &kn) < 0) {
			msg->err = j.err ? j.err : "json: bad key";
			return -1;
		}
		if (jp_eat(&j, ':') < 0) {
			msg->err = "json: expected ':'";
			return -1;
		}
		jp_skip(&j);

		if (kn == 1 && kp[0] == 'q') {
			const char *vp; size_t vn; size_t i;
			if (jp_string_span(&j, &vp, &vn) < 0) {
				msg->err = "json: 'q' must be a string";
				return -1;
			}
			q_span_p = vp;
			q_span_n = vn;
			for (i = 0; i < vn; i++) {
				if (vp[i] == '\\') { q_has_escape = 1; break; }
			}
			saw_q = 1;
		} else if (kn == 5 && memcmp(kp, "limit", 5) == 0) {
			if (jp_int(&j, &limit) < 0) {
				msg->err = "json: 'limit' must be int";
				return -1;
			}
		} else if (kn == 4 && memcmp(kp, "ping", 4) == 0) {
			int64_t v;
			if (jp_int(&j, &v) < 0) {
				msg->err = "json: 'ping' must be int";
				return -1;
			}
			saw_ping = 1;
		} else if (kn == 4 && memcmp(kp, "quit", 4) == 0) {
			int64_t v;
			if (jp_int(&j, &v) < 0) {
				msg->err = "json: 'quit' must be int";
				return -1;
			}
			saw_quit = 1;
		} else {
			if (jp_skip_value(&j) < 0) {
				msg->err = j.err ? j.err :
				    "json: unrecognized value";
				return -1;
			}
		}

		jp_skip(&j);
		if (j.pos < j.len && j.p[j.pos] == ',') {
			j.pos++; continue;
		}
		break;
	}
	if (jp_eat(&j, '}') < 0) {
		msg->err = "json: expected '}'";
		return -1;
	}

done:
	if (saw_q) {
		msg->kind = QUACK_MSG_QUERY;
		if (q_has_escape) {
			/* Allocate a fresh buffer; caller must free.  We
			 * stash the pointer in msg->q and indicate this
			 * is owned via a NUL-byte trick: callers free
			 * msg->q if msg->q != q_span_p.  In practice the
			 * caller frees the input buffer separately, so
			 * we LEAK here unless the caller calls a free
			 * helper.  Simpler: do the unescape on the
			 * caller side -- we just return a flag. */
			static __thread char *unesc_buf = NULL;
			static __thread size_t unesc_cap = 0;
			size_t out_n = 0;
			char *u = jp_string_dup_unesc(q_span_p, q_span_n,
			                              &out_n);
			if (!u) { msg->err = "json: bad escape"; return -1; }
			if (unesc_cap < out_n + 1) {
				char *nb = (char *)realloc(unesc_buf, out_n + 1);
				if (!nb) { free(u); msg->err = "oom"; return -1; }
				unesc_buf = nb;
				unesc_cap = out_n + 1;
			}
			memcpy(unesc_buf, u, out_n + 1);
			free(u);
			msg->q = unesc_buf;
			msg->q_len = out_n;
		} else {
			msg->q = q_span_p;
			msg->q_len = q_span_n;
		}
		msg->limit = limit;
		return 0;
	}
	if (saw_ping) { msg->kind = QUACK_MSG_PING; return 0; }
	if (saw_quit) { msg->kind = QUACK_MSG_QUIT; return 0; }
	msg->kind = QUACK_MSG_UNKNOWN;
	return 0;
}

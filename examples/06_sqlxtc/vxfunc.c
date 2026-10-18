/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/vxfunc.c
 *	Scalar SQL functions for vexec (see vxfunc.h).  The date/time code
 *	is a port of the algorithms in SQLite's date.c (public domain:
 *	Meeus julian-day conversion, modifier grammar, strftime set); the
 *	printf and quote/hex/char/concat semantics follow SQLite's printf.c
 *	and func.c.  Every behaviour here is differential-tested against
 *	python's sqlite3 (3.46) by test/sqlxtc/test_sqlxtc_compat.py.
 *
 *	NOT implemented (the caller declines): the 'localtime' and 'utc'
 *	modifiers (they need the TZ database / environment, a
 *	nondeterminism source), timediff(), and printf's %n.
 */

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "vxfunc.h"
#include "xtc.h"         /* xtc_clock_mono / xtc_clock_real */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum vxf_id {
	F_DATE = 1, F_TIME, F_DATETIME, F_JULIANDAY, F_UNIXEPOCH, F_STRFTIME,
	F_CDATE, F_CTIME, F_CTIMESTAMP,
	F_PRINTF, F_QUOTE, F_HEX, F_CHAR, F_UNICODE, F_CONCAT, F_CONCAT_WS,
	F_IIF, F_SIGN,
	F_CEIL, F_FLOOR, F_TRUNC, F_LN, F_LOG, F_LOG10, F_LOG2, F_EXP,
	F_POW, F_MOD, F_SQRT, F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN,
	F_ATAN2, F_SINH, F_COSH, F_TANH, F_ASINH, F_ACOSH, F_ATANH,
	F_RADIANS, F_DEGREES, F_PI
};

static const struct vxf_def {
	const char *name;
	int id, min, max;   /* max -1 = unbounded */
	enum vxf_class cls;
} k_funcs[] = {
	{ "date", F_DATE, 0, -1, VXF_C_TEXT },
	{ "time", F_TIME, 0, -1, VXF_C_TEXT },
	{ "datetime", F_DATETIME, 0, -1, VXF_C_TEXT },
	{ "julianday", F_JULIANDAY, 0, -1, VXF_C_NUM },
	{ "unixepoch", F_UNIXEPOCH, 0, -1, VXF_C_NUM },
	{ "strftime", F_STRFTIME, 1, -1, VXF_C_TEXT },
	{ "current_date", F_CDATE, 0, 0, VXF_C_TEXT },
	{ "current_time", F_CTIME, 0, 0, VXF_C_TEXT },
	{ "current_timestamp", F_CTIMESTAMP, 0, 0, VXF_C_TEXT },
	{ "printf", F_PRINTF, 1, -1, VXF_C_TEXT },
	{ "format", F_PRINTF, 1, -1, VXF_C_TEXT },
	{ "quote", F_QUOTE, 1, 1, VXF_C_TEXT },
	{ "hex", F_HEX, 1, 1, VXF_C_TEXT },
	{ "char", F_CHAR, 0, -1, VXF_C_TEXT },
	{ "unicode", F_UNICODE, 1, 1, VXF_C_NUM },
	{ "concat", F_CONCAT, 1, -1, VXF_C_TEXT },
	{ "concat_ws", F_CONCAT_WS, 2, -1, VXF_C_TEXT },
	{ "iif", F_IIF, 3, 3, VXF_C_DYN },
	{ "sign", F_SIGN, 1, 1, VXF_C_NUM },
	{ "ceil", F_CEIL, 1, 1, VXF_C_NUM },
	{ "ceiling", F_CEIL, 1, 1, VXF_C_NUM },
	{ "floor", F_FLOOR, 1, 1, VXF_C_NUM },
	{ "trunc", F_TRUNC, 1, 1, VXF_C_NUM },
	{ "ln", F_LN, 1, 1, VXF_C_NUM },
	{ "log", F_LOG, 1, 2, VXF_C_NUM },
	{ "log10", F_LOG10, 1, 1, VXF_C_NUM },
	{ "log2", F_LOG2, 1, 1, VXF_C_NUM },
	{ "exp", F_EXP, 1, 1, VXF_C_NUM },
	{ "pow", F_POW, 2, 2, VXF_C_NUM },
	{ "power", F_POW, 2, 2, VXF_C_NUM },
	{ "mod", F_MOD, 2, 2, VXF_C_NUM },
	{ "sqrt", F_SQRT, 1, 1, VXF_C_NUM },
	{ "sin", F_SIN, 1, 1, VXF_C_NUM },
	{ "cos", F_COS, 1, 1, VXF_C_NUM },
	{ "tan", F_TAN, 1, 1, VXF_C_NUM },
	{ "asin", F_ASIN, 1, 1, VXF_C_NUM },
	{ "acos", F_ACOS, 1, 1, VXF_C_NUM },
	{ "atan", F_ATAN, 1, 1, VXF_C_NUM },
	{ "atan2", F_ATAN2, 2, 2, VXF_C_NUM },
	{ "sinh", F_SINH, 1, 1, VXF_C_NUM },
	{ "cosh", F_COSH, 1, 1, VXF_C_NUM },
	{ "tanh", F_TANH, 1, 1, VXF_C_NUM },
	{ "asinh", F_ASINH, 1, 1, VXF_C_NUM },
	{ "acosh", F_ACOSH, 1, 1, VXF_C_NUM },
	{ "atanh", F_ATANH, 1, 1, VXF_C_NUM },
	{ "radians", F_RADIANS, 1, 1, VXF_C_NUM },
	{ "degrees", F_DEGREES, 1, 1, VXF_C_NUM },
	{ "pi", F_PI, 0, 0, VXF_C_NUM }
};
#define N_FUNCS ((int)(sizeof k_funcs / sizeof k_funcs[0]))

int
vxf_lookup(const char *name, size_t len, int nargs)
{
	int i;
	for (i = 0; i < N_FUNCS; i++) {
		if (strlen(k_funcs[i].name) != len ||
		    strncasecmp(k_funcs[i].name, name, len) != 0)
			continue;
		if (nargs < k_funcs[i].min ||
		    (k_funcs[i].max >= 0 && nargs > k_funcs[i].max))
			return -1;
		return k_funcs[i].id;
	}
	return 0;
}

enum vxf_class
vxf_class_of(int id)
{
	int i;
	for (i = 0; i < N_FUNCS; i++)
		if (k_funcs[i].id == id) return k_funcs[i].cls;
	return VXF_C_DYN;
}

static int
is_date_fn(int id)
{
	return id >= F_DATE && id <= F_CTIMESTAMP;
}

/* ---- the 'now' clock ---------------------------------------------- */

int64_t
vxf_now_ms(void)
{
	/* Same scheme as xstore.c hlc_phys_ms: the monotonic clock (which
	 * the simulator virtualizes) plus a wall-clock offset sampled once,
	 * so 'now' replays under DST and never runs backward. */
	static _Atomic int64_t epoch_off_ms;
	static _Atomic int     epoch_set;
	int64_t mono_ns = xtc_clock_mono();
	int64_t mono_ms = (mono_ns < 0 ? 0 : mono_ns) / 1000000LL;
	if (!atomic_load_explicit(&epoch_set, memory_order_acquire)) {
		int64_t wall_ns = xtc_clock_real();
		int64_t wall_ms = (wall_ns < 0 ? 0 : wall_ns) / 1000000LL;
		atomic_store_explicit(&epoch_off_ms, wall_ms - mono_ms,
		    memory_order_relaxed);
		atomic_store_explicit(&epoch_set, 1, memory_order_release);
	}
	return mono_ms + atomic_load_explicit(&epoch_off_ms, memory_order_relaxed);
}

/* ---- small text helpers ------------------------------------------- */

/* A growable output buffer over the caller's allocator. */
struct sbuf {
	char *p;
	size_t n, cap;
	vxf_alloc_fn alloc;
	void *actx;
	int oom;
};

static void
sb_reserve(struct sbuf *b, size_t add)
{
	char *np;
	size_t nc;
	if (b->oom || b->n + add + 1 <= b->cap) return;
	nc = b->cap ? b->cap * 2 : 64;
	while (nc < b->n + add + 1) nc *= 2;
	np = (char *)b->alloc(b->actx, nc);
	if (np == NULL) { b->oom = 1; return; }
	if (b->n) memcpy(np, b->p, b->n);
	b->p = np; b->cap = nc;
}

static void
sb_put(struct sbuf *b, const char *s, size_t n)
{
	sb_reserve(b, n);
	if (b->oom) return;
	if (n) memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = '\0';
}

static void
sb_pad(struct sbuf *b, char c, long n)
{
	for (; n > 0; n--) sb_put(b, &c, 1);
}

static void
set_text(vx_cell_t *out, struct sbuf *b)
{
	if (b->oom) { out->type = VX_NULL; return; }
	if (b->p == NULL) sb_put(b, "", 0);
	if (b->oom) { out->type = VX_NULL; return; }
	out->type = VX_TEXT;
	out->bytes = (const uint8_t *)b->p;
	out->nbytes = (uint32_t)b->n;
}

/* Text view of a non-NULL cell (numbers rendered as SQLite does).  The
 * bytes are NUL-terminated copies in tmp (cap >= 40) or the arena. */
static const char *
cell_str(const vx_cell_t *c, char *tmp, size_t cap, uint32_t *n,
         vxf_alloc_fn alloc, void *actx)
{
	char *p;
	int m;
	switch (c->type) {
	case VX_INT:
		m = snprintf(tmp, cap, "%lld", (long long)c->i);
		*n = (uint32_t)m; return tmp;
	case VX_REAL:
		m = vxf_real_text(c->r, tmp, cap);
		*n = (uint32_t)m; return tmp;
	case VX_TEXT: case VX_BLOB:
		p = (char *)alloc(actx, (size_t)c->nbytes + 1);
		if (p == NULL) { *n = 0; return NULL; }
		if (c->nbytes) memcpy(p, c->bytes, c->nbytes);
		p[c->nbytes] = '\0';
		*n = c->nbytes; return p;
	default:
		*n = 0; return NULL;
	}
}

/* SQLite sqlite3AtoF-style: parse the longest numeric prefix.  Returns
 * 1 if the whole (trimmed) string is a number, 0 if only a prefix (or
 * nothing -- *v = 0) is; *is_int set when the prefix has no '.' / 'e'. */
static int
text_num(const char *s, size_t n, double *v, int *is_int)
{
	char buf[128];
	size_t i = 0, j;
	int digits = 0, real = 0;
	*v = 0; *is_int = 1;
	while (i < n && isspace((unsigned char)s[i])) i++;
	j = i;
	if (j < n && (s[j] == '+' || s[j] == '-')) j++;
	while (j < n && isdigit((unsigned char)s[j])) { j++; digits++; }
	if (j < n && s[j] == '.') {
		real = 1; j++;
		while (j < n && isdigit((unsigned char)s[j])) { j++; digits++; }
	}
	if (digits == 0) return 0;
	if (j < n && (s[j] == 'e' || s[j] == 'E')) {
		size_t k = j + 1;
		if (k < n && (s[k] == '+' || s[k] == '-')) k++;
		if (k < n && isdigit((unsigned char)s[k])) {
			real = 1;
			while (k < n && isdigit((unsigned char)s[k])) k++;
			j = k;
		}
	}
	if (j - i >= sizeof buf) return 0;
	memcpy(buf, s + i, j - i); buf[j - i] = '\0';
	*v = strtod(buf, NULL);
	*is_int = !real;
	while (j < n && isspace((unsigned char)s[j])) j++;
	return j == n;
}

/* 1 if s starts (after spaces / a sign / a '.') with a digit. */
static int
has_num_prefix(const char *s)
{
	while (isspace((unsigned char)*s)) s++;
	if (*s == '+' || *s == '-') s++;
	if (*s == '.') s++;
	return isdigit((unsigned char)*s);
}

/* sqlite3_value_numeric_type: the numeric value of a cell, or 0 if the
 * cell is not numeric (non-numeric text / blob / NULL). */
static int
num_val(const vx_cell_t *c, double *d, int *is_int, int64_t *iv)
{
	int ii;
	double v;
	if (c->type == VX_INT) { *d = (double)c->i; *iv = c->i; *is_int = 1; return 1; }
	if (c->type == VX_REAL) { *d = c->r; *is_int = 0; return 1; }
	if (c->type == VX_TEXT && text_num((const char *)c->bytes, c->nbytes, &v, &ii)) {
		*d = v; *is_int = ii && v >= -9.2233720368547758e18 &&
		    v < 9.2233720368547758e18;
		*iv = *is_int ? (int64_t)v : 0;
		return 1;
	}
	return 0;
}

/* sqlite3_value_double / _int64 conversions (prefix parse, else 0). */
static double
as_double(const vx_cell_t *c)
{
	double v; int ii;
	if (c->type == VX_INT) return (double)c->i;
	if (c->type == VX_REAL) return c->r;
	if (c->type == VX_TEXT || c->type == VX_BLOB) {
		(void)text_num((const char *)c->bytes, c->nbytes, &v, &ii);
		return v;
	}
	return 0.0;
}

static int64_t
as_int64(const vx_cell_t *c)
{
	double d;
	if (c->type == VX_INT) return c->i;
	d = as_double(c);
	if (d != d) return 0;
	if (d >= 9.2233720368547758e18) return INT64_MAX;
	if (d <= -9.2233720368547758e18) return INT64_MIN;
	return (int64_t)d;
}

/* ---- REAL -> text (SQLite's FpDecode + %!.Ng) ---------------------- *
 *
 * SQLite decodes a double to (at most) 18 significant digits, then rounds
 * those digits HALF-UP to the requested precision -- a double rounding
 * that differs from printf's exact rounding at many ties
 * (400760634880118.5 -> "400760634880119.0").  Emulate it: take 39 exact
 * digits from the C library, round half-up to 18, then to the target. */
struct fpdec {
	char z[48];   /* significant digits, no leading zeros */
	int  n;       /* number of digits in z */
	int  dp;      /* decimal point position: value = 0.z * 10^dp */
	int  neg;
};

static void
round_digits(struct fpdec *f, int keep)
{
	int j;
	if (keep >= f->n) return;
	if (keep < 0) { f->n = 0; return; }
	if (f->z[keep] >= '5') {
		for (j = keep - 1; j >= 0; j--) {
			if (f->z[j] < '9') { f->z[j]++; break; }
			f->z[j] = '0';
		}
		if (j < 0) {   /* carry out: 999 -> 1000 */
			memmove(f->z + 1, f->z, (size_t)keep);
			f->z[0] = '1';
			f->dp++;
			keep++;
		}
	}
	f->n = keep;
}

static void
fp_decode(double r, struct fpdec *f)
{
	char buf[64];
	int e;
	f->neg = (r < 0);
	if (r < 0) r = -r;
	if (r == 0.0) { strcpy(f->z, "0"); f->n = 1; f->dp = 1; f->neg = 0; return; }
	snprintf(buf, sizeof buf, "%.38e", r);   /* d.ddd...e+XX, 39 digits */
	e = atoi(strchr(buf, 'e') + 1);
	f->z[0] = buf[0];
	memcpy(f->z + 1, buf + 2, 38);
	f->n = 39;
	f->dp = e + 1;
	round_digits(f, 18);
}

/* Drop trailing zeros (keep at least one digit). */
static void
trim_zeros(struct fpdec *f)
{
	while (f->n > 1 && f->z[f->n - 1] == '0') f->n--;
}

int
vxf_real_text(double r, char *out, size_t cap)
{
	struct fpdec f;
	char buf[64];
	int o = 0, i, e;
	if (r != r) return snprintf(out, cap, "NaN");
	if (isinf(r)) return snprintf(out, cap, "%s", r < 0 ? "-Inf" : "Inf");
	fp_decode(r, &f);
	if (f.z[0] == '0') return snprintf(out, cap, "0.0");
	round_digits(&f, 15);
	trim_zeros(&f);
	e = f.dp - 1;
	if (f.neg) buf[o++] = '-';
	if (e < -4 || e > 14) {
		buf[o++] = f.z[0];
		buf[o++] = '.';
		if (f.n > 1) { memcpy(buf + o, f.z + 1, (size_t)(f.n - 1)); o += f.n - 1; }
		else buf[o++] = '0';
		o += snprintf(buf + o, sizeof buf - (size_t)o, "e%c%02d",
		    e < 0 ? '-' : '+', e < 0 ? -e : e);
	} else if (e >= 0) {
		for (i = 0; i <= e; i++) buf[o++] = i < f.n ? f.z[i] : '0';
		buf[o++] = '.';
		if (f.n > e + 1) { memcpy(buf + o, f.z + e + 1, (size_t)(f.n - e - 1)); o += f.n - e - 1; }
		else buf[o++] = '0';
	} else {
		buf[o++] = '0'; buf[o++] = '.';
		for (i = 0; i < -e - 1; i++) buf[o++] = '0';
		memcpy(buf + o, f.z, (size_t)f.n); o += f.n;
	}
	buf[o] = '\0';
	return snprintf(out, cap, "%s", buf);
}

/* quote() of a REAL.  SQLite prints "%!0.17g", whose 3.46 digit choice
 * (an 18-digit decode, then a round-trip shortening heuristic) is not
 * reproduced here.  A value that round-trips in 15 significant digits
 * renders exactly like CAST AS TEXT; anything longer returns -1.  The
 * compiler only lets quote() through when its argument can never be such
 * a REAL (vxf_arg_ok), so the -1 path is unreachable from SQL.
 * shortcut: no 16-17 digit quote(REAL); port FpDecode's iRound==17 path
 * if a workload needs it. */
static int
real_text17(double r, char *out, size_t cap)
{
	char tmp[64];
	if (isinf(r)) return snprintf(out, cap, "%s", r < 0 ? "-9.0e+999" : "9.0e+999");
	snprintf(tmp, sizeof tmp, "%.15g", r);
	if (strtod(tmp, NULL) != r) return -1;
	return vxf_real_text(r, out, cap);
}

/* ---- date/time (port of SQLite date.c) ---------------------------- */

typedef struct {
	int64_t iJD;         /* julian day * 86400000 */
	int Y, M, D, h, m, tz;
	double s;
	char validJD, validYMD, validHMS, nFloor;
	unsigned rawS : 1, isError : 1, useSubsec : 1;
} dt_t;

#define JD_MAX 464269060799999LL   /* 9999-12-31 23:59:59.999 */

static int
get_digits(const char *z, const char *fmt, int *out[])
{
	static const int mx[] = { 12, 14, 24, 31, 59, 14712 };
	int cnt = 0;
	char next;
	do {
		int N = fmt[0] - '0', min = fmt[1] - '0', max = mx[fmt[2] - 'a'], val = 0;
		next = fmt[3];
		while (N--) {
			if (!isdigit((unsigned char)*z)) return cnt;
			val = val * 10 + (*z - '0');
			z++;
		}
		if (val < min || val > max || (next != 0 && next != *z)) return cnt;
		*out[cnt] = val;
		z++;
		cnt++;
		fmt += 4;
	} while (next);
	return cnt;
}

static int
parse_tz(const char *z, dt_t *p)
{
	int sgn = 0, hr, mn, c;
	int *o[2];
	while (isspace((unsigned char)*z)) z++;
	p->tz = 0;
	c = *z;
	if (c == '-') sgn = -1;
	else if (c == '+') sgn = 1;
	else if (c == 'Z' || c == 'z') { z++; goto zulu; }
	else return c != 0;
	z++;
	o[0] = &hr; o[1] = &mn;
	if (get_digits(z, "20b:20e", o) != 2) return 1;
	z += 5;
	p->tz = sgn * (mn + hr * 60);
zulu:
	while (isspace((unsigned char)*z)) z++;
	return *z != 0;
}

static int
parse_hms(const char *z, dt_t *p)
{
	int h, m, s = 0;
	double ms = 0.0;
	int *o[2];
	o[0] = &h; o[1] = &m;
	if (get_digits(z, "20c:20e", o) != 2) return 1;
	z += 5;
	if (*z == ':') {
		z++;
		o[0] = &s;
		if (get_digits(z, "20e", o) != 1) return 1;
		z += 2;
		if (*z == '.' && isdigit((unsigned char)z[1])) {
			double scale = 1.0;
			z++;
			while (isdigit((unsigned char)*z)) {
				ms = ms * 10.0 + (*z - '0');
				scale *= 10.0;
				z++;
			}
			ms /= scale;
			if (ms > 0.999) ms = 0.999;
		}
	}
	p->validJD = 0; p->rawS = 0; p->validHMS = 1;
	p->h = h; p->m = m; p->s = s + ms;
	return parse_tz(z, p);
}

static void
dt_error(dt_t *p)
{
	memset(p, 0, sizeof *p);
	p->isError = 1;
}

static void
compute_jd(dt_t *p)
{
	int Y, M, D, A, B, X1, X2;
	if (p->validJD) return;
	if (p->validYMD) { Y = p->Y; M = p->M; D = p->D; }
	else { Y = 2000; M = 1; D = 1; }
	if (Y < -4713 || Y > 9999 || p->rawS) { dt_error(p); return; }
	if (M <= 2) { Y--; M += 12; }
	A = (Y + 4800) / 100;
	B = 38 - A + (A / 4);
	X1 = 36525 * (Y + 4716) / 100;
	X2 = 306001 * (M + 1) / 10000;
	p->iJD = (int64_t)((X1 + X2 + D + B - 1524.5) * 86400000);
	p->validJD = 1;
	if (p->validHMS) {
		p->iJD += p->h * 3600000 + p->m * 60000 + (int64_t)(p->s * 1000 + 0.5);
		if (p->tz) {
			p->iJD -= p->tz * 60000;
			p->validYMD = 0; p->validHMS = 0; p->tz = 0;
		}
	}
}

static void
compute_floor(dt_t *p)
{
	if (p->D <= 28) p->nFloor = 0;
	else if ((1 << p->M) & 0x15aa) p->nFloor = 0;
	else if (p->M != 2) p->nFloor = (p->D == 31);
	else if (p->Y % 4 != 0 || (p->Y % 100 == 0 && p->Y % 400 != 0)) p->nFloor = (char)(p->D - 28);
	else p->nFloor = (char)(p->D - 29);
}

static int
parse_ymd(const char *z, dt_t *p)
{
	int Y, M, D, neg = 0;
	int *o[3];
	if (z[0] == '-') { z++; neg = 1; }
	o[0] = &Y; o[1] = &M; o[2] = &D;
	if (get_digits(z, "40f-21a-21d", o) != 3) return 1;
	z += 10;
	while (isspace((unsigned char)*z) || *z == 'T') z++;
	if (parse_hms(z, p) == 0) {
		/* got the time */
	} else if (*z == 0) {
		p->validHMS = 0;
	} else {
		return 1;
	}
	p->validJD = 0; p->validYMD = 1;
	p->Y = neg ? -Y : Y; p->M = M; p->D = D;
	compute_floor(p);
	if (p->tz) compute_jd(p);
	return 0;
}

static void
clear_ymd_hms(dt_t *p)
{
	p->validYMD = 0; p->validHMS = 0; p->tz = 0;
}

static void
set_now(dt_t *p, int64_t now_ms)
{
	p->iJD = now_ms + 210866760000000LL;
	p->validJD = 1;
	clear_ymd_hms(p);
}

static void
set_raw_number(dt_t *p, double r)
{
	p->s = r;
	p->rawS = 1;
	if (r >= 0.0 && r < 5373484.5) {
		p->iJD = (int64_t)(r * 86400000.0 + 0.5);
		p->validJD = 1;
	}
}

static int
parse_date_or_time(const char *z, dt_t *p, int64_t now_ms)
{
	double r;
	int ii;
	if (parse_ymd(z, p) == 0) return 0;
	if (parse_hms(z, p) == 0) return 0;
	if (strcasecmp(z, "now") == 0) { set_now(p, now_ms); return 0; }
	if (text_num(z, strlen(z), &r, &ii)) { set_raw_number(p, r); return 0; }
	if (strcasecmp(z, "subsec") == 0 || strcasecmp(z, "subsecond") == 0) {
		p->useSubsec = 1;
		set_now(p, now_ms);
		return 0;
	}
	return 1;
}

static int
valid_jd(int64_t j)
{
	return j >= 0 && j <= JD_MAX;
}

static void
compute_ymd(dt_t *p)
{
	int Z, alpha, A, B, C, D, E, X1;
	if (p->validYMD) return;
	if (!p->validJD) { p->Y = 2000; p->M = 1; p->D = 1; }
	else if (!valid_jd(p->iJD)) { dt_error(p); return; }
	else {
		Z = (int)((p->iJD + 43200000) / 86400000);
		alpha = (int)((Z + 32044.75) / 36524.25) - 52;
		A = Z + 1 + alpha - ((alpha + 100) / 4) + 25;
		B = A + 1524;
		C = (int)((B - 122.1) / 365.25);
		D = (36525 * (C & 32767)) / 100;
		E = (int)((B - D) / 30.6001);
		X1 = (int)(30.6001 * E);
		p->D = B - D - X1;
		p->M = E < 14 ? E - 1 : E - 13;
		p->Y = p->M > 2 ? C - 4716 : C - 4715;
	}
	p->validYMD = 1;
}

static void
compute_hms(dt_t *p)
{
	int day_ms, day_min;
	if (p->validHMS) return;
	compute_jd(p);
	day_ms = (int)((p->iJD + 43200000) % 86400000);
	p->s = (day_ms % 60000) / 1000.0;
	day_min = day_ms / 60000;
	p->m = day_min % 60;
	p->h = day_min / 60;
	p->rawS = 0;
	p->validHMS = 1;
}

static void
compute_ymd_hms(dt_t *p)
{
	compute_ymd(p);
	compute_hms(p);
}

static const struct {
	int n;
	const char *name;
	double limit, xform;
} k_xform[] = {
	{ 6, "second", 4.6427e+14, 1.0 },
	{ 6, "minute", 7.7379e+12, 60.0 },
	{ 4, "hour", 1.2897e+11, 3600.0 },
	{ 3, "day", 5373485.0, 86400.0 },
	{ 5, "month", 176546.0, 2592000.0 },
	{ 4, "year", 14713.0, 31536000.0 }
};

static void
auto_adjust(dt_t *p)
{
	if (!p->rawS || p->validJD) {
		p->rawS = 0;
	} else if (p->s >= -210866760000.0 && p->s <= 253402300799.0) {
		double r = p->s * 1000.0 + 210866760000000.0;
		clear_ymd_hms(p);
		p->iJD = (int64_t)(r + 0.5);
		p->validJD = 1;
		p->rawS = 0;
	}
}

/* One modifier.  0 on success, 1 on error.  idx = argument position. */
static int
parse_modifier(const char *z, dt_t *p, int idx)
{
	int rc = 1, n;
	double r;
	int ii;
	switch (tolower((unsigned char)z[0])) {
	case 'a':
		if (strcasecmp(z, "auto") == 0) {
			if (idx > 1) return 1;
			auto_adjust(p);
			rc = 0;
		}
		break;
	case 'c':
		if (strcasecmp(z, "ceiling") == 0) {
			compute_jd(p);
			clear_ymd_hms(p);
			rc = 0;
			p->nFloor = 0;
		}
		break;
	case 'f':
		if (strcasecmp(z, "floor") == 0) {
			compute_jd(p);
			p->iJD -= p->nFloor * 86400000LL;
			clear_ymd_hms(p);
			rc = 0;
		}
		break;
	case 'j':
		if (strcasecmp(z, "julianday") == 0) {
			if (idx > 1) return 1;
			if (p->validJD && p->rawS) { rc = 0; p->rawS = 0; }
		}
		break;
	case 'u':
		if (strcasecmp(z, "unixepoch") == 0 && p->rawS) {
			if (idx > 1) return 1;
			r = p->s * 1000.0 + 210866760000000.0;
			if (r >= 0.0 && r < 464269060800000.0) {
				clear_ymd_hms(p);
				p->iJD = (int64_t)(r + 0.5);
				p->validJD = 1;
				p->rawS = 0;
				rc = 0;
			}
		}
		break;
	case 'w':
		if (strncasecmp(z, "weekday ", 8) == 0 &&
		    text_num(z + 8, strlen(z + 8), &r, &ii) &&
		    r >= 0.0 && r < 7.0 && (n = (int)r) == r) {
			int64_t Zd;
			compute_ymd_hms(p);
			p->tz = 0;
			p->validJD = 0;
			compute_jd(p);
			Zd = ((p->iJD + 129600000) / 86400000) % 7;
			if (Zd > n) Zd -= 7;
			p->iJD += (n - Zd) * 86400000LL;
			clear_ymd_hms(p);
			rc = 0;
		}
		break;
	case 's':
		if (strncasecmp(z, "start of ", 9) != 0) {
			if (strcasecmp(z, "subsec") == 0 || strcasecmp(z, "subsecond") == 0) {
				p->useSubsec = 1;
				rc = 0;
			}
			break;
		}
		if (!p->validJD && !p->validYMD && !p->validHMS) break;
		z += 9;
		compute_ymd(p);
		p->validHMS = 1;
		p->h = p->m = 0;
		p->s = 0.0;
		p->rawS = 0;
		p->tz = 0;
		p->validJD = 0;
		if (strcasecmp(z, "month") == 0) { p->D = 1; rc = 0; }
		else if (strcasecmp(z, "year") == 0) { p->M = 1; p->D = 1; rc = 0; }
		else if (strcasecmp(z, "day") == 0) rc = 0;
		break;
	case '+': case '-': case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9': {
		double rounder;
		int i, Y, M, D, h, m, x;
		const char *z2 = z;
		char z0 = z[0], copy[64];
		int *o[3];
		for (n = 1; z[n]; n++) {
			if (z[n] == ':') break;
			if (isspace((unsigned char)z[n])) break;
			if (z[n] == '-') {
				o[0] = &Y;
				if (n == 5 && get_digits(&z[1], "40f", o) == 1) break;
				if (n == 6 && get_digits(&z[1], "50f", o) == 1) break;
			}
		}
		if (n >= (int)sizeof copy) break;
		memcpy(copy, z, (size_t)n); copy[n] = '\0';
		/* sqlite3AtoF() <= 0: no numeric prefix at all -> error. */
		if (!text_num(copy, (size_t)n, &r, &ii) && !has_num_prefix(copy))
			break;
		if (z[n] == '-') {
			if (z0 != '+' && z0 != '-') break;
			o[0] = &Y; o[1] = &M; o[2] = &D;
			if (n == 5) {
				if (get_digits(&z[1], "40f-20a-20d", o) != 3) break;
			} else {
				if (get_digits(&z[1], "50f-20a-20d", o) != 3) break;
				z++;
			}
			if (M >= 12 || D >= 31) break;
			compute_ymd_hms(p);
			p->validJD = 0;
			if (z0 == '-') { p->Y -= Y; p->M -= M; D = -D; }
			else { p->Y += Y; p->M += M; }
			x = p->M > 0 ? (p->M - 1) / 12 : (p->M - 12) / 12;
			p->Y += x;
			p->M -= x * 12;
			compute_floor(p);
			compute_jd(p);
			p->validHMS = 0;
			p->validYMD = 0;
			p->iJD += (int64_t)D * 86400000;
			if (z[11] == 0) { rc = 0; break; }
			o[0] = &h; o[1] = &m;
			if (isspace((unsigned char)z[11]) && get_digits(&z[12], "20c:20e", o) == 2) {
				z2 = &z[12];
				n = 2;
			} else {
				break;
			}
		}
		if (z2[n] == ':') {
			dt_t tx;
			int64_t day;
			if (!isdigit((unsigned char)*z2)) z2++;
			memset(&tx, 0, sizeof tx);
			if (parse_hms(z2, &tx)) break;
			compute_jd(&tx);
			tx.iJD -= 43200000;
			day = tx.iJD / 86400000;
			tx.iJD -= day * 86400000;
			if (z0 == '-') tx.iJD = -tx.iJD;
			compute_jd(p);
			clear_ymd_hms(p);
			p->iJD += tx.iJD;
			rc = 0;
			break;
		}
		z += n;
		while (isspace((unsigned char)*z)) z++;
		n = (int)strlen(z);
		if (n < 3 || n > 10) break;
		if (tolower((unsigned char)z[n - 1]) == 's') n--;
		compute_jd(p);
		rounder = r < 0 ? -0.5 : 0.5;
		p->nFloor = 0;
		for (i = 0; i < (int)(sizeof k_xform / sizeof k_xform[0]); i++) {
			if (k_xform[i].n == n && strncasecmp(k_xform[i].name, z, (size_t)n) == 0 &&
			    r > -k_xform[i].limit && r < k_xform[i].limit) {
				if (i == 4) {   /* months */
					compute_ymd_hms(p);
					p->M += (int)r;
					x = p->M > 0 ? (p->M - 1) / 12 : (p->M - 12) / 12;
					p->Y += x;
					p->M -= x * 12;
					compute_floor(p);
					p->validJD = 0;
					r -= (int)r;
				} else if (i == 5) {   /* years */
					compute_ymd_hms(p);
					p->Y += (int)r;
					compute_floor(p);
					p->validJD = 0;
					r -= (int)r;
				}
				compute_jd(p);
				p->iJD += (int64_t)(r * 1000.0 * k_xform[i].xform + rounder);
				rc = 0;
				break;
			}
		}
		clear_ymd_hms(p);
		break;
	}
	default:
		break;
	}
	return rc;
}

/* Parse the date arguments; 0 on success, 1 -> the result is NULL. */
static int
is_date(const vx_cell_t *argv, int argc, int64_t now_ms, dt_t *p,
        vxf_alloc_fn alloc, void *actx)
{
	char tmp[48];
	const char *z;
	uint32_t n;
	int i;
	memset(p, 0, sizeof *p);
	if (argc == 0) { set_now(p, now_ms); return 0; }
	if (argv[0].type == VX_INT || argv[0].type == VX_REAL) {
		set_raw_number(p, argv[0].type == VX_INT ? (double)argv[0].i : argv[0].r);
	} else {
		if (argv[0].type == VX_NULL) return 1;
		z = cell_str(&argv[0], tmp, sizeof tmp, &n, alloc, actx);
		if (z == NULL || parse_date_or_time(z, p, now_ms)) return 1;
	}
	for (i = 1; i < argc; i++) {
		if (argv[i].type == VX_NULL) return 1;
		z = cell_str(&argv[i], tmp, sizeof tmp, &n, alloc, actx);
		if (z == NULL || parse_modifier(z, p, i)) return 1;
	}
	compute_jd(p);
	if (p->isError || !valid_jd(p->iJD)) return 1;
	if (argc == 1 && p->validYMD && p->D > 28) p->validYMD = 0;
	return 0;
}

static int
fmt_ymd(char *b, const dt_t *x)
{
	int Y = x->Y < 0 ? -x->Y : x->Y;
	return sprintf(b, "%s%04d-%02d-%02d", x->Y < 0 ? "-" : "", Y, x->M, x->D);
}

static int
fmt_hms(char *b, const dt_t *x)
{
	if (x->useSubsec) {
		int s = (int)(1000.0 * x->s + 0.5);
		return sprintf(b, "%02d:%02d:%02d.%03d", x->h, x->m, s / 1000, s % 1000);
	}
	return sprintf(b, "%02d:%02d:%02d", x->h, x->m, (int)x->s);
}

static int
days_after_jan01(const dt_t *d)
{
	dt_t j = *d;
	j.validJD = 0; j.M = 1; j.D = 1;
	compute_jd(&j);
	return (int)((d->iJD - j.iJD + 43200000) / 86400000);
}

static int
days_after_monday(const dt_t *d)
{
	return (int)((d->iJD + 43200000) / 86400000) % 7;
}

static int
days_after_sunday(const dt_t *d)
{
	return (int)((d->iJD + 129600000) / 86400000) % 7;
}

static void
fn_strftime(const vx_cell_t *argv, int argc, int64_t now_ms,
            struct sbuf *b, vx_cell_t *out)
{
	char tmp[48], nb[64];
	const char *fmt;
	uint32_t fl, i, j;
	dt_t x, y;
	if (argv[0].type == VX_NULL) { out->type = VX_NULL; return; }
	fmt = cell_str(&argv[0], tmp, sizeof tmp, &fl, b->alloc, b->actx);
	if (fmt == NULL || is_date(argv + 1, argc - 1, now_ms, &x, b->alloc, b->actx)) {
		out->type = VX_NULL; return;
	}
	compute_jd(&x);
	compute_ymd_hms(&x);
	for (i = j = 0; i < fl; i++) {
		char cf;
		int k = 0;
		if (fmt[i] != '%') continue;
		if (j < i) sb_put(b, fmt + j, i - j);
		i++;
		j = i + 1;
		cf = fmt[i];
		switch (cf) {
		case 'd': k = snprintf(nb, sizeof nb, "%02d", x.D); break;
		case 'e': k = snprintf(nb, sizeof nb, "%2d", x.D); break;
		case 'f': k = snprintf(nb, sizeof nb, "%06.3f", x.s > 59.999 ? 59.999 : x.s); break;
		case 'F': k = snprintf(nb, sizeof nb, "%04d-%02d-%02d", x.Y, x.M, x.D); break;
		case 'G': case 'g':
			y = x;
			y.iJD += (3 - days_after_monday(&x)) * 86400000LL;
			y.validYMD = 0;
			compute_ymd(&y);
			k = cf == 'g' ? snprintf(nb, sizeof nb, "%02d", y.Y % 100)
			              : snprintf(nb, sizeof nb, "%04d", y.Y);
			break;
		case 'H': k = snprintf(nb, sizeof nb, "%02d", x.h); break;
		case 'k': k = snprintf(nb, sizeof nb, "%2d", x.h); break;
		case 'I': case 'l': {
			int h = x.h;
			if (h > 12) h -= 12;
			if (h == 0) h = 12;
			k = snprintf(nb, sizeof nb, cf == 'I' ? "%02d" : "%2d", h);
			break;
		}
		case 'j': k = snprintf(nb, sizeof nb, "%03d", days_after_jan01(&x) + 1); break;
		case 'J': k = snprintf(nb, sizeof nb, "%.16g", x.iJD / 86400000.0); break;
		case 'm': k = snprintf(nb, sizeof nb, "%02d", x.M); break;
		case 'M': k = snprintf(nb, sizeof nb, "%02d", x.m); break;
		case 'p': k = snprintf(nb, sizeof nb, "%s", x.h >= 12 ? "PM" : "AM"); break;
		case 'P': k = snprintf(nb, sizeof nb, "%s", x.h >= 12 ? "pm" : "am"); break;
		case 'R': k = snprintf(nb, sizeof nb, "%02d:%02d", x.h, x.m); break;
		case 's':
			if (x.useSubsec)
				k = snprintf(nb, sizeof nb, "%.3f", (x.iJD - 210866760000000LL) / 1000.0);
			else
				k = snprintf(nb, sizeof nb, "%lld",
				    (long long)(x.iJD / 1000 - 210866760000LL));
			break;
		case 'S': k = snprintf(nb, sizeof nb, "%02d", (int)x.s); break;
		case 'T': k = snprintf(nb, sizeof nb, "%02d:%02d:%02d", x.h, x.m, (int)x.s); break;
		case 'u': case 'w': {
			int c = days_after_sunday(&x);
			if (c == 0 && cf == 'u') c = 7;
			k = snprintf(nb, sizeof nb, "%d", c);
			break;
		}
		case 'U':
			k = snprintf(nb, sizeof nb, "%02d",
			    (days_after_jan01(&x) - days_after_sunday(&x) + 7) / 7);
			break;
		case 'V':
			y = x;
			y.iJD += (3 - days_after_monday(&x)) * 86400000LL;
			y.validYMD = 0;
			compute_ymd(&y);
			k = snprintf(nb, sizeof nb, "%02d", days_after_jan01(&y) / 7 + 1);
			break;
		case 'W':
			k = snprintf(nb, sizeof nb, "%02d",
			    (days_after_jan01(&x) - days_after_monday(&x) + 7) / 7);
			break;
		case 'Y': k = snprintf(nb, sizeof nb, "%04d", x.Y); break;
		case '%': nb[0] = '%'; k = 1; break;
		default:
			out->type = VX_NULL;   /* unknown conversion -> NULL */
			return;
		}
		sb_put(b, nb, (size_t)k);
	}
	if (j < i) sb_put(b, fmt + j, i - j);
	set_text(out, b);
}

static void
fn_date(int id, const vx_cell_t *argv, int argc, int64_t now_ms,
        struct sbuf *b, vx_cell_t *out)
{
	char tmp[64];
	int k = 0;
	dt_t x;
	if (id == F_CDATE || id == F_CTIME || id == F_CTIMESTAMP) argc = 0;
	if (is_date(argv, argc, now_ms, &x, b->alloc, b->actx)) { out->type = VX_NULL; return; }
	switch (id) {
	case F_JULIANDAY:
		compute_jd(&x);
		out->type = VX_REAL; out->r = x.iJD / 86400000.0;
		return;
	case F_UNIXEPOCH:
		compute_jd(&x);
		if (x.useSubsec) {
			out->type = VX_REAL; out->r = (x.iJD - 210866760000000LL) / 1000.0;
		} else {
			out->type = VX_INT; out->i = x.iJD / 1000 - 210866760000LL;
		}
		return;
	case F_DATE: case F_CDATE:
		compute_ymd(&x);
		k = fmt_ymd(tmp, &x);
		break;
	case F_TIME: case F_CTIME:
		compute_hms(&x);
		k = fmt_hms(tmp, &x);
		break;
	default:   /* datetime, current_timestamp */
		compute_ymd_hms(&x);
		k = fmt_ymd(tmp, &x);
		tmp[k++] = ' ';
		k += fmt_hms(tmp + k, &x);
		break;
	}
	sb_put(b, tmp, (size_t)k);
	set_text(out, b);
}

/* ---- printf (SQLite's sqlite3_str_appendf subset) ------------------ */

/* Fixed/exp rendering of a double the SQLite way (18-digit decode, then
 * half-up), for %f %e %g.  `alt` = '#', `alt2` = '!'. */
static void
fmt_float(struct sbuf *b, double r, char conv, int prec, int width,
          int left, int zero, char sign, int alt, int comma)
{
	struct fpdec f;
	char buf[400];
	int o = 0, i, e2, flag_dp, rtz = 0, nsig;
	char xt = (char)tolower((unsigned char)conv), prefix = 0;
	if (prec < 0) prec = 6;
	if (prec > 350) prec = 350;
	if (r != r) { sb_pad(b, ' ', left ? 0 : width - 3); sb_put(b, "NaN", 3); sb_pad(b, ' ', left ? width - 3 : 0); return; }
	if (isinf(r)) {
		const char *s = r < 0 ? "-Inf" : (sign ? (sign == '+' ? "+Inf" : " Inf") : "Inf");
		int sl = (int)strlen(s);
		sb_pad(b, ' ', left ? 0 : width - sl);
		sb_put(b, s, (size_t)sl);
		sb_pad(b, ' ', left ? width - sl : 0);
		return;
	}
	fp_decode(r, &f);
	if (xt == 'g' && prec == 0) prec = 1;
	if (f.z[0] != '0') {
		if (xt == 'f') nsig = f.dp + prec;
		else if (xt == 'g') nsig = prec;
		else nsig = prec + 1;
		/* FpDecode's mxRound: without the '!' flag at most 16 significant
		 * digits survive; the rest print as zeros. */
		if (nsig > 16) nsig = 16;
		if (nsig <= 0) {
			/* rounds to 0 or to one unit in the last place */
			if (nsig == 0 && f.z[0] >= '5') { strcpy(f.z, "1"); f.n = 1; f.dp++; }
			else { strcpy(f.z, "0"); f.n = 1; f.dp = 1; }
		} else {
			round_digits(&f, nsig);
		}
	}
	if (f.neg && !(f.z[0] == '0' && xt == 'f' && alt)) prefix = '-';
	else if (!f.neg) prefix = sign;
	if (f.z[0] == '0') { f.n = 1; f.dp = 1; }
	if (xt == 'g') {
		int exp = f.dp - 1;
		prec--;
		rtz = !alt;
		if (exp < -4 || exp > prec) xt = 'e';
		else { prec = prec - exp; xt = 'f'; }
	}
	e2 = (xt == 'e') ? 0 : f.dp - 1;
	if (f.z[0] == '0') e2 = 0;
	flag_dp = (prec > 0) || alt;
	if (prefix) buf[o++] = prefix;
	i = 0;
	if (e2 < 0) {
		buf[o++] = '0';
	} else {
		int cnt = e2;
		for (; e2 >= 0; e2--) {
			buf[o++] = i < f.n ? f.z[i++] : '0';
			if (comma && (e2 % 3) == 0 && e2 > 1) buf[o++] = ',';
		}
		(void)cnt;
	}
	if (flag_dp) buf[o++] = '.';
	for (e2++; e2 < 0 && prec > 0; e2++) { buf[o++] = '0'; prec--; }
	while (prec-- > 0) buf[o++] = i < f.n ? f.z[i++] : '0';
	if (rtz && flag_dp) {
		while (buf[o - 1] == '0') o--;
		if (buf[o - 1] == '.') o--;
	}
	if (xt == 'e') {
		int exp = (f.z[0] == '0') ? 0 : f.dp - 1;
		buf[o++] = (conv >= 'A' && conv <= 'Z') ? 'E' : 'e';
		buf[o++] = exp < 0 ? '-' : '+';
		if (exp < 0) exp = -exp;
		if (exp >= 100) { buf[o++] = (char)('0' + exp / 100); exp %= 100; }
		buf[o++] = (char)('0' + exp / 10);
		buf[o++] = (char)('0' + exp % 10);
	}
	if (o < width) {
		int pad = width - o;
		if (left) { sb_put(b, buf, (size_t)o); sb_pad(b, ' ', pad); return; }
		if (zero) {
			int adj = prefix != 0;
			sb_put(b, buf, (size_t)adj);
			sb_pad(b, '0', pad);
			sb_put(b, buf + adj, (size_t)(o - adj));
			return;
		}
		sb_pad(b, ' ', pad);
	}
	sb_put(b, buf, (size_t)o);
}

static void
fmt_field(struct sbuf *b, const char *s, size_t n, int width, int left)
{
	if (!left) sb_pad(b, ' ', width - (long)n);
	sb_put(b, s, n);
	if (left) sb_pad(b, ' ', width - (long)n);
}

static size_t
utf8_prefix(const char *s, size_t n, long nchar)
{
	size_t i = 0;
	long c = 0;
	while (i < n && c < nchar) {
		i++;
		while (i < n && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
		c++;
	}
	return i;
}

static long
utf8_count(const char *s, size_t n)
{
	size_t i;
	long c = 0;
	for (i = 0; i < n; i++)
		if (((unsigned char)s[i] & 0xc0) != 0x80) c++;
	return c;
}

static int
utf8_put(char *o, uint32_t ch)
{
	if (ch < 0x80) { o[0] = (char)ch; return 1; }
	if (ch < 0x800) { o[0] = (char)(0xc0 | (ch >> 6)); o[1] = (char)(0x80 | (ch & 0x3f)); return 2; }
	if (ch < 0x10000) {
		o[0] = (char)(0xe0 | (ch >> 12)); o[1] = (char)(0x80 | ((ch >> 6) & 0x3f));
		o[2] = (char)(0x80 | (ch & 0x3f)); return 3;
	}
	o[0] = (char)(0xf0 | ((ch >> 18) & 0x07)); o[1] = (char)(0x80 | ((ch >> 12) & 0x3f));
	o[2] = (char)(0x80 | ((ch >> 6) & 0x3f)); o[3] = (char)(0x80 | (ch & 0x3f));
	return 4;
}

static void
fn_printf(const vx_cell_t *argv, int argc, struct sbuf *b, vx_cell_t *out)
{
	char tmp[48], nb[96];
	const char *fmt;
	uint32_t fl, i;
	int ai = 1;
	if (argc < 1 || argv[0].type == VX_NULL) { out->type = VX_NULL; return; }
	fmt = cell_str(&argv[0], tmp, sizeof tmp, &fl, b->alloc, b->actx);
	if (fmt == NULL) { out->type = VX_NULL; return; }
	for (i = 0; i < fl; i++) {
		int left = 0, zero = 0, alt = 0, comma = 0, width = 0, prec = -1;
		char sign = 0, c;
		const vx_cell_t *a;
		static const vx_cell_t k_null = { VX_NULL, 0, 0, NULL, 0 };
		if (fmt[i] != '%') { sb_put(b, fmt + i, 1); continue; }
		if (++i >= fl) { sb_put(b, "%", 1); break; }   /* trailing '%' is literal */
		for (;; i++) {
			c = fmt[i];
			if (c == '-') left = 1;
			else if (c == '+') sign = '+';
			else if (c == ' ') { if (sign != '+') sign = ' '; }
			else if (c == '#') alt = 1;
			else if (c == '!') ;
			else if (c == '0') zero = 1;
			else if (c == ',') comma = 1;
			else break;
			if (i + 1 >= fl) goto done;
		}
		if (fmt[i] == '*') {
			int64_t w = ai < argc ? as_int64(&argv[ai]) : 0;
			ai++;
			if (w < 0) { left = 1; w = w >= -2147483647 ? -w : 0; }
			width = (int)(w > 100000 ? 100000 : w);
			i++;
		} else {
			while (i < fl && isdigit((unsigned char)fmt[i])) {
				width = width * 10 + (fmt[i] - '0');
				if (width > 100000) width = 100000;
				i++;
			}
		}
		if (i < fl && fmt[i] == '.') {
			i++;
			if (i < fl && fmt[i] == '*') {
				int64_t p = ai < argc ? as_int64(&argv[ai]) : 0;
				ai++;
				prec = (int)(p < 0 ? (p >= -2147483647 ? -p : 0) : (p > 100000 ? 100000 : p));
				i++;
			} else {
				prec = 0;
				while (i < fl && isdigit((unsigned char)fmt[i])) {
					prec = prec * 10 + (fmt[i] - '0');
					if (prec > 100000) prec = 100000;
					i++;
				}
			}
		}
		/* length modifiers l / ll are accepted and ignored */
		if (i < fl && fmt[i] == 'l') { i++; if (i < fl && fmt[i] == 'l') i++; }
		if (i >= fl) break;
		c = fmt[i];
		a = ai < argc ? &argv[ai] : &k_null;
		switch (c) {
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': case 'p':
		case 'f': case 'e': case 'E': case 'g': case 'G': case 's': case 'z':
		case 'q': case 'Q': case 'w':
			ai++;
			break;
		default:
			break;
		}
		switch (c) {
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
			int64_t v = as_int64(a);
			uint64_t uv;
			char digs[80];
			int nd = 0, neg = 0, k, o = 0, base = (c == 'x' || c == 'X' || c == 'p') ? 16 : (c == 'o' ? 8 : 10);
			const char *set = (c == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
			char pre[4];
			int npre = 0;
			if (c == 'd' || c == 'i') {
				if (v < 0) { neg = 1; uv = (uint64_t)0 - (uint64_t)v; }
				else uv = (uint64_t)v;
			} else {
				uv = (uint64_t)v;
			}
			do { digs[nd++] = set[uv % (uint64_t)base]; uv /= (uint64_t)base; } while (uv);
			if (prec >= 0) { while (nd < prec && nd < 70) digs[nd++] = '0'; }
			if (zero && !left && prec < 0) {
				int want = width - ((neg || sign) && base == 10 ? 1 : 0);
				while (nd < want && nd < 70) digs[nd++] = '0';
			}
			if (base == 10 && (c == 'd' || c == 'i')) {
				if (neg) pre[npre++] = '-';
				else if (sign) pre[npre++] = sign;
			}
			if (alt && v != 0 && base == 16) { pre[npre++] = '0'; pre[npre++] = (c == 'X') ? 'X' : 'x'; }
			if (alt && base == 8 && digs[nd - 1] != '0') digs[nd++] = '0';
			for (k = 0; k < npre; k++) nb[o++] = pre[k];
			for (k = nd - 1; k >= 0; k--) {
				nb[o++] = digs[k];
				if (comma && base == 10 && k > 0 && k % 3 == 0) nb[o++] = ',';
			}
			fmt_field(b, nb, (size_t)o, width, left);
			break;
		}
		case 'f': case 'e': case 'E': case 'g': case 'G':
			fmt_float(b, as_double(a), c, prec, width, left, zero, sign, alt, comma);
			break;
		case 'c': {
			uint32_t sn;
			const char *s;
			size_t cl = 1;
			int rep = prec > 1 ? prec : 1, k;
			char one[8] = { 0 };
			if (a->type == VX_NULL) {
				cl = 1;   /* NUL character */
			} else {
				s = cell_str(a, tmp, sizeof tmp, &sn, b->alloc, b->actx);
				if (s == NULL || sn == 0) { cl = 1; }
				else { cl = utf8_prefix(s, sn, 1); memcpy(one, s, cl); }
			}
			if (!left) sb_pad(b, ' ', width - rep);
			for (k = 0; k < rep; k++) sb_put(b, one, cl);
			if (left) sb_pad(b, ' ', width - rep);
			break;
		}
		case 's': case 'z': {
			uint32_t sn = 0;
			const char *s = "";
			size_t use;
			if (a->type != VX_NULL) {
				s = cell_str(a, tmp, sizeof tmp, &sn, b->alloc, b->actx);
				if (s == NULL) { s = ""; sn = 0; }
			}
			use = prec >= 0 ? utf8_prefix(s, sn, prec) : sn;
			if (!left) sb_pad(b, ' ', width - utf8_count(s, use));
			sb_put(b, s, use);
			if (left) sb_pad(b, ' ', width - utf8_count(s, use));
			break;
		}
		case 'q': case 'Q': case 'w': {
			uint32_t sn = 0, k;
			const char *s;
			char q = (c == 'w') ? '"' : '\'';
			size_t use, start = b->n;
			long w;
			if (a->type == VX_NULL) {
				s = (c == 'Q') ? "NULL" : "(NULL)";
				sb_put(b, s, strlen(s));
				w = (long)(b->n - start);
				if (w < width) {
					/* re-pad: SQLite applies width to the whole field */
					size_t len = b->n - start;
					char save[16];
					memcpy(save, b->p + start, len);
					b->n = start;
					fmt_field(b, save, len, width, left);
				}
				break;
			}
			s = cell_str(a, tmp, sizeof tmp, &sn, b->alloc, b->actx);
			if (s == NULL) break;
			use = prec >= 0 ? utf8_prefix(s, sn, prec) : sn;
			if (c == 'Q') sb_put(b, "'", 1);
			for (k = 0; k < use; k++) {
				sb_put(b, s + k, 1);
				if (s[k] == q) sb_put(b, s + k, 1);
			}
			if (c == 'Q') sb_put(b, "'", 1);
			if (!b->oom) {
				long nchar = utf8_count(b->p + start, b->n - start);
				if (nchar < width) {
					size_t len = b->n - start;
					char *save = (char *)b->alloc(b->actx, len + 1);
					if (save == NULL) { b->oom = 1; break; }
					memcpy(save, b->p + start, len);
					b->n = start;
					if (!left) sb_pad(b, ' ', width - nchar);
					sb_put(b, save, len);
					if (left) sb_pad(b, ' ', width - nchar);
				}
			}
			break;
		}
		case '%':
			fmt_field(b, "%", 1, width, left);
			break;
		default:
			/* unknown conversion (%y, %n, ...): SQLite stops output */
			goto done;
		}
	}
done:
	set_text(out, b);
}

/* ---- the rest of the scalar set ----------------------------------- */

static void
fn_quote(const vx_cell_t *a, struct sbuf *b, vx_cell_t *out)
{
	char tmp[64];
	uint32_t k;
	static const char hexd[] = "0123456789ABCDEF";
	switch (a->type) {
	case VX_INT:
		sb_put(b, tmp, (size_t)snprintf(tmp, sizeof tmp, "%lld", (long long)a->i));
		break;
	case VX_REAL: {
		int k = real_text17(a->r, tmp, sizeof tmp);
		if (k < 0) { out->type = VX_NULL; return; }
		sb_put(b, tmp, (size_t)k);
		break;
	}
	case VX_TEXT:
		sb_put(b, "'", 1);
		for (k = 0; k < a->nbytes; k++) {
			sb_put(b, (const char *)a->bytes + k, 1);
			if (a->bytes[k] == '\'') sb_put(b, "'", 1);
		}
		sb_put(b, "'", 1);
		break;
	case VX_BLOB:
		sb_put(b, "X'", 2);
		for (k = 0; k < a->nbytes; k++) {
			char h[2];
			h[0] = hexd[a->bytes[k] >> 4]; h[1] = hexd[a->bytes[k] & 15];
			sb_put(b, h, 2);
		}
		sb_put(b, "'", 1);
		break;
	default:
		sb_put(b, "NULL", 4);
		break;
	}
	set_text(out, b);
}

static void
fn_hex(const vx_cell_t *a, struct sbuf *b, vx_cell_t *out)
{
	char tmp[48];
	uint32_t n, k;
	const char *s;
	static const char hexd[] = "0123456789ABCDEF";
	if (a->type == VX_NULL) { sb_put(b, "", 0); set_text(out, b); return; }
	s = cell_str(a, tmp, sizeof tmp, &n, b->alloc, b->actx);
	if (s == NULL) { out->type = VX_NULL; return; }
	for (k = 0; k < n; k++) {
		char h[2];
		h[0] = hexd[(unsigned char)s[k] >> 4]; h[1] = hexd[(unsigned char)s[k] & 15];
		sb_put(b, h, 2);
	}
	set_text(out, b);
}

static void
fn_char(const vx_cell_t *argv, int argc, struct sbuf *b, vx_cell_t *out)
{
	int i;
	char u[4];
	for (i = 0; i < argc; i++) {
		int64_t x = as_int64(&argv[i]);
		uint32_t ch = (x < 0 || x > 0x10ffff) ? 0xfffd : (uint32_t)x;
		sb_put(b, u, (size_t)utf8_put(u, ch));
	}
	set_text(out, b);
}

static void
fn_unicode(const vx_cell_t *a, struct sbuf *b, vx_cell_t *out)
{
	char tmp[48];
	uint32_t n, ch;
	const unsigned char *s;
	if (a->type == VX_NULL) { out->type = VX_NULL; return; }
	s = (const unsigned char *)cell_str(a, tmp, sizeof tmp, &n, b->alloc, b->actx);
	if (s == NULL || n == 0) { out->type = VX_NULL; return; }
	ch = s[0];
	if (ch >= 0xc0) {
		int more = ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : 1, k;
		ch &= (ch >= 0xf0) ? 0x07 : (ch >= 0xe0) ? 0x0f : 0x1f;
		for (k = 1; k <= more && (uint32_t)k < n && (s[k] & 0xc0) == 0x80; k++)
			ch = (ch << 6) | (s[k] & 0x3f);
	}
	out->type = VX_INT; out->i = ch;
}

static void
fn_concat(const vx_cell_t *argv, int argc, const vx_cell_t *sep,
          struct sbuf *b, vx_cell_t *out)
{
	char tmp[48], stmp[48];
	const char *s, *sp = NULL;
	uint32_t n, sn = 0;
	int i, first = 1;
	if (sep != NULL) {
		if (sep->type == VX_NULL) { out->type = VX_NULL; return; }
		sp = cell_str(sep, stmp, sizeof stmp, &sn, b->alloc, b->actx);
		if (sp == NULL) { out->type = VX_NULL; return; }
	}
	for (i = 0; i < argc; i++) {
		if (argv[i].type == VX_NULL) continue;
		s = cell_str(&argv[i], tmp, sizeof tmp, &n, b->alloc, b->actx);
		if (s == NULL) { out->type = VX_NULL; return; }
		if (sp != NULL && !first) sb_put(b, sp, sn);
		sb_put(b, s, n);
		first = 0;
	}
	set_text(out, b);
}

/* Truthiness the way SQLite's CASE/WHERE/iif test a value. */
static int
truthy(const vx_cell_t *c)
{
	double d; int ii;
	if (c->type == VX_INT) return c->i != 0;
	if (c->type == VX_REAL) return c->r != 0.0;
	if (c->type == VX_TEXT || c->type == VX_BLOB) {
		(void)text_num((const char *)c->bytes, c->nbytes, &d, &ii);
		return d != 0.0;
	}
	return 0;
}

static void
fn_math(int id, const vx_cell_t *argv, int argc, vx_cell_t *out)
{
	double x, y = 0, r;
	int xi, yi;
	int64_t iv = 0, jv = 0;
	out->type = VX_NULL;
	if (id == F_PI) { out->type = VX_REAL; out->r = M_PI; return; }
	if (!num_val(&argv[0], &x, &xi, &iv)) return;
	if (argc > 1 && !num_val(&argv[1], &y, &yi, &jv)) return;
	switch (id) {
	case F_CEIL: case F_FLOOR: case F_TRUNC:
		if (argv[0].type == VX_INT || (argv[0].type == VX_TEXT && xi)) {
			out->type = VX_INT; out->i = argv[0].type == VX_INT ? argv[0].i : iv;
			return;
		}
		r = id == F_CEIL ? ceil(x) : id == F_FLOOR ? floor(x) : trunc(x);
		break;
	case F_LN: if (x <= 0) return; r = log(x); break;
	case F_LOG:
		if (argc == 2) {
			double bse = x;
			if (bse <= 0 || log(bse) <= 0 || y <= 0) return;
			r = log(y) / log(bse);
		} else {
			if (x <= 0) return;
			r = log10(x);
		}
		break;
	case F_LOG10: if (x <= 0) return; r = log10(x); break;
	case F_LOG2: if (x <= 0) return; r = log2(x); break;
	case F_EXP: r = exp(x); break;
	case F_POW: r = pow(x, y); break;
	case F_MOD: r = fmod(x, y); break;
	case F_SQRT: r = sqrt(x); break;
	case F_SIN: r = sin(x); break;
	case F_COS: r = cos(x); break;
	case F_TAN: r = tan(x); break;
	case F_ASIN: r = asin(x); break;
	case F_ACOS: r = acos(x); break;
	case F_ATAN: r = atan(x); break;
	case F_ATAN2: r = atan2(x, y); break;
	case F_SINH: r = sinh(x); break;
	case F_COSH: r = cosh(x); break;
	case F_TANH: r = tanh(x); break;
	case F_ASINH: r = asinh(x); break;
	case F_ACOSH: r = acosh(x); break;
	case F_ATANH: r = atanh(x); break;
	case F_RADIANS: r = x * (M_PI / 180.0); break;
	case F_DEGREES: r = x * (180.0 / M_PI); break;
	default: return;
	}
	if (r != r) return;   /* NaN -> NULL, as SQLite */
	out->type = VX_REAL; out->r = r;
}

void
vxf_eval(int id, const vx_cell_t *args, int nargs, int64_t now_ms,
         vxf_alloc_fn alloc, void *actx, vx_cell_t *out)
{
	struct sbuf b;
	double d;
	int ii;
	int64_t iv;
	memset(out, 0, sizeof *out);
	memset(&b, 0, sizeof b);
	b.alloc = alloc; b.actx = actx;
	if (is_date_fn(id)) {
		if (id == F_STRFTIME) fn_strftime(args, nargs, now_ms, &b, out);
		else fn_date(id, args, nargs, now_ms, &b, out);
		return;
	}
	switch (id) {
	case F_PRINTF: fn_printf(args, nargs, &b, out); return;
	case F_QUOTE: fn_quote(&args[0], &b, out); return;
	case F_HEX: fn_hex(&args[0], &b, out); return;
	case F_CHAR: fn_char(args, nargs, &b, out); return;
	case F_UNICODE: fn_unicode(&args[0], &b, out); return;
	case F_CONCAT: fn_concat(args, nargs, NULL, &b, out); return;
	case F_CONCAT_WS: fn_concat(args + 1, nargs - 1, &args[0], &b, out); return;
	case F_IIF: *out = truthy(&args[0]) ? args[1] : args[2]; return;
	case F_SIGN:
		if (!num_val(&args[0], &d, &ii, &iv)) { out->type = VX_NULL; return; }
		out->type = VX_INT; out->i = d < 0 ? -1 : d > 0 ? 1 : 0;
		return;
	default:
		fn_math(id, args, nargs, out);
		return;
	}
}

int
vxf_arg_ok(int id, int argi, const vx_cell_t *lit, int maybe_real)
{
	char tmp[64];
	if (id != F_QUOTE || argi != 0) return 1;
	if (lit != NULL) {
		if (lit->type != VX_REAL) return 1;
		snprintf(tmp, sizeof tmp, "%.15g", lit->r);
		return strtod(tmp, NULL) == lit->r || isinf(lit->r);
	}
	return !maybe_real;
}

int
vxf_wants_now(int id, const vx_cell_t *const *args, int nargs)
{
	const vx_cell_t *a;
	int first = (id == F_STRFTIME) ? 1 : 0;
	if (!is_date_fn(id)) return 0;
	if (id == F_CDATE || id == F_CTIME || id == F_CTIMESTAMP) return 1;
	if (nargs <= first) return 1;
	a = args[first];
	if (a == NULL) return 1;   /* not a literal: may be 'now' at run time */
	if (a->type != VX_TEXT) return 0;
	return (a->nbytes == 3 && strncasecmp((const char *)a->bytes, "now", 3) == 0) ||
	       (a->nbytes == 6 && strncasecmp((const char *)a->bytes, "subsec", 6) == 0) ||
	       (a->nbytes == 9 && strncasecmp((const char *)a->bytes, "subsecond", 9) == 0);
}

int
vxf_unsupported_mod(int id, const vx_cell_t *const *args, int nargs)
{
	int i;
	if (!is_date_fn(id)) return 0;
	for (i = 0; i < nargs; i++) {
		const vx_cell_t *a = args[i];
		if (a == NULL || a->type != VX_TEXT) continue;
		if ((a->nbytes == 9 && strncasecmp((const char *)a->bytes, "localtime", 9) == 0) ||
		    (a->nbytes == 3 && strncasecmp((const char *)a->bytes, "utc", 3) == 0))
			return 1;
	}
	return 0;
}

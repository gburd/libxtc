/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/fuzz/fuzz_cfg.c -- libFuzzer harness for the xtc_cfg file parser:
 * xtc_cfg_load_file and xtc_cfg_reload, the only xtc_cfg entry points
 * that parse text (every other setter takes a typed value).
 *
 *	The input is written to a scratch file and loaded against a fixed
 *	registry holding every kind (bool, bounded and unbounded int,
 *	int64, bounded double, string, enum), then reloaded.  After each
 *	load every variable must still satisfy its declared bounds and the
 *	applied count must be within [0, lines].  The input is also a
 *	stand-in for a hostile config file: the parser must not read or
 *	write out of bounds whatever the line lengths, quotes or bytes.
 */

#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_cfg.h"

static char g_path[256];
static const char *const g_levels[] = { "low", "mid", "high", NULL };

static void
fz_check(int ok)
{
	if (!ok)
		abort();
}

static void
fz_reg(const char *name, xtc_cfg_kind_t k, int64_t lo, int64_t hi, double dlo,
    double dhi)
{
	xtc_cfg_spec_t s;
	memset(&s, 0, sizeof s);
	s.name = name;
	s.kind = k;
	s.min_int = lo;
	s.max_int = hi;
	s.min_double = dlo;
	s.max_double = dhi;
	if (k == XTC_CFG_ENUM) {
		s.enum_labels = g_levels;
		s.n_enum_labels = 3;
	}
	if (k == XTC_CFG_STRING)
		s.dflt.d_string = "dflt";
	fz_check(xtc_cfg_register(&s) == XTC_OK);
}

static void
fz_init(void)
{
	const char *t = getenv("TMPDIR");
	int fd;

	snprintf(g_path, sizeof g_path, "%s/xtc-fuzz-cfg.XXXXXX",
	    t != NULL && t[0] != '\0' ? t : "/tmp");
	fd = mkstemp(g_path);
	fz_check(fd >= 0);
	(void)close(fd);
	fz_reg("b", XTC_CFG_BOOL, 0, 0, 0, 0);
	fz_reg("i", XTC_CFG_INT, 0, 0, 0, 0);
	fz_reg("ib", XTC_CFG_INT, -5, 100, 0, 0);
	fz_reg("l", XTC_CFG_INT64, 0, 0, 0, 0);
	fz_reg("lb", XTC_CFG_INT64, 1, 1LL << 40, 0, 0);
	fz_reg("d", XTC_CFG_DOUBLE, 0, 0, -1.5, 10.0);
	fz_reg("s", XTC_CFG_STRING, 0, 0, 0, 0);
	fz_reg("e", XTC_CFG_ENUM, 0, 0, 0, 0);
}

static void
fz_invariants(void)
{
	int b, i, e;
	int64_t l;
	double d;
	char *s = NULL;

	fz_check(xtc_cfg_get_bool("b", &b) == XTC_OK && (b == 0 || b == 1));
	fz_check(xtc_cfg_get_int("i", &i) == XTC_OK);
	fz_check(xtc_cfg_get_int("ib", &i) == XTC_OK && i >= -5 && i <= 100);
	fz_check(xtc_cfg_get_int64("l", &l) == XTC_OK);
	fz_check(xtc_cfg_get_int64("lb", &l) == XTC_OK);
	fz_check(l == 0 || (l >= 1 && l <= (1LL << 40))); /* 0: the default */
	fz_check(xtc_cfg_get_double("d", &d) == XTC_OK);
	fz_check(!isnan(d) && d >= -1.5 && d <= 10.0);
	fz_check(xtc_cfg_get_enum("e", &e) == XTC_OK && e >= 0 && e < 3);
	fz_check(xtc_cfg_get_string_copy("s", &s) == XTC_OK && s != NULL);
	xtc_free(s);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int
LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	static int inited;
	size_t i, lines = 1;
	int fd, rc;

	if (!inited) {
		fz_init();
		inited = 1;
	}
	fd = open(g_path, O_WRONLY | O_TRUNC);
	fz_check(fd >= 0);
	fz_check(write(fd, d, n) == (ssize_t)n);
	(void)close(fd);
	/* fgets splits a line at 2047 bytes, so count the pieces too. */
	for (i = 0; i < n; i++)
		if (d[i] == '\n' || (i % 2047) == 0)
			lines++;
	rc = xtc_cfg_load_file(g_path);
	fz_check(rc >= 0 && (size_t)rc <= lines);
	fz_invariants();
	rc = xtc_cfg_reload();
	fz_check(rc >= 0 && (size_t)rc <= lines);
	fz_invariants();
	return 0;
}

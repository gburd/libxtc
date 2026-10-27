/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/fuzz/fuzz_tls.c -- libFuzzer harness for the one parser in
 * xtc_tls_opts_t that libxtc owns: alpn_protos, the ALPN protocol list in
 * wire form ("\x02h2\x08http/1.1": a length byte, then that many bytes,
 * repeated).  Every other xtc_tls_opts_t string (cert/key/CA/CRL paths,
 * cipher lists, groups) is handed to the TLS library unparsed, so a
 * fuzzer there would test the TLS library, not libxtc.
 *
 *	The input, cut at its first NUL (alpn_protos is a C string), is the
 *	list.  A context is created as SERVER and as CLIENT (verification
 *	off, so no trust store or CA file is needed) and destroyed.  The
 *	only valid outcomes are XTC_OK and XTC_E_INVAL; a list whose length
 *	bytes do not tile the string exactly must be XTC_E_INVAL.
 *
 *	Build it against each TLS backend that decodes the list itself
 *	(mbedTLS, GnuTLS, wolfSSL) as well as OpenSSL: FUZZ_CONFIGURE=
 *	"--with-tls=mbedtls" scripts/fuzz.sh tls SECS.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_tls.h"

static void
fz_check(int ok)
{
	if (!ok)
		abort();
}

/* 1 if the wire list tiles s[0..n) exactly with non-zero lengths. */
static int
fz_wire_ok(const unsigned char *s, size_t n)
{
	size_t off = 0;
	while (off < n) {
		if (s[off] == 0 || s[off] > n - off - 1)
			return 0;
		off += 1 + (size_t)s[off];
	}
	return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int
LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx;
	char *s;
	size_t len;
	int role, rc;

	if (n > 4096)
		return 0;
	/* An exact-size heap copy, so a read past the NUL is caught. */
	len = strnlen((const char *)d, n);
	s = malloc(len + 1);
	fz_check(s != NULL);
	memcpy(s, d, len);
	s[len] = '\0';
	for (role = 0; role < 2; role++) {
		memset(&o, 0, sizeof o);
		o.alpn_protos = s;
		o.verify_peer_mode = XTC_TLS_VERIFY_NONE;
		ctx = NULL;
		rc = xtc_tls_ctx_create(role ? XTC_TLS_CLIENT : XTC_TLS_SERVER,
		    &o, &ctx);
		if (rc == XTC_E_NOSYS)
			break;                  /* built without TLS */
		fz_check(rc == XTC_OK || rc == XTC_E_INVAL);
		fz_check((rc == XTC_OK) == (ctx != NULL));
		if (len > 0 && !fz_wire_ok((const unsigned char *)s, len))
			fz_check(rc == XTC_E_INVAL);
		if (ctx != NULL)
			xtc_tls_ctx_destroy(ctx);
	}
	free(s);
	return 0;
}

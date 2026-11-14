/*
 * docs/_includes/snippets/20_logf.c
 *
 * xtc_logf: a typed log call.  Each {} takes the next argument and formats
 * it by the argument's own type, so a uint64_t, a size_t and an int64_t
 * need no PRIu64 / %zu / PRId64, and a wrong conversion letter cannot
 * happen.  The sink captures the records so the snippet can check them.
 *
 * Compiled AND run by test/docs/test_doc_snippets.sh (a release gate): it
 * must exit 0.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_log.h"

static char records[1024];
static size_t nrec;

/* Keep each record's text (the part after "[seconds] LEVEL "). */
static int
capture(void *user, xtc_log_level_t lvl, const char *line, size_t len)
{
	const char *text = strchr(line, ']') + 8;

	(void)user;
	(void)lvl;
	len -= (size_t)(text - line);
	if (nrec + len < sizeof records) {
		memcpy(records + nrec, text, len);
		nrec += len;
	}
	return 0;
}

int
main(void)
{
	xtc_log_opts_t opts = XTC_LOG_OPTS_DEFAULT;
	xtc_log_t *log;
	uint32_t loop_id = 3;
	uint64_t nbytes = 1ull << 40;
	int64_t us = -12;
	size_t queued = 17;
	const char *peer = NULL;

	opts.sink = capture;
	opts.sink_fd = -1;
	if (xtc_log_create(&opts, &log) != XTC_OK)
		return 1;
	(void)xtc_log_set_default(log);

	/* !region logf */
	/* printf style: one conversion per type, and PRIu64 for uint64_t. */
	xtc_log_write(log, XTC_LOG_INFO,
	    "loop %" PRIu32 " drained %" PRIu64 " bytes in %" PRId64 " us",
	    loop_id, nbytes, us);

	/* xtc_logf: {} for every argument, whatever its type. */
	xtc_logf(XTC_LOG_INFO, "loop {} drained {} bytes in {} us", loop_id,
	    nbytes, us);

	/* Specs: hex, zero fill, width, precision; a NULL string is safe. */
	xtc_logf(XTC_LOG_INFO, "flags {:08x}, {:6} queued, {:.2} full, peer {}",
	    0x2au, queued, 0.6667, peer);

	/* A missing argument is marked in the record, never undefined. */
	xtc_logf(XTC_LOG_WARN, "{} of {} done", queued);
	/* !endregion logf */

	(void)xtc_log_drain(log);
	(void)xtc_log_set_default(NULL);
	xtc_log_destroy(log);
	records[nrec] = '\0';
	fputs(records, stdout);
	return strcmp(records,
	    "loop 3 drained 1099511627776 bytes in -12 us\n"
	    "loop 3 drained 1099511627776 bytes in -12 us\n"
	    "flags 0000002a,     17 queued, 0.67 full, peer (null)\n"
	    "[xtc_logf: 1 arg, 2 {}] 17 of {} done\n") != 0;
}

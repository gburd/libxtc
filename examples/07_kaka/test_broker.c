/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_broker.c -- in-process broker tests through a real xtc loop
 *	(no socket, no daemon).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "broker.h"

static int g_rc;

static void
check(const char *name, int rc)
{
	if (rc == 0) {
		printf("  ok   %s\n", name);
	} else {
		fprintf(stderr, "FAIL: %s step %d\n", name, rc);
		g_rc = 1;
	}
}

static void
rmtree(const char *dir)
{
	char cmd[256];
	snprintf(cmd, sizeof cmd, "find '%s' -delete 2>/dev/null", dir);
	if (system(cmd) != 0) { /* best effort */ }
}

static int
with_dir(int (*fn)(const char *))
{
	char dir[] = "/tmp/kaka-broker-XXXXXX";
	int rc;
	if (mkdtemp(dir) == NULL) return -1;
	rc = fn(dir);
	rmtree(dir);
	return rc;
}

int
main(void)
{
	int maxif = -1, crc;
	uint64_t hwm = 0;

	check("broker_selftest (produce 3, fetch 3)", broker_selftest());
	crc = broker_credit_selftest(&maxif, &hwm);
	if (crc == 0 && (maxif < 1 || maxif > 4 || hwm != 200))
		crc = 100;
	printf("       (credit peak in-flight=%d<=4, hwm=%llu)\n", maxif,
	    (unsigned long long)hwm);
	check("broker_credit_selftest", crc);
	check("broker_metrics_selftest (stats move; over-cap produce "
	    "answered KAKA_ERR_MEMORY)", broker_metrics_selftest());
	check("broker_durable_selftest (ack after fdatasync, group commit, "
	    "fail-stop on fsync error)", with_dir(broker_durable_selftest));
	check("broker_budget_selftest (budget = live RAM, not lifetime)",
	    with_dir(broker_budget_selftest));
	check("broker_timeout_selftest (abandoned request owns its payload)",
	    with_dir(broker_timeout_selftest));
	printf("%s\n", g_rc == 0 ? "All kaka broker tests passed."
	    : "kaka broker test FAILED.");
	return g_rc;
}

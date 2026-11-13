/*
 * docs snippet: an address-range table as an ordered map (examples
 * 19_edgeacl).  The range holding an address is the one with the largest
 * start <= the address: xtc_cskip_floor.  A replaced record is retired
 * through RCU, never freed while a reader may hold it.  Exits 0 iff the
 * lookups find the right ranges before and after the replace.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_cskip.h"
#include "xtc_rcu.h"

struct range {
	uint32_t start, end;
	const char *owner;
};

static int
key_cmp(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static void
free_cb(void *p)
{
	xtc_free(p);
}

/* !region lookup */
/* Which range holds `ip`?  Copy the answer out inside the read-side. */
static const char *
owner_of(xtc_cskip_t *t, uint32_t ip)
{
	const char *owner = "none";
	void *val;

	xtc_rcu_read_lock();
	if (xtc_cskip_floor(t, &ip, NULL, &val) == XTC_OK) {
		const struct range *r = val;
		if (ip <= r->end)
			owner = r->owner;
	}
	xtc_rcu_read_unlock();
	return owner;
}

/* Insert or replace.  On a replace the map keeps its old key and swaps
 * the value: free the unused new key, retire the old value. */
static int
put(xtc_cskip_t *t, uint32_t start, uint32_t end, const char *owner)
{
	uint32_t *key = xtc_malloc(sizeof *key);
	struct range *r = xtc_malloc(sizeof *r);
	void *old = NULL;

	if (key == NULL || r == NULL)
		return XTC_E_NOMEM;
	*key = start;
	r->start = start;
	r->end = end;
	r->owner = owner;
	if (xtc_cskip_insert(t, key, r, &old) != XTC_OK)
		return XTC_E_NOMEM;
	if (old != NULL) {
		xtc_free(key);
		xtc_rcu_retire(old, free_cb);
	}
	return XTC_OK;
}
/* !endregion lookup */

int
main(void)
{
	xtc_cskip_t *t;
	void *key, *val;
	int ok;

	if (xtc_rcu_init() != XTC_OK || xtc_cskip_create(key_cmp, &t) != XTC_OK)
		return 1;
	(void)put(t, 0x0a000000, 0x0a00ffff, "ACME");    /* 10.0.0.0/16 */
	(void)put(t, 0x0a020000, 0x0a02ffff, "Initech"); /* 10.2.0.0/16 */
	ok = strcmp(owner_of(t, 0x0a000505), "ACME") == 0 &&
	    strcmp(owner_of(t, 0x0a010101), "none") == 0 && /* a gap */
	    strcmp(owner_of(t, 0x0a02abcd), "Initech") == 0;
	(void)put(t, 0x0a020000, 0x0a02ffff, "Globex"); /* reassigned */
	xtc_rcu_synchronize();
	ok = ok && strcmp(owner_of(t, 0x0a02abcd), "Globex") == 0;
	printf("10.2.171.205 belongs to %s\n", owner_of(t, 0x0a02abcd));

	/* Tear down: remove every range, retiring its key and record;
	 * xtc_rcu_fini runs whatever is still pending. */
	for (;;) {
		xtc_rcu_read_lock();
		if (xtc_cskip_min(t, &key, &val) != XTC_OK) {
			xtc_rcu_read_unlock();
			break;
		}
		xtc_rcu_read_unlock();
		(void)xtc_cskip_remove(t, key, NULL);
		xtc_rcu_retire(key, free_cb);
		xtc_rcu_retire(val, free_cb);
	}
	xtc_cskip_destroy(t);
	xtc_rcu_fini();
	return ok ? 0 : 1;
}

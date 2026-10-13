/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/11_lorb/test_lob_diff.c
 *	Differential test: drive lorb and a trivially-correct reference
 *	matcher (flat arrays, linear scans) with the same random stream of
 *	limit / market / cancel / modify / stop / stop-limit operations,
 *	and after EVERY operation require the two books to be identical:
 *	same resting orders (side, shares, price, category), same FIFO
 *	position inside each level, same level totals, same book edges.
 *	The reference implements the rules documented in README.md
 *	("Matching rules"); any divergence is a lorb bug or a README lie.
 *
 *	Usage: test_lob_diff [seeds] [ops]   (default 1000 seeds x 2000 ops)
 *	Exit 0 iff no seed diverged.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lob.h"

#define MAXO  16384
#define NPX   64

enum { K_LIMIT = 0, K_STOP = 1, K_STOP_LIMIT = 2 };

typedef struct {
	int id, buy, shares;
	int px;    /* limit price; 0 for a stop market order */
	int stop;  /* trigger price (stops only) */
	int kind;
	int live;
} ro_t;

/* Reference book: R[] is kept in time-priority (sequence) order. */
static ro_t R[MAXO];
static int  nR;
static int  P;   /* last trade price, 0 = none yet */

static uint64_t rs;

static uint32_t
rnd(void)
{
	uint64_t z = (rs += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return (uint32_t)((z ^ (z >> 31)) >> 32);
}

static int
rint_in(int lo, int hi)
{
	return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* ---- reference matcher -------------------------------------------- */

static void
ref_compact(void)
{
	int i, n = 0;

	for (i = 0; i < nR; i++)
		if (R[i].live)
			R[n++] = R[i];
	nR = n;
}

static void
ref_rest(int id, int buy, int shares, int px, int kind, int stop)
{
	if (nR == MAXO)
		ref_compact();
	if (nR == MAXO) {
		fprintf(stderr, "reference book full\n");
		exit(2);
	}
	R[nR++] = (ro_t){ id, buy, shares, px, stop, kind, 1 };
}

static int
ref_find(int id)
{
	int i;

	for (i = 0; i < nR; i++)
		if (R[i].live && R[i].id == id)
			return i;
	return -1;
}

/* Best resting limit order on a side: best price, then oldest. */
static int
ref_best(int buy_side)
{
	int i, b = -1;

	for (i = 0; i < nR; i++) {
		if (!R[i].live || R[i].kind != K_LIMIT ||
		    R[i].buy != buy_side)
			continue;
		if (b < 0 || (buy_side ? R[i].px > R[b].px : R[i].px < R[b].px))
			b = i;
	}
	return b;
}

/* Match an aggressive order; limit 0 = market.  Returns the remainder. */
static int
ref_match(int buy, int shares, int limit)
{
	while (shares > 0) {
		int o = ref_best(!buy), t;

		if (o < 0 || (limit != 0 &&
		    (buy ? R[o].px > limit : R[o].px < limit)))
			break;
		t = shares < R[o].shares ? shares : R[o].shares;
		shares -= t;
		R[o].shares -= t;
		P = R[o].px;
		if (R[o].shares == 0)
			R[o].live = 0;
	}
	return shares;
}

/* Fire triggered stops until none is triggered by the last trade. */
static void
ref_run_stops(void)
{
	for (;;) {
		int i, f = -1, rem;

		for (i = 0; i < nR; i++)
			if (R[i].live && R[i].kind != K_LIMIT && R[i].buy &&
			    R[i].stop <= P && (f < 0 || R[i].stop < R[f].stop))
				f = i;
		if (f < 0)
			for (i = 0; i < nR; i++)
				if (R[i].live && R[i].kind != K_LIMIT &&
				    !R[i].buy && R[i].stop >= P &&
				    (f < 0 || R[i].stop > R[f].stop))
					f = i;
		if (f < 0)
			return;
		R[f].live = 0;
		rem = ref_match(R[f].buy, R[f].shares, R[f].px);
		if (R[f].kind == K_STOP_LIMIT && rem > 0)
			ref_rest(R[f].id, R[f].buy, rem, R[f].px, K_LIMIT, 0);
	}
}

static void
ref_market(int buy, int shares)
{
	if (ref_match(buy, shares, 0) != shares)
		ref_run_stops();
}

static void
ref_limit(int id, int buy, int shares, int px)
{
	int rem = ref_match(buy, shares, px);

	if (rem > 0)
		ref_rest(id, buy, rem, px, K_LIMIT, 0);
	if (rem != shares)
		ref_run_stops();
}

static void
ref_modify(int id, int shares, int px)
{
	int i = ref_find(id);

	if (i < 0 || R[i].kind != K_LIMIT)
		return;
	if (px == R[i].px && shares <= R[i].shares) {
		R[i].shares = shares;   /* pure decrease: keeps priority */
		return;
	}
	R[i].live = 0;
	ref_limit(id, R[i].buy, shares, px);
}

/* limit 0 = stop market. */
static void
ref_add_stop(int id, int buy, int shares, int stop, int limit)
{
	int o = ref_best(!buy);

	if (o >= 0 && (buy ? stop <= R[o].px : stop >= R[o].px)) {
		if (limit != 0)
			ref_limit(id, buy, shares, limit);
		else
			ref_market(buy, shares);
		return;
	}
	ref_rest(id, buy, shares, limit, limit ? K_STOP_LIMIT : K_STOP, stop);
}

static void
ref_modify_stop(int id, int shares, int stop, int limit)
{
	int i = ref_find(id);

	if (i < 0 || R[i].kind == K_LIMIT)
		return;
	if (stop == R[i].stop && limit == R[i].px && shares <= R[i].shares) {
		R[i].shares = shares;
		return;
	}
	R[i].live = 0;
	ref_add_stop(id, R[i].buy, shares, stop, limit);
}

/* ---- comparison ---------------------------------------------------- */

/*
 * 0 = identical.  1 = order missing / field mismatch, 2 = FIFO
 * position, 3 = level totals or a stray level, 4 = book edge.
 */
static int
compare(const lob_book_t *b)
{
	int last[2][2][NPX] = {{{0}}}, cnt[2][2][NPX] = {{{0}}};
	int vol[2][2][NPX] = {{{0}}};
	int i, px, s, t, hb = 0, ls = 0, lsb = 0, hss = 0;

	for (i = 0; i < nR; i++) {
		const ro_t *r = &R[i];
		const lob_order_t *o;
		const lob_limit_t *lvl;
		int tree = r->kind != K_LIMIT, key = tree ? r->stop : r->px;

		if (!r->live)
			continue;
		o = lob_search_order(b, r->id);
		lvl = tree ? lob_search_stop(b, key) :
		    lob_search_limit(b, key, r->buy);
		if (o == NULL || o->buy != r->buy || o->shares != r->shares ||
		    o->limit != r->px || (int)o->cat != r->kind ||
		    lvl == NULL || o->parent != lvl)
			return 1;
		if ((o->prev ? o->prev->id : 0) != last[tree][r->buy][key])
			return 2;
		last[tree][r->buy][key] = r->id;
		cnt[tree][r->buy][key]++;
		vol[tree][r->buy][key] += r->shares;
	}
	for (px = 1; px < NPX; px++) {
		for (s = 0; s < 2; s++) {
			for (t = 0; t < 2; t++) {
				const lob_limit_t *l = t ? lob_search_stop(b, px) :
				    lob_search_limit(b, px, s);

				if (t && l != NULL && l->buy != s)
					continue;   /* the other side's level */
				if (cnt[t][s][px] == 0) {
					if (l != NULL)
						return 3;
					continue;
				}
				if (l->size != cnt[t][s][px] ||
				    l->total_volume != vol[t][s][px] ||
				    l->tail == NULL ||
				    l->tail->id != last[t][s][px])
					return 3;
			}
		}
		if (cnt[0][1][px])
			hb = px;
		if (cnt[0][0][px] && ls == 0)
			ls = px;
		if (cnt[1][1][px] && lsb == 0)
			lsb = px;
		if (cnt[1][0][px])
			hss = px;
	}
	if ((b->highest_buy ? b->highest_buy->price : 0) != hb ||
	    (b->lowest_sell ? b->lowest_sell->price : 0) != ls ||
	    (b->lowest_stop_buy ? b->lowest_stop_buy->price : 0) != lsb ||
	    (b->highest_stop_sell ? b->highest_stop_sell->price : 0) != hss)
		return 4;
	return 0;
}

/* Every id the reference does not hold must be absent from lorb. */
static int
compare_dead(const lob_book_t *b, int nid)
{
	int id;

	for (id = 1; id < nid; id++)
		if ((ref_find(id) >= 0) != (lob_search_order(b, id) != NULL))
			return 1;
	return 0;
}

/* ---- driver -------------------------------------------------------- */

/* A live reference order of the wanted class (stop = any stop kind). */
static int
pick(int want_stop)
{
	int i, n = 0, k;

	for (i = 0; i < nR; i++)
		if (R[i].live && (R[i].kind != K_LIMIT) == want_stop)
			n++;
	if (n == 0)
		return -1;
	k = rint_in(0, n - 1);
	for (i = 0; i < nR; i++)
		if (R[i].live && (R[i].kind != K_LIMIT) == want_stop && k-- == 0)
			return i;
	return -1;
}

/*
 * lorb keys every stop level by price alone, so a buy stop and a sell
 * stop must not share a price (documented limitation): skip such ops.
 */
static int
stop_collides(int buy, int stop)
{
	int i;

	for (i = 0; i < nR; i++)
		if (R[i].live && R[i].kind != K_LIMIT && R[i].stop == stop &&
		    R[i].buy != buy)
			return 1;
	return 0;
}

enum { OP_LIMIT, OP_MARKET, OP_CANCEL, OP_MODIFY, OP_STOP, OP_CANCEL_STOP,
       OP_MODIFY_STOP, OP_N };
static const char *op_name[OP_N] = { "add_limit", "market", "cancel",
    "modify", "add_stop", "cancel_stop", "modify_stop" };

/* Run one random op on both books; returns its OP_ kind or -1 if skipped. */
static int
one_op(lob_book_t *b, int *nid)
{
	int r = rint_in(0, 99), buy = (int)(rnd() & 1), sh = rint_in(1, 50);
	int i, px, stop, lim;

	if (r < 30) {
		px = buy ? rint_in(10, 30) : rint_in(16, 36);
		lob_add_limit_order(b, *nid, buy, sh, px);
		ref_limit((*nid)++, buy, sh, px);
		return OP_LIMIT;
	}
	if (r < 35) {
		lob_market_order(b, (*nid)++, buy, sh);
		ref_market(buy, sh);
		return OP_MARKET;
	}
	if (r < 45) {
		if ((i = pick(0)) < 0)
			return -1;
		lob_cancel_limit_order(b, R[i].id);
		R[i].live = 0;
		return OP_CANCEL;
	}
	if (r < 63) {
		if ((i = pick(0)) < 0)
			return -1;
		switch (rint_in(0, 2)) {
		case 0:   /* pure decrease, same price */
			px = R[i].px;
			sh = rint_in(1, R[i].shares);
			break;
		case 1:   /* same price, any size */
			px = R[i].px;
			break;
		default:  /* new price (may cross) */
			px = rint_in(10, 36);
			break;
		}
		lob_modify_limit_order(b, R[i].id, sh, px);
		ref_modify(R[i].id, sh, px);
		return OP_MODIFY;
	}
	if (r < 81) {
		stop = rint_in(5, 45);
		lim = 0;
		if (r >= 73) {
			lim = buy ? stop + rint_in(0, 4) : stop - rint_in(0, 4);
			if (lim < 1)
				lim = 1;
		}
		if (stop_collides(buy, stop))
			return -1;
		if (lim != 0)
			lob_add_stop_limit_order(b, *nid, buy, sh, lim, stop);
		else
			lob_add_stop_order(b, *nid, buy, sh, stop);
		ref_add_stop((*nid)++, buy, sh, stop, lim);
		return OP_STOP;
	}
	if ((i = pick(1)) < 0)
		return -1;
	if (r < 87) {
		if (R[i].kind == K_STOP)
			lob_cancel_stop_order(b, R[i].id);
		else
			lob_cancel_stop_limit_order(b, R[i].id);
		R[i].live = 0;
		return OP_CANCEL_STOP;
	}
	buy = R[i].buy;
	stop = R[i].stop;
	lim = R[i].px;
	if (rint_in(0, 1) == 0)
		sh = rint_in(1, R[i].shares);          /* pure decrease */
	else
		stop = rint_in(5, 45);
	if (lim != 0) {
		lim = buy ? stop + rint_in(0, 4) : stop - rint_in(0, 4);
		if (lim < 1)
			lim = 1;
		if (stop == R[i].stop && sh <= R[i].shares)
			lim = R[i].px;
	}
	if (stop_collides(buy, stop))
		return -1;
	if (lim != 0)
		lob_modify_stop_limit_order(b, R[i].id, sh, lim, stop);
	else
		lob_modify_stop_order(b, R[i].id, sh, stop);
	ref_modify_stop(R[i].id, sh, stop, lim);
	return OP_MODIFY_STOP;
}

int
main(int argc, char **argv)
{
	int seeds = argc > 1 ? atoi(argv[1]) : 1000;
	int ops = argc > 2 ? atoi(argv[2]) : 2000;
	long div[OP_N] = {0}, nops[OP_N] = {0};
	int s, k, bad = 0, printed = 0;

	for (s = 1; s <= seeds; s++) {
		lob_book_t *b = NULL;
		int nid = 1, kind = -1, c = 0;

		if (lob_book_create(&b) != 0) {
			fprintf(stderr, "lob_book_create failed\n");
			return 2;
		}
		nR = 0;
		P = 0;
		rs = (uint64_t)s;
		for (k = 0; k < ops; k++) {
			if (nR > MAXO / 2)
				ref_compact();
			if ((kind = one_op(b, &nid)) < 0)
				continue;
			nops[kind]++;
			c = compare(b);
			if (c == 0 && (k % 128 == 0 || k == ops - 1))
				c = compare_dead(b, nid) ? 1 : 0;
			if (c != 0)
				break;
		}
		if (c != 0) {
			div[kind]++;
			bad++;
			if (printed++ < 5)
				printf("seed %d op %d (%s): divergence class %d\n",
				    s, k, op_name[kind], c);
		}
		lob_book_destroy(b);
	}
	printf("lorb differential: %d seeds x %d ops\n", seeds, ops);
	for (k = 0; k < OP_N; k++)
		printf("  %-12s ops=%-8ld first-divergences=%ld\n", op_name[k],
		    nops[k], div[k]);
	printf("%s: %d/%d seeds diverged\n", bad ? "FAIL" : "ok", bad, seeds);
	return bad ? 1 : 0;
}

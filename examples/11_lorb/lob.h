/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/11_lorb/lob.h
 *	A limit order book (matching engine) built on libxtc.  This is
 *	a faithful C port of the C++ engine at
 *	https://github.com/.../limit-order-book: a binary (AVL) tree of
 *	price Limits per side, each Limit holding a FIFO doubly-linked
 *	list of Orders, with book-edge pointers for O(1) best-bid/offer
 *	and hash maps (xtc_chash) keyed by order id and price for O(1)
 *	lookup.  Order/Limit nodes come from xtc_slab pools.
 *
 *	Price-time priority: within a price level orders match FIFO;
 *	across levels the best price matches first.  Supports Market,
 *	Limit (add/cancel/modify), Stop, and Stop-Limit orders.
 *
 *	Prices and share counts are positive ints; a limit price of 0
 *	on an Order marks it as a (converted) stop MARKET order, exactly
 *	as in the original.
 */

#ifndef LORB_LOB_H
#define LORB_LOB_H

#include <stdbool.h>

#include "xtc_chash.h"
#include "xtc_slab.h"

typedef struct lob_limit lob_limit_t;

/* Order category -- lets a driver pick a same-category order to
 * cancel/modify (the C++ generator keeps three separate live-order
 * sets; this tag stands in for that). */
typedef enum lob_cat {
	LOB_LIMIT = 0,
	LOB_STOP = 1,
	LOB_STOP_LIMIT = 2
} lob_cat_t;

/*
 * An order is an intrusive node in its parent limit's doubly-linked
 * FIFO list.  `id` is also the chash key (the map stores a pointer to
 * this field).
 */
typedef struct lob_order {
	int             id;
	bool            buy;      /* true = buy, false = sell */
	int             shares;
	int             limit;    /* limit price; 0 means "market" */
	lob_cat_t       cat;      /* which live-order set it belongs to */
	struct lob_order *next;
	struct lob_order *prev;
	lob_limit_t    *parent;
} lob_order_t;

/*
 * A limit is one price level: an AVL-tree node holding a FIFO order
 * list.  Used for both the limit trees and the stop trees.  `price`
 * is the chash key.
 */
struct lob_limit {
	int             price;
	int             size;        /* number of orders */
	int             total_volume;/* sum of order shares */
	bool            buy;
	lob_limit_t    *parent;
	lob_limit_t    *left;
	lob_limit_t    *right;
	lob_order_t    *head;
	lob_order_t    *tail;
};

typedef struct lob_book {
	lob_limit_t *buy_tree;
	lob_limit_t *sell_tree;
	lob_limit_t *lowest_sell;
	lob_limit_t *highest_buy;

	lob_limit_t *stop_buy_tree;
	lob_limit_t *stop_sell_tree;
	lob_limit_t *highest_stop_sell;
	lob_limit_t *lowest_stop_buy;

	xtc_chash_t *order_map;      /* id     -> lob_order_t*  */
	xtc_chash_t *limit_buy_map;  /* price  -> lob_limit_t*  */
	xtc_chash_t *limit_sell_map; /* price  -> lob_limit_t*  */
	xtc_chash_t *stop_map;       /* price  -> lob_limit_t*  */

	xtc_slab_t  *order_slab;
	xtc_slab_t  *limit_slab;

	/* Perf counters, reset per public op, as in the original. */
	int executed_orders_count;
	int avl_balance_count;
} lob_book_t;

/* Lifecycle. */
int  lob_book_create(lob_book_t **out);
void lob_book_destroy(lob_book_t *b);

/* Order operations (mirror the C++ Book public API). */
void lob_market_order(lob_book_t *b, int id, bool buy, int shares);
void lob_add_limit_order(lob_book_t *b, int id, bool buy, int shares,
                         int limit_price);
void lob_cancel_limit_order(lob_book_t *b, int id);
void lob_modify_limit_order(lob_book_t *b, int id, int new_shares,
                            int new_limit);
void lob_add_stop_order(lob_book_t *b, int id, bool buy, int shares,
                        int stop_price);
void lob_cancel_stop_order(lob_book_t *b, int id);
void lob_modify_stop_order(lob_book_t *b, int id, int new_shares,
                           int new_stop_price);
void lob_add_stop_limit_order(lob_book_t *b, int id, bool buy, int shares,
                              int limit_price, int stop_price);
void lob_cancel_stop_limit_order(lob_book_t *b, int id);
void lob_modify_stop_limit_order(lob_book_t *b, int id, int new_shares,
                                 int new_limit_price, int new_stop_price);

/* Lookups (needed by tests). */
lob_order_t *lob_search_order(const lob_book_t *b, int id);
lob_limit_t *lob_search_limit(const lob_book_t *b, int price, bool buy);
lob_limit_t *lob_search_stop(const lob_book_t *b, int price);
int          lob_limit_height(const lob_limit_t *limit);

/*
 * In-order / pre-order / post-order price traversals.  Writes up to
 * `cap` prices into `out`, returns the count written (the tree size).
 */
size_t lob_inorder(const lob_limit_t *root, int *out, size_t cap);
size_t lob_preorder(const lob_limit_t *root, int *out, size_t cap);
size_t lob_postorder(const lob_limit_t *root, int *out, size_t cap);

#endif /* LORB_LOB_H */

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/11_lorb/lob.c
 *	Limit order book engine -- a C port of the C++ matching engine.
 *	Single-threaded and sequential (matching is inherently serial),
 *	so it uses xtc_chash + xtc_slab as the map/pool primitives but
 *	does not need cross-thread concurrency; every chash_get is still
 *	wrapped in the required xtc_rcu read-side bracket.
 */

#include "lob.h"

#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_rcu.h"

/* ---- chash key helpers ---------------------------------------------
 *
 * chash stores caller-owned void* keys.  We key on an int stored
 * inside each node (order->id, limit->price), so the key pointer is
 * &node->field.  cmp/hash operate on those int pointers.
 */
static int
int_cmp(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;
	return (x > y) - (x < y);
}

static uint64_t
int_hash(const void *k)
{
	/* splitmix64 on the int -- cheap, well-distributed. */
	uint64_t z = (uint64_t)(uint32_t)*(const int *)k;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static lob_order_t *
map_get_order(xtc_chash_t *h, int id)
{
	void *v = NULL;
	xtc_rcu_read_lock();
	(void)xtc_chash_get(h, &id, &v);
	xtc_rcu_read_unlock();
	return (lob_order_t *)v;
}

static lob_limit_t *
map_get_limit(xtc_chash_t *h, int price)
{
	void *v = NULL;
	xtc_rcu_read_lock();
	(void)xtc_chash_get(h, &price, &v);
	xtc_rcu_read_unlock();
	return (lob_limit_t *)v;
}

/* ---- node allocation ----------------------------------------------- */

static lob_order_t *
order_new(lob_book_t *b, int id, bool buy, int shares, int limit,
          lob_cat_t cat)
{
	lob_order_t *o = xtc_slab_alloc(b->order_slab);
	if (o == NULL)
		return NULL;
	o->id = id;
	o->buy = buy;
	o->shares = shares;
	o->limit = limit;
	o->cat = cat;
	o->next = o->prev = NULL;
	o->parent = NULL;
	return o;
}

static lob_limit_t *
limit_new(lob_book_t *b, int price, bool buy)
{
	lob_limit_t *l = xtc_slab_alloc(b->limit_slab);
	if (l == NULL)
		return NULL;
	l->price = price;
	l->size = 0;
	l->total_volume = 0;
	l->buy = buy;
	l->parent = l->left = l->right = NULL;
	l->head = l->tail = NULL;
	return l;
}

/* ---- Order operations (Order.cpp) ---------------------------------- */

static void
order_partially_fill(lob_order_t *o, int taken)
{
	o->shares -= taken;
	o->parent->total_volume -= taken;
}

/* Unlink an arbitrary order from its parent limit's list (cancel). */
static void
order_cancel(lob_order_t *o)
{
	if (o->prev == NULL)
		o->parent->head = o->next;
	else
		o->prev->next = o->next;
	if (o->next == NULL)
		o->parent->tail = o->prev;
	else
		o->next->prev = o->prev;

	o->parent->total_volume -= o->shares;
	o->parent->size -= 1;
}

/* Pop the head order off its parent limit (execute). */
static void
order_execute(lob_order_t *o)
{
	o->parent->head = o->next;
	if (o->next == NULL)
		o->parent->tail = NULL;
	else
		o->next->prev = NULL;
	o->next = o->prev = NULL;

	o->parent->total_volume -= o->shares;
	o->parent->size -= 1;
}

static void
order_modify(lob_order_t *o, int new_shares, int new_limit)
{
	o->shares = new_shares;
	o->limit = new_limit;
	o->next = o->prev = NULL;
	o->parent = NULL;
}

/* ---- Limit operations (Limit.cpp) ---------------------------------- */

static void
limit_append(lob_limit_t *l, lob_order_t *o)
{
	if (l->head == NULL) {
		l->head = l->tail = o;
	} else {
		l->tail->next = o;
		o->prev = l->tail;
		o->next = NULL;
		l->tail = o;
	}
	l->size += 1;
	l->total_volume += o->shares;
	o->parent = l;
}

/*
 * Splice a limit out of its BST, preserving BST order.  Returns the
 * node from which the caller must rebalance upward (the deepest node
 * whose subtree structure changed) -- NULL if nothing needs
 * rebalancing.  This is the C++ Limit-destructor splice plus the
 * standard AVL-delete correction: after promoting a deep in-order
 * successor, the imbalance is at the SUCCESSOR'S ORIGINAL PARENT, which
 * can be far below the deleted node, so rebalancing must begin there,
 * not at the deleted node's parent (starting too high leaves a lower
 * subtree permanently unbalanced, which compounds over many deletes
 * into a corrupt tree).
 */
static lob_limit_t *
limit_splice_out(lob_limit_t *node)
{
	lob_limit_t *parent = node->parent;
	lob_limit_t *left = node->left;
	lob_limit_t *right = node->right;

	if (parent != NULL) {
		bool is_left = (node->price < parent->price);

		/* Case 1: at most one child. */
		if (left == NULL) {
			if (is_left)
				parent->left = right;
			else
				parent->right = right;
			if (right != NULL)
				right->parent = parent;
			return parent;
		} else if (right == NULL) {
			if (is_left)
				parent->left = left;
			else
				parent->right = left;
			left->parent = parent;
			return parent;
		}

		/* Case 2: two children -- promote in-order successor. */
		lob_limit_t *temp = right;
		while (temp->left != NULL)
			temp = temp->left;
		lob_limit_t *rebal;

		if (right->left != NULL) {
			rebal = temp->parent; /* deepest changed node */
			temp->parent->left = temp->right;
			if (temp->right != NULL)
				temp->right->parent = temp->parent;
			temp->right = right;
			right->parent = temp;
		} else {
			rebal = temp;         /* successor is node's right child */
		}

		temp->parent = parent;
		temp->left = left;
		left->parent = temp;
		if (is_left)
			parent->left = temp;
		else
			parent->right = temp;
		return rebal;
	} else {
		/* Root. */
		if (left == NULL && right == NULL) {
			return NULL;
		} else if (left == NULL) {
			right->parent = NULL;
			return right;
		} else if (right == NULL) {
			left->parent = NULL;
			return left;
		}

		lob_limit_t *temp = right;
		while (temp->left != NULL)
			temp = temp->left;
		lob_limit_t *rebal;
		if (right->left != NULL) {
			rebal = temp->parent;
			temp->parent->left = temp->right;
			if (temp->right != NULL)
				temp->right->parent = temp->parent;
			temp->right = right;
			right->parent = temp;
		} else {
			rebal = temp;
		}
		temp->parent = parent; /* NULL */
		temp->left = left;
		left->parent = temp;
		return rebal;
	}
}

/* ---- tree height / balance (AVL) ----------------------------------- */

int
lob_limit_height(const lob_limit_t *l)
{
	if (l == NULL)
		return 0;
	int lh = lob_limit_height(l->left);
	int rh = lob_limit_height(l->right);
	return (lh > rh ? lh : rh) + 1;
}

static int
height_diff(const lob_limit_t *l)
{
	return lob_limit_height(l->left) - lob_limit_height(l->right);
}

/*
 * Rotations.  Each rotation fully re-links parent pointers AND the
 * grandparent's child pointer (or the tree-root slot when rotating the
 * root), so callers never need a separate re-link fixup -- the whole
 * class of "child's ->parent disagrees with the parent's child slot"
 * bugs simply cannot occur.  Returns the new subtree root.
 */
static void
relink_child(lob_limit_t *gp, lob_limit_t *old_child, lob_limit_t *new_child,
             lob_limit_t **tree_slot)
{
	if (gp == NULL) {
		*tree_slot = new_child;
	} else if (gp->left == old_child) {
		gp->left = new_child;
	} else {
		gp->right = new_child;
	}
}

static lob_limit_t *
rr_rotate(lob_limit_t *parent, lob_limit_t **tree_slot)
{
	lob_limit_t *gp = parent->parent;
	lob_limit_t *np = parent->right;
	parent->right = np->left;
	if (np->left != NULL)
		np->left->parent = parent;
	np->left = parent;
	np->parent = gp;
	parent->parent = np;
	relink_child(gp, parent, np, tree_slot);
	return np;
}

static lob_limit_t *
ll_rotate(lob_limit_t *parent, lob_limit_t **tree_slot)
{
	lob_limit_t *gp = parent->parent;
	lob_limit_t *np = parent->left;
	parent->left = np->right;
	if (np->right != NULL)
		np->right->parent = parent;
	np->right = parent;
	np->parent = gp;
	parent->parent = np;
	relink_child(gp, parent, np, tree_slot);
	return np;
}

static lob_limit_t *
lr_rotate(lob_limit_t *parent, lob_limit_t **tree_slot)
{
	(void)rr_rotate(parent->left, tree_slot);
	return ll_rotate(parent, tree_slot);
}

static lob_limit_t *
rl_rotate(lob_limit_t *parent, lob_limit_t **tree_slot)
{
	(void)ll_rotate(parent->right, tree_slot);
	return rr_rotate(parent, tree_slot);
}

static lob_limit_t *
balance(lob_book_t *b, lob_limit_t *l, lob_limit_t **tree_slot)
{
	int bf = height_diff(l);
	if (bf > 1) {
		if (height_diff(l->left) >= 0)
			l = ll_rotate(l, tree_slot);
		else
			l = lr_rotate(l, tree_slot);
		b->avl_balance_count += 1;
	} else if (bf < -1) {
		if (height_diff(l->right) > 0)
			l = rl_rotate(l, tree_slot);
		else
			l = rr_rotate(l, tree_slot);
		b->avl_balance_count += 1;
	}
	return l;
}

/* Resolve the tree-root slot for a limit's side (limit or stop). */
static lob_limit_t **
limit_tree_slot(lob_book_t *b, bool buy)
{
	return buy ? &b->buy_tree : &b->sell_tree;
}

static lob_limit_t **
stop_tree_slot(lob_book_t *b, bool buy)
{
	return buy ? &b->stop_buy_tree : &b->stop_sell_tree;
}

/* Recursive BST insert with rebalance (port of Book::insert). */
static lob_limit_t *
tree_insert(lob_book_t *b, lob_limit_t *root, lob_limit_t *node,
            lob_limit_t *parent, lob_limit_t **tree_slot, bool stop)
{
	if (root == NULL) {
		node->parent = parent;
		return node;
	}
	if (node->price < root->price) {
		root->left = tree_insert(b, root->left, node, root, tree_slot,
		    stop);
		root = balance(b, root, tree_slot);
	} else if (node->price > root->price) {
		root->right = tree_insert(b, root->right, node, root, tree_slot,
		    stop);
		root = balance(b, root, tree_slot);
	}
	return root;
}

/* ---- book-edge maintenance ----------------------------------------- */

/* Leftmost (min price) / rightmost (max price) of a tree, or NULL. */
static lob_limit_t *
tree_min(lob_limit_t *root)
{
	if (root == NULL)
		return NULL;
	while (root->left != NULL)
		root = root->left;
	return root;
}

static lob_limit_t *
tree_max(lob_limit_t *root)
{
	if (root == NULL)
		return NULL;
	while (root->right != NULL)
		root = root->right;
	return root;
}

/*
 * Recompute every book edge from the current tree roots.  The insert
 * path keeps edges up to date in O(1); the DELETE path (below) calls
 * this to reset the edges in O(log n) after a level is removed, which
 * is both correct and simpler than the C++ original's edge-patching
 * heuristic (that heuristic could leave an edge pointer dangling or
 * NULL while the tree still held levels -- the latent fragility that
 * makes the C++ engine crash under sustained mixed stop load).
 */
static void
recompute_edges(lob_book_t *b)
{
	b->highest_buy = tree_max(b->buy_tree);
	b->lowest_sell = tree_min(b->sell_tree);
	b->lowest_stop_buy = tree_min(b->stop_buy_tree);
	b->highest_stop_sell = tree_max(b->stop_sell_tree);
}

static void
update_book_edge_insert(lob_book_t *b, lob_limit_t *nl)
{
	if (nl->buy) {
		if (nl->price > b->highest_buy->price)
			b->highest_buy = nl;
	} else {
		if (nl->price < b->lowest_sell->price)
			b->lowest_sell = nl;
	}
}

static void
update_stop_book_edge_insert(lob_book_t *b, lob_limit_t *ns)
{
	if (ns->buy) {
		if (ns->price < b->lowest_stop_buy->price)
			b->lowest_stop_buy = ns;
	} else {
		if (ns->price > b->highest_stop_sell->price)
			b->highest_stop_sell = ns;
	}
}

/* ---- root replacement when the tree root is deleted ---------------- */

static void
change_book_roots(lob_book_t *b, lob_limit_t *l)
{
	lob_limit_t **tree = limit_tree_slot(b, l->buy);
	if (l != *tree)
		return;
	if (l->right != NULL) {
		*tree = (*tree)->right;
		while ((*tree)->left != NULL)
			*tree = (*tree)->left;
	} else {
		*tree = l->left;
	}
}

static void
change_stop_book_roots(lob_book_t *b, lob_limit_t *s)
{
	lob_limit_t **tree = stop_tree_slot(b, s->buy);
	if (s != *tree)
		return;
	if (s->right != NULL) {
		*tree = (*tree)->right;
		while ((*tree)->left != NULL)
			*tree = (*tree)->left;
	} else {
		*tree = s->left;
	}
}

/* ---- add a new (empty) limit / stop level -------------------------- */

static void
add_limit(lob_book_t *b, int price, bool buy)
{
	xtc_chash_t *map = buy ? b->limit_buy_map : b->limit_sell_map;
	lob_limit_t **tree = limit_tree_slot(b, buy);
	lob_limit_t **edge = buy ? &b->highest_buy : &b->lowest_sell;

	lob_limit_t *nl = limit_new(b, price, buy);
	(void)xtc_chash_insert(map, &nl->price, nl, NULL);

	if (*tree == NULL) {
		*tree = nl;
		*edge = nl;
	} else {
		(void)tree_insert(b, *tree, nl, NULL, tree, false);
		update_book_edge_insert(b, nl);
	}
}

static void
add_stop(lob_book_t *b, int price, bool buy)
{
	lob_limit_t **tree = stop_tree_slot(b, buy);
	lob_limit_t **edge = buy ? &b->lowest_stop_buy : &b->highest_stop_sell;

	lob_limit_t *ns = limit_new(b, price, buy);
	(void)xtc_chash_insert(b->stop_map, &ns->price, ns, NULL);

	if (*tree == NULL) {
		*tree = ns;
		*edge = ns;
	} else {
		(void)tree_insert(b, *tree, ns, NULL, tree, true);
		update_stop_book_edge_insert(b, ns);
	}
}

/* ---- delete an emptied limit / stop level -------------------------- */

static void
delete_limit(lob_book_t *b, lob_limit_t *l)
{
	lob_limit_t **tree = limit_tree_slot(b, l->buy);
	xtc_chash_t *map = l->buy ? b->limit_buy_map : b->limit_sell_map;

	(void)xtc_chash_remove(map, &l->price, NULL);
	change_book_roots(b, l);

	lob_limit_t *parent = limit_splice_out(l);
	xtc_slab_free(b->limit_slab, l);

	while (parent != NULL) {
		lob_limit_t *up = parent->parent;
		(void)balance(b, parent, tree);
		parent = up;
	}
	recompute_edges(b);
}

static void
delete_stop_level(lob_book_t *b, lob_limit_t *s)
{
	lob_limit_t **tree = stop_tree_slot(b, s->buy);

	(void)xtc_chash_remove(b->stop_map, &s->price, NULL);
	change_stop_book_roots(b, s);

	lob_limit_t *parent = limit_splice_out(s);
	xtc_slab_free(b->limit_slab, s);

	while (parent != NULL) {
		lob_limit_t *up = parent->parent;
		(void)balance(b, parent, tree);
		parent = up;
	}
	recompute_edges(b);
}

/* ---- matching core ------------------------------------------------- */

/* Forward decls for the mutually recursive stop machinery. */
static void execute_stop_orders(lob_book_t *b, bool buy);

/*
 * Execute a market order against the book.  If the book cannot fully
 * fill it, the remainder is simply forgotten (as in the original).
 */
static void
market_order_helper(lob_book_t *b, bool buy, int shares)
{
	lob_limit_t **edge = buy ? &b->lowest_sell : &b->highest_buy;

	while (*edge != NULL && (*edge)->head->shares <= shares) {
		lob_order_t *ho = (*edge)->head;
		shares -= ho->shares;
		order_execute(ho);
		if ((*edge)->size == 0)
			delete_limit(b, *edge);
		(void)xtc_chash_remove(b->order_map, &ho->id, NULL);
		xtc_slab_free(b->order_slab, ho);
		b->executed_orders_count += 1;
	}
	if (*edge != NULL && shares != 0) {
		order_partially_fill((*edge)->head, shares);
		b->executed_orders_count += 1;
	}
}

/*
 * A limit order that crosses the book executes its crossing part as a
 * market order.  Returns the shares left to rest on the book (0 if
 * fully filled).
 */
static int
limit_order_as_market_order(lob_book_t *b, int id, bool buy, int shares,
                            int limit_price)
{
	(void)id;
	if (buy) {
		while (b->lowest_sell != NULL && shares != 0 &&
		    b->lowest_sell->price <= limit_price) {
			if (shares <= b->lowest_sell->total_volume) {
				market_order_helper(b, buy, shares);
				return 0;
			}
			shares -= b->lowest_sell->total_volume;
			market_order_helper(b, buy,
			    b->lowest_sell->total_volume);
		}
		return shares;
	}
	while (b->highest_buy != NULL && shares != 0 &&
	    b->highest_buy->price >= limit_price) {
		if (shares <= b->highest_buy->total_volume) {
			market_order_helper(b, buy, shares);
			return 0;
		}
		shares -= b->highest_buy->total_volume;
		market_order_helper(b, buy, b->highest_buy->total_volume);
	}
	return shares;
}

/*
 * An already-existing order (a triggered stop-limit) crosses the book:
 * execute its crossing part as a market order.  Frees the order if it
 * is fully filled.  Returns the remaining shares.
 */
static int
existing_order_as_market_order(lob_book_t *b, lob_order_t *ho, bool buy)
{
	int shares = ho->shares;
	int id = ho->id;
	int limit_price = ho->limit;

	if (buy) {
		while (b->lowest_sell != NULL &&
		    b->lowest_sell->price <= limit_price) {
			if (shares <= b->lowest_sell->total_volume) {
				(void)xtc_chash_remove(b->order_map, &id, NULL);
				xtc_slab_free(b->order_slab, ho);
				market_order_helper(b, buy, shares);
				return 0;
			}
			shares -= b->lowest_sell->total_volume;
			market_order_helper(b, buy,
			    b->lowest_sell->total_volume);
		}
		return shares;
	}
	while (b->highest_buy != NULL &&
	    b->highest_buy->price >= limit_price) {
		if (shares <= b->highest_buy->total_volume) {
			(void)xtc_chash_remove(b->order_map, &id, NULL);
			xtc_slab_free(b->order_slab, ho);
			market_order_helper(b, buy, shares);
			return 0;
		}
		shares -= b->highest_buy->total_volume;
		market_order_helper(b, buy, b->highest_buy->total_volume);
	}
	return shares;
}

/* Convert a triggered stop-limit order's head order into a limit order. */
static void
stop_limit_order_to_limit_order(lob_book_t *b, lob_order_t *ho, bool buy)
{
	lob_limit_t **edge = buy ? &b->lowest_stop_buy : &b->highest_stop_sell;
	order_execute(ho);
	if ((*edge)->size == 0)
		delete_stop_level(b, *edge);

	int shares = existing_order_as_market_order(b, ho, buy);
	if (shares != 0) {
		ho->shares = shares;
		ho->cat = LOB_LIMIT;
		xtc_chash_t *map = buy ? b->limit_buy_map : b->limit_sell_map;
		if (map_get_limit(map, ho->limit) == NULL)
			add_limit(b, ho->limit, buy);
		limit_append(map_get_limit(map, ho->limit), ho);
	}
}

/*
 * Fire any stop orders whose stop price has been reached.  Stop market
 * orders re-enter as market orders; stop-limit orders convert to limit
 * orders.  Iterates until no more trigger (a fired stop can move the
 * book edge and trigger the next).
 */
static void
execute_stop_orders(lob_book_t *b, bool buy)
{
	if (buy) {
		while (b->lowest_stop_buy != NULL &&
		    (b->lowest_sell == NULL ||
		     b->lowest_stop_buy->price <= b->lowest_sell->price)) {
			lob_order_t *ho = b->lowest_stop_buy->head;
			if (ho->limit == 0) {
				int shares = ho->shares;
				order_execute(ho);
				if (b->lowest_stop_buy->size == 0)
					delete_stop_level(b, b->lowest_stop_buy);
				(void)xtc_chash_remove(b->order_map, &ho->id,
				    NULL);
				xtc_slab_free(b->order_slab, ho);
				market_order_helper(b, true, shares);
			} else {
				stop_limit_order_to_limit_order(b, ho, buy);
			}
		}
	} else {
		while (b->highest_stop_sell != NULL &&
		    (b->highest_buy == NULL ||
		     b->highest_stop_sell->price >= b->highest_buy->price)) {
			lob_order_t *ho = b->highest_stop_sell->head;
			if (ho->limit == 0) {
				int shares = ho->shares;
				order_execute(ho);
				if (b->highest_stop_sell->size == 0)
					delete_stop_level(b,
					    b->highest_stop_sell);
				(void)xtc_chash_remove(b->order_map, &ho->id,
				    NULL);
				xtc_slab_free(b->order_slab, ho);
				market_order_helper(b, false, shares);
			} else {
				stop_limit_order_to_limit_order(b, ho, buy);
			}
		}
	}
}

/* Stop MARKET order that already crosses the book -> fire immediately. */
static int
stop_order_as_market_order(lob_book_t *b, int id, bool buy, int shares,
                           int stop_price)
{
	if (buy && b->lowest_sell != NULL &&
	    stop_price <= b->lowest_sell->price) {
		lob_market_order(b, id, true, shares);
		return 0;
	} else if (!buy && b->highest_buy != NULL &&
	    stop_price >= b->highest_buy->price) {
		lob_market_order(b, id, false, shares);
		return 0;
	}
	return shares;
}

/* Stop-LIMIT order that already crosses -> enter immediately as limit. */
static int
stop_limit_order_as_limit_order(lob_book_t *b, int id, bool buy, int shares,
                                int limit_price, int stop_price)
{
	if (buy && b->lowest_sell != NULL &&
	    stop_price <= b->lowest_sell->price) {
		lob_add_limit_order(b, id, true, shares, limit_price);
		return 0;
	} else if (!buy && b->highest_buy != NULL &&
	    stop_price >= b->highest_buy->price) {
		lob_add_limit_order(b, id, false, shares, limit_price);
		return 0;
	}
	return shares;
}

/* ---- public order API ---------------------------------------------- */

void
lob_market_order(lob_book_t *b, int id, bool buy, int shares)
{
	(void)id;
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	market_order_helper(b, buy, shares);
	execute_stop_orders(b, buy);
}

void
lob_add_limit_order(lob_book_t *b, int id, bool buy, int shares,
                    int limit_price)
{
	b->avl_balance_count = 0;
	shares = limit_order_as_market_order(b, id, buy, shares, limit_price);

	if (shares != 0) {
		lob_order_t *o = order_new(b, id, buy, shares, limit_price,
		    LOB_LIMIT);
		(void)xtc_chash_insert(b->order_map, &o->id, o, NULL);

		xtc_chash_t *map = buy ? b->limit_buy_map : b->limit_sell_map;
		if (map_get_limit(map, limit_price) == NULL)
			add_limit(b, limit_price, buy);
		limit_append(map_get_limit(map, limit_price), o);
	} else {
		execute_stop_orders(b, buy);
	}
}

void
lob_cancel_limit_order(lob_book_t *b, int id)
{
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	lob_order_t *o = map_get_order(b->order_map, id);
	if (o == NULL)
		return;

	order_cancel(o);
	if (o->parent->size == 0)
		delete_limit(b, o->parent);
	(void)xtc_chash_remove(b->order_map, &o->id, NULL);
	xtc_slab_free(b->order_slab, o);
}

void
lob_modify_limit_order(lob_book_t *b, int id, int new_shares, int new_limit)
{
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	lob_order_t *o = map_get_order(b->order_map, id);
	if (o == NULL)
		return;

	bool buy = o->buy;
	order_cancel(o);
	if (o->parent->size == 0)
		delete_limit(b, o->parent);

	order_modify(o, new_shares, new_limit);
	xtc_chash_t *map = buy ? b->limit_buy_map : b->limit_sell_map;
	if (map_get_limit(map, new_limit) == NULL)
		add_limit(b, new_limit, buy);
	limit_append(map_get_limit(map, new_limit), o);
}

void
lob_add_stop_order(lob_book_t *b, int id, bool buy, int shares, int stop_price)
{
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	shares = stop_order_as_market_order(b, id, buy, shares, stop_price);

	if (shares != 0) {
		lob_order_t *o = order_new(b, id, buy, shares, 0, LOB_STOP);
		(void)xtc_chash_insert(b->order_map, &o->id, o, NULL);
		if (map_get_limit(b->stop_map, stop_price) == NULL)
			add_stop(b, stop_price, buy);
		limit_append(map_get_limit(b->stop_map, stop_price), o);
	}
}

void
lob_cancel_stop_order(lob_book_t *b, int id)
{
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	lob_order_t *o = map_get_order(b->order_map, id);
	if (o == NULL)
		return;

	order_cancel(o);
	if (o->parent->size == 0)
		delete_stop_level(b, o->parent);
	(void)xtc_chash_remove(b->order_map, &o->id, NULL);
	xtc_slab_free(b->order_slab, o);
}

void
lob_modify_stop_order(lob_book_t *b, int id, int new_shares, int new_stop_price)
{
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	lob_order_t *o = map_get_order(b->order_map, id);
	if (o == NULL)
		return;

	bool buy = o->buy;
	order_cancel(o);
	if (o->parent->size == 0)
		delete_stop_level(b, o->parent);

	order_modify(o, new_shares, 0);
	if (map_get_limit(b->stop_map, new_stop_price) == NULL)
		add_stop(b, new_stop_price, buy);
	limit_append(map_get_limit(b->stop_map, new_stop_price), o);
}

void
lob_add_stop_limit_order(lob_book_t *b, int id, bool buy, int shares,
                         int limit_price, int stop_price)
{
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	shares = stop_limit_order_as_limit_order(b, id, buy, shares,
	    limit_price, stop_price);

	if (shares != 0) {
		lob_order_t *o = order_new(b, id, buy, shares, limit_price,
		    LOB_STOP_LIMIT);
		(void)xtc_chash_insert(b->order_map, &o->id, o, NULL);
		if (map_get_limit(b->stop_map, stop_price) == NULL)
			add_stop(b, stop_price, buy);
		limit_append(map_get_limit(b->stop_map, stop_price), o);
	}
}

void
lob_cancel_stop_limit_order(lob_book_t *b, int id)
{
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	lob_order_t *o = map_get_order(b->order_map, id);
	if (o == NULL)
		return;

	order_cancel(o);
	if (o->parent->size == 0)
		delete_stop_level(b, o->parent);
	(void)xtc_chash_remove(b->order_map, &o->id, NULL);
	xtc_slab_free(b->order_slab, o);
}

void
lob_modify_stop_limit_order(lob_book_t *b, int id, int new_shares,
                            int new_limit_price, int new_stop_price)
{
	b->executed_orders_count = 0;
	b->avl_balance_count = 0;
	lob_order_t *o = map_get_order(b->order_map, id);
	if (o == NULL)
		return;

	bool buy = o->buy;
	order_cancel(o);
	if (o->parent->size == 0)
		delete_stop_level(b, o->parent);

	order_modify(o, new_shares, new_limit_price);
	if (map_get_limit(b->stop_map, new_stop_price) == NULL)
		add_stop(b, new_stop_price, buy);
	limit_append(map_get_limit(b->stop_map, new_stop_price), o);
}

/* ---- lookups / traversals ------------------------------------------ */

lob_order_t *
lob_search_order(const lob_book_t *b, int id)
{
	return map_get_order(b->order_map, id);
}

lob_limit_t *
lob_search_limit(const lob_book_t *b, int price, bool buy)
{
	return map_get_limit(buy ? b->limit_buy_map : b->limit_sell_map,
	    price);
}

lob_limit_t *
lob_search_stop(const lob_book_t *b, int price)
{
	return map_get_limit(b->stop_map, price);
}

static size_t
inorder(const lob_limit_t *r, int *out, size_t cap, size_t n)
{
	if (r == NULL)
		return n;
	n = inorder(r->left, out, cap, n);
	if (n < cap)
		out[n] = r->price;
	n++;
	n = inorder(r->right, out, cap, n);
	return n;
}

static size_t
preorder(const lob_limit_t *r, int *out, size_t cap, size_t n)
{
	if (r == NULL)
		return n;
	if (n < cap)
		out[n] = r->price;
	n++;
	n = preorder(r->left, out, cap, n);
	n = preorder(r->right, out, cap, n);
	return n;
}

static size_t
postorder(const lob_limit_t *r, int *out, size_t cap, size_t n)
{
	if (r == NULL)
		return n;
	n = postorder(r->left, out, cap, n);
	n = postorder(r->right, out, cap, n);
	if (n < cap)
		out[n] = r->price;
	n++;
	return n;
}

size_t
lob_inorder(const lob_limit_t *root, int *out, size_t cap)
{
	return inorder(root, out, cap, 0);
}

size_t
lob_preorder(const lob_limit_t *root, int *out, size_t cap)
{
	return preorder(root, out, cap, 0);
}

size_t
lob_postorder(const lob_limit_t *root, int *out, size_t cap)
{
	return postorder(root, out, cap, 0);
}

/* ---- lifecycle ----------------------------------------------------- */

int
lob_book_create(lob_book_t **out)
{
	int rc;

	if (xtc_rcu_init() != XTC_OK)
		return XTC_E_NOMEM;

	lob_book_t *b = xtc_calloc(1, sizeof(*b));
	if (b == NULL)
		return XTC_E_NOMEM;

	xtc_slab_opts_t oopts = XTC_SLAB_OPTS_DEFAULT;
	oopts.name = "lob-order";
	oopts.obj_size = sizeof(lob_order_t);
	oopts.align = _Alignof(lob_order_t);
	xtc_slab_opts_t lopts = XTC_SLAB_OPTS_DEFAULT;
	lopts.name = "lob-limit";
	lopts.obj_size = sizeof(lob_limit_t);
	lopts.align = _Alignof(lob_limit_t);

	if ((rc = xtc_slab_create(&oopts, &b->order_slab)) != XTC_OK)
		goto fail;
	if ((rc = xtc_slab_create(&lopts, &b->limit_slab)) != XTC_OK)
		goto fail;

	if ((rc = xtc_chash_create(int_cmp, int_hash, 4096,
	    &b->order_map)) != XTC_OK)
		goto fail;
	if ((rc = xtc_chash_create(int_cmp, int_hash, 1024,
	    &b->limit_buy_map)) != XTC_OK)
		goto fail;
	if ((rc = xtc_chash_create(int_cmp, int_hash, 1024,
	    &b->limit_sell_map)) != XTC_OK)
		goto fail;
	if ((rc = xtc_chash_create(int_cmp, int_hash, 1024,
	    &b->stop_map)) != XTC_OK)
		goto fail;

	*out = b;
	return XTC_OK;

fail:
	lob_book_destroy(b);
	return rc;
}

void
lob_book_destroy(lob_book_t *b)
{
	if (b == NULL)
		return;
	/* Slab destroy reclaims all order/limit nodes wholesale; the
	 * chash tables own only their own bookkeeping (caller-owned
	 * key/value pointers, which live in the slabs). */
	if (b->order_map != NULL)
		xtc_chash_destroy(b->order_map);
	if (b->limit_buy_map != NULL)
		xtc_chash_destroy(b->limit_buy_map);
	if (b->limit_sell_map != NULL)
		xtc_chash_destroy(b->limit_sell_map);
	if (b->stop_map != NULL)
		xtc_chash_destroy(b->stop_map);
	if (b->order_slab != NULL)
		xtc_slab_destroy(b->order_slab);
	if (b->limit_slab != NULL)
		xtc_slab_destroy(b->limit_slab);
	xtc_free(b);
}

/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/11_lorb/test_lob.c
 *	Unit tests for the lorb matching engine.  These are C ports of
 *	the C++ engine's GoogleTest suite -- add/cancel/modify, BST +
 *	AVL structure, market/limit matching with partial fills, and
 *	each stop / stop-limit order type.  Pass/fail by exit code.
 */

#include <stdbool.h>
#include <string.h>

#include "lob.h"
#include "t_assert.h"

/* Helper: compare a traversal against an expected int array. */
static void
check_traversal(size_t (*fn)(const lob_limit_t *, int *, size_t),
                const lob_limit_t *root, const int *want, size_t nwant)
{
	int got[64];
	size_t n = fn(root, got, 64);
	ASSERT(n == nwant);
	for (size_t i = 0; i < nwant; i++)
		ASSERT(got[i] == want[i]);
}

/* ---- add / lookup -------------------------------------------------- */

static void
test_adding_an_order(lob_book_t *b)
{
	ASSERT(lob_search_order(b, 357) == NULL);
	ASSERT(lob_search_limit(b, 100, true) == NULL);

	lob_add_limit_order(b, 357, true, 27, 100);
	ASSERT(lob_search_order(b, 357)->shares == 27);
	ASSERT(lob_search_limit(b, 100, true)->total_volume == 27);
	ASSERT(lob_search_limit(b, 20, false) == NULL);

	lob_add_limit_order(b, 222, false, 35, 110);
	ASSERT(lob_search_limit(b, 110, false)->total_volume == 35);
}

static void
test_multiple_orders_in_a_limit(lob_book_t *b)
{
	lob_add_limit_order(b, 5, true, 80, 20);
	ASSERT(lob_search_limit(b, 20, true)->total_volume == 80);
	ASSERT(lob_search_limit(b, 20, true)->size == 1);

	lob_add_limit_order(b, 6, true, 32, 20);
	ASSERT(lob_search_limit(b, 20, true)->total_volume == 112);
	ASSERT(lob_search_limit(b, 20, true)->size == 2);

	lob_add_limit_order(b, 7, true, 111, 20);
	ASSERT(lob_search_limit(b, 20, true)->total_volume == 223);
	ASSERT(lob_search_limit(b, 20, true)->size == 3);
}

/* ---- cancel -------------------------------------------------------- */

static void
test_cancel_leaving_nonempty(lob_book_t *b)
{
	lob_add_limit_order(b, 5, true, 80, 20);
	lob_add_limit_order(b, 6, true, 32, 20);
	lob_add_limit_order(b, 7, true, 111, 20);
	ASSERT(lob_search_limit(b, 20, true)->size == 3);
	ASSERT(lob_search_limit(b, 20, true)->total_volume == 223);

	lob_cancel_limit_order(b, 6);
	ASSERT(lob_search_limit(b, 20, true)->size == 2);
	ASSERT(lob_search_limit(b, 20, true)->total_volume == 191);

	lob_cancel_limit_order(b, 7);
	ASSERT(lob_search_limit(b, 20, true)->size == 1);
	ASSERT(lob_search_limit(b, 20, true)->total_volume == 80);
}

static void
test_head_change_on_cancel(lob_book_t *b)
{
	lob_add_limit_order(b, 5, true, 80, 20);
	lob_add_limit_order(b, 6, true, 32, 20);
	lob_add_limit_order(b, 7, true, 111, 20);
	lob_limit_t *l = lob_search_limit(b, 20, true);
	ASSERT(l->head->id == 5);
	lob_cancel_limit_order(b, 5);
	ASSERT(l->head->id == 6);
}

static void
test_cancel_leaving_empty(lob_book_t *b)
{
	lob_add_limit_order(b, 5, true, 80, 20);
	lob_add_limit_order(b, 6, true, 80, 15);
	lob_limit_t *l1 = lob_search_limit(b, 20, true);
	ASSERT(lob_search_limit(b, 15, true)->head->id == 6);
	ASSERT(l1->left->price == 15);

	lob_cancel_limit_order(b, 6);
	ASSERT(lob_search_limit(b, 15, true) == NULL);
	ASSERT(l1->left == NULL);
}

/* ---- BST structure ------------------------------------------------- */

static void
test_correct_parent_children(lob_book_t *b)
{
	lob_add_limit_order(b, 5, true, 80, 20);
	lob_add_limit_order(b, 6, true, 80, 15);
	lob_add_limit_order(b, 7, true, 80, 25);
	lob_limit_t *l1 = lob_search_limit(b, 20, true);
	lob_limit_t *l2 = lob_search_limit(b, 15, true);
	lob_limit_t *l3 = lob_search_limit(b, 25, true);
	ASSERT(l1->parent == NULL);
	ASSERT(l2->parent->price == 20);
	ASSERT(l3->parent->price == 20);
	ASSERT(l1->left->price == 15);
	ASSERT(l1->right->price == 25);
	ASSERT(l2->left == NULL && l2->right == NULL);
}

static void
test_tree_heights(lob_book_t *b)
{
	lob_add_limit_order(b, 5, true, 80, 20);
	lob_limit_t *l1 = lob_search_limit(b, 20, true);
	ASSERT(lob_limit_height(l1) == 1);
	lob_add_limit_order(b, 6, true, 80, 15);
	lob_limit_t *l2 = lob_search_limit(b, 15, true);
	ASSERT(lob_limit_height(l2) == 1);
	ASSERT(lob_limit_height(l1) == 2);
	lob_add_limit_order(b, 7, true, 80, 25);
	ASSERT(lob_limit_height(l1) == 2);
	lob_add_limit_order(b, 8, true, 80, 10);
	lob_limit_t *l4 = lob_search_limit(b, 10, true);
	ASSERT(lob_limit_height(l4) == 1);
	ASSERT(lob_limit_height(l2) == 2);
	ASSERT(lob_limit_height(l1) == 3);
}

static void
test_bst_traversals(lob_book_t *b)
{
	lob_add_limit_order(b, 5, false, 80, 20);
	lob_add_limit_order(b, 6, false, 80, 15);
	lob_add_limit_order(b, 7, false, 80, 25);
	lob_add_limit_order(b, 8, false, 80, 10);
	lob_add_limit_order(b, 9, false, 80, 19);

	int in[] = {10, 15, 19, 20, 25};
	check_traversal(lob_inorder, b->sell_tree, in, 5);
	int pre[] = {20, 15, 10, 19, 25};
	check_traversal(lob_preorder, b->sell_tree, pre, 5);
	int post[] = {10, 19, 15, 25, 20};
	check_traversal(lob_postorder, b->sell_tree, post, 5);
}

/* ---- BST removal cases --------------------------------------------- */

static void
test_remove_two_children(lob_book_t *b)
{
	int ids[] = {5, 6, 7, 8, 9, 10, 11, 12, 13};
	int px[]  = {20, 15, 25, 10, 18, 23, 27, 17, 19};
	for (int i = 0; i < 9; i++)
		lob_add_limit_order(b, ids[i], true, 80, px[i]);

	lob_limit_t *l1 = lob_search_limit(b, 20, true);
	lob_limit_t *l2 = lob_search_limit(b, 10, true);
	lob_limit_t *l3 = lob_search_limit(b, 18, true);
	lob_limit_t *l4 = lob_search_limit(b, 17, true);
	ASSERT(l1->left->price == 15);
	ASSERT(l2->parent->price == 15);
	ASSERT(l3->left->price == 17);

	lob_cancel_limit_order(b, 6);
	ASSERT(l1->left->price == 17);
	ASSERT(l2->parent->price == 17);
	ASSERT(l3->left == NULL);
	ASSERT(l4->left->price == 10);
	ASSERT(l4->right->price == 18);
	ASSERT(l4->parent->price == 20);
}

static void
test_emptying_a_tree(lob_book_t *b)
{
	lob_add_limit_order(b, 5, true, 80, 20);
	ASSERT(b->buy_tree->price == 20);
	lob_cancel_limit_order(b, 5);
	ASSERT(b->buy_tree == NULL);
}

static void
test_remove_root_two_children(lob_book_t *b)
{
	int px[] = {20, 15, 25, 27, 22};
	for (int i = 0; i < 5; i++)
		lob_add_limit_order(b, 5 + i, true, 80, px[i]);
	lob_limit_t *l1 = lob_search_limit(b, 15, true);
	lob_limit_t *l2 = lob_search_limit(b, 25, true);
	lob_limit_t *l3 = lob_search_limit(b, 22, true);
	ASSERT(b->buy_tree->price == 20);
	ASSERT(l2->left->price == 22);
	ASSERT(l3->parent->price == 25);

	lob_cancel_limit_order(b, 5);
	ASSERT(b->buy_tree->price == 22);
	ASSERT(l1->parent->price == 22);
	ASSERT(l2->parent->price == 22);
	ASSERT(l2->left == NULL);
	ASSERT(l3->parent == NULL);
}

/* ---- AVL rotations ------------------------------------------------- */

static void
test_avl_rr_rotate(lob_book_t *b)
{
	int px[] = {20, 15, 25, 10, 17, 30};
	for (int i = 0; i < 6; i++)
		lob_add_limit_order(b, 5 + i, true, 80, px[i]);
	lob_add_limit_order(b, 11, true, 80, 35);

	int in[] = {10, 15, 17, 20, 25, 30, 35};
	check_traversal(lob_inorder, b->buy_tree, in, 7);
	int pre[] = {20, 15, 10, 17, 30, 25, 35};
	check_traversal(lob_preorder, b->buy_tree, pre, 7);
}

/* ---- market orders ------------------------------------------------- */

static void
test_market_single_fill(lob_book_t *b)
{
	lob_add_limit_order(b, 111, false, 100, 80);
	lob_add_limit_order(b, 112, false, 30, 80);
	ASSERT(b->lowest_sell->head->shares == 100);
	lob_market_order(b, 113, true, 20);
	ASSERT(b->lowest_sell->head->shares == 80);
}

static void
test_market_multi_fill(lob_book_t *b)
{
	lob_add_limit_order(b, 111, false, 10, 80);
	lob_add_limit_order(b, 112, false, 10, 80);
	lob_add_limit_order(b, 113, false, 10, 80);
	lob_add_limit_order(b, 114, false, 30, 80);
	ASSERT(b->lowest_sell->head->shares == 10);
	ASSERT(b->lowest_sell->total_volume == 60);
	lob_market_order(b, 115, true, 40);
	ASSERT(b->lowest_sell->head->shares == 20);
	ASSERT(b->lowest_sell->total_volume == 20);
}

static void
test_market_into_next_limit(lob_book_t *b)
{
	lob_add_limit_order(b, 111, false, 10, 80);
	lob_add_limit_order(b, 112, false, 5, 80);
	lob_add_limit_order(b, 113, false, 20, 85);
	lob_market_order(b, 115, true, 20);
	ASSERT(b->lowest_sell->head->shares == 15);
	ASSERT(b->lowest_sell->price == 85);
}

static void
test_market_empty_book(lob_book_t *b)
{
	ASSERT(b->lowest_sell == NULL);
	lob_market_order(b, 115, true, 18);
	ASSERT(b->lowest_sell == NULL);
}

/* ---- modify -------------------------------------------------------- */

static void
test_modify_to_existing_limit(lob_book_t *b)
{
	lob_add_limit_order(b, 111, true, 10, 80);
	lob_add_limit_order(b, 112, true, 20, 80);
	lob_add_limit_order(b, 113, true, 7, 85);
	lob_add_limit_order(b, 114, true, 14, 85);
	lob_modify_limit_order(b, 113, 40, 80);
	ASSERT(lob_search_limit(b, 80, true)->head->id == 111);
	ASSERT(lob_search_limit(b, 85, true)->head->id == 114);
	ASSERT(lob_search_limit(b, 80, true)->total_volume == 70);
	ASSERT(lob_search_limit(b, 85, true)->total_volume == 14);
}

static void
test_modify_invalid_id(lob_book_t *b)
{
	lob_add_limit_order(b, 111, true, 10, 80);
	lob_add_limit_order(b, 113, true, 7, 85);
	lob_modify_limit_order(b, 110, 40, 82); /* no such id */
	ASSERT(lob_search_limit(b, 82, true) == NULL);
	ASSERT(lob_search_order(b, 110) == NULL);
	ASSERT(lob_search_limit(b, 80, true)->total_volume == 10);
}

/* ---- limit order that crosses (market limit) ----------------------- */

static void
test_sell_limit_that_is_market(lob_book_t *b)
{
	lob_add_limit_order(b, 357, true, 40, 100);
	lob_add_limit_order(b, 222, false, 35, 100);
	ASSERT(b->highest_buy->total_volume == 5);
	ASSERT(b->highest_buy->head->shares == 5);
	ASSERT(b->lowest_sell == NULL);
}

static void
test_partial_market_limit(lob_book_t *b)
{
	lob_add_limit_order(b, 357, true, 40, 100);
	lob_add_limit_order(b, 358, true, 40, 99);
	lob_add_limit_order(b, 222, false, 45, 100);
	ASSERT(b->highest_buy->total_volume == 40);
	ASSERT(b->highest_buy->head->shares == 40);
	ASSERT(b->lowest_sell->total_volume == 5);
	ASSERT(b->lowest_sell->head->shares == 5);
}

/* ---- stop orders --------------------------------------------------- */

static void
test_adding_a_stop(lob_book_t *b)
{
	ASSERT(lob_search_stop(b, 100) == NULL);
	lob_add_stop_order(b, 357, true, 27, 100);
	ASSERT(lob_search_order(b, 357)->shares == 27);
	ASSERT(lob_search_stop(b, 100)->total_volume == 27);
	lob_add_stop_order(b, 222, false, 35, 110);
	ASSERT(lob_search_stop(b, 110)->total_volume == 35);
}

static void
test_stop_triggered_by_market_sell(lob_book_t *b)
{
	lob_add_limit_order(b, 111, true, 10, 100);
	lob_add_limit_order(b, 112, true, 10, 99);
	lob_add_limit_order(b, 113, true, 10, 98);
	lob_add_stop_order(b, 114, false, 15, 99);
	ASSERT(b->highest_buy->price == 100);
	ASSERT(b->highest_stop_sell->price == 99);

	lob_market_order(b, 115, false, 11);
	ASSERT(b->highest_buy->price == 98);
	ASSERT(b->highest_buy->total_volume == 4);
	ASSERT(b->highest_stop_sell == NULL);
}

static void
test_multiple_stop_levels_by_market_buy(lob_book_t *b)
{
	lob_add_limit_order(b, 111, false, 30, 101);
	lob_add_limit_order(b, 112, false, 10, 100);
	lob_add_limit_order(b, 113, false, 10, 98);
	lob_add_stop_order(b, 114, true, 15, 99);
	lob_add_stop_order(b, 115, true, 15, 100);
	lob_add_stop_order(b, 116, true, 15, 102);

	int pre[] = {100, 99, 102};
	check_traversal(lob_preorder, b->stop_buy_tree, pre, 3);
	ASSERT(b->lowest_sell->price == 98);
	ASSERT(b->lowest_stop_buy->price == 99);

	lob_market_order(b, 117, true, 11);
	ASSERT(b->lowest_sell->price == 101);
	ASSERT(b->lowest_sell->total_volume == 9);
	ASSERT(b->lowest_stop_buy->price == 102);
}

static void
test_stop_cascade_two_iterations(lob_book_t *b)
{
	lob_add_limit_order(b, 111, true, 10, 100);
	lob_add_limit_order(b, 112, true, 10, 99);
	lob_add_limit_order(b, 113, true, 20, 97);
	lob_add_limit_order(b, 118, true, 30, 95);
	lob_add_stop_order(b, 114, false, 15, 99);
	lob_add_stop_order(b, 115, false, 15, 98);
	lob_add_stop_order(b, 116, false, 15, 96);
	lob_add_stop_order(b, 119, false, 15, 94);

	lob_market_order(b, 117, false, 11);
	ASSERT(b->highest_buy->price == 95);
	ASSERT(b->highest_buy->total_volume == 14);
	ASSERT(b->highest_stop_sell->price == 94);
}

static void
test_stop_that_is_market(lob_book_t *b)
{
	lob_add_limit_order(b, 357, true, 40, 100);
	lob_add_stop_order(b, 222, false, 35, 100);
	ASSERT(b->highest_buy->total_volume == 5);
	ASSERT(b->highest_buy->head->shares == 5);
	ASSERT(b->highest_stop_sell == NULL);
}

/* ---- stop limit orders --------------------------------------------- */

static void
test_adding_a_stop_limit(lob_book_t *b)
{
	lob_add_stop_limit_order(b, 357, true, 27, 110, 100);
	ASSERT(lob_search_order(b, 357)->shares == 27);
	ASSERT(lob_search_stop(b, 100)->total_volume == 27);
	lob_add_stop_limit_order(b, 222, false, 35, 105, 110);
	ASSERT(lob_search_stop(b, 110)->total_volume == 35);
}

static void
test_stop_limit_to_market(lob_book_t *b)
{
	lob_add_limit_order(b, 111, true, 10, 100);
	lob_add_limit_order(b, 112, true, 10, 99);
	lob_add_limit_order(b, 113, true, 10, 98);
	lob_add_stop_limit_order(b, 114, false, 15, 97, 99);
	ASSERT(b->highest_stop_sell->price == 99);

	lob_market_order(b, 115, false, 11);
	ASSERT(b->highest_buy->price == 98);
	ASSERT(b->highest_buy->total_volume == 4);
	ASSERT(b->highest_stop_sell == NULL);
}

static void
test_stop_limit_partial_to_market(lob_book_t *b)
{
	lob_add_limit_order(b, 111, true, 10, 100);
	lob_add_limit_order(b, 112, true, 10, 99);
	lob_add_limit_order(b, 113, true, 10, 98);
	lob_add_stop_limit_order(b, 114, false, 15, 99, 99);

	lob_market_order(b, 115, false, 11);
	ASSERT(b->highest_buy->price == 98);
	ASSERT(b->highest_buy->total_volume == 10);
	ASSERT(b->lowest_sell->price == 99);
	ASSERT(b->lowest_sell->total_volume == 6);
	ASSERT(b->highest_stop_sell == NULL);
}

static void
test_stop_limit_to_limit(lob_book_t *b)
{
	lob_add_limit_order(b, 111, true, 10, 100);
	lob_add_limit_order(b, 112, true, 10, 99);
	lob_add_limit_order(b, 113, true, 10, 98);
	lob_add_stop_limit_order(b, 114, false, 15, 100, 99);

	lob_market_order(b, 115, false, 11);
	ASSERT(b->highest_buy->price == 99);
	ASSERT(b->highest_buy->total_volume == 9);
	ASSERT(b->lowest_sell->price == 100);
	ASSERT(b->lowest_sell->total_volume == 15);
	ASSERT(b->highest_stop_sell == NULL);
}

static void
test_stop_and_stop_limit_same_level(lob_book_t *b)
{
	lob_add_stop_limit_order(b, 114, false, 15, 97, 99);
	lob_add_stop_order(b, 115, false, 26, 99);
	lob_add_stop_limit_order(b, 116, false, 5, 99, 99);
	lob_add_stop_order(b, 117, false, 19, 99);
	ASSERT(b->highest_stop_sell->total_volume == 65);

	lob_add_limit_order(b, 111, true, 10, 100);
	lob_add_limit_order(b, 112, true, 10, 99);
	lob_add_limit_order(b, 113, true, 60, 98);

	lob_market_order(b, 118, false, 11);
	ASSERT(b->highest_buy->price == 98);
	ASSERT(b->highest_buy->total_volume == 9);
	ASSERT(b->lowest_sell->price == 99);
	ASSERT(b->lowest_sell->total_volume == 5);
	ASSERT(b->highest_stop_sell == NULL);
}

/* ---- runner: each case gets a fresh book --------------------------- */

typedef void (*case_fn)(lob_book_t *);

static void
run(const char *name, case_fn fn)
{
	lob_book_t *b = NULL;
	ASSERT(lob_book_create(&b) == 0);
	fn(b);
	lob_book_destroy(b);
	printf("  ok  %s\n", name);
}

#define RUN(f) run(#f, f)

int
main(void)
{
	printf("lorb unit tests\n");
	RUN(test_adding_an_order);
	RUN(test_multiple_orders_in_a_limit);
	RUN(test_cancel_leaving_nonempty);
	RUN(test_head_change_on_cancel);
	RUN(test_cancel_leaving_empty);
	RUN(test_correct_parent_children);
	RUN(test_tree_heights);
	RUN(test_bst_traversals);
	RUN(test_remove_two_children);
	RUN(test_emptying_a_tree);
	RUN(test_remove_root_two_children);
	RUN(test_avl_rr_rotate);
	RUN(test_market_single_fill);
	RUN(test_market_multi_fill);
	RUN(test_market_into_next_limit);
	RUN(test_market_empty_book);
	RUN(test_modify_to_existing_limit);
	RUN(test_modify_invalid_id);
	RUN(test_sell_limit_that_is_market);
	RUN(test_partial_market_limit);
	RUN(test_adding_a_stop);
	RUN(test_stop_triggered_by_market_sell);
	RUN(test_multiple_stop_levels_by_market_buy);
	RUN(test_stop_cascade_two_iterations);
	RUN(test_stop_that_is_market);
	RUN(test_adding_a_stop_limit);
	RUN(test_stop_limit_to_market);
	RUN(test_stop_limit_partial_to_market);
	RUN(test_stop_limit_to_limit);
	RUN(test_stop_and_stop_limit_same_level);
	printf("all lorb tests passed\n");
	return 0;
}

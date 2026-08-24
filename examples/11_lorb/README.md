# lorb: a limit order book (matching engine) on libxtc

`lorb` is a C port of a C++ limit-order-book / matching engine, built
on libxtc's public primitives.  It is a faithful re-implementation of
the data structures and matching semantics of
[Matthew Ding's Limit-Order-Book](https://github.com/) -- price-time
(FIFO) priority, partial fills, and the four order types a real
exchange matching engine handles: Market, Limit, Stop, and Stop-Limit
(each with add / cancel / modify).

The engine is deliberately single-threaded: matching is inherently
sequential (it is the throughput bottleneck of any exchange), so lorb
uses libxtc's map and pool primitives rather than its concurrency
machinery:

  * `xtc_chash` -- the concurrent hash tables for the order map (id ->
    order) and the price->level maps (buy limits, sell limits, stops).
    Reads go through the required `xtc_rcu` read-side bracket.
  * `xtc_slab` -- fixed-size object pools for the `Order` and `Limit`
    nodes, so per-order allocation is a magazine pop, not a `malloc`.
  * `xtc_malloc`/`xtc_free` -- the book struct and the driver's latency
    sample buffer.
  * `xtc_clock_mono` -- per-order latency timing in the driver.

Only the public `xtc_*` API is used (the examples are the consumer
exemplar; no internal `__os_*` / `__xtc_*` symbols).

## Architecture

The classic "How to Build a Fast Limit Order Book" structure, mirrored
one-to-one from the C++ classes as C structs:

```
lob_order   id, buy/sell, shares, limit-price, cat,
            {next,prev} intrusive FIFO links, parent level

lob_limit   one price level: price, size, total_volume, buy/sell,
            {parent,left,right} AVL-tree links,
            {head,tail} FIFO order list

lob_book    buy_tree / sell_tree           (AVL, by price)
            stop_buy_tree / stop_sell_tree  (AVL, by price)
            highest_buy / lowest_sell       (O(1) best bid/offer)
            lowest_stop_buy / highest_stop_sell
            order_map / limit_buy_map / limit_sell_map / stop_map (chash)
            order_slab / limit_slab
```

Each side of the book is a balanced (AVL) tree of price `Limit`s; each
`Limit` holds a doubly-linked FIFO list of `Order`s.  Book-edge
pointers give O(1) best-bid / best-offer.  The maps give O(1) lookup by
order id and by price.  So:

  * Add order      -- O(log M) for the first order at a new price
                      level, O(1) for subsequent orders (M = number of
                      price levels, generally << N orders).
  * Cancel / modify -- O(1) lookup + O(log M) tree fix-up if a level
                      empties.
  * Execute        -- O(1) at the book edge.
  * Best bid/offer -- O(1).

Assumptions (as in the original): shares > 0, prices > 0, order ids
unique.  A `Limit` price of `0` on an `Order` marks a (converted) stop
*market* order.

## Files

  * `lob.h` / `lob.c` -- the engine (order, limit, book merged into one
    translation unit; the C++ three-class split adds nothing in C).
  * `driver.c` -- a self-contained generate-and-process benchmark:
    seeds a resting book, replays a mixed order stream (same order-type
    mix and normal price distribution as the C++ generator), times
    every order with `xtc_clock_mono`, and reports throughput plus
    p50/p99/p999 latency from a sorted latency sample.
  * `test_lob.c` -- unit tests ported from the C++ GoogleTest suite:
    add/cancel/modify, BST + AVL structure, market/limit matching with
    partial fills, and each stop / stop-limit order type.
  * `t_assert.h` -- the pass/fail-by-exit-code check macro.

## Build and run

From the repo root, in the nix devShell:

```sh
nix develop --command bash -c 'cd examples/11_lorb && make'
```

(Override `XTC_BUILD` if your libxtc build dir is not `../../build_unix`.)

```sh
make test                       # build + run the unit tests
./lorb                          # 1,000,000 orders, 11,000 resting, seed 12345
./lorb 2000000 11000 42         # n_orders  n_initial  seed
```

The driver prints throughput and the latency percentiles, e.g.:

```
processed 2000000 orders in 0.77 s
throughput: 2589539 orders/s (2.59 M TPS)
latency ns: mean=370  p50=272  p99=1036  p999=2429  max=30147255
```

## Design decisions and trade-offs vs the C++ original

**Merged translation unit.**  The C++ splits Order/Limit/Book across
six files with getters/setters and `friend` access.  In C that split
is pure ceremony, so the engine is one `.c`/`.h` pair with the fields
accessed directly.

**Node pools instead of `new`/`delete`.**  Orders and levels come from
two `xtc_slab` pools.  The original heap-allocates every node; the slab
turns the hot per-order allocation into a magazine pop and frees the
whole population in two `xtc_slab_destroy` calls.

**chash for every map.**  The original uses four
`std::unordered_map<int, T*>`.  lorb uses `xtc_chash`, keying on an int
stored inside each node (the map holds a pointer to that field).  Every
`get` is wrapped in the `xtc_rcu` read-side bracket the API requires,
even though the engine is single-threaded -- that is the price of using
the concurrent map, and it is a deliberate demonstration of the API
contract rather than the fastest possible choice for a single thread.

**Correct AVL delete + O(log M) edge recompute (a fix, not a port).**
This is the one place lorb deliberately *diverges* from the original,
because the original is wrong here:

  * The C++ delete rebalances starting from the deleted node's parent,
    which for a deep in-order-successor promotion leaves a lower subtree
    permanently unbalanced.  Over a long mixed stream the tree
    degenerates.  lorb rebalances from the deepest structurally-changed
    node (the successor's original parent) -- the standard AVL-delete
    correction.
  * The C++ patches the best-bid/offer (and stop-edge) pointers with a
    local heuristic on delete that can leave an edge pointer dangling or
    NULL while the tree still holds levels.  lorb instead recomputes the
    four edges from the tree roots (`tree_min`/`tree_max`) in O(log M)
    after any level is removed -- simpler and always correct.
  * lorb's rotations fully re-link parent pointers and the
    grandparent's child slot inside the rotation, so the "child's
    `->parent` disagrees with the parent's child pointer" class of bug
    cannot occur.

These changes preserve the matching semantics exactly (the trees are
only a price index; order/fill logic is unchanged) and are validated by
an invariant fuzzer: 30 seeds x 2,000,000 mixed operations each, with
every tree checked for BST order, AVL balance, parent-pointer
consistency, and acyclicity -- all pass.  The stock C++ engine, built
`-O2` and run on its *own* generator, hangs or crashes (use-after-free
in `cancelStopLimit`, then tree degeneration) well before 100,000
orders in the same setup; lorb runs millions of orders per seed
cleanly under AddressSanitizer and UndefinedBehaviorSanitizer.

**Single stop price map = one shared engine limitation, kept.**  Like
the original, all stops (buy and sell, market and limit) live in a
single price-keyed map, so a buy stop and a sell stop cannot coexist at
the same price.  The driver keeps buy stops above the spread and sell
stops below it (the original's convention), which keeps the two sides
in disjoint price ranges so the collision never arises.  This is a
faithful limitation, not a lorb bug.

**Known ceiling.**  The AVL height query (`lob_limit_height`) is
recursive, exactly as in the original; on the shallow AVL trees here
that is fine, but a combined ASan+UBSan build's inflated stack frames
can overflow on it (ASan-only and UBSan-only are fine).  A production
engine would cache subtree heights on the node instead of recomputing.
(`lorb:` cache heights on the node if you build under combined
sanitizers or push M into the millions.)

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

## Matching rules

These are the engine's contract; `test_lob_diff.c` checks lorb against
an independent reference matcher that implements exactly this list.

  * **Price-time priority.**  An incoming order matches the best
    opposite price first, FIFO within a level, while it crosses (a buy
    at or above the ask, a sell at or below the bid; a market order has
    no bound).  Each fill trades at the resting order's price.  A limit
    remainder rests at the back of its level; a market remainder is
    dropped.
  * **Modify a limit order.**  Same price and no more shares: the size
    is reduced in place and the order keeps its time priority.  Any
    other modify (a new price, or more shares) loses priority: the order
    is pulled and re-entered as a new aggressive order at the new price,
    so it trades against whatever it now crosses and any remainder rests
    at the back of its level.  The book is never left crossed.
  * **Stop election.**  The engine records the last trade price.  After
    every operation that traded, a buy stop is elected when the last
    trade is at or above its stop price, a sell stop when it is at or
    below.  Elected stops fire one at a time -- the lowest elected
    buy-stop level first, then the highest elected sell-stop level, FIFO
    within a level -- and the test is repeated after each, because a
    fired stop's own fills move the last trade price (cascade).  A stop
    market order then trades as a market order; a stop-limit order as a
    limit order at its limit price, resting any remainder.  So stops
    fire on any trade: a market order, a limit order that crosses only
    partly, a modify, or another stop.
  * **Stop on entry.**  A stop whose price is already at or through the
    opposite best quote when it is added (or modified) is elected
    immediately.
  * **Modify a stop / stop-limit.**  Same stop price, same limit price
    and no more shares: changed in place, keeping its place in the
    trigger queue.  Anything else is a cancel plus a re-add under the
    same id (and the stop-on-entry rule applies).

lorb 1.55.0, following the C++ original, did neither of the
trade-driven parts: its modify
re-linked the order at the new price without matching (a modify could
leave best bid above best ask), and it examined stops only after some
operations, against the opposite quote rather than the trade price (a
partly-crossing limit order never fired a stop; an emptied bid side
fired every resting sell stop with no trade).  The differential test
found both; they are fixed.

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
    partial fills, and each stop / stop-limit order type; plus targeted
    cases for the matching rules above.
  * `test_lob_diff.c` -- differential test: lorb and a flat-array
    reference matcher take the same random stream of limit / market /
    cancel / modify / stop / stop-limit operations (1000 seeds x 2000
    ops by default), and the two books (orders, FIFO positions, level
    totals, book edges) must be identical after every operation.
  * `t_assert.h` -- the pass/fail-by-exit-code check macro.

## Build and run

From the repo root, in the nix devShell:

```sh
nix develop --command bash -c 'cd examples/11_lorb && make'
```

(Override `XTC_BUILD` if your libxtc build dir is not `../../build_unix`.)

```sh
make test                       # build + run the unit + differential tests
./lorb                          # 1,000,000 orders, 11,000 resting, seed 12345
./lorb 2000000 11000 42         # n_orders  n_initial  seed
```

The driver prints throughput and the latency percentiles.  One run of
`./lorb 2000000` on a 32-vCPU host (Xeon Platinum 8488C, Debian
13, gcc 14, default `configure` build of libxtc), pinned to one core:

```
processed 2000000 orders in 1.43 s
throughput: 1405459 orders/s (1.41 M TPS)
latency ns: mean=683  p50=519  p99=1887  p999=3740  max=44454072
```

Five pinned runs there gave 1.33-1.41 M orders/s (p50 ~510-550 ns,
p99 ~1.9-2.0 us); the default 1,000,000-order run gives 1.7-1.9 M/s.  The matching-rule fixes above did not
change it: the 1.55.0 engine measured 1.34-1.43 M/s in the same
interleaved runs.  Earlier figures of ~2.5-2.6 M/s were measured on a
different machine and build and were not reproduced on this one; treat
any number as specific to its host.

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

These tree changes do not affect matching (the trees are only a price
index) and are validated by
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
in disjoint price ranges so the collision does not arise in practice
(checked: none in 6 seeds x 2,000,000 driver orders); the differential
test simply skips a stop that would land on an opposite-side stop
level.  This is a
faithful limitation, not a lorb bug.

**Known ceiling.**  The AVL height query (`lob_limit_height`) is
recursive, exactly as in the original; on the shallow AVL trees here
that is fine, but a combined ASan+UBSan build's inflated stack frames
can overflow on it (ASan-only and UBSan-only are fine).  A production
engine would cache subtree heights on the node instead of recomputing.
(`lorb:` cache heights on the node if you build under combined
sanitizers or push M into the millions.)

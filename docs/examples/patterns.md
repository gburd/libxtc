---
title: Concurrency patterns in small services
parent: Examples
nav_order: 14
permalink: /examples/patterns/
lede: >-
  Sagas, connection pools, request context, stream pipelines, Bloom filters, HyperLogLog, an RCU-read ordered map, and per-session settings, each in a small service where it earns its place.
---

1. TOC
{:toc}

---

libxtc ships several building blocks that are easy to overlook because no
large example uses them: `xtc_saga`, `xtc_pool`, `xtc_pdict`,
`xtc_stream`, `xtc_bloom` and `xtc_hll` (`xtc_prob.h`), `xtc_cskip`, and
the session layer of `xtc_cfg`.  Three small programs put them to work.
Each is a service you would recognize, not a unit test, and each prints
deterministic output that `make check` compares with a recorded copy:

| Example | The service | Patterns |
|---|---|---|
| [`17_checkout`](https://codeberg.org/gregburd/libxtc-examples/src/branch/main/17_checkout) | an order checkout over inventory, payments and shipping | saga, connection pool, per-process request context |
| [`18_clickstream`](https://codeberg.org/gregburd/libxtc-examples/src/branch/main/18_clickstream) | web analytics over four front ends' access logs | stream pipeline, Bloom filter dedup, mergeable HyperLogLog |
| [`19_edgeacl`](https://codeberg.org/gregburd/libxtc-examples/src/branch/main/19_edgeacl) | an edge gateway's IP access table | ordered map read under RCU while it changes, per-session settings |

Build and run them from a configured tree:

```sh
make 17_checkout 18_clickstream 19_edgeacl
./17_checkout && ./18_clickstream && ./19_edgeacl
```

Each example's README has the full design notes; this page summarizes
what each pattern buys and what it costs.

## 17_checkout: a saga over pooled connections

A checkout reserves stock, charges a card and books a carrier, in three
systems that share no transaction.  Holding a lock on the stock while the
card network answers would stall every other buyer, and the payment
network does not speak two-phase commit.  The checkout is a saga instead:
each step comes with the step that undoes it, and when a step fails,
`xtc_saga_run` runs the undo of every step that completed, newest first,
then returns the failing step's error code.

The backend takes two connections and six orders are in flight, so the
orders check connections out of an `xtc_pool`.  A checkout with every
connection busy parks the order's fiber, not the loop's thread, until a
connection comes back: the pool is also the limit on concurrent work.

{% include snippet.html file="20_saga_pool.c" region="saga" %}

The steps take one `void *`.  The backend client and the trace helper get
the connection and the request's trace id from the order proc's
dictionary (`xtc_pdict_get("conn", ...)`) rather than through every
signature.  Thread-local storage would be wrong for this: all six orders
share the loop's one thread and interleave at every round trip.  The
dictionary is per process.

What the saga does not give you: isolation.  Between `reserve` and a
failed `charge`, another order can see the reserved stock.  For a checkout
that is acceptable; for a bank transfer between two of your own accounts
it is not, and a local transaction is the right tool.

## 18_clickstream: a pipeline, a Bloom filter, and sketches that merge

Each front end's log is read by one proc on its own loop, through a lazy
pipeline: `xtc_stream_create` over the log, then `xtc_stream_map` (parse),
three `xtc_stream_filter` stages (malformed lines, crawlers and health
checks, redeliveries) and `xtc_stream_for_each` (count).  One line is in
flight at a time, so a shard's memory does not grow with its log, and each
stage is a function with one job.

The log shipper delivers at least once, so about 3% of the lines arrive
twice.  An exact set of the request ids seen grows with the log; a Bloom
filter sized for a day's lines does not.  The filter never lets a repeat
through, and occasionally drops a new line it mistakes for a repeat: in
the example, 72 of 169,549 page views, under the 1% it was sized for.

Distinct users do not add up across front ends: the four per-front-end
counts sum to 66,121 for 19,980 real users, because people hit several
front ends.  Each shard keeps its own 4 KiB HyperLogLog sketch, and main
merges the four with `xtc_hll_merge`.  The merge is exact, so the merged
sketch estimates the union (20,111, +0.66%), and the shards share nothing
while they run.  The probabilistic structures take no locks; this
one-instance-per-shard-then-merge shape is how they are meant to be used
across threads.

The generator keeps an exact oracle alongside, so the example can show the
error and the test can bound it.  A production pipeline has no oracle,
which is the reason it uses the sketches.

## 19_edgeacl: an ordered map read while it changes, and per-session settings

An address belongs to the range with the largest start at or below it, if
that range's end reaches it.  That is a floor lookup, which an ordered map
answers (`xtc_cskip_floor`) and a hash table cannot.  Eight sessions on
four loops look up addresses while a feed reassigns, adds, withdraws and
splits ranges.  A lookup is an RCU read-side around the floor lookup, with
the answer copied out before the read-side ends; the feed retires what it
replaced or removed, and frees it with `xtc_rcu_synchronize` once no
reader can still hold it.

{% include snippet.html file="21_cskip_floor.c" region="lookup" %}

Note the replace: `xtc_cskip_insert` on an existing key keeps the key the
map already holds and swaps the value, so the new key is freed and the old
value retired.  `xtc_cskip_remove` does not return the key, so the
example looks it up first and retires it with the value.

The example checks itself while it runs.  The feed publishes a version
number before and after each update, and the program keeps a snapshot of
the table at every version.  A live answer must equal the table's answer at
some version between the one finished before the lookup began and the one
started before it ended.  The feed waits, after each update, until every
session has made 64 more lookups, so each version is read from every loop.

The gateway's two settings live in `xtc_cfg` and are read on every request
through `xtc_cfg_ref` handles.  Each session binds its own
`xtc_cfg_session`, so a read resolves "this session's override, else the
global value": the partner sessions run in audit mode from their startup
options, the operator changes a global setting mid-run and every other
session sees it at once, a dry-run request sets audit mode for one request
and rolls it back (`xtc_cfg_session_push` / `_abort`, SET LOCAL), and a
lower-ranked source cannot displace a higher one.

Note that in libxtc 2.2.0, `xtc_rcu_read_lock` and `_read_unlock` on a
fiber take a short internal mutex to find the fiber's reader slot, so many
fiber readers on many loops contend on it.  The example is correct either
way; a read-mostly service that needs its readers to scale across loops
should measure this before it relies on it.

## What the examples check

| Example | Self-check | Fails when (verified by breaking the code) |
|---|---|---|
| 17_checkout | stock, balances, shipments, charges and refunds equal what the successful orders imply; the pool's limit reached and every connection back | a compensation is removed; a connection is not checked in |
| 18_clickstream | every redelivery dropped, every new line counted or lost to a false positive, the false-positive rate under 1%, estimates within four standard errors | the dedup stage is removed; one shard's sketch is left out of the merge |
| 19_edgeacl | every live answer matches a table version; scripted answers match the frozen table; verdicts follow each session's settings | an exact lookup replaces the floor lookup; a session is not bound; the dry-run level is committed instead of aborted |

Each runs in `make check` (`check-examples`), and its output is compared
with the `expected.txt` next to its source.

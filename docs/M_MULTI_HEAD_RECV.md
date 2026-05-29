# Multi-headed receive patterns: should xtc adopt them?

This is a research note evaluating whether xtc_recv should support
the multi-headed selective-receive extension proposed in:

  Sulzmann, Lam, Van Weert.  "Actors with Multi-Headed Message
  Receive Patterns."  COORDINATION 2008, LNCS 5052, 315-330.

The paper is in the project root as
`Actors_with_Multi-Headed_Message_Receive_Patterns.pdf`.  This doc
summarises what the paper proposes and gives a recommendation
for xtc.

## What the paper proposes

Erlang's `receive` matches one message at a time.  If you need to
synchronise on a combination of messages -- say, "deliver toys when
nine reindeer have arrived" or "match a buy-order with a sell-order
of the same quantity" -- you must either:

* Manually buffer non-matching messages and accumulate state until
  you can commit, or
* Nest receive clauses and re-receive the buffered ones each time.

Both are clumsy.  The paper proposes letting a `receive` clause
contain *N* patterns that must all match simultaneously:

```
receive
    Deer x1, Deer x2, ..., Deer x9 -> deliver_toys();
    Elf  e1, Elf  e2, Elf  e3      -> consult_R_and_D();
end
```

Salient features:

1. **Multi-head**: a clause can contain multiple patterns; all must
   bind to distinct messages from the mailbox.
2. **Non-linear**: the same variable can appear in two heads,
   forcing the matched messages to share that value (e.g.
   `Buy x, Sell x` matches a buy-and-sell with equal price).
3. **Guards**: a boolean expression over the bound variables
   constrains the match further.
4. **Two semantics offered**:
   - First-match (Erlang-compatible):  scan the mailbox in arrival
     order, matching the first clause that fires.
   - Rule-priority-match: clauses are tried top-to-bottom; an early
     clause wins even if a later clause's messages arrived first.
5. **Unordered**: the order of patterns in a clause does not change
   semantics; "Buy x, Sell y" == "Sell y, Buy x".
6. **Implementation**: the paper's prototype is a Haskell library;
   it does sequential search of the mailbox.  They mention hash
   indexing as a future optimisation.

The semantics are formalised by mapping to Constraint Handling
Rules (CHR) refined-operational-semantics; the multi-set matching
question is the same one.

## What xtc has today

xtc's selective receive is BEAM-equivalent:

* `xtc_recv` -- single-message FIFO receive.
* `xtc_recv_match(predicate_fn, user)` -- single-message selective
  receive.  The predicate is called for each envelope in arrival
  order; the first that returns true is delivered.  Non-matching
  envelopes go to the save queue.
* save-queue + recv-mark optimisation: messages already tested
  with this predicate are skipped on subsequent calls.  Brought
  the worst-case scan cost from O(M*K) (where M = mailbox depth
  and K = number of receive calls) to O(new arrivals per call)
  in the steady state.

There is **no support today** for multi-head, non-linear, or
guard-constrained patterns at the receive primitive level.

## Where multi-headed receive is genuinely useful

Patterns where this would be a real ergonomic win:

* **Two-phase commit**: wait for ACK from coordinator AND ACK
  from log writer before committing.  Today the user buffers one
  ACK, then receives the other; ~20 lines of code.  Multi-head
  shrinks to 1 line.
* **Fork-join**: a parent spawned 5 children; wait for all 5 to
  send results before proceeding.  Today: count-down state
  machine.  Multi-head: 1 line with non-linear pattern matching
  on a shared correlation id.
* **Quorum decisions**: K-of-N votes.  Multi-head with guards
  expresses this directly.
* **Buy-sell matching**: the canonical CHR example.  Two messages
  with equal price; bind once, transact.

These are real concurrency patterns that show up in production
systems.  The current xtc API handles them all with state-machine
code; the paper's extension would handle them more concisely.

## Where it isn't useful

Patterns where the paper's extension adds little or hurts:

* **Single-message patterns**: still 99% of receive calls.  No
  benefit; multi-head reduces to single-head.
* **Streaming protocols**: where each message is processed
  independently and the order is significant.  Multi-head doesn't
  help; might even hide the per-message processing logic.
* **Patterns with state**: where the receive logic depends on
  prior state (counters, accumulated lists).  Multi-head is purely
  pattern-matching; state still has to live somewhere outside the
  receive.

## Cost of adopting it

If we wanted to bring this to xtc, here's the engineering reality:

### API ergonomics in C

In Erlang/Haskell the syntax is native pattern matching.  In C we'd
need something like:

```c
typedef int (*xtc_head_fn)(const void *msg, size_t sz, void *ctx);
typedef int (*xtc_guard_fn)(const xtc_msg_t heads[], int n,
                             void *ctx);

int xtc_recv_join(xtc_head_fn heads[], int n_heads,
                  xtc_guard_fn guard,
                  void *ctx,
                  xtc_msg_t out[],   /* matched messages, deliver-all */
                  int64_t timeout_ns);
```

Calling code:

```c
static int is_deer(const void *m, size_t sz, void *u)  { ... }
static int is_elf (const void *m, size_t sz, void *u)  { ... }

xtc_head_fn deer_pattern[9] = { is_deer, is_deer, is_deer,
                                 is_deer, is_deer, is_deer,
                                 is_deer, is_deer, is_deer };
xtc_msg_t   deer[9];
xtc_recv_join(deer_pattern, 9, NULL, NULL, deer, -1);
```

This is more verbose than the manual state-machine equivalent for
small N, and only wins for genuinely declarative join patterns.
Compare to the paper's Erlang/Haskell example: their version is
strictly less verbose because their host language has pattern
matching as a first-class construct.  C doesn't.

For non-linear patterns ("the price field must match across two
messages"), in C the user has to express that as a guard function
that re-extracts the field from each message.  Equally awkward
either way.

### Implementation complexity

The paper acknowledges naive multi-set matching is sequential search
through O(M^N) combinations, where M is mailbox depth and N is
pattern arity.  For Santa-with-9-reindeer, that's M^9 -- catastrophic
on a busy actor.

Mitigations the paper mentions for future work:

* Hash indexing on the messages: lets a "find message of type X
  with field f = v" lookup go O(1).  But you have to know which
  field to index by (compile-time analysis or user annotation).
* Early scheduling of guards: prune the search space as soon as
  one guard fails.
* Constraint propagation a la CHR.

These are non-trivial.  A realistic xtc implementation needs at
least the hash-index optimisation to be production-grade, which
adds substantial complexity to the proc/mailbox code.

### Atomicity semantics

The paper specifies that all matched messages are delivered as a
unit -- either all come out of the mailbox or none do.  The
calling code sees N messages atomically.  This is a non-trivial
invariant if the mailbox is mutated between the find-match and
the deliver-all phases; we'd need a lock or a CAS on the matched
set.

In the BEAM today, even single-message selective receive only
removes a message after it has been chosen.  Multi-head needs the
equivalent for N messages.

### The historical record

The Sulzmann/Lam/Van Weert paper is from 2008.  Erlang/OTP itself
has not adopted multi-headed receive patterns in the 17+ years
since.  Akka (Scala) doesn't.  Elixir doesn't.  Pony (the language
that took the actor model furthest) doesn't.

The Polyphonic C# / chord-style join calculus did adopt it, and
JoCaml had it natively, but those are research languages, not
production systems.

This isn't dispositive -- maybe the actor-language community is
just slow, or didn't see the benefit -- but it does suggest the
practical value is bounded.  When a feature gets proposed in 2008
and isn't adopted by major implementations in 2024, the burden
shifts to the proposer.

## Recommendation: don't add multi-headed receive as a primitive

xtc should leave `xtc_recv` and `xtc_recv_match` as they are.

The reasons:

1. **C ergonomics are wrong**.  The win in Erlang/Haskell comes
   from native pattern matching syntax.  In C the user writes
   predicate functions either way; the savings are small.

2. **Performance footguns**.  Naive multi-head match is O(M^N).
   Without hash indexing it's a production hazard.  Implementing
   hash indexing requires user annotations or compile-time
   analysis we don't have.

3. **Real-world demand is bounded**.  17 years of non-adoption
   in major actor implementations is evidence that the pattern,
   while elegant in papers, isn't the bottleneck most actor
   programmers hit.

4. **Composability beats specialisation**.  We can offer the same
   value through targeted helpers built on top of `xtc_recv_match`
   and small state machines, without adding general-purpose join
   semantics to the runtime.

## What we should do instead

Three concrete additions, none requiring new runtime primitives:

### 1. A "patterns" doc + canonical examples

Document the standard recipes that multi-headed receive aims to
simplify, in `docs/M_RECEIVE_PATTERNS.md`:

- Two-phase commit pattern with current xtc_recv_match
- Fork-join with N children
- K-of-N quorum
- Request-correlation by id field
- Buy-sell matching by price

Each pattern: ~40 lines of well-commented C demonstrating the
state-machine version.  Operators copy/paste; we don't ship a new
primitive.

### 2. A `xtc_recv_correlate` helper for the most common case

The single most common multi-message pattern is "wait for the reply
to my request" -- a request-id correlation across N replies.  Ship
this as a thin helper:

```c
/*
 * Wait for `n_expected` messages whose first `corr_size` bytes
 * match `corr_value`.  Useful for fork-join with stable ids.
 */
int xtc_recv_correlate(const void *corr_value, size_t corr_size,
                       int n_expected,
                       xtc_msg_t out_msgs[],
                       int64_t timeout_ns);
```

Implementation: ~80 LOC on top of xtc_recv_match.  Uses a struct
with the correlation prefix as the predicate user-data; matching
the first n_expected messages with the prefix delivers all.

This handles 90% of "I want multi-head" use cases without any
runtime changes.

### 3. (Maybe) `xtc_recv_quorum` for K-of-N

```c
int xtc_recv_quorum(xtc_match_fn pred, void *user,
                    int k, int n,
                    xtc_msg_t out_msgs[],
                    int64_t timeout_ns);
```

Wait until K of the next N matching messages have arrived; the
remaining N-K can be discarded or saved.  Ship if there's
demand; otherwise leave to userspace.

## When we'd revisit

We should reopen this question if any of the following happen:

* xtc-based applications repeatedly hit a use case where the
  state-machine pattern is genuinely painful and the abstraction
  is general (not just one app's cleverness).
* We add a higher-level language-binding layer to xtc (e.g. a
  Lisp/Lua scripting frontend) where multi-head matching becomes
  expressible without C-shaped function-pointer-array contortions.
* Someone offers a clean implementation of multi-headed receive
  with hash-indexing for production-grade performance.

For now: leave `xtc_recv_match` alone, document the patterns,
ship the targeted helpers if needed.

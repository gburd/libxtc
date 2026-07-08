---
title: Let it crash
parent: Philosophy
nav_order: 3
permalink: /philosophy/let-it-crash/
lede: >-
  Why writing code that assumes success -- and lets failures kill and restart a process -- is more robust than defending against every error.
---

# Let it crash

"Let it crash" is the most counter-intuitive idea libxtc borrows from
Erlang, and the most valuable. It says: **do not try to handle every
error where it happens. Write the happy path clearly, let a process die
when something unexpected goes wrong, and recover at a higher level from
a known-good state.**

Joe Armstrong -- Erlang's co-creator -- made the case better than anyone;
his talk *"The Do's and Don'ts of Error Handling"*
([youtube.com/watch?v=TTM_b7EJg5E](https://www.youtube.com/watch?v=TTM_b7EJg5E))
is the canonical explanation, and it is worth an hour. The short version:

- **You cannot foresee every error.** Defensive code handles the errors
  you thought of. The one that takes you down in production is the one
  you did not -- and for that one, local error handling has no plan.
- **A process that has hit an unexpected state is already corrupt.**
  Trying to limp on from a state you do not understand does more damage
  than stopping. The safe move is to discard the corrupt process and
  start a clean one.
- **Recovery belongs in one place.** If every function recovers from
  every error, the recovery logic is smeared across the whole program and
  is itself untested. A supervisor concentrates recovery in one small,
  well-tested place.

## How libxtc protects you and cleans up

Letting a process crash is only safe if the crash is *contained* and the
cleanup is *automatic*. libxtc provides both:

- **Fault containment.** A process that takes a hardware fault
  (`SIGSEGV`, `SIGBUS`) can be *contained* rather than taking the whole
  program down: libxtc's fault guard unwinds that one process to its
  recovery point and reports a `DOWN`, while a fault inside a critical
  section deliberately escalates (some invariants must not be limped
  past). See the contained-fault recipe in
  [Debugging]({{ '/guide/debugging/' | relative_url }}).
- **Automatic resource cleanup.** When a process exits -- cleanly or by
  crashing -- the resources it registered (file descriptors, buffers,
  callbacks) are released by its recovery registry. A crash does not
  leak.
- **Notification, not silence.** The crash is delivered as a `DOWN`
  message to monitors and an exit signal to links, carrying *how* the
  process died (clean, exit code, or signal). Nothing fails quietly.
- **Supervised restart.** A [supervisor]({{ '/guide/04-supervision/' | relative_url }})
  watching the process restarts it by policy from a known-good state,
  with a restart budget so a crash-loop escalates instead of spinning.

Put together: your worker code assumes success and stays readable; when
reality disagrees, the process dies, its resources are reclaimed, its
supervisor notices, and a fresh instance takes over -- all without a line
of error-handling in the worker itself.

## Why this is sustainable

The sustainability argument is the one Armstrong keeps returning to: a
system built this way *keeps running while it is being fixed.* A crash is
not an outage -- it is one process restarting while the rest serve
traffic. You get a `DOWN` with the reason, you fix the root cause at your
leisure, and in the meantime the supervisor has been papering over it.
Compare the defensive style, where an unforeseen error either crashes the
whole program or, worse, corrupts it silently and surfaces as a
data-integrity bug days later.

It is also sustainable for the *code*: happy-path functions are shorter,
clearer, and easier to review than functions half-composed of error
handling for cases that rarely fire and are rarely tested. The error
handling that remains -- in the supervisor -- is small and exercised on
every restart.

"Let it crash" is not "be careless." Input validation at trust
boundaries, the handling that prevents data loss, and security checks all
stay. What you drop is the *defensive middle*: the speculative
error-handling for the unforeseeable, which never works for the case that
matters. Let that crash, contain it, and restart.

---

&larr; [Choices and roads not taken]({{ '/philosophy/choices/' | relative_url }}) &middot;
Next: [Message passing and the compromise]({{ '/philosophy/message-passing/' | relative_url }}) &rarr;

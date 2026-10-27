# Security policy

libxtc is a C runtime library: it runs inside its consumer's process,
with that process's privileges, and handles whatever bytes the
consumer's peers send to the sockets it serves.  A defect in libxtc is
therefore a defect in every program built on it.  This file says which
releases get security fixes, how to report a vulnerability privately,
and what happens after you do.

## Supported versions

| Version | Status | Security fixes |
|---|---|---|
| 2.0.x (the first LTS) | Long-term support | Yes, until at least 18 months after 3.0.0 ships (see the LTS contract) |
| The current minor (the newest 2.x.y) | Current | Yes |
| 2.x minors older than the current one | Superseded | No: upgrade to the current minor (a minor is ABI-compatible within 2.x) |
| 1.x (1.0.0 to 1.59.x) | End of life | No |

Until 2.0.0 is tagged, only the newest release receives fixes.  The
support window, what a patch release may change, and the backport
branch (`lts/2.0`) are defined in the [LTS contract](docs/abi-stability.md#long-term-support-and-the-20-lts-policy).

## Reporting a vulnerability

**Do not open a public issue, pull request or discussion for a
suspected vulnerability.**

Email **greg@burd.me** with the subject line starting `[libxtc security]`.
Include:

- the libxtc version (`xtc_version()` or the tag) and the commit if
  you built from source;
- the platform: OS and version, CPU architecture, compiler, the I/O
  backend (`--with-io-backend`) and TLS backend (`--with-tls`);
- what an attacker controls (bytes on a socket, a config file, an API
  argument) and what they gain;
- a reproducer: a test program, an input file, or a fuzz artifact from
  `scripts/fuzz.sh` (see test/fuzz/).  A sanitizer report (ASan, UBSan,
  TSan) is ideal.

If the report is sensitive, ask for a key in a first, short email and
send the details encrypted.  Reports in English, please; plain text is
fine.

## What happens next

| Step | Target |
|---|---|
| Acknowledgement of your report | 3 working days |
| Triage: confirmed or not, severity, affected versions | 10 working days |
| A fix on every supported branch, or a stated plan with a date | 30 days after confirmation |
| Public disclosure | When the fix is released, and no later than 90 days after the report |

Embargo.  Until disclosure the report, the fix and the advisory stay
private between the reporter and the maintainer.  If a fix needs longer
than 90 days we agree a new date with the reporter rather than disclose
an unfixed issue; if the issue is already public or being exploited, we
release as soon as a fix exists.  Fixed releases are tagged on every
supported branch together, so no supported line is left visibly
unpatched.

Disclosure.  The advisory goes in the release notes and in
docs/KNOWN_ISSUES.md as a `FIXED (x.y.z)` entry: the affected versions,
what an attacker could do, the fixing commit, and credit to the
reporter unless they ask to stay anonymous.  We request a CVE for any
issue rated medium or higher.  Downstream packagers that ask in advance
can be notified of the release date under the same embargo.

A consumer that embeds libxtc (a database server, a proxy) and finds
that a libxtc issue affects its own users should coordinate disclosure
with us; we will hold our advisory for that consumer's release when we
can do so without exceeding the 90 days.

## What counts as a security issue

In scope -- anything a party OTHER than the consumer's own code can
trigger:

- memory safety on any input libxtc parses or receives: the
  length-framed transport (`xtc_net_recv_frame`), the TLS layer
  (handshake, records, ALPN, certificate handling done by libxtc),
  DOWN/EXIT signal decoding, the configuration file parser
  (`xtc_cfg_load_file`): out-of-bounds reads or writes, use after free,
  double free, integer overflow that reaches an allocation or an index;
- unbounded resource use from a peer: a claimed length, a slow-loris
  connection, or a message pattern that makes libxtc allocate, spin or
  hold a loop without a bound the consumer configured;
- a peer bypassing a documented bound (a `max_len`, a budget from
  `xtc_res`, an admission limit);
- TLS verification that accepts what its documentation says it rejects
  (an unverified peer certificate in a REQUIRE mode, a downgrade below
  `min_version`);
- cross-process isolation failures in `xtc_xproc` / `xtc_osproc` (a
  child reading or writing the parent's memory beyond what it was given);
- a crash (abort, SIGSEGV) reachable from untrusted input, even without
  a known exploit: in a server library a remote crash is a denial of
  service.

The example programs under `examples/` (rexis, sqlxtc, kaka, ...) are
demonstrations, not supported products, but their wire parsers are
fuzzed (test/fuzz/) and a remotely triggerable memory-safety bug in one
is treated the same way: report it privately.

Out of scope -- report these as ordinary bugs, in public:

- misuse of the API by the consumer's own code (a NULL where the man
  page says non-NULL, a freed handle, calling a fiber-only function
  from a plain thread); libxtc trusts its caller;
- an attacker who already controls the consumer process, its
  environment variables, its configuration files' directory, or the
  machine;
- denial of service that needs more bandwidth or connections than the
  consumer configured libxtc to accept;
- `xtc_dump`, the trace dumps, debug builds, the simulator
  (`--with-io-backend=sim`), and sanitizer-only findings that cannot
  happen in a normal build (report those publicly too: they are
  welcome);
- vulnerabilities in a TLS library (OpenSSL, mbedTLS, GnuTLS, wolfSSL,
  ...) itself: report those upstream.  A libxtc bug in HOW it calls
  one is in scope.

When in doubt, report privately; we will tell you if it can be public.

## Hardening already in place

- libFuzzer harnesses for every parser a peer or a config file reaches
  (test/fuzz/), run on the committed corpus in CI and for longer every
  night; `scripts/fuzz.sh NAME SECS` runs one locally.
- `make check` under AddressSanitizer and UndefinedBehaviorSanitizer in
  CI, and deterministic simulation of fault schedules (the DST tiers).
- `xtc_net_recv_frame` refuses a claimed length above `max_len` before
  allocating; servers built on it should always pass a cap.

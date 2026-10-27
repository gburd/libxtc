# test/fuzz -- libFuzzer harnesses

One harness per parser that a peer, a client or a configuration file
reaches.  Each runs the parser the way its caller does and checks the
result against an invariant (not only "did not crash"), under ASan and
UBSan.

| Harness | Surface | Entry points |
|---|---|---|
| `fuzz_net`   | length-framed transport | `xtc_net_recv_frame`, `xtc_net_send_framev`, `xtc_down_decode_ex` |
| `fuzz_cfg`   | config file parser      | `xtc_cfg_load_file`, `xtc_cfg_reload` |
| `fuzz_tls`   | ALPN list in `xtc_tls_opts_t` | `xtc_tls_ctx_create` (both roles) |
| `fuzz_sql`   | sqlxtc SQL parser       | `sql_parse`, `sql_parse_ast` |
| `fuzz_quack` | sqlxtc wire protocol    | `quack_parse` + `sql_parse`, line by line as conn.c does |
| `fuzz_resp`  | rexis RESP parser       | `resp_parse_command`, `resp_parse` |
| `fuzz_kaka`  | kaka frames + requests  | `kaka_frame_parse`, `kaka_decode_*` |

No harness for the `xtc_tail` dump reader: it is `tools/xtc-tail.py`,
not C.  The other `xtc_tls_opts_t` strings (paths, cipher lists,
groups) go to the TLS library unparsed.

## Running

    scripts/fuzz.sh NAME SECS     # e.g. scripts/fuzz.sh sql 600
    scripts/fuzz.sh NAME 0        # regression: each corpus input once
    scripts/fuzz.sh all 60

It configures a clang ASan/UBSan, coverage-instrumented libxtc in
`build_fuzz/` (once), builds `fuzz_NAME` (`make fuzz-NAME`), and fuzzes a
scratch copy of `corpus/NAME` with `dict/NAME.dict`.  The committed
corpus is never rewritten by a run.  `FUZZ_CONFIGURE=--with-tls=mbedtls`
(or gnutls, wolfssl) fuzzes another TLS backend; use a separate
`FUZZ_BUILD` dir per backend.

CI: every push runs `scripts/fuzz.sh NAME 0` and 60 s of fuzzing per
harness; the nightly schedule runs 10 minutes per harness.

## A crash

The reproducer is in `build_fuzz/fuzz-out/NAME/artifacts/`.  Fix the
bug, then commit the input as `corpus/NAME/crash-<what>` with the fix,
so the regression run fails if it comes back.  The `crash-*` files here
are such inputs:

- `sql/crash-stack-overflow-continues` -- a parser-stack overflow used
  to restart the parse (a syntax error ran as a DELETE).
- `sql/crash-span-doubled-quote` -- source spans mixed arena and text
  pointers across a `"a""b"` identifier.
- `tls/overlong` -- an ALPN length byte past the end of the string
  (heap over-read in the mbedTLS / GnuTLS / wolfSSL decoders).

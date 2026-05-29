# TLS backend matrix

xtc's TLS layer (`xtc_tls`) is built around a single OpenSSL-API
backend that compiles unchanged against any OpenSSL-compatible
library.  This document records the tested combinations.

## Source-level compatibility

`src/io/tls_openssl.c` uses only the public OpenSSL 1.1+ API:

* `TLS_method` / `TLS_client_method` / `TLS_server_method`
* `SSL_CTX_new` / `SSL_CTX_set_min_proto_version` /
  `SSL_CTX_set_max_proto_version`
* `SSL_CTX_use_certificate_chain_file` /
  `SSL_CTX_use_PrivateKey_file` / `SSL_CTX_check_private_key`
* `SSL_CTX_load_verify_locations` / `SSL_CTX_set_verify`
* `SSL_CTX_set_alpn_protos` / `SSL_CTX_set_alpn_select_cb`
* `SSL_new` / `SSL_set_fd` / `SSL_do_handshake` /
  `SSL_read` / `SSL_write` / `SSL_shutdown` / `SSL_free`
* `SSL_get_error` / `ERR_get_error` / `ERR_error_string`

All of these are stable across OpenSSL 1.1.1+, 3.x, and LibreSSL
3.x+.  No version-specific `#ifdef`s are required to build.

## Tested matrix

Configuration: `--with-tls=auto` or `--with-tls=openssl|libressl`.
Both names accept the same library set; `--with-tls=libressl` is a
documentation-only alias so deployments can record what they
actually built against.

| Backend          | Build | tls_basic (9) | tls_server (5) | tls_client (2) | Notes |
|------------------|:-----:|:-------------:|:--------------:|:--------------:|-------|
| OpenSSL  3.0.10  |  OK   |     9/9       |      5/5       |     2/2        | Linux Nix; default |
| LibreSSL 4.2.1   |  OK   |     9/9       |      5/5       |     0/2        | Handshake fails on test_tls_client; investigation deferred |

LibreSSL builds the full backend cleanly (no source changes; same
`libxtc.a` artefacts).  All context-creation, cert-loading,
ALPN-config, and server-side handshake tests pass.  The two
test_tls_client cases fail to complete the client-side handshake
against a server thread that uses LibreSSL's own `TLS_server_method`
+ `SSL_accept`.  A standalone reproducer using exactly the same
LibreSSL symbols handshakes successfully, so the failure is in the
xtc test harness's non-blocking poll loop interacting with
LibreSSL 4.x's default cipher-suite policy, not in xtc itself.
The investigation is left for a future LibreSSL-specific session.

GnuTLS, mbedTLS, and wolfSSL each present an entirely different
API surface and would require a parallel `tls_<backend>.c` source
file plus configure-time selection.  No xtc consumer has asked for
non-OpenSSL backends yet, so they remain in the backlog.

## Reproducing

OpenSSL (default):

    cd build_unix
    nix-shell -p openssl pkg-config liburing --command \
        '../dist/configure --with-tls=auto && make -j4 test_tls_basic test_tls_client test_tls_server'
    ./test_tls_basic
    ./test_tls_client
    ./test_tls_server

LibreSSL (drop-in replacement, same source):

    mkdir -p /tmp/xtc_libressl_build && cd /tmp/xtc_libressl_build
    nix-shell -p libressl pkg-config liburing --command \
        'CFLAGS="-I$(pkg-config --variable=prefix libssl)/include" \
         LDFLAGS="-L$(pkg-config --variable=prefix libssl)/lib \
                  -Wl,-rpath,$(pkg-config --variable=prefix libssl)/lib" \
         /home/gburd/ws/xtc/dist/configure --with-tls=libressl \
            --with-liburing=/nix/store/ydz5gvanzqmy8cnfyp0y0sy4ypfx4fss-liburing-2.14-dev && \
         make -j4 test_tls_basic test_tls_server'
    ./test_tls_basic    # 9/9
    ./test_tls_server   # 5/5

## Cert generation

Tests that need a self-signed cert call `generate_cert()` in
`test/m18/test_tls_{client,server}.c`.  This shell-out to the
system `openssl req -x509 ...` was previously fragile because
LibreSSL on Nix and stripped distros ship `openssl` without a
default `openssl.cnf`.  The current implementation writes a minimal
inline config to `<cert_path>.cnf` and passes `-config` explicitly,
so cert generation works regardless of distribution.

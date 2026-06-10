# TLS backend matrix

xtc's TLS layer (`xtc_tls`) is one internal seam fronted by several
configure-time-selected backends.  The OpenSSL-API backend
(`src/io/tls_openssl.c`) compiles unchanged against any
OpenSSL-compatible library -- OpenSSL, LibreSSL, and BoringSSL -- and
the other libraries have their own backend source.  This document
records the tested combinations.

## Backends and their source

| --with-tls | source file | flavor notes |
|------------|-------------|--------------|
| openssl    | tls_openssl.c | default |
| libressl   | tls_openssl.c | OpenSSL-API alias; documents what you built against |
| boringssl  | tls_openssl.c | OpenSSL-API; tls_openssl.c gates the API gap (no SSL_read_ex/SSL_write_ex) on OPENSSL_IS_BORINGSSL |
| mbedtls    | tls_mbedtls.c | native mbedTLS API |
| gnutls     | tls_gnutls.c  | native GnuTLS API |
| wolfssl    | tls_wolfssl.c | native wolfSSL API |
| schannel   | tls_schannel.c | Windows SSPI; compile-only |
| none       | tls_none.c    | XTC_E_NOSYS stubs |

## OpenSSL-API source compatibility

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

These are stable across OpenSSL 1.1.1+, 3.x, and LibreSSL 3.x+ with no
version-specific `#ifdef`s.  BoringSSL is the one exception: it omits
the OpenSSL 1.1.1 `SSL_read_ex` / `SSL_write_ex` byte-count forms, so
tls_openssl.c wraps the classic `SSL_read` / `SSL_write` behind the
same 1-on-success/size_t-out contract, gated on the
`OPENSSL_IS_BORINGSSL` macro BoringSSL defines.

## Tested matrix

Each backend builds the library and runs the m18 TLS suite
(`test_tls_server`, `test_tls_client`; `test_tls_basic` where present).
All certs are generated at runtime with an explicit `-config`, so the
suite is backend-agnostic.

| Backend             | Build | tls_server (5) | tls_client (2) | Notes |
|---------------------|:-----:|:--------------:|:--------------:|-------|
| OpenSSL 3.x         |  OK   |      5/5       |     2/2        | Linux Nix; default |
| LibreSSL 4.x        |  OK   |      5/5       |     2/2        | OpenSSL-API; same source |
| BoringSSL (2026-05) |  OK   |      5/5       |     2/2        | OpenSSL-API; SSL_read_ex shim; static libssl.a + -lstdc++ |
| mbedTLS 3.x         |  OK   |      5/5       |     2/2        | native backend; client needs mbedtls_ssl_set_hostname(NULL) |
| GnuTLS 3.x          |  OK   |      5/5       |     2/2        | native backend |
| wolfSSL 5.x         |  OK   |      5/5       |     2/2        | native backend |
| SChannel (Windows)  | compile-only | -- | -- | written to the seam; not runtime-verified (no Windows host) |

All listed backends except SChannel are runtime-verified on Linux/Nix.
A dedicated CI job (`tls-backends`) re-builds and re-runs the suite
against openssl, boringssl, mbedtls, gnutls, and wolfssl on every push.

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

BoringSSL (OpenSSL-API; ships static libssl.a/libcrypto.a and is part
C++, so name the lib dir and add -lstdc++):

    mkdir -p /tmp/xtc_boringssl_build && cd /tmp/xtc_boringssl_build
    nix-shell -p boringssl pkg-config liburing --command '
        DEV=$(echo /nix/store/*boringssl*-dev); LIB=$(dirname $(echo /nix/store/*boringssl*/lib/libssl.a));
        CPPFLAGS="-I$DEV/include" LDFLAGS="-L$LIB" LIBS="-lstdc++" \
          /home/gburd/ws/xtc/dist/configure --with-tls=boringssl && \
        make -j4 test_tls_server test_tls_client'
    ./test_tls_server   # 5/5
    ./test_tls_client   # 2/2

## Cert generation

Tests that need a self-signed cert call `generate_cert()` in
`test/m18/test_tls_{client,server}.c`.  This shell-out to the
system `openssl req -x509 ...` was previously fragile because
LibreSSL on Nix and stripped distros ship `openssl` without a
default `openssl.cnf`.  The current implementation writes a minimal
inline config to `<cert_path>.cnf` and passes `-config` explicitly,
so cert generation works regardless of distribution.

---
title: TLS support
parent: Reference
nav_order: 10
lede: >-
  The Transport Layer Security design and backend strategy.
permalink: /reference/tls/
---
**Status:** implemented.  TLS 1.2/1.3 over `xtc_io` sockets with the
same async, single-threaded event-loop discipline as the rest of xtc.
A single internal `xtc_tls` seam fronts several configure-time-selected
backends.  Verified backends (test/m18 tls_server + tls_client pass):
OpenSSL, LibreSSL, mbedTLS, GnuTLS, and wolfSSL.  An SChannel (Windows
SSPI) backend is written to the same seam but is compile-only -- not
yet runtime-verified, awaiting a Windows host.  Select with
`configure --with-tls=openssl|libressl|mbedtls|gnutls|wolfssl|schannel|auto|none`.
See docs/tls-matrix.md for the per-backend / per-version matrix.

## Goal

Provide TLS 1.2/1.3 over `xtc_io` sockets with the same async,
single-threaded event-loop discipline as the rest of xtc.  The API
works identically across the backends below:

- **OpenSSL** (default on Linux/BSD) -- implemented, verified.
- **LibreSSL** (OpenSSL-API compatible, same backend) -- verified.
- **mbedTLS** -- implemented, verified.
- **GnuTLS** -- implemented, verified.
- **wolfSSL** -- implemented, verified.
- **SChannel** (Windows native) -- implemented, compile-only.

BoringSSL builds through the OpenSSL-API backend; a macOS Network
framework backend is not implemented.

## Design overview

`xtc_tls_t` is a per-connection state machine.  The user owns the
underlying `int fd`; xtc_tls layers on top with read/write that
never block the event loop.  They return `XTC_OK`, `XTC_E_AGAIN` when
the fd is not ready, `XTC_E_IO` on a transport failure or truncation,
`XTC_E_INVAL` for a NULL argument, and `XTC_E_NOSYS` when TLS is not
compiled in; a read can also return another hard TLS error.

A condensed view of `xtc_tls.h` (the header has the full option set:
`verify_peer_mode`, cipher and group lists, CRLs, a passphrase callback,
SNI, a caller-supplied transport, kTLS, and peer-certificate queries):

```c
typedef struct xtc_tls_ctx  xtc_tls_ctx_t;   /* per-process: cert + key */
typedef struct xtc_tls      xtc_tls_t;       /* per-connection state */

typedef enum xtc_tls_role {
    XTC_TLS_SERVER = 0,
    XTC_TLS_CLIENT = 1
} xtc_tls_role_t;

typedef struct xtc_tls_opts {
    size_t      size;            /* XTC_TLS_OPTS_INIT */
    const char *cert_file;       /* PEM; a client's makes it mutual TLS */
    const char *key_file;        /* PEM */
    const char *ca_file;         /* PEM CA bundle for peer verification */
    const char *alpn_protos;     /* "\x02h2\x08http/1.1" wire form */
    int         verify_peer;     /* legacy on/off; see verify_peer_mode */
    xtc_tls_verify_mode_t verify_peer_mode;
    int         min_version;     /* XTC_TLS_VER_12, _13 */
    int         max_version;
    /* ... cipher_list, ciphersuites_13, groups, crl_file, crl_dir,
     * prefer_server_ciphers, passphrase_cb, passphrase_userdata */
} xtc_tls_opts_t;

int  xtc_tls_ctx_create(xtc_tls_role_t role,
                        const xtc_tls_opts_t *opts,
                        xtc_tls_ctx_t **out);
void xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx);

/* Wrap an existing fd.  The fd remains owned by the caller. */
int  xtc_tls_create(xtc_tls_ctx_t *ctx, int fd, xtc_tls_t **out);
void xtc_tls_destroy(xtc_tls_t *tls);

/* Drive the handshake.  Returns XTC_OK when complete, XTC_E_AGAIN
 * if the underlying fd needs more poll() iterations.  Caller waits
 * on `xtc_tls_wants_read(tls) ? XTC_IO_READABLE : XTC_IO_WRITABLE`. */
int  xtc_tls_handshake(xtc_tls_t *tls);

/* Encrypted read/write.  XTC_E_AGAIN means the fd isn't ready yet. */
int  xtc_tls_read (xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n);
int  xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen, size_t *out_n);

/* The caller polls based on these. */
int  xtc_tls_wants_read (const xtc_tls_t *tls);
int  xtc_tls_wants_write(const xtc_tls_t *tls);

/* Graceful shutdown -- sends close_notify; may need multiple calls. */
int  xtc_tls_shutdown(xtc_tls_t *tls);
```

## Backend pluggability

`configure --with-tls=BACKEND` picks the backend at compile time.
Default: `auto`, which uses OpenSSL (or LibreSSL, which installs the
same library names) when its library and header are found, and
otherwise builds with no TLS.  No other backend is picked
automatically, so a build stays reproducible.  There is no runtime
selection, the same discipline as the `xtc_io` backends.

```sh
./configure --with-tls=openssl    # what auto picks when it is found
./configure --with-tls=libressl   # same backend as openssl
./configure --with-tls=boringssl  # OpenSSL-API backend; set CPPFLAGS
./configure --with-tls=mbedtls
./configure --with-tls=gnutls
./configure --with-tls=wolfssl
./configure --with-tls=schannel   # Windows only
./configure --with-tls=none       # link without TLS
```

Each backend lives in `src/io/tls_<backend>.c` and defines the public
`xtc_tls_*` functions directly; the build compiles exactly one of them
(`tls_none.c` for `--with-tls=none`, whose calls check their
arguments, returning `XTC_E_INVAL` for bad ones, and otherwise return
`XTC_E_NOSYS`).  There is no vtable and no runtime dispatch: the choice
is made at configure time, exactly like `XTC_IO_BACKEND_*`.  The pieces
the backends share are static inlines in `src/io/tls_common.h`.

## Non-blocking handshake protocol

OpenSSL's standard idiom is:

```c
int rc = SSL_do_handshake(ssl);
if (rc <= 0) {
    int err = SSL_get_error(ssl, rc);
    if (err == SSL_ERROR_WANT_READ)  poll for readability;
    if (err == SSL_ERROR_WANT_WRITE) poll for writability;
}
```

`xtc_tls_handshake` wraps this and exposes `wants_read/wants_write`.
Caller pattern:

```c
xtc_tls_t *tls;
xtc_tls_create(ctx, fd, &tls);
for (;;) {
    int rc = xtc_tls_handshake(tls);
    if (rc == XTC_OK) break;
    if (rc != XTC_E_AGAIN) abort();   /* hard error */
    uint32_t want = xtc_tls_wants_read(tls)
        ? XTC_IO_READABLE : XTC_IO_WRITABLE;
    xtc_io_mod_fd(io, fd, want, tls);
    /* await event ... */
}
```

## Memory + allocation strategy

The TLS library allocates internally with its own allocator (for
OpenSSL, `OPENSSL_malloc`); libxtc does not override it.  libxtc's own
per-context and per-connection structs come from the library allocator
(`__os_calloc`), so they go through any allocator hook the embedder
installed.  TLS objects are not charged to `xtc_res`.

## Scope

- **Mutual TLS** works through the options: a client context given
  `cert_file` and `key_file` presents that certificate, and a server's
  `verify_peer_mode` asks for or requires one.  There is no separate
  per-connection call for it.
- **SNI and ALPN** are supported: `xtc_tls_set_hostname` on a client,
  `xtc_tls_ctx_set_sni_cb` on a server, `opts.alpn_protos` on both, and
  `xtc_tls_get_alpn_selected` after the handshake.
- **Session resumption and TLS 1.3 0-RTT** are not supported: the API
  has no call for either, and the OpenSSL backend turns session
  tickets and the session cache off.
- **BoringSSL** is not vendored; `--with-tls=boringssl` builds the
  OpenSSL-API backend against a BoringSSL the caller points
  `CPPFLAGS` at.

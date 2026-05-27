# M_TLS -- Transport Layer Security design

**Status:** research; not yet implemented.  This document describes
the proposed `xtc_tls_t` API, the configure-time backend pluggability,
and the integration with `xtc_io` for non-blocking handshakes.

## Goal

Provide TLS 1.2/1.3 over `xtc_io` sockets with the same async,
single-threaded event-loop discipline as the rest of xtc.  The API
must work identically across at least four backends:

- **OpenSSL** (default on Linux/BSD; Tier-1)
- **BoringSSL** (Google's fork; tier-1 alt)
- **wolfSSL** (embedded; tier-2)
- **macOS Network framework** (tier-2 native)
- **SChannel** (Windows native; tier-2)

## Design overview

`xtc_tls_t` is a per-connection state machine.  The user owns the
underlying `int fd`; xtc_tls layers on top with read/write that
return `XTC_OK` / `XTC_E_AGAIN` / `XTC_E_INVAL` and never block the
event loop.

```c
typedef struct xtc_tls_ctx  xtc_tls_ctx_t;   /* per-process: cert + key */
typedef struct xtc_tls      xtc_tls_t;       /* per-connection state */

typedef enum xtc_tls_role {
    XTC_TLS_SERVER = 0,
    XTC_TLS_CLIENT = 1
} xtc_tls_role_t;

typedef struct xtc_tls_opts {
    const char *cert_file;       /* PEM, server only */
    const char *key_file;        /* PEM, server only */
    const char *ca_file;         /* PEM CA bundle for client verification */
    int         verify_peer;     /* boolean; 1 = require valid cert */
    const char *alpn_protos;     /* "\x02h2\x08http/1.1" wire form */
    int         min_version;     /* XTC_TLS_VER_12, _13 */
    int         max_version;
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
Default: `auto`, which tries OpenSSL first, BoringSSL, then disables
TLS if neither is available.  No runtime selection -- same discipline
as `xtc_io` backends.

```sh
./configure --with-tls=openssl    # default
./configure --with-tls=boringssl  # if you've vendored BoringSSL
./configure --with-tls=wolfssl
./configure --with-tls=schannel   # auto-picked on Windows
./configure --with-tls=darwin     # auto-picked on macOS
./configure --with-tls=none       # link without TLS
```

Each backend lives in `src/io/tls_<backend>.c` and exports the
same internal vtable:

```c
typedef struct xtc_tls_vtable {
    int  (*ctx_create) (xtc_tls_role_t, const xtc_tls_opts_t *, xtc_tls_ctx_t **);
    void (*ctx_destroy)(xtc_tls_ctx_t *);
    int  (*tls_create) (xtc_tls_ctx_t *, int, xtc_tls_t **);
    void (*tls_destroy)(xtc_tls_t *);
    int  (*handshake)  (xtc_tls_t *);
    int  (*read)       (xtc_tls_t *, void *, size_t, size_t *);
    int  (*write)      (xtc_tls_t *, const void *, size_t, size_t *);
    int  (*wants_read) (const xtc_tls_t *);
    int  (*wants_write)(const xtc_tls_t *);
    int  (*shutdown)   (xtc_tls_t *);
} xtc_tls_vtable_t;
```

This vtable is **not** runtime-dispatched on the hot path -- it's
selected at configure time, exactly like `XTC_IO_BACKEND_*`.  The
preprocessor inlines the right impl.

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

OpenSSL allocates internally via `OPENSSL_malloc`.  We:

1. Don't override OpenSSL's allocator (would couple us tightly).
2. Use `xtc_slab` for per-connection wrappers (`struct xtc_tls`).
3. Track outstanding TLS contexts via `xtc_res` for memory cap +
   alert under pressure.

## Open questions

1. **Vendoring** -- do we vendor BoringSSL or require system?
   Recommendation: system-only for v1; vendoring adds 100 MB to
   the source tree.

2. **mTLS** -- verify_peer + client cert workflow is in scope; we
   need a `xtc_tls_set_client_cert(tls, cert, key)` for client
   role.  (Optional in v1.)

3. **SNI / ALPN** -- both must work in v1; modern HTTP/2 / gRPC
   demands them.

4. **TLS 1.3 0-RTT** -- out of scope for v1; revisit if needed.

5. **Session resumption tickets** -- out of scope for v1.

## Effort estimate

| Component | Lines | Days |
|---|---|---|
| xtc_tls.h API | ~150 | 1 |
| OpenSSL backend | ~700 | 5 |
| BoringSSL backend | ~700 | 3 (mostly identical to OpenSSL) |
| wolfSSL backend | ~600 | 4 |
| SChannel backend | ~900 | 8 |
| macOS Network framework | ~700 | 6 |
| Tests | ~600 | 4 |
| Bench | ~200 | 2 |

Total: ~33 person-days for a 1-backend ship (OpenSSL only).
Adding 4 more backends doubles that.

## Next concrete step

When greenlit:

1. Ship `xtc_tls.h` + OpenSSL backend only.
2. Add 1 example: `examples/05_tls_echo.c` -- a TLS echo server +
   client.  ~150 lines each.
3. Tag a v0.3.0 once landed.

The vtable shape lets us grow into other backends incrementally
without reworking the public API.

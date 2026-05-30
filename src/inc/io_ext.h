/* DO NOT EDIT: automatically built by dist/s_include. */
/* See M0_CLAIMS.md [T2]. */

#ifndef IO_EXT_H
#define IO_EXT_H

const char *xtc_io_backend_name __P((void));
int  xtc_tls_create  __P((xtc_tls_ctx_t *, int, xtc_tls_t **));
int  xtc_tls_ctx_create __P((xtc_tls_role_t, const xtc_tls_opts_t *, xtc_tls_ctx_t **));
int  xtc_tls_handshake __P((xtc_tls_t *));
int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
int  xtc_tls_shutdown __P((xtc_tls_t *));
int  xtc_tls_wants_read  __P((const xtc_tls_t *));
int  xtc_tls_wants_write __P((const xtc_tls_t *));
int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
int xtc_io_del_fd __P((xtc_io_t *, int));
int xtc_io_fini __P((xtc_io_t *));
int xtc_io_init __P((xtc_io_t **));
int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *));
int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *));
int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *));
int xtc_io_wakeup __P((xtc_io_t *));
int xtc_net_recv_frame __P((int, void **, size_t *, size_t, int64_t));
int xtc_net_send_frame __P((int, const void *, size_t));
void xtc_tls_ctx_destroy __P((xtc_tls_ctx_t *));
void xtc_tls_destroy __P((xtc_tls_t *));

#endif /* IO_EXT_H */

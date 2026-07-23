/* DO NOT EDIT: automatically built by dist/s_include. */
/* See M0_CLAIMS.md [T2]. */

#ifndef IO_EXT_H
#define IO_EXT_H

const char *xtc_io_backend_name __P((void));
const char *xtc_tls_get_cipher __P((const xtc_tls_t *));
const char *xtc_tls_get_version __P((const xtc_tls_t *));
int  xtc_tls_create  __P((xtc_tls_ctx_t *, int, xtc_tls_t **));
int  xtc_tls_ctx_create __P((xtc_tls_role_t, const xtc_tls_opts_t *, xtc_tls_ctx_t **));
int  xtc_tls_get_alpn_selected __P((const xtc_tls_t *, const unsigned char **, unsigned int *));
int  xtc_tls_get_cipher_bits __P((const xtc_tls_t *));
int  xtc_tls_get_peer_common_name __P((const xtc_tls_t *, char *, size_t));
int  xtc_tls_get_peer_issuer_dn __P((const xtc_tls_t *, char *, size_t));
int  xtc_tls_get_peer_serial __P((const xtc_tls_t *, char *, size_t));
int  xtc_tls_get_peer_subject_dn __P((const xtc_tls_t *, char *, size_t));
int  xtc_tls_get_server_cert_hash __P((const xtc_tls_t *, unsigned char *, size_t, size_t *));
int  xtc_tls_handshake __P((xtc_tls_t *));
int  xtc_tls_has_peer_cert __P((const xtc_tls_t *));
int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
int  xtc_tls_shutdown __P((xtc_tls_t *));
int  xtc_tls_wants_read  __P((const xtc_tls_t *));
int  xtc_tls_wants_write __P((const xtc_tls_t *));
int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
int __xtc_io_backend_init __P((xtc_io_t *));
int __xtc_io_register_wakeup __P((xtc_io_t *, int));
int __xtc_io_sim_defer_cb __P((xtc_io_t *, int64_t, void (*)(void *), void *));
int xtc_bdev_flush __P((xtc_bdev_t *));
int xtc_bdev_open __P((const char *, int, xtc_bdev_t **));
int xtc_io_aio_submit __P((xtc_io_t *, xtc_aio_t *));
int xtc_io_del_fd __P((xtc_io_t *, int));
int xtc_io_fini __P((xtc_io_t *));
int xtc_io_init __P((xtc_io_t **));
int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *));
int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *));
int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *));
int xtc_io_wakeup __P((xtc_io_t *));
int xtc_net_recv_frame __P((int, void **, size_t *, size_t, int64_t));
int xtc_net_send_frame __P((int, const void *, size_t));
int64_t __xtc_io_sim_next_due __P((xtc_io_t *));
ssize_t xtc_bdev_pread __P((xtc_bdev_t *, void *, size_t, uint64_t));
ssize_t xtc_bdev_pwrite __P((xtc_bdev_t *, const void *, size_t, uint64_t));
uint32_t xtc_bdev_logical_sector __P((const xtc_bdev_t *));
uint32_t xtc_bdev_physical_sector __P((const xtc_bdev_t *));
uint64_t xtc_bdev_capacity __P((const xtc_bdev_t *));
void __xtc_io_backend_fini __P((xtc_io_t *));
void xtc_bdev_close __P((xtc_bdev_t *));
void xtc_io_set_iowq_max_workers __P((unsigned, unsigned));
void xtc_tls_ctx_destroy __P((xtc_tls_ctx_t *));
void xtc_tls_destroy __P((xtc_tls_t *));

#endif /* IO_EXT_H */

/* DO NOT EDIT: automatically built by dist/s_include. */
/* See M0_CLAIMS.md [T2]. */

#ifndef IO_EXT_H
#define IO_EXT_H

const char *xtc_io_backend_name __P((void));
int xtc_io_del_fd __P((xtc_io_t *, int));
int xtc_io_fini __P((xtc_io_t *));
int xtc_io_init __P((xtc_io_t **));
int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *));
int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *));
int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *));
int xtc_io_wakeup __P((xtc_io_t *));

#endif /* IO_EXT_H */

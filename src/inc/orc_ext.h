/* DO NOT EDIT: automatically built by dist/s_include. */
/* See M0_CLAIMS.md [T2]. */

#ifndef ORC_EXT_H
#define ORC_EXT_H

int xtc_sup_add_child __P((xtc_supervisor_t *, const xtc_child_spec_t *, xtc_pid_t *));
int xtc_sup_join __P((xtc_supervisor_t *, int64_t));
int xtc_sup_stop __P((xtc_supervisor_t *));
int xtc_svr_call __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t));
int xtc_svr_call_abortable __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t, xtc_abort_token_t *));
xtc_svr_call_t *xtc_svr_call_save __P((const xtc_svr_call_t *));

#endif /* ORC_EXT_H */

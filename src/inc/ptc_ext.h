/* DO NOT EDIT: automatically built by dist/s_include. */
/* See M0_CLAIMS.md [T2]. */

#ifndef PTC_EXT_H
#define PTC_EXT_H

int xtc_down_decode __P((const void *, size_t, xtc_pid_t *, int *));
int xtc_exit_pid __P((xtc_pid_t, int));
int xtc_exit_self __P((int));
int xtc_fault_guard_install __P((void));
int xtc_inject_check __P((const char *));
int xtc_link __P((xtc_pid_t));
int xtc_monitor __P((xtc_pid_t, uint64_t *));
int xtc_proc_at_exit __P((void (*)(void *), void *));
int xtc_proc_mailbox_stats __P((xtc_pid_t, xtc_mailbox_stats_t *));
int xtc_proc_spawn __P((xtc_loop_t *, xtc_proc_fn, void *, const xtc_proc_opts_t *, xtc_pid_t *));
int xtc_proc_wait_fd __P((int, uint32_t, int64_t, uint32_t *));
int xtc_recv __P((void **, size_t *, int64_t));
int xtc_recv_correlate __P((const void *, size_t, int, xtc_msg_t *, int *, int64_t));
int xtc_recv_match __P((xtc_match_fn, void *, void **, size_t *, int64_t));
int xtc_res_acquire __P((xtc_res_t *, xtc_res_kind_t, int64_t));
int xtc_res_init __P((xtc_res_t *, const xtc_res_caps_t *));
int xtc_res_set_alert __P((xtc_res_t *, xtc_res_kind_t, double));
int xtc_res_set_alert_fn __P((xtc_res_t *, void (*)(xtc_res_kind_t, int64_t, int64_t, void *), void *));
int xtc_send __P((xtc_pid_t, const void *, size_t));
int xtc_unlink __P((xtc_pid_t));
int64_t xtc_res_high __P((const xtc_res_t *, xtc_res_kind_t));
int64_t xtc_res_rejects __P((const xtc_res_t *, xtc_res_kind_t));
int64_t xtc_res_used __P((const xtc_res_t *, xtc_res_kind_t));
struct xtc_mctx *xtc_proc_mctx __P((void));
void  __xtc_proc_ctx_restore __P((void *));
void *__xtc_proc_ctx_save __P((void));
void xtc_proc_critical_enter __P((void));
void xtc_proc_critical_leave __P((void));
void xtc_proc_recovery_disarm __P((void));
void xtc_res_release __P((xtc_res_t *, xtc_res_kind_t, int64_t));
void xtc_res_set_cap __P((xtc_res_t *, xtc_res_kind_t, int64_t));
xtc_pid_t xtc_self __P((void));

#endif /* PTC_EXT_H */

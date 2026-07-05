/* DO NOT EDIT: automatically built by dist/s_include. */
/* See M0_CLAIMS.md [T2]. */

#ifndef ORC_EXT_H
#define ORC_EXT_H

int xtc_launch __P((xtc_loop_t *, xtc_launch_fn, void *, int64_t, const xtc_launch_opts_t *, intptr_t *));
int xtc_sup_add_child __P((xtc_supervisor_t *, const xtc_child_spec_t *, xtc_pid_t *));
int xtc_sup_join __P((xtc_supervisor_t *, int64_t));
int xtc_sup_stop __P((xtc_supervisor_t *));
int xtc_svr_call __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t));
int xtc_svr_call_abortable __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t, xtc_abort_token_t *));
int xtc_tnt_start __P((const xtc_tnt_spec_t *));
uint8_t xtc_tnt_shard_id __P((void));
void *xtc_tnt_scratch_arena __P((size_t));
void xtc_tnt_register_timer __P((uint64_t, uint16_t));
void xtc_tnt_stop __P((void));
xtc_svr_call_t *xtc_svr_call_save __P((const xtc_svr_call_t *));
xtc_tnt_handle_t xtc_tnt_self __P((void));
xtc_tnt_io_result_t xtc_tnt_io_send __P((int, const void *, size_t));
xtc_tnt_io_result_t xtc_tnt_submit_close __P((int));
xtc_tnt_io_result_t xtc_tnt_submit_recv __P((int));
xtc_tnt_send_result_t xtc_tnt_send __P((xtc_tnt_handle_t, uint16_t, const void *, size_t));
xtc_tnt_spawn_error_t xtc_tnt_spawn __P((uint8_t, const void *, size_t, xtc_tnt_handle_t *));
xtc_tnt_spawn_error_t xtc_tnt_spawn_on __P((uint8_t, uint8_t, const void *, size_t));

#endif /* ORC_EXT_H */

/* DO NOT EDIT: automatically built by dist/s_include. */
/* See M0_CLAIMS.md [T2]. */

#ifndef EVT_EXT_H
#define EVT_EXT_H

int xtc_async __P((xtc_loop_t *, xtc_coro_fn, void *, xtc_task_t **));
int xtc_await __P((xtc_task_t *, intptr_t *));
int xtc_exec_async __P((xtc_exec_t *, xtc_coro_fn, void *, xtc_task_t **));
int xtc_exec_async_on __P((xtc_exec_t *, int, xtc_coro_fn, void *, xtc_task_t **));
int xtc_exec_fini __P((xtc_exec_t *));
int xtc_exec_init __P((xtc_exec_t **, int));
int xtc_exec_loop_id __P((void));
int xtc_exec_n_loops __P((xtc_exec_t *));
int xtc_exec_run __P((xtc_exec_t *));
int xtc_exec_spawn __P((xtc_exec_t *, xtc_task_fn, void *, xtc_task_t **));
int xtc_exec_spawn_on __P((xtc_exec_t *, int, xtc_task_fn, void *, xtc_task_t **));
int xtc_exec_stop __P((xtc_exec_t *));
int xtc_loop_fini __P((xtc_loop_t *));
int xtc_loop_init __P((xtc_loop_t **));
int xtc_loop_run __P((xtc_loop_t *));
int xtc_loop_stop __P((xtc_loop_t *));
int xtc_task_park_on_fd __P((xtc_task_t *, int, uint32_t));
int xtc_task_park_on_timer __P((xtc_task_t *, int64_t));
int xtc_task_spawn __P((xtc_loop_t *, xtc_task_fn, void *, xtc_task_t **));
int xtc_task_waker __P((xtc_task_t *, xtc_waker_t *));
int xtc_timer_cancel __P((xtc_timer_t *));
int xtc_timer_set __P((xtc_loop_t *, int64_t, xtc_timer_fn, void *, xtc_timer_t **));
int xtc_waker_wake __P((const xtc_waker_t *));
struct xtc_res *xtc_loop_res __P((xtc_loop_t *));
void xtc_yield __P((void));
xtc_loop_t *xtc_exec_loop __P((xtc_exec_t *, int));

#endif /* EVT_EXT_H */

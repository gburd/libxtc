/* DO NOT EDIT: automatically built by dist/s_include. */
/* See M0_CLAIMS.md [T2]. */

#ifndef OS_EXT_H
#define OS_EXT_H

int __os_aligned_alloc __P((size_t, size_t, void **));
int __os_alloc_get_hook __P((struct __os_alloc_hook *));
int __os_alloc_set_hook __P((const struct __os_alloc_hook *));
int __os_calloc __P((size_t, size_t, void **));
int __os_clock_mono __P((int64_t *));
int __os_clock_real __P((int64_t *));
int __os_cond_broadcast __P((__os_cond_t *));
int __os_cond_destroy __P((__os_cond_t *));
int __os_cond_init __P((__os_cond_t *));
int __os_cond_signal __P((__os_cond_t *));
int __os_cond_wait __P((__os_cond_t *, __os_mutex_t *));
int __os_malloc __P((size_t, void **));
int __os_mutex_destroy __P((__os_mutex_t *));
int __os_mutex_init __P((__os_mutex_t *));
int __os_mutex_lock __P((__os_mutex_t *));
int __os_mutex_trylock __P((__os_mutex_t *));
int __os_mutex_unlock __P((__os_mutex_t *));
int __os_ncpus __P((void));
int __os_numa_current_node __P((void));
int __os_numa_nnodes __P((void));
int __os_numa_node_of_cpu __P((int));
int __os_realloc __P((void *, size_t, void **));
int __os_rwlock_destroy __P((__os_rwlock_t *));
int __os_rwlock_init __P((__os_rwlock_t *));
int __os_rwlock_rdlock __P((__os_rwlock_t *));
int __os_rwlock_unlock __P((__os_rwlock_t *));
int __os_rwlock_wrlock __P((__os_rwlock_t *));
int __os_sem_destroy __P((__os_sem_t *));
int __os_sem_init __P((__os_sem_t *, unsigned));
int __os_sem_post __P((__os_sem_t *));
int __os_sem_trywait __P((__os_sem_t *));
int __os_sem_wait __P((__os_sem_t *));
int __os_sleep_ns __P((int64_t));
int __os_strdup __P((const char *, char **));
int __os_thread_create __P((__os_thread_t *, __os_thread_fn, void *));
int __os_thread_detach __P((__os_thread_t *));
int __os_thread_join __P((__os_thread_t *, void **));
int __os_thread_self __P((__os_thread_t *));
int __os_thread_setname __P((const char *));
int __os_tls_create __P((__os_tls_key_t *, __os_tls_dtor));
int __os_tls_destroy __P((__os_tls_key_t));
int __os_tls_set __P((__os_tls_key_t, void *));
void *__os_tls_get __P((__os_tls_key_t));
void __os_free __P((void *));
void __os_thread_yield __P((void));

#endif /* OS_EXT_H */

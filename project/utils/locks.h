#ifndef __LOCKS_H__
#define __LOCKS_H__

#include <pthread.h>

void mutex_init(pthread_mutex_t *lock);
void mutex_lock(pthread_mutex_t *lock);
void mutex_unlock(pthread_mutex_t *lock);
void mutex_destroy(pthread_mutex_t *lock);

void rwl_init(pthread_rwlock_t *lock);
void rwl_rdlock(pthread_rwlock_t *lock);
void rwl_wrlock(pthread_rwlock_t *lock);
void rwl_unlock(pthread_rwlock_t *lock);
void rwl_destroy(pthread_rwlock_t *lock);

void cond_init(pthread_cond_t *cond);
void cond_signal(pthread_cond_t *cond);
void cond_broadcast(pthread_cond_t *cond);
void cond_wait(pthread_cond_t *cond, pthread_mutex_t *lock);
void cond_destroy(pthread_cond_t *cond);

#endif

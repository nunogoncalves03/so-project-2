#include "locks.h"
#include "betterassert.h"

/**
 * Mutex functions
 */

/*
 * Tries to init the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void mutex_init(pthread_mutex_t *lock) {
    ALWAYS_ASSERT(pthread_mutex_init(lock, NULL) == 0, "Failed to init mutex");
}

/*
 * Tries to lock the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void mutex_lock(pthread_mutex_t *lock) {
    ALWAYS_ASSERT(pthread_mutex_lock(lock) == 0, "Failed to lock mutex");
}

/*
 * Tries to unlock the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void mutex_unlock(pthread_mutex_t *lock) {
    ALWAYS_ASSERT(pthread_mutex_unlock(lock) == 0, "Failed to unlock mutex");
}

/*
 * Tries to destroy the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void mutex_destroy(pthread_mutex_t *lock) {
    ALWAYS_ASSERT(pthread_mutex_destroy(lock) == 0, "Failed to destroy mutex");
}

/**
 * RWLOCK functions
 */

/*
 * Tries to init the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void rwl_init(pthread_rwlock_t *lock) {
    ALWAYS_ASSERT(pthread_rwlock_init(lock, NULL) == 0,
                  "Failed to init rwlock");
}

/*
 * Tries to read lock the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void rwl_rdlock(pthread_rwlock_t *lock) {
    ALWAYS_ASSERT(pthread_rwlock_rdlock(lock) == 0, "Failed to rdlock rwlock");
}

/*
 * Tries to write lock the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void rwl_wrlock(pthread_rwlock_t *lock) {
    ALWAYS_ASSERT(pthread_rwlock_wrlock(lock) == 0, "Failed to wrlock rwlock");
}

/*
 * Tries to unlock the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void rwl_unlock(pthread_rwlock_t *lock) {
    ALWAYS_ASSERT(pthread_rwlock_unlock(lock) == 0, "Failed to unlock rwlock");
}

/*
 * Tries to destroy the lock, exits the program if some error occurs
 *
 * Input:
 *   - lock: a pointer to the lock
 */
void rwl_destroy(pthread_rwlock_t *lock) {
    ALWAYS_ASSERT(pthread_rwlock_destroy(lock) == 0,
                  "Failed to destroy rwlock");
}

/*
 * Tries to init the cond var, exits the program if some error occurs
 *
 * Input:
 *   - cond: a pointer to the cond var
 */
void cond_init(pthread_cond_t *cond) {
    ALWAYS_ASSERT(pthread_cond_init(cond, NULL) == 0, "Failed to init cond");
}

/*
 * Tries to unblock a thread blocked on the cond var, exits the program if some
 * error occurs
 *
 * Input:
 *   - cond: a pointer to the cond var
 */
void cond_signal(pthread_cond_t *cond) {
    ALWAYS_ASSERT(pthread_cond_signal(cond) == 0, "Failed to signal cond");
}

/*
 * Tries to unblock all threads blocked on the cond var, exits the program if
 * some error occurs
 *
 * Input:
 *   - cond: a pointer to the cond var
 */
void cond_broadcast(pthread_cond_t *cond) {
    ALWAYS_ASSERT(pthread_cond_broadcast(cond) == 0,
                  "Failed to broadcast cond");
}

/*
 * Tries to block a thread on the cond var, exits the program if some error
 * occurs
 *
 * Input:
 *   - cond: a pointer to the cond var
 *   - lock: a pointer to the lock
 */
void cond_wait(pthread_cond_t *cond, pthread_mutex_t *lock) {
    ALWAYS_ASSERT(pthread_cond_wait(cond, lock) == 0, "Failed to wait cond");
}

/*
 * Tries to destroy the cond var, exits the program if some error occurs
 *
 * Input:
 *   - cond: a pointer to the cond var
 */
void cond_destroy(pthread_cond_t *cond) {
    ALWAYS_ASSERT(pthread_cond_destroy(cond) == 0, "Failed to destroy cond");
}

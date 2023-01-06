#include "producer-consumer.h"
#include "betterassert.h"
#include "common.h"
#include "locks.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pcq_create(pc_queue_t *queue, size_t capacity) {
    if (capacity <= 0)
        return -1; // Can't create a queue with null or negative capacity
    queue->pcq_buffer = (void **)malloc(capacity * sizeof(void *));
    if (queue->pcq_buffer == NULL) {
        return -1;
    }

    queue->pcq_capacity = capacity;

    mutex_init(&queue->pcq_current_size_lock);
    queue->pcq_current_size = 0;

    mutex_init(&queue->pcq_head_lock);
    queue->pcq_head = 0;

    mutex_init(&queue->pcq_tail_lock);
    queue->pcq_tail = 0;

    mutex_init(&queue->pcq_pusher_condvar_lock);
    mutex_init(&queue->pcq_popper_condvar_lock);

    cond_init(&queue->pcq_pusher_condvar);
    cond_init(&queue->pcq_popper_condvar);

    return 0;
}

int pcq_destroy(pc_queue_t *queue) {
    free(queue->pcq_buffer);

    mutex_destroy(&queue->pcq_current_size_lock);
    mutex_destroy(&queue->pcq_head_lock);
    mutex_destroy(&queue->pcq_tail_lock);
    mutex_destroy(&queue->pcq_pusher_condvar_lock);
    mutex_destroy(&queue->pcq_popper_condvar_lock);
    cond_destroy(&queue->pcq_pusher_condvar);
    cond_destroy(&queue->pcq_popper_condvar);

    return 0;
}

int pcq_enqueue(pc_queue_t *queue, void *elem) {
    mutex_lock(&queue->pcq_pusher_condvar_lock);
    mutex_lock(&queue->pcq_current_size_lock);

    // Wait while the queue is full
    while (!(queue->pcq_current_size < queue->pcq_capacity))
        cond_wait(&queue->pcq_pusher_condvar, &queue->pcq_current_size_lock);

    mutex_lock(&queue->pcq_tail_lock);
    queue->pcq_buffer[queue->pcq_tail] = elem;
    queue->pcq_tail = (queue->pcq_tail + 1) % queue->pcq_capacity;
    mutex_unlock(&queue->pcq_tail_lock);

    queue->pcq_current_size++;

    // Inform that there's a new element in the queue
    cond_signal(&queue->pcq_popper_condvar);

    mutex_unlock(&queue->pcq_current_size_lock);
    mutex_unlock(&queue->pcq_pusher_condvar_lock);

    return 0;
}

void *pcq_dequeue(pc_queue_t *queue) {
    mutex_lock(&queue->pcq_popper_condvar_lock);
    mutex_lock(&queue->pcq_current_size_lock);

    // Wait while the queue is empty
    while (!(queue->pcq_current_size > 0))
        cond_wait(&queue->pcq_popper_condvar, &queue->pcq_current_size_lock);

    mutex_lock(&queue->pcq_head_lock);
    void *pop_elem = queue->pcq_buffer[queue->pcq_head];
    queue->pcq_head = (queue->pcq_head + 1) % queue->pcq_capacity;
    mutex_unlock(&queue->pcq_head_lock);

    queue->pcq_current_size--;

    // Inform that one element was removed from the queue
    cond_signal(&queue->pcq_pusher_condvar);

    mutex_unlock(&queue->pcq_current_size_lock);
    mutex_unlock(&queue->pcq_popper_condvar_lock);

    return pop_elem;
}

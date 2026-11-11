#ifndef RTOS_INTERNAL_H_
#define RTOS_INTERNAL_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define RTOS_ASSERT(expression) assert(expression)
#define RTOS_FALSE 0U
#define RTOS_OK 0
#define RTOS_ERR_SCHED (-1)

typedef uint32_t rtos_prio_t;
typedef uint32_t rtos_tick_t;
typedef int32_t rtos_status_t;
typedef struct rtos_tcb rtos_tcb_t;

typedef struct rtos_wait_node {
    void (*on_timeout)(struct rtos_wait_node *node);
    rtos_tcb_t *tcb;
    void *wait_obj;
    struct rtos_wait_node *next;
    struct rtos_wait_node *prev;
    struct rtos_wait_node **list_head;
    volatile uint32_t *queue_bitmap;
    rtos_prio_t queued_priority;
    uint8_t timed_out;
    uint8_t is_mutex_wait;
} rtos_wait_node_t;

struct rtos_tcb {
    void *blocked_on;
    rtos_wait_node_t wait_node;
    rtos_prio_t priority;
    rtos_status_t wait_result;
};

typedef struct rtos_kernel {
    rtos_tcb_t *current_tcb;
} rtos_kernel_t;

extern rtos_kernel_t rtos_kernel;

rtos_status_t rtos_internal_block_permitted(void);
void rtos_sched_block(rtos_tcb_t *task, rtos_tick_t ticks);
void rtos_sched_schedule(void);
void rtos_sched_unblock(rtos_tcb_t *task);

void rtos_internal_wait_insert_by_prio(rtos_wait_node_t **head, rtos_wait_node_t *node);
void rtos_internal_wait_remove_sync_bitmap(rtos_wait_node_t **head, rtos_wait_node_t *node);
void rtos_internal_wake_head_with_bitmap(rtos_wait_node_t **head, rtos_wait_node_t *node,
                                         volatile uint32_t *bitmap);
void rtos_internal_requeue_wait_lists(rtos_tcb_t *task);

#endif

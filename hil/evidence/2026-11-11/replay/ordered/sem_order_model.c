/** @file sem_order_model.c
 *  @brief Compare the actual ordered wait implementation against an independent ledger.
 */
#include "rtos_internal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    SEM_MODEL_WORKERS = 64U,
    SEM_MODEL_STEPS = 200000U,
    SEM_MODEL_PRIORITY_COUNT = 32U,
};

typedef struct sem_model_worker {
    rtos_tcb_t task;
    uint32_t ticket;
    bool is_waiting;
} sem_model_worker_t;

static sem_model_worker_t workers[SEM_MODEL_WORKERS];
static rtos_wait_node_t *wait_head;
static volatile uint32_t wait_bitmap;
static rtos_tcb_t *last_unblocked;
static uint32_t next_ticket;

rtos_kernel_t rtos_kernel;

rtos_status_t rtos_internal_block_permitted(void)
{
    return RTOS_ERR_SCHED;
}

void rtos_sched_block(rtos_tcb_t *task, rtos_tick_t ticks)
{
    (void)task;
    (void)ticks;
    assert(false);
}

void rtos_sched_schedule(void)
{
    assert(false);
}

void rtos_sched_unblock(rtos_tcb_t *task)
{
    last_unblocked = task;
}

static uint32_t random_next(uint32_t *state)
{
    *state ^= *state << 13U;
    *state ^= *state >> 17U;
    *state ^= *state << 5U;
    return *state;
}

static bool precedes(const sem_model_worker_t *left, const sem_model_worker_t *right)
{
    if (left->task.priority != right->task.priority) {
        return left->task.priority < right->task.priority;
    }
    return left->ticket < right->ticket;
}

static sem_model_worker_t *find_expected_head(void)
{
    sem_model_worker_t *best = NULL;
    for (uint32_t index = 0U; index < SEM_MODEL_WORKERS; ++index) {
        if (workers[index].is_waiting && ((best == NULL) || precedes(&workers[index], best))) {
            best = &workers[index];
        }
    }
    return best;
}

static void register_waiter(sem_model_worker_t *worker, rtos_prio_t priority)
{
    if (worker->is_waiting) {
        return;
    }
    rtos_wait_node_t *const node = &worker->task.wait_node;
    node->tcb = &worker->task;
    node->wait_obj = &wait_head;
    node->list_head = &wait_head;
    node->queue_bitmap = &wait_bitmap;
    node->queued_priority = priority;
    worker->task.blocked_on = &wait_head;
    worker->task.priority = priority;
    worker->task.wait_result = RTOS_ERR_SCHED;
    worker->ticket = next_ticket++;
    worker->is_waiting = true;
    rtos_internal_wait_insert_by_prio(&wait_head, node);
    wait_bitmap |= UINT32_C(1) << priority;
}

static void remove_waiter(sem_model_worker_t *worker)
{
    if (!worker->is_waiting) {
        return;
    }
    rtos_internal_wait_remove_sync_bitmap(&wait_head, &worker->task.wait_node);
    worker->task.wait_node.list_head = NULL;
    worker->task.blocked_on = NULL;
    worker->is_waiting = false;
}

static void change_priority(sem_model_worker_t *worker, rtos_prio_t priority)
{
    if (!worker->is_waiting) {
        return;
    }
    if (worker->task.priority != priority) {
        worker->ticket = next_ticket++;
    }
    worker->task.priority = priority;
    rtos_internal_requeue_wait_lists(&worker->task);
}

static void wake_waiter(void)
{
    sem_model_worker_t *const expected = find_expected_head();
    if (expected == NULL) {
        assert(wait_head == NULL);
        return;
    }
    assert(wait_head != NULL);
    last_unblocked = NULL;
    rtos_internal_wake_head_with_bitmap(&wait_head, wait_head, &wait_bitmap);
    assert(last_unblocked == &expected->task);
    assert(expected->task.wait_result == RTOS_OK);
    expected->is_waiting = false;
}

static void verify_wait_list(void)
{
    uint32_t expected_count = 0U;
    uint32_t actual_count = 0U;
    uint32_t bitmap = 0U;
    const sem_model_worker_t *previous = NULL;
    for (uint32_t index = 0U; index < SEM_MODEL_WORKERS; ++index) {
        expected_count += workers[index].is_waiting ? 1U : 0U;
    }

    for (const rtos_wait_node_t *node = wait_head; node != NULL; node = node->next) {
        const sem_model_worker_t *const worker = (const sem_model_worker_t *)node->tcb;
        assert(actual_count < expected_count);
        assert(worker->is_waiting);
        assert(node == &worker->task.wait_node);
        assert(node->list_head == &wait_head);
        assert(node->queued_priority == worker->task.priority);
        assert(node->prev == ((previous == NULL) ? NULL : &previous->task.wait_node));
        assert((previous == NULL) || precedes(previous, worker));
        bitmap |= UINT32_C(1) << node->queued_priority;
        previous = worker;
        ++actual_count;
    }
    assert(expected_count == actual_count);
    assert(bitmap == wait_bitmap);
}

int main(void)
{
    const uint32_t seeds[] = {1U, UINT32_C(0x12345678), UINT32_C(0xA5A5A5A5),
                              UINT32_C(0xDEADBEEF)};
    for (size_t seed_index = 0U; seed_index < sizeof(seeds) / sizeof(seeds[0]); ++seed_index) {
        uint32_t state = seeds[seed_index];
        wait_head = NULL;
        wait_bitmap = 0U;
        next_ticket = 0U;
        (void)memset(workers, 0, sizeof(workers));
        for (uint32_t step = 0U; step < SEM_MODEL_STEPS; ++step) {
            const uint32_t input = random_next(&state);
            sem_model_worker_t *const worker = &workers[(input >> 3U) % SEM_MODEL_WORKERS];
            const rtos_prio_t priority = (input >> 9U) % SEM_MODEL_PRIORITY_COUNT;
            switch (input & 7U) {
                case 0U:
                case 1U:
                case 2U:
                    register_waiter(worker, priority);
                    break;
                case 3U:
                    remove_waiter(worker);
                    break;
                case 4U:
                case 5U:
                case 6U:
                    change_priority(worker, priority);
                    break;
                default:
                    wake_waiter();
                    break;
            }
            verify_wait_list();
        }
        (void)printf("PASS seed=%08" PRIx32 " steps=%u\n", seeds[seed_index], SEM_MODEL_STEPS);
    }
    return 0;
}

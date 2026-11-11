/** @file h743_priority_noop_regression.c
 * @brief Verify that setting the current priority preserves ready-queue order.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtos.h"

enum {
    PRIORITY_TEST_PEERS = 3U,
    PRIORITY_TEST_TRACE_LENGTH = 4U,
    PRIORITY_TEST_PEER_PRIORITY = 10U,
    PRIORITY_TEST_STACK_WORDS = 256U,
    PRIORITY_TEST_WAIT_TICKS = 30U
};

static rtos_tcb_t s_peers[PRIORITY_TEST_PEERS];
static _Alignas(8) rtos_stack_t s_stacks[PRIORITY_TEST_PEERS][PRIORITY_TEST_STACK_WORDS];
static rtos_sem_t s_completed;
static uint8_t s_trace[PRIORITY_TEST_TRACE_LENGTH];
static uint32_t s_trace_count;
static bool s_change_running_priority;
static rtos_status_t s_running_change_status;

void h743_priority_noop_regression_run(void *argument);

static void record_peer(uint8_t id)
{
    RTOS_PORT_ENTER_CRITICAL();
    if (s_trace_count < PRIORITY_TEST_TRACE_LENGTH) {
        s_trace[s_trace_count++] = id;
    }
    RTOS_PORT_EXIT_CRITICAL();
}

static void run_peer(void *argument)
{
    const uint8_t id = (uint8_t)((rtos_tcb_t *)argument - s_peers) + 1U;
    record_peer(id);
    if (s_change_running_priority && id == 1U) {
        s_running_change_status = rtos_task_set_priority(NULL, PRIORITY_TEST_PEER_PRIORITY);
        record_peer(PRIORITY_TEST_TRACE_LENGTH);
    }
    /* The coordinator verifies all three successful takes with a finite deadline. */
    (void)rtos_sem_give(&s_completed);
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static bool check_order(bool change_running, const uint8_t *expected, uint32_t length)
{
    uint32_t created = 0U;
    if (rtos_sem_init(&s_completed, 0U, PRIORITY_TEST_PEERS) != RTOS_OK) {
        return false;
    }
    s_trace_count = 0U;
    s_change_running_priority = change_running;
    s_running_change_status = RTOS_ERR_SCHED;
    while (created < PRIORITY_TEST_PEERS &&
           rtos_task_create_static(&s_peers[created], s_stacks[created], PRIORITY_TEST_STACK_WORDS,
                                   run_peer, &s_peers[created], PRIORITY_TEST_PEER_PRIORITY,
                                   "priority-peer") == RTOS_OK) {
        ++created;
    }

    bool is_ok = created == PRIORITY_TEST_PEERS;
    if (is_ok && !change_running) {
        is_ok = rtos_task_set_priority(&s_peers[0], PRIORITY_TEST_PEER_PRIORITY) == RTOS_OK;
    }
    for (uint32_t index = 0U; is_ok && index < PRIORITY_TEST_PEERS; ++index) {
        is_ok = rtos_sem_take(&s_completed, PRIORITY_TEST_WAIT_TICKS) == RTOS_OK;
    }
    is_ok = is_ok && (!change_running || s_running_change_status == RTOS_OK);
    is_ok = is_ok && s_trace_count == length && memcmp(s_trace, expected, length) == 0;
    (void)printf("H7PRIO|TRACE|running=%u|length=%lu|order=%u%u%u%u\r\n",
                 (unsigned int)change_running, (unsigned long)s_trace_count,
                 (unsigned int)s_trace[0], (unsigned int)s_trace[1],
                 (unsigned int)s_trace[2], (unsigned int)s_trace[3]);
    for (uint32_t index = 0U; index < created; ++index) {
        is_ok = (rtos_task_delete(&s_peers[index]) == RTOS_OK) && is_ok;
    }
    return (rtos_sem_deinit(&s_completed) == RTOS_OK) && is_ok;
}

void h743_priority_noop_regression_run(void *argument)
{
    (void)argument;
    static const uint8_t ready_order[] = {1U, 2U, 3U};
    static const uint8_t running_order[] = {1U, 4U, 2U, 3U};

    const bool ready_ok = check_order(false, ready_order, sizeof(ready_order));
    const bool running_ok = check_order(true, running_order, sizeof(running_order));
    (void)printf("H7PRIO|DONE|status=%u|ready=%u|running=%u\r\n",
                 (ready_ok && running_ok) ? 0U : 1U,
                 (unsigned int)ready_ok, (unsigned int)running_ok);
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

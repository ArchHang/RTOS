/** @file h743_idle_fifo_regression.c
 *  @brief Verify the lowest-priority ready ring with three application tasks and idle.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "rtos.h"

enum {
    FIFO_PEERS = 3U,
    FIFO_VISITS = 3U,
    FIFO_TRACE_LENGTH = FIFO_PEERS * (FIFO_VISITS + 1U),
    FIFO_STACK_WORDS = 192U,
    FIFO_SOLO_TICKS = 4U,
    FIFO_TEST_WAIT = 32U,
    FIFO_HEAD_BEFORE_IDLE = 0U,
    FIFO_RETAINED_PEER = 2U
};

typedef struct idle_fifo_trace {
    uint32_t order[FIFO_TRACE_LENGTH];
    uint32_t entries;
    rtos_tick_t first_tick;
    rtos_tick_t last_tick;
    rtos_tick_t solo_ticks;
    uint32_t solo_switches;
    uint32_t idle_switches;
    bool delete_head_before_idle;
    bool delete_returned;
} idle_fifo_trace_t;

static rtos_tcb_t s_peers[FIFO_PEERS];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t s_stacks[FIFO_PEERS][FIFO_STACK_WORDS];
static idle_fifo_trace_t s_trace;
static rtos_sem_t s_done;
static uint32_t s_idle_switches_before;

void h743_idle_fifo_regression_run(void *argument);

static void record_peer(uint32_t id)
{
    RTOS_PORT_ENTER_CRITICAL();
    const rtos_tick_t now = rtos_sched_get_tick_count();
    if (s_trace.entries == 0U) {
        s_trace.first_tick = now;
    }
    if (s_trace.entries < FIFO_TRACE_LENGTH) {
        s_trace.order[s_trace.entries] = id;
    }
    ++s_trace.entries;
    s_trace.last_tick = now;
    RTOS_PORT_EXIT_CRITICAL();
}

static void verify_solo_ticks(const rtos_tcb_t *peer)
{
    const uint32_t switches_before = peer->switch_count;
    const rtos_tick_t before = rtos_sched_get_tick_count();
    rtos_tick_t elapsed;
    do {
        elapsed = rtos_sched_get_tick_count() - before;
    } while (elapsed < FIFO_SOLO_TICKS);

    s_trace.solo_ticks = elapsed;
    s_trace.solo_switches = peer->switch_count - switches_before;
    s_trace.idle_switches = rtos_kernel.idle_tcb->switch_count - s_idle_switches_before;
    (void)rtos_sem_give(&s_done);
}

static void run_peer(void *argument)
{
    rtos_tcb_t *peer = argument;
    const uint32_t id = (uint32_t)(peer - s_peers);
    for (uint32_t visit = 0U; visit < FIFO_VISITS; ++visit) {
        record_peer(id);
        rtos_task_yield();
    }

    record_peer(id);
    if (id == FIFO_RETAINED_PEER) {
        verify_solo_ticks(peer);
    } else {
        if (id == FIFO_HEAD_BEFORE_IDLE) {
            s_trace.delete_head_before_idle = peer->next_ready == rtos_kernel.idle_tcb &&
                rtos_kernel.ready_heads[peer->priority] == peer;
        }
        (void)rtos_task_delete(NULL);
        s_trace.delete_returned = true;
    }
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static bool trace_is_fifo(void)
{
    if (s_trace.entries != FIFO_TRACE_LENGTH || s_trace.first_tick != s_trace.last_tick) {
        return false;
    }
    for (uint32_t index = 0U; index < FIFO_TRACE_LENGTH; ++index) {
        if (s_trace.order[index] != index % FIFO_PEERS) {
            return false;
        }
    }
    return true;
}

void h743_idle_fifo_regression_run(void *argument)
{
    (void)argument;
    const bool initialized = rtos_sem_init(&s_done, 0U, 1U) == RTOS_OK;
    uint32_t created = 0U;

    (void)printf("H7FIFO|BEGIN|preempt=%u|time_slice=%u\r\n",
                 (unsigned int)RTOS_CONFIG_USE_PREEMPTION,
                 (unsigned int)RTOS_CONFIG_USE_TIME_SLICING);
    if (initialized) {
        /* Start the explicit-yield trace at the beginning of a hardware Tick. */
        rtos_task_delay(1U);
        while (created < FIFO_PEERS &&
               rtos_task_create_static(&s_peers[created], s_stacks[created], FIFO_STACK_WORDS,
                                       run_peer, &s_peers[created],
                                       RTOS_CONFIG_MAX_PRIORITIES - 1U, "fifo-peer") == RTOS_OK) {
            ++created;
        }
    }

    s_idle_switches_before = rtos_kernel.idle_tcb->switch_count;
    const bool completed = created == FIFO_PEERS && rtos_sem_take(&s_done, FIFO_TEST_WAIT) ==
        RTOS_OK;
    const bool fifo = completed && trace_is_fifo();
    const bool solo = completed && s_trace.solo_ticks >= FIFO_SOLO_TICKS &&
        s_trace.solo_switches == 0U && s_trace.idle_switches == 0U;
    const bool removed = completed && s_trace.delete_head_before_idle && !s_trace.delete_returned &&
        s_peers[0].state == RTOS_TASK_DELETED && s_peers[1].state == RTOS_TASK_DELETED;
    bool clean = initialized;
    for (uint32_t index = 0U; index < created; ++index) {
        if (s_peers[index].state != RTOS_TASK_DELETED) {
            clean = (rtos_task_delete(&s_peers[index]) == RTOS_OK) && clean;
        }
    }
    if (initialized) {
        clean = (rtos_sem_deinit(&s_done) == RTOS_OK) && clean;
    }
    (void)printf("H7FIFO|DONE|status=%u|created=%lu|completed=%u|fifo=%u|removed=%u|solo=%u"
                 "|clean=%u|entries=%lu|trace_ticks=%lu|solo_ticks=%lu|solo_switches=%lu"
                 "|idle_switches=%lu\r\n", (fifo && solo && removed && clean) ? 0U : 1U,
                 (unsigned long)created, (unsigned int)completed, (unsigned int)fifo,
                 (unsigned int)removed, (unsigned int)solo, (unsigned int)clean,
                 (unsigned long)s_trace.entries,
                 (unsigned long)(s_trace.last_tick - s_trace.first_tick),
                 (unsigned long)s_trace.solo_ticks, (unsigned long)s_trace.solo_switches,
                 (unsigned long)s_trace.idle_switches);
}

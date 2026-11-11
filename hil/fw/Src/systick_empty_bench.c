/**
 * @file systick_empty_bench.c
 * @brief Measure the real scheduler Tick with no due task.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "rtos.h"
#include "hil_target.h"

enum {
    SAMPLE_COUNT = 128U,
    FUTURE_DELAY_TICKS = 10000U,
    FUTURE_PRIORITY = 4U,
    FUTURE_STACK_WORDS = 192U,
    SYSTICK_EXCEPTION = 15U,
};

static rtos_tcb_t future_task;
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t future_stack[FUTURE_STACK_WORDS];
static rtos_tcb_t *volatile expected_controller;
static rtos_tcb_t *volatile expected_head;
static volatile bool future_returned;
static volatile bool sampling;
static volatile bool invariant_failed;
static volatile uint32_t sample_count;
static volatile rtos_tick_t last_sampled_tick;
static volatile uint32_t samples[SAMPLE_COUNT];

void __real_rtos_sched_tick(void);
void __wrap_rtos_sched_tick(void);

void __wrap_rtos_sched_tick(void)
{
    if (!sampling) {
        __real_rtos_sched_tick();
        return;
    }

    rtos_tcb_t *const head = expected_head;
    rtos_tcb_t *const controller = expected_controller;
    const rtos_tick_t expected_tick = rtos_kernel.tick_count + 1U;
    const bool is_valid_before = (__get_IPSR() == SYSTICK_EXCEPTION)
        && (rtos_kernel.current_tcb == controller) && (rtos_kernel.next_tcb == NULL)
        && (expected_tick == (last_sampled_tick + 1U))
        && (rtos_kernel.delay_head == head)
        && ((head == NULL) || ((int32_t)(head->wake_tick - expected_tick) > 0));
    const uint32_t started = DWT->CYCCNT;
    __real_rtos_sched_tick();
    const uint32_t elapsed = DWT->CYCCNT - started;

    if (!is_valid_before || (rtos_kernel.tick_count != expected_tick)
        || (rtos_kernel.delay_head != head) || (rtos_kernel.current_tcb != controller)
        || (rtos_kernel.next_tcb != NULL) || (elapsed == 0U)) {
        invariant_failed = true;
    }
    const uint32_t index = sample_count;
    samples[index] = elapsed;
    last_sampled_tick = rtos_kernel.tick_count;
    sample_count = index + 1U;
    if (sample_count == SAMPLE_COUNT) {
        sampling = false;
    }
}

static void wait_future(void *argument)
{
    (void)argument;
    rtos_task_delay(FUTURE_DELAY_TICKS);
    future_returned = true;
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void report_distribution(const char *path)
{
    uint32_t sorted[SAMPLE_COUNT];
    uint32_t sum = 0U;
    for (uint32_t index = 0U; index < SAMPLE_COUNT; ++index) {
        const uint32_t value = samples[index];
        uint32_t slot = index;
        sum += value;
        while ((slot > 0U) && (sorted[slot - 1U] > value)) {
            sorted[slot] = sorted[slot - 1U];
            --slot;
        }
        sorted[slot] = value;
    }
    (void)printf("STB|CASE|PASS|path=%s|n=%u|avg=%lu|min=%lu|p50=%lu|p95=%lu|max=%lu\r\n",
                 path, (unsigned int)SAMPLE_COUNT, (unsigned long)(sum / SAMPLE_COUNT),
                 (unsigned long)sorted[0], (unsigned long)sorted[SAMPLE_COUNT / 2U - 1U],
                 (unsigned long)sorted[(SAMPLE_COUNT * 95U + 99U) / 100U - 1U],
                 (unsigned long)sorted[SAMPLE_COUNT - 1U]);
}

static bool measure_path(const char *path, rtos_tcb_t *head)
{
    RTOS_PORT_ENTER_CRITICAL();
    expected_controller = rtos_task_get_current();
    expected_head = head;
    const rtos_tick_t initial_tick = rtos_kernel.tick_count;
    const bool is_valid_start = (rtos_kernel.delay_head == head)
        && (rtos_kernel.next_tcb == NULL) && (expected_controller != NULL);
    invariant_failed = false;
    sample_count = 0U;
    last_sampled_tick = initial_tick;
    sampling = is_valid_start;
    RTOS_PORT_EXIT_CRITICAL();

    const uint32_t started = DWT->CYCCNT;
    while (sampling && ((DWT->CYCCNT - started) < SystemCoreClock)) {
        __NOP();
    }

    RTOS_PORT_ENTER_CRITICAL();
    sampling = false;
    const uint32_t actual_count = sample_count;
    const rtos_tick_t tick_span = last_sampled_tick - initial_tick;
    const bool is_valid = !invariant_failed && (actual_count == SAMPLE_COUNT)
        && (tick_span == SAMPLE_COUNT) && !future_returned;
    RTOS_PORT_EXIT_CRITICAL();
    if (is_valid) {
        report_distribution(path);
    } else {
        (void)printf("STB|CASE|FAIL|path=%s|n=%lu|ticks=%lu|invariant=%u\r\n", path,
                     (unsigned long)actual_count, (unsigned long)tick_span,
                     (unsigned int)invariant_failed);
    }
    return is_valid;
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();

    const bool empty_ok = measure_path("empty", NULL);
    const bool created = rtos_task_create_static(&future_task, future_stack, FUTURE_STACK_WORDS,
                                                 wait_future, NULL, FUTURE_PRIORITY,
                                                 "systick-future") == RTOS_OK;
    bool future_ok = false;
    if (created) {
        future_ok = measure_path("future", &future_task);
        const bool cleaned = (rtos_task_delete(&future_task) == RTOS_OK)
            && (rtos_kernel.delay_head == NULL) && !future_returned;
        future_ok = future_ok && cleaned;
    }

    const unsigned int passed = (unsigned int)empty_ok + (unsigned int)future_ok;
    (void)printf("STB|DONE|pass=%u|fail=%u\r\n", passed, 2U - passed);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

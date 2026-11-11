/** @file h743_service_edges.c
 *  @brief Check timer phase, rollover and performance query boundaries.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hil_target.h"
#include "rtos.h"
#include "rtos_mem.h"
#include "rtos_perf.h"

enum {
    SERVICE_EDGE_TRACE_CAPACITY = 4U,
    SERVICE_EDGE_BEFORE_PERIOD = 5U,
    SERVICE_EDGE_AFTER_PERIOD = 24U,
    SERVICE_EDGE_PERIOD = 8U,
    SERVICE_EDGE_CALLBACK_DELAY = 18U,
    SERVICE_EDGE_POOL_BLOCK_SIZE = 32U,
    SERVICE_EDGE_POOL_BLOCK_COUNT = 3U
};

typedef struct timer_trace_entry {
    uint32_t id;
    rtos_tick_t deadline;
    rtos_tick_t observed;
    bool is_task_context;
} timer_trace_entry_t;

typedef struct timer_case {
    rtos_timer_t timer;
    uint32_t id;
} timer_case_t;

static timer_case_t s_cases[3];
static timer_trace_entry_t s_trace[SERVICE_EDGE_TRACE_CAPACITY];
static uint32_t s_trace_count;
static uint32_t s_failures;
static uint32_t s_checks;
static rtos_status_t s_self_stop_status;
static rtos_mempool_t s_pool;
static _Alignas(uintptr_t) uint8_t s_pool_storage[SERVICE_EDGE_POOL_BLOCK_SIZE *
                                                 SERVICE_EDGE_POOL_BLOCK_COUNT];

void h743_service_edges_run(void *argument);

static void record_result(const char *label, bool is_passed)
{
    ++s_checks;
    if (!is_passed) {
        ++s_failures;
    }
    (void)printf("H7S|CHECK|%s|%s\r\n", label, is_passed ? "PASS" : "FAIL");
}

static void record_callback(void *argument)
{
    timer_case_t *timer_case = argument;
    const bool is_unmasked_task = !RTOS_PORT_IN_ISR() &&
        __get_BASEPRI() == 0U && __get_PRIMASK() == 0U;
    RTOS_PORT_ENTER_CRITICAL();
    if (s_trace_count < SERVICE_EDGE_TRACE_CAPACITY) {
        timer_trace_entry_t *entry = &s_trace[s_trace_count];
        entry->id = timer_case->id;
        entry->deadline = timer_case->timer.expire_tick;
        entry->observed = rtos_sched_get_tick_count();
        entry->is_task_context = is_unmasked_task;
    }
    const uint32_t count = ++s_trace_count;
    RTOS_PORT_EXIT_CRITICAL();

    if (timer_case->timer.mode == RTOS_TIMER_PERIODIC) {
        if (count == 1U) {
            rtos_task_delay(SERVICE_EDGE_CALLBACK_DELAY);
        } else {
            const rtos_status_t status = rtos_timer_stop(&timer_case->timer);
            RTOS_PORT_ENTER_CRITICAL();
            s_self_stop_status = status;
            RTOS_PORT_EXIT_CRITICAL();
        }
    }
}

static bool check_trace(uint32_t expected_count)
{
    RTOS_PORT_ENTER_CRITICAL();
    bool is_valid = s_trace_count == expected_count;
    for (uint32_t index = 0U; index < expected_count; ++index) {
        const timer_trace_entry_t *entry = &s_trace[index];
        is_valid = is_valid && entry->is_task_context &&
            (entry->observed - entry->deadline <= 1U);
    }
    RTOS_PORT_EXIT_CRITICAL();
    return is_valid;
}

static void test_timer_rollover(void)
{
    const rtos_tick_t periods[] = { SERVICE_EDGE_BEFORE_PERIOD,
                                   SERVICE_EDGE_AFTER_PERIOD, SERVICE_EDGE_AFTER_PERIOD };
    bool is_ready = rtos_timer_init() == RTOS_ERR_ALREADY;
    for (uint32_t index = 0U; index < 3U; ++index) {
        s_cases[index].id = index;
        const rtos_status_t status = rtos_timer_create(&s_cases[index].timer, "rollover",
            record_callback, &s_cases[index], periods[index], RTOS_TIMER_ONE_SHOT);
        is_ready = is_ready && status == RTOS_OK;
    }

    /* Seed only the test's empty scheduler delay chain. Actual IRQs advance the clock. */
    RTOS_PORT_ENTER_CRITICAL();
    if (rtos_kernel.delay_head == NULL && is_ready) {
        rtos_kernel.tick_count = UINT32_MAX - 20U;
        for (uint32_t index = 0U; index < 3U; ++index) {
            const rtos_status_t status = rtos_timer_start(&s_cases[index].timer);
            is_ready = is_ready && status == RTOS_OK;
        }
    } else {
        is_ready = false;
    }
    RTOS_PORT_EXIT_CRITICAL();
    if (is_ready) {
        rtos_task_delay(30U);
    }

    record_result("timer_duplicate_init_and_start", is_ready);
    record_result("timer_real_tick_rollover", is_ready && check_trace(3U) &&
        s_trace[0].observed > UINT32_MAX - 20U && s_trace[1].observed < 12U);
    record_result("timer_equal_deadline_fifo", is_ready && s_trace_count == 3U &&
        s_trace[0].id == 0U && s_trace[1].id == 1U && s_trace[2].id == 2U &&
        s_trace[1].deadline == s_trace[2].deadline);
    (void)printf("H7S|TIMER_WRAP|before=%lu|after=%lu|equal=%lu\r\n",
        (unsigned long)s_trace[0].observed, (unsigned long)s_trace[1].observed,
        (unsigned long)s_trace[2].observed);
}

static void test_timer_periodic(void)
{
    timer_case_t *timer_case = &s_cases[0];
    s_trace_count = 0U;
    s_self_stop_status = RTOS_ERR_PARAM;
    const rtos_status_t create = rtos_timer_create(&timer_case->timer, "slow-periodic",
        record_callback, timer_case, 100U, RTOS_TIMER_PERIODIC);
    const rtos_status_t change = rtos_timer_change_period(&timer_case->timer, SERVICE_EDGE_PERIOD);
    rtos_task_delay(3U);
    const bool is_stopped = timer_case->timer.is_active == 0U && s_trace_count == 0U;
    const rtos_status_t start = rtos_timer_start(&timer_case->timer);
    rtos_task_delay(50U);

    record_result("timer_period_change_does_not_start_stopped", create == RTOS_OK &&
        change == RTOS_OK && is_stopped && timer_case->timer.period == SERVICE_EDGE_PERIOD);
    record_result("timer_slow_callback_skips_two_missed_phases", start == RTOS_OK &&
        check_trace(2U) && (s_trace[1].deadline - s_trace[0].deadline) ==
        3U * SERVICE_EDGE_PERIOD);
    record_result("timer_self_stop_blocks_future_callbacks", s_self_stop_status == RTOS_OK &&
        s_trace_count == 2U && timer_case->timer.is_active == 0U);
    (void)printf("H7S|TIMER_PERIODIC|count=%lu|first=%lu|second=%lu\r\n",
        (unsigned long)s_trace_count, (unsigned long)s_trace[0].deadline,
        (unsigned long)s_trace[1].deadline);

    bool is_deleted = true;
    for (uint32_t index = 0U; index < 3U; ++index) {
        const rtos_status_t status = rtos_timer_delete(&s_cases[index].timer);
        is_deleted = is_deleted && status == RTOS_OK;
    }
    rtos_task_delay(3U);
    for (uint32_t index = 0U; index < 3U; ++index) {
        is_deleted = is_deleted && rtos_timer_start(&s_cases[index].timer) == RTOS_ERR_PARAM;
    }
    record_result("timer_async_delete_consumed", is_deleted);
}

static void test_cycle_conversion(void)
{
    /* Isolate the conversion input. The actual clocks and SysTick are untouched. */
    RTOS_PORT_ENTER_CRITICAL();
    const uint32_t board_clock = SystemCoreClock;
    const uint32_t board_value = rtos_perf_cycles_to_us(board_clock);
    SystemCoreClock = 1500000U;
    const uint32_t fractional = rtos_perf_cycles_to_us(1500000U);
    SystemCoreClock = 750000U;
    const uint32_t sub_mhz = rtos_perf_cycles_to_us(750000U);
    const uint32_t wrap = rtos_perf_cycles_to_us(UINT32_MAX);
    SystemCoreClock = 0U;
    const uint32_t zero = rtos_perf_cycles_to_us(1500000U);
    SystemCoreClock = board_clock;
    RTOS_PORT_EXIT_CRITICAL();

    (void)printf("H7S|CYCLES|clock=%lu|native=%lu|fractional=%lu|sub_mhz=%lu"
        "|wrap=%lu|zero=%lu\r\n", (unsigned long)board_clock, (unsigned long)board_value,
        (unsigned long)fractional, (unsigned long)sub_mhz, (unsigned long)wrap,
        (unsigned long)zero);
    record_result("perf_native_clock_and_zero_guard", board_clock != 0U &&
        board_value == 1000000U && zero == 0U);
    record_result("perf_fractional_and_sub_mhz_clock", fractional == 1000000U &&
        sub_mhz == 1000000U && wrap == UINT32_C(1431655764));
}

static void test_performance_queries(void)
{
    rtos_perf_system_t system = {0};
    rtos_perf_task_t current = {0};
    rtos_perf_task_t tasks[2] = {0};
    rtos_perf_task_t untouched;
    uint32_t actual = UINT32_MAX;
    rtos_perf_memory_t before = {0};
    rtos_perf_memory_t after = {0};

    rtos_perf_reset();
    rtos_task_delay(3U);
    const rtos_status_t task_status = rtos_perf_get_task(NULL, &current);
    const rtos_status_t system_status = rtos_perf_get_system(&system);
    record_result("perf_window_queries_and_cache", task_status == RTOS_OK &&
        system_status == RTOS_OK && system.cpu_freq_hz == SystemCoreClock &&
        system.tick_rate_hz == RTOS_CONFIG_TICK_RATE_HZ &&
        current.stack_used_bytes + current.stack_free_bytes == current.stack_total_bytes &&
        system.cpu_usage_x100 + system.idle_usage_x100 == 10000U &&
        rtos_perf_get_cpu_usage() == system.cpu_usage_x100 &&
        rtos_perf_get_switch_rate() == system.switch_rate);

    memset(&tasks[1], 0xA5, sizeof(tasks[1]));
    memcpy(&untouched, &tasks[1], sizeof(untouched));
    const rtos_status_t empty = rtos_perf_get_all_tasks(tasks, 0U, &actual);
    const bool is_untouched = actual == UINT32_MAX;
    const rtos_status_t one = rtos_perf_get_all_tasks(tasks, 1U, &actual);
    record_result("perf_array_bounds_and_null_arguments", empty == RTOS_ERR_PARAM &&
        is_untouched && one == RTOS_OK && actual == 1U &&
        memcmp(&tasks[1], &untouched, sizeof(untouched)) == 0 &&
        rtos_perf_get_task(NULL, NULL) == RTOS_ERR_NULL &&
        rtos_perf_get_system(NULL) == RTOS_ERR_NULL &&
        rtos_perf_get_memory(NULL) == RTOS_ERR_NULL);

    const rtos_status_t before_status = rtos_perf_get_memory(&before);
    const rtos_status_t pool_status = rtos_mempool_init(&s_pool, s_pool_storage,
        SERVICE_EDGE_POOL_BLOCK_SIZE, SERVICE_EDGE_POOL_BLOCK_COUNT);
    void *const block = pool_status == RTOS_OK ? rtos_mempool_alloc(&s_pool) : NULL;
    const rtos_status_t after_status = rtos_perf_get_memory(&after);
    const rtos_status_t free_status = block != NULL ? rtos_mempool_free(&s_pool, block)
                                                     : RTOS_ERR_PARAM;
    record_result("perf_pool_registration_and_byte_ledger", before_status == RTOS_OK &&
        after_status == RTOS_OK && free_status == RTOS_OK &&
        after.registered_pools == before.registered_pools + 1U &&
        after.pool_used_bytes == before.pool_used_bytes + SERVICE_EDGE_POOL_BLOCK_SIZE &&
        after.pool_total_bytes == before.pool_total_bytes + sizeof(s_pool_storage) &&
        after.pool_total_bytes == after.pool_used_bytes + after.pool_free_bytes);
}

void h743_service_edges_run(void *argument)
{
    (void)argument;
    test_timer_rollover();
    test_timer_periodic();
    test_cycle_conversion();
    test_performance_queries();
    (void)printf("H7S|DONE|status=%u|checks=%lu|failures=%lu\r\n",
        s_failures == 0U ? 0U : 1U, (unsigned long)s_checks, (unsigned long)s_failures);
}

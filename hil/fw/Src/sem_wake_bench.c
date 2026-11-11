/**
 * @file sem_wake_bench.c
 * @brief Time semaphore give with real blocked tasks and verify bitmap cleanup.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"

#include "rtos.h"

enum {
    SEM_WAKE_MAX_WORKERS = 16U,
    SEM_WAKE_STACK_WORDS = 192U,
    SEM_WAKE_HIGH_PRIORITY = 6U,
    SEM_WAKE_LOW_PRIORITY = 7U,
    SEM_WAKE_TIMEOUT_PRIORITY = 8U,
    SEM_WAKE_STALE_PRIORITY = 1U,
    SEM_WAKE_SAMPLES = 128U,
    SEM_WAKE_TIMEOUT_TICKS = 20U
};

typedef struct sem_wake_worker {
    rtos_tcb_t task;
    rtos_status_t status;
    bool has_completed;
} sem_wake_worker_t;

static sem_wake_worker_t workers[SEM_WAKE_MAX_WORKERS];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t
    worker_stacks[SEM_WAKE_MAX_WORKERS][SEM_WAKE_STACK_WORDS];
static rtos_sem_t semaphore;
static uint32_t passed_checks;
static uint32_t failed_checks;

static uint32_t priority_bit(rtos_prio_t priority)
{
    return 1U << priority;
}

static void run_worker(void *argument)
{
    sem_wake_worker_t *const worker = (sem_wake_worker_t *)argument;
    for (;;) {
        uint32_t timeout = 0U;
        rtos_status_t status = rtos_task_notify_wait(RTOS_WAIT_FOREVER, &timeout);
        if (status == RTOS_OK) {
            status = rtos_sem_take(&semaphore, timeout);
        }
        RTOS_PORT_ENTER_CRITICAL();
        worker->status = status;
        worker->has_completed = true;
        RTOS_PORT_EXIT_CRITICAL();
    }
}

static uint32_t create_workers(const rtos_prio_t *priorities, uint32_t count)
{
    uint32_t created = 0U;
    while (created < count) {
        const rtos_status_t status = rtos_task_create_static(&workers[created].task,
            worker_stacks[created], SEM_WAKE_STACK_WORDS, run_worker, &workers[created],
            priorities[created], "sem-wake");
        if (status != RTOS_OK) {
            break;
        }
        ++created;
    }
    return created;
}

static bool delete_workers(uint32_t count)
{
    bool is_clean = true;
    for (uint32_t index = 0U; index < count; ++index) {
        if (rtos_task_delete(&workers[index].task) != RTOS_OK) {
            is_clean = false;
        }
    }
    return is_clean;
}

static bool enroll_worker(uint32_t index, rtos_tick_t timeout)
{
    sem_wake_worker_t *const worker = &workers[index];
    RTOS_PORT_ENTER_CRITICAL();
    worker->has_completed = false;
    RTOS_PORT_EXIT_CRITICAL();
    if (rtos_task_notify(&worker->task, timeout, RTOS_NOTIFY_VALUE) != RTOS_OK) {
        return false;
    }
    RTOS_PORT_ENTER_CRITICAL();
    const bool is_waiting = (worker->task.blocked_on == &semaphore)
        && (worker->task.wait_node.list_head == &semaphore.wait_head);
    RTOS_PORT_EXIT_CRITICAL();
    return is_waiting;
}

static bool worker_returned(uint32_t index, rtos_status_t expected)
{
    RTOS_PORT_ENTER_CRITICAL();
    const bool has_returned = workers[index].has_completed && (workers[index].status == expected);
    RTOS_PORT_EXIT_CRITICAL();
    return has_returned;
}

static bool give_one(uint32_t index, uint32_t expected_bitmap, uint32_t *elapsed)
{
    /* The outer mask keeps PendSV outside the DWT interval. */
    RTOS_PORT_ENTER_CRITICAL();
    const uint32_t start = DWT->CYCCNT;
    const rtos_status_t status = rtos_sem_give(&semaphore);
    *elapsed = DWT->CYCCNT - start;
    const bool is_valid = (status == RTOS_OK) && (*elapsed != 0U)
        && (workers[index].task.wait_node.list_head == NULL)
        && (workers[index].task.wait_result == RTOS_OK) && (semaphore.count == 0U)
        && (semaphore.wait_bitmap == expected_bitmap);
    RTOS_PORT_EXIT_CRITICAL();
    return is_valid && worker_returned(index, RTOS_OK);
}

static bool run_sample(uint32_t count, bool is_high_first, uint32_t *elapsed)
{
    if (rtos_sem_init(&semaphore, 0U, 1U) != RTOS_OK) {
        return false;
    }

    bool is_valid = !is_high_first || enroll_worker(0U, RTOS_WAIT_FOREVER);
    for (uint32_t index = 1U; is_valid && (index < count); ++index) {
        is_valid = enroll_worker(index, RTOS_WAIT_FOREVER);
    }
    is_valid = is_valid && (is_high_first || enroll_worker(0U, RTOS_WAIT_FOREVER));

    const uint32_t low_bit = (count > 1U) ? priority_bit(SEM_WAKE_LOW_PRIORITY) : 0U;
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t waiting_count = 0U;
    for (const rtos_wait_node_t *node = semaphore.wait_head;
         (node != NULL) && (waiting_count <= count); node = node->next) {
        ++waiting_count;
    }
    is_valid = is_valid && (waiting_count == count)
        && (semaphore.wait_bitmap == (low_bit | priority_bit(SEM_WAKE_HIGH_PRIORITY)));
    RTOS_PORT_EXIT_CRITICAL();
    is_valid = is_valid && give_one(0U, low_bit, elapsed);

    const bool is_clean = (rtos_sem_deinit(&semaphore) == RTOS_OK)
        && (semaphore.wait_head == NULL) && (semaphore.wait_bitmap == 0U);
    return is_valid && is_clean;
}

static void record_check(bool is_valid)
{
    if (is_valid) {
        ++passed_checks;
    } else {
        ++failed_checks;
    }
}

static void measure_give(uint32_t count, bool is_high_first)
{
    rtos_prio_t priorities[SEM_WAKE_MAX_WORKERS];
    priorities[0] = SEM_WAKE_HIGH_PRIORITY;
    for (uint32_t index = 1U; index < count; ++index) {
        priorities[index] = SEM_WAKE_LOW_PRIORITY;
    }

    const uint32_t created = create_workers(priorities, count);
    uint32_t samples = 0U;
    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0U;
    uint64_t total = 0U;
    while ((created == count) && (samples < SEM_WAKE_SAMPLES)) {
        uint32_t elapsed = 0U;
        if (!run_sample(count, is_high_first, &elapsed)) {
            break;
        }
        if (elapsed < minimum) {
            minimum = elapsed;
        }
        if (elapsed > maximum) {
            maximum = elapsed;
        }
        total += elapsed;
        ++samples;
    }
    const bool is_valid = delete_workers(created) && (samples == SEM_WAKE_SAMPLES);
    record_check(is_valid);
    (void)printf("SWB|GIVE|waiters=%lu|high_enrolled=%s|n=%lu|avg=%lu|min=%lu|max=%lu|%s\r\n",
                 (unsigned long)count, is_high_first ? "first" : "last", (unsigned long)samples,
                 (unsigned long)((samples != 0U) ? (total / samples) : 0U),
                 (unsigned long)((samples != 0U) ? minimum : 0U), (unsigned long)maximum,
                 is_valid ? "PASS" : "FAIL");
}

static bool test_fifo(uint32_t injected_bitmap)
{
    static const rtos_prio_t priorities[] = {
        SEM_WAKE_LOW_PRIORITY, SEM_WAKE_HIGH_PRIORITY, SEM_WAKE_LOW_PRIORITY, SEM_WAKE_HIGH_PRIORITY
    };
    const uint32_t count = (uint32_t)(sizeof(priorities) / sizeof(priorities[0]));
    const uint32_t created = create_workers(priorities, count);
    const uint32_t low_bit = priority_bit(SEM_WAKE_LOW_PRIORITY);
    const uint32_t high_bit = priority_bit(SEM_WAKE_HIGH_PRIORITY);
    const bool is_initialized = rtos_sem_init(&semaphore, 0U, 1U) == RTOS_OK;
    bool is_valid = is_initialized && (created == count);
    uint32_t elapsed;

    for (uint32_t index = 0U; is_valid && (index < count); ++index) {
        is_valid = enroll_worker(index, RTOS_WAIT_FOREVER);
    }
    RTOS_PORT_ENTER_CRITICAL();
    is_valid = is_valid && (semaphore.wait_bitmap == (high_bit | low_bit));
    if (is_valid) {
        /* Only this diagnostic corrupts bitmap metadata. Waiting tasks stay real. */
        semaphore.wait_bitmap = injected_bitmap;
    }
    RTOS_PORT_EXIT_CRITICAL();

    is_valid = is_valid && give_one(1U, injected_bitmap, &elapsed)
        && give_one(3U, injected_bitmap & ~high_bit, &elapsed);
    RTOS_PORT_ENTER_CRITICAL();
    if (is_valid) {
        semaphore.wait_bitmap = low_bit;
    }
    RTOS_PORT_EXIT_CRITICAL();
    is_valid = is_valid && give_one(0U, low_bit, &elapsed) && give_one(2U, 0U, &elapsed);

    const bool is_clean = !is_initialized || (rtos_sem_deinit(&semaphore) == RTOS_OK);
    return delete_workers(created) && is_clean && is_valid;
}

static bool test_timeout_bitmap(void)
{
    static const rtos_prio_t priorities[] = {
        SEM_WAKE_LOW_PRIORITY, SEM_WAKE_HIGH_PRIORITY,
        SEM_WAKE_HIGH_PRIORITY, SEM_WAKE_TIMEOUT_PRIORITY
    };
    const uint32_t count = (uint32_t)(sizeof(priorities) / sizeof(priorities[0]));
    const uint32_t created = create_workers(priorities, count);
    const uint32_t low_bit = priority_bit(SEM_WAKE_LOW_PRIORITY);
    const uint32_t high_bit = priority_bit(SEM_WAKE_HIGH_PRIORITY);
    const uint32_t timeout_bit = priority_bit(SEM_WAKE_TIMEOUT_PRIORITY);
    const bool is_initialized = rtos_sem_init(&semaphore, 0U, 1U) == RTOS_OK;
    bool is_valid = is_initialized && (created == count)
        && enroll_worker(0U, RTOS_WAIT_FOREVER)
        && enroll_worker(1U, SEM_WAKE_TIMEOUT_TICKS)
        && enroll_worker(2U, RTOS_WAIT_FOREVER)
        && enroll_worker(3U, SEM_WAKE_TIMEOUT_TICKS);
    uint32_t elapsed;

    RTOS_PORT_ENTER_CRITICAL();
    is_valid = is_valid && (semaphore.wait_bitmap == (low_bit | high_bit | timeout_bit));
    RTOS_PORT_EXIT_CRITICAL();
    if (is_valid) {
        rtos_task_delay(SEM_WAKE_TIMEOUT_TICKS + 1U);
        RTOS_PORT_ENTER_CRITICAL();
        is_valid = (semaphore.wait_bitmap == (low_bit | high_bit));
        RTOS_PORT_EXIT_CRITICAL();
        is_valid = is_valid && worker_returned(1U, RTOS_ERR_TIMEOUT)
            && worker_returned(3U, RTOS_ERR_TIMEOUT)
            && give_one(2U, low_bit, &elapsed) && give_one(0U, 0U, &elapsed);
    }

    const bool is_clean = !is_initialized || (rtos_sem_deinit(&semaphore) == RTOS_OK);
    return delete_workers(created) && is_clean && is_valid;
}

static void report_check(const char *name, bool is_valid)
{
    record_check(is_valid);
    (void)printf("SWB|%s|%s\r\n", name, is_valid ? "PASS" : "FAIL");
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    static const uint32_t counts[] = {1U, 2U, 8U, SEM_WAKE_MAX_WORKERS};
    const uint32_t valid_bitmap = priority_bit(SEM_WAKE_LOW_PRIORITY)
        | priority_bit(SEM_WAKE_HIGH_PRIORITY);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    (void)printf("SWB|META|clock_hz=%lu\r\n", (unsigned long)SystemCoreClock);

    for (size_t index = 0U; index < sizeof(counts) / sizeof(counts[0]); ++index) {
        measure_give(counts[index], true);
        if (counts[index] > 1U) {
            measure_give(counts[index], false);
        }
    }
    report_check("PRIORITY_FIFO", test_fifo(valid_bitmap));
    report_check("STALE_BITMAP_PRIORITY_FIFO",
                 test_fifo(valid_bitmap | priority_bit(SEM_WAKE_STALE_PRIORITY)));
    report_check("EMPTY_BITMAP_PRIORITY_FIFO", test_fifo(0U));
    report_check("TIMEOUT_BITMAP", test_timeout_bitmap());

    (void)printf("SWB|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)passed_checks,
                 (unsigned long)failed_checks);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

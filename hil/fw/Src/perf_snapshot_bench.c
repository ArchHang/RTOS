/**
 * @file perf_snapshot_bench.c
 * @brief Measure bounded task snapshots with and without hot-path statistics.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"
#include "rtos.h"

enum {
    SNAPSHOT_WORKERS = 14U,
    SNAPSHOT_STACK_WORDS = 160U,
    SNAPSHOT_ROWS = 16U,
    SNAPSHOT_SAMPLES = 256U
};

static rtos_tcb_t g_workers[SNAPSHOT_WORKERS];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t
    g_worker_stacks[SNAPSHOT_WORKERS][SNAPSHOT_STACK_WORDS];
static rtos_perf_task_t g_rows[SNAPSHOT_ROWS + 1U];
static uint32_t g_failures;

static void parked_task(void *argument)
{
    (void)argument;
    for (;;) {
        (void)rtos_task_notify_wait(RTOS_WAIT_FOREVER, NULL);
    }
}

static bool verify_snapshot(const rtos_perf_task_t *row, const rtos_tcb_t *task)
{
    return (task != NULL) && (row->task_id == task->task_id) &&
           (memcmp(row->name, task->name, sizeof(row->name)) == 0) &&
           (row->priority == task->priority) && (row->state == task->state) &&
           (row->cpu_usage_x100 <= 10000U) &&
           (row->stack_total_bytes == task->stack_size * sizeof(rtos_stack_t)) &&
           ((row->stack_used_bytes + row->stack_free_bytes) == row->stack_total_bytes);
}

static void measure_snapshot(uint32_t rows, bool is_single)
{
    uint64_t sum = 0U;
    uint32_t low = UINT32_MAX;
    uint32_t high = 0U;
    uint32_t count = 0U;
    rtos_tcb_t *const task = rtos_perf_task_at(SNAPSHOT_ROWS - 1U);
    bool passed = true;
    rtos_perf_task_t guard;

    memset(&guard, 0xa5, sizeof(guard));
    memcpy(&g_rows[rows], &guard, sizeof(guard));
    for (uint32_t index = 0U; index < SNAPSHOT_SAMPLES; ++index) {
        rtos_status_t status;
        const uint32_t mask = rtos_port_get_interrupt_mask();
        const uint32_t nesting = rtos_port_get_critical_nesting();
        const uint32_t start = DWT->CYCCNT;
        if (is_single) {
            status = rtos_perf_get_task(task, &g_rows[0]);
            count = 1U;
        } else {
            status = rtos_perf_get_all_tasks(g_rows, rows, &count);
        }
        const uint32_t cycles = DWT->CYCCNT - start;
        passed = (status == RTOS_OK) && (count == rows) &&
                 (rtos_port_get_interrupt_mask() == mask) &&
                 (rtos_port_get_critical_nesting() == nesting) && passed;
        sum += cycles;
        if (cycles < low) {
            low = cycles;
        }
        if (cycles > high) {
            high = cycles;
        }
    }

    for (uint32_t index = 0U; index < rows; ++index) {
        const rtos_tcb_t *expected = is_single ? task : rtos_perf_task_at(index);
        passed = verify_snapshot(&g_rows[index], expected) && passed;
#if !RTOS_CONFIG_PERF_HOTPATH_STATS
        passed = (g_rows[index].cpu_usage_x100 == 0U) &&
                 (g_rows[index].run_cycles == 0U) && passed;
#endif
    }
    passed = (memcmp(&g_rows[rows], &guard, sizeof(guard)) == 0) && passed;
    if (!passed) {
        ++g_failures;
    }
    (void)printf("PSB|SAMPLE|%s|rows=%lu|n=%lu|avg=%lu|min=%lu|max=%lu|%s\r\n",
                 is_single ? "single" : "batch", (unsigned long)rows,
                 (unsigned long)SNAPSHOT_SAMPLES, (unsigned long)(sum / SNAPSHOT_SAMPLES),
                 (unsigned long)low, (unsigned long)high, passed ? "PASS" : "FAIL");
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    uint32_t created = 0U;
    rtos_perf_task_t current;
    bool passed;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    (void)printf("PSB|BEGIN|clock=%lu|hot=%u\r\n", (unsigned long)SystemCoreClock,
                 (unsigned int)RTOS_CONFIG_PERF_HOTPATH_STATS);
    while (created < SNAPSHOT_WORKERS) {
        if (rtos_task_create_static(&g_workers[created], g_worker_stacks[created],
                                    SNAPSHOT_STACK_WORDS, parked_task, NULL, 20U,
                                    "snapshot") != RTOS_OK) {
            break;
        }
        ++created;
    }
    rtos_task_delay(3U);
    passed = (created == SNAPSHOT_WORKERS) &&
             (rtos_perf_task_registry_count() >= SNAPSHOT_ROWS) &&
             (rtos_perf_get_task(NULL, &current) == RTOS_OK) &&
             verify_snapshot(&current, rtos_task_get_current());
#if RTOS_CONFIG_PERF_HOTPATH_STATS
    passed = (current.run_cycles != 0U) && passed;
#endif
    if (passed) {
        measure_snapshot(1U, true);
        measure_snapshot(1U, false);
        measure_snapshot(8U, false);
        measure_snapshot(16U, false);
    } else {
        ++g_failures;
        (void)printf("PSB|setup|FAIL\r\n");
    }

    for (uint32_t index = 0U; index < created; ++index) {
        if (rtos_task_delete(&g_workers[index]) != RTOS_OK) {
            ++g_failures;
        }
    }
    (void)printf("PSB|DONE|fail=%lu\r\n", (unsigned long)g_failures);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

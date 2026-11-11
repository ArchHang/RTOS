/**
 * @file task_registry_bench.c
 * @brief Measure real task notification by registry position and verify task identity.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"

#include "rtos.h"

enum {
    TRB_MAX_TASKS = 32U,
    TRB_HASH_BUCKETS = 16U,
    TRB_STACK_WORDS = 128U,
    TRB_WORKER_PRIORITY = 20U,
    TRB_SAMPLES = 512U
};

typedef struct trb_measurement {
    uint64_t total;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t count;
} trb_measurement_t;

static rtos_tcb_t measured_tasks[TRB_MAX_TASKS + TRB_HASH_BUCKETS];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT)
    rtos_stack_t measured_stacks[TRB_MAX_TASKS][TRB_STACK_WORDS];
static uint32_t pass_count;
static uint32_t fail_count;

static void run_worker(void *argument)
{
    (void)argument;
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static rtos_status_t create_worker(uint32_t index, rtos_stack_t *stack)
{
    return rtos_task_create_static(&measured_tasks[index], stack, TRB_STACK_WORDS,
                                   run_worker, NULL, TRB_WORKER_PRIORITY, "registry");
}

static const char *record_result(bool is_valid)
{
    if (is_valid) {
        ++pass_count;
        return "PASS";
    }
    ++fail_count;
    return "FAIL";
}

static void report_check(const char *name, bool is_valid)
{
    (void)printf("TRB|CHECK|%s|%s\r\n", name, record_result(is_valid));
}

static void measure_notify(const char *name, uint32_t active, uint32_t target, bool is_sweep,
                           rtos_status_t expected)
{
    trb_measurement_t measurement = {.minimum = UINT32_MAX};
    while (measurement.count < TRB_SAMPLES) {
        const uint32_t index = is_sweep ? (measurement.count & (active - 1U)) : target;
        rtos_tcb_t *const task = &measured_tasks[index];
        const uint32_t value = 1U + measurement.count;
        const uint32_t previous_value = task->notify_value;
        const uint32_t start = DWT->CYCCNT;
        const rtos_status_t status = rtos_task_notify(task, value, RTOS_NOTIFY_VALUE);
        const uint32_t elapsed = DWT->CYCCNT - start;
        const uint32_t expected_value = (expected == RTOS_OK) ? value : previous_value;
        if ((status != expected) || (task->notify_value != expected_value) || (elapsed == 0U)) {
            break;
        }
        if (elapsed < measurement.minimum) {
            measurement.minimum = elapsed;
        }
        if (elapsed > measurement.maximum) {
            measurement.maximum = elapsed;
        }
        measurement.total += elapsed;
        ++measurement.count;
    }

    const char *const result = record_result(measurement.count == TRB_SAMPLES);
    const uint64_t average = measurement.count == 0U ? 0U : measurement.total / measurement.count;
    (void)printf("TRB|NOTIFY|active=%lu|path=%s|N=%lu|avg=%lu|min=%lu|max=%lu|%s\r\n",
                 (unsigned long)active, name, (unsigned long)measurement.count,
                 (unsigned long)average, (unsigned long)measurement.minimum,
                 (unsigned long)measurement.maximum, result);
}

static uint32_t expected_bucket(const rtos_tcb_t *task)
{
    /* Select actual address collisions even if the linker places each image differently. */
    const uintptr_t address = (uintptr_t)task;
    return (uint32_t)(((address >> 2U) ^ (address >> 6U) ^ (address >> 10U))
                      & (TRB_HASH_BUCKETS - 1U));
}

static void check_collisions(void)
{
    uint32_t counts[TRB_HASH_BUCKETS] = {0};
    uint32_t largest = 0U;
    for (uint32_t index = 0U; index < TRB_MAX_TASKS; ++index) {
        const uint32_t bucket = expected_bucket(&measured_tasks[index]);
        ++counts[bucket];
        if (counts[bucket] > counts[largest]) {
            largest = bucket;
        }
    }

    uint32_t first = 0U;
    while (expected_bucket(&measured_tasks[first]) != largest) {
        ++first;
    }
    uint32_t second = first + 1U;
    while (expected_bucket(&measured_tasks[second]) != largest) {
        ++second;
    }
    uint32_t unused = TRB_MAX_TASKS;
    while ((unused < (TRB_MAX_TASKS + TRB_HASH_BUCKETS)) &&
           (counts[expected_bucket(&measured_tasks[unused])] == 0U)) {
        ++unused;
    }
    if (unused == (TRB_MAX_TASKS + TRB_HASH_BUCKETS)) {
        report_check("same_bucket_miss", false);
        return;
    }

    uint32_t used = 0U;
    for (uint32_t bucket = 0U; bucket < TRB_HASH_BUCKETS; ++bucket) {
        if (counts[bucket] != 0U) {
            ++used;
        }
    }
    (void)printf("TRB|HASH|used=%lu|max_depth=%lu|collision_a=%lu|collision_b=%lu|miss=%lu\r\n",
                 (unsigned long)used, (unsigned long)counts[largest], (unsigned long)first,
                 (unsigned long)second, (unsigned long)unused);
    const rtos_status_t first_status =
        rtos_task_notify(&measured_tasks[first], 0x12345678U, RTOS_NOTIFY_VALUE);
    const rtos_status_t second_status =
        rtos_task_notify(&measured_tasks[second], 0x87654321U, RTOS_NOTIFY_VALUE);
    report_check("collision_identity", (first_status == RTOS_OK) && (second_status == RTOS_OK) &&
                 (measured_tasks[first].notify_value == 0x12345678U) &&
                 (measured_tasks[second].notify_value == 0x87654321U));
    measure_notify("collision_oldest", TRB_MAX_TASKS, first, false, RTOS_OK);
    measure_notify("same_bucket_miss", TRB_MAX_TASKS, unused, false, RTOS_ERR_PARAM);
}

static void run_case(uint32_t active)
{
    uint32_t created = 0U;
    while ((created < active) && (create_worker(created, measured_stacks[created]) == RTOS_OK)) {
        ++created;
    }

    report_check("setup", created == active);
    if (created == active) {
        measure_notify("newest", active, active - 1U, false, RTOS_OK);
        measure_notify("oldest", active, 0U, false, RTOS_OK);
        measure_notify("sweep", active, 0U, true, RTOS_OK);
        if (active == TRB_MAX_TASKS) {
            check_collisions();
            report_check("duplicate_reservation",
                         create_worker(0U, measured_stacks[0]) == RTOS_ERR_ALREADY);
            const uint32_t first_bucket = expected_bucket(&measured_tasks[0]);
            uint32_t other = active;
            while ((other < (TRB_MAX_TASKS + TRB_HASH_BUCKETS)) &&
                   (expected_bucket(&measured_tasks[other]) == first_bucket)) {
                ++other;
            }
            report_check("cross_bucket_stack_overlap", (other < (TRB_MAX_TASKS + TRB_HASH_BUCKETS))
                         && (create_worker(other, measured_stacks[0]) == RTOS_ERR_PARAM));
            rtos_tcb_t *const invalid = (rtos_tcb_t *)(uintptr_t)1U;
            report_check("arbitrary_address",
                         rtos_task_notify(invalid, 0U, RTOS_NOTIFY_VALUE) == RTOS_ERR_PARAM);
        }
    }

    bool is_clean = true;
    while (created != 0U) {
        --created;
        is_clean = (rtos_task_delete(&measured_tasks[created]) == RTOS_OK) && is_clean;
    }
    report_check("cleanup", is_clean);
}

static void check_reuse(void)
{
    report_check("deleted_address",
                 rtos_task_notify(&measured_tasks[0], 7U, RTOS_NOTIFY_VALUE) == RTOS_ERR_PARAM);
    const bool created = create_worker(0U, measured_stacks[0]) == RTOS_OK;
    const bool accepted = created &&
        (rtos_task_notify(&measured_tasks[0], 0x11223344U, RTOS_NOTIFY_VALUE) == RTOS_OK) &&
        (measured_tasks[0].notify_value == 0x11223344U);
    const bool deleted = created && (rtos_task_delete(&measured_tasks[0]) == RTOS_OK);
    report_check("same_address_recreated", accepted && deleted);
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    run_case(1U);
    run_case(8U);
    run_case(16U);
    run_case(32U);
    check_reuse();
    (void)printf("TRB|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)pass_count,
                 (unsigned long)fail_count);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

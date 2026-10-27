/**
 * @file diagnostic_bench.c
 * @brief Equal-work diagnostic ON/OFF measurements using the real scheduler.
 *
 * Six paths each execute exactly 1024 operations in three samples. Ping-pong
 * workers have higher priority than the controller and must block again before
 * the controller resumes. Timing includes kernel calls, two context switches per
 * operation, SysTick interference and the loop; it is not a WCET measurement.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"

#include "rtos.h"

#define DG_BENCH_OPERATIONS (1024U)
#define DG_BENCH_ROUNDS (3U)
#define DG_BENCH_STACK_WORDS (256U)
#define DG_BENCH_CHECKSUM (524800U)

typedef enum dg_bench_path {
    DG_BENCH_SEM = 0,
    DG_BENCH_QUEUE,
    DG_BENCH_NOTIFY
} dg_bench_path_t;

typedef struct dg_bench_result {
    uint32_t cycles;
    uint32_t ticks;
    uint32_t checksum;
    uint32_t errors;
} dg_bench_result_t;

static rtos_sem_t request_sem;
static rtos_sem_t response_sem;
static rtos_queue_t request_queue;
static rtos_queue_t response_queue;
static uint32_t request_storage;
static uint32_t response_storage;
static rtos_tcb_t worker;
static rtos_stack_t worker_stack[DG_BENCH_STACK_WORDS] __attribute__((aligned(8)));
static rtos_tcb_t *controller;
static dg_bench_path_t worker_path;
static volatile uint32_t worker_errors;
static volatile uint32_t worker_checksum;
static volatile uint32_t worker_finished;
static uint32_t pass_count;
static uint32_t fail_count;

static void dg_expect(const char *name, uint32_t condition)
{
    if (condition != 0U) {
        pass_count++;
    } else {
        fail_count++;
    }
    (void)printf("DG|CHECK|%s|id=%lu name=%s\r\n",
        condition != 0U ? "PASS" : "FAIL",
        (unsigned long)(pass_count + fail_count), name);
}

static rtos_status_t worker_receive(uint32_t *value)
{
    switch (worker_path) {
        case DG_BENCH_SEM:
            return rtos_sem_take(&request_sem, RTOS_WAIT_FOREVER);
        case DG_BENCH_QUEUE:
            return rtos_queue_recv(&request_queue, value, RTOS_WAIT_FOREVER);
        case DG_BENCH_NOTIFY:
            return rtos_task_notify_wait(RTOS_WAIT_FOREVER, value);
        default:
            return RTOS_ERR_PARAM;
    }
}

static rtos_status_t worker_reply(uint32_t value)
{
    switch (worker_path) {
        case DG_BENCH_SEM:
            return rtos_sem_give(&response_sem);
        case DG_BENCH_QUEUE:
            return rtos_queue_send(&response_queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
        case DG_BENCH_NOTIFY:
            return rtos_task_notify(controller, value, RTOS_NOTIFY_VALUE);
        default:
            return RTOS_ERR_PARAM;
    }
}

static void worker_entry(void *argument)
{
    (void)argument;
    for (uint32_t index = 1U; index <= DG_BENCH_OPERATIONS; index++) {
        uint32_t value = index;
        if (worker_receive(&value) != RTOS_OK || value != index) {
            worker_errors++;
        }
        worker_checksum += value;
        if (worker_reply(value) != RTOS_OK) {
            worker_errors++;
        }
    }
    worker_finished = 1U;
    (void)rtos_task_suspend(NULL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static uint32_t exchange(dg_bench_path_t path, uint32_t value, uint32_t pingpong)
{
    uint32_t response = value;
    rtos_status_t sent = RTOS_ERR_PARAM;
    rtos_status_t received = RTOS_ERR_PARAM;

    switch (path) {
        case DG_BENCH_SEM:
            sent = rtos_sem_give(&request_sem);
            received = rtos_sem_take(pingpong != 0U ? &response_sem : &request_sem,
                RTOS_NO_WAIT);
            break;
        case DG_BENCH_QUEUE:
            sent = rtos_queue_send(&request_queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
            received = rtos_queue_recv(pingpong != 0U ? &response_queue : &request_queue,
                &response, RTOS_NO_WAIT);
            break;
        case DG_BENCH_NOTIFY:
            sent = rtos_task_notify(pingpong != 0U ? &worker : controller, value,
                RTOS_NOTIFY_VALUE);
            received = rtos_task_notify_wait(RTOS_NO_WAIT, &response);
            break;
        default:
            break;
    }
    return sent == RTOS_OK && received == RTOS_OK && response == value;
}

static uint32_t prepare(dg_bench_path_t path, uint32_t pingpong)
{
    worker_errors = 0U;
    worker_checksum = 0U;
    worker_finished = 0U;
    worker_path = path;
    uint32_t valid = rtos_sem_init(&request_sem, 0U, 1U) == RTOS_OK;
    valid &= rtos_sem_init(&response_sem, 0U, 1U) == RTOS_OK;
    valid &= rtos_queue_init(&request_queue, &request_storage,
        sizeof(request_storage), 1U) == RTOS_OK;
    valid &= rtos_queue_init(&response_queue, &response_storage,
        sizeof(response_storage), 1U) == RTOS_OK;
    if (valid != 0U && pingpong != 0U) {
        valid = rtos_task_create_static(&worker, worker_stack, DG_BENCH_STACK_WORDS,
            worker_entry, NULL, 8U, "diag-bench") == RTOS_OK;
    }
    return valid;
}

static void cleanup(uint32_t pingpong)
{
    uint32_t valid = 1U;

    if (pingpong != 0U) {
        valid = rtos_task_delete(&worker) == RTOS_OK;
    }
    valid &= rtos_queue_deinit(&response_queue) == RTOS_OK;
    valid &= rtos_queue_deinit(&request_queue) == RTOS_OK;
    valid &= rtos_sem_deinit(&response_sem) == RTOS_OK;
    valid &= rtos_sem_deinit(&request_sem) == RTOS_OK;
    dg_expect("bench_cleanup", valid);
}

static dg_bench_result_t measure(dg_bench_path_t path, uint32_t pingpong)
{
    dg_bench_result_t result = {0U, 0U, 0U, 0U};
    rtos_tick_t start_tick = rtos_sched_get_tick_count();
    __DSB();
    __ISB();
    uint32_t start_cycles = DWT->CYCCNT;

    for (uint32_t index = 1U; index <= DG_BENCH_OPERATIONS; index++) {
        if (exchange(path, index, pingpong) == 0U) {
            result.errors++;
        }
        result.checksum += index;
    }
    /* Unsigned differences intentionally measure the wrapping hardware counters. */
    result.cycles = DWT->CYCCNT - start_cycles;
    result.ticks = rtos_sched_get_tick_count() - start_tick;
    return result;
}

static void run_sample(dg_bench_path_t path, uint32_t pingpong, uint32_t round)
{
    static const char *const names[2][3] = {
        {"sem_nowait", "queue_nowait", "notify_nowait"},
        {"sem_pingpong", "queue_pingpong", "notify_pingpong"}
    };
    uint32_t prepared = prepare(path, pingpong);

    dg_expect("bench_prepared", prepared);
    if (prepared == 0U) {
        return;
    }
    dg_bench_result_t result = measure(path, pingpong);
    dg_expect("bench_exact_work", result.errors == 0U
        && result.checksum == DG_BENCH_CHECKSUM && result.cycles > 0U);
    dg_expect("bench_worker_accounted", pingpong == 0U
        || (worker_finished == 1U && worker_errors == 0U
            && worker_checksum == DG_BENCH_CHECKSUM && worker.state == RTOS_TASK_SUSPENDED));
    (void)printf("DG|BENCH|name=%s round=%lu operations=%lu cycles=%lu ticks=%lu "
        "checksum=%lu errors=%lu\r\n", names[pingpong][path], (unsigned long)round,
        (unsigned long)DG_BENCH_OPERATIONS, (unsigned long)result.cycles,
        (unsigned long)result.ticks, (unsigned long)result.checksum,
        (unsigned long)(result.errors + worker_errors));
    cleanup(pingpong);
}

/** @brief Emit the fixed operation counts before the scheduler starts. */
void audit_pre_start(void)
{
    (void)printf("DG|META|mode=bench diagnostics=%u capacity=%u operations=%u rounds=%u\r\n",
        (unsigned int)RTOS_CONFIG_USE_DIAGNOSTICS,
        (unsigned int)RTOS_CONFIG_DIAGNOSTIC_CAPACITY,
        (unsigned int)DG_BENCH_OPERATIONS, (unsigned int)DG_BENCH_ROUNDS);
}

/** @brief Measure each equal-work path and retain the board for inspection.
 *  @param argument [in] Unused audit task argument.
 */
void audit_regressions_task(void *argument)
{
    (void)argument;
    controller = rtos_task_get_current();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    for (uint32_t round = 0U; round < DG_BENCH_ROUNDS; round++) {
        for (uint32_t pingpong = 0U; pingpong < 2U; pingpong++) {
            for (uint32_t path = 0U; path < 3U; path++) {
                run_sample((dg_bench_path_t)path, pingpong, round);
            }
        }
    }
    (void)printf("DG|SUMMARY|mode=bench pass=%lu fail=%lu\r\n",
        (unsigned long)pass_count, (unsigned long)fail_count);
    (void)printf("DG|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

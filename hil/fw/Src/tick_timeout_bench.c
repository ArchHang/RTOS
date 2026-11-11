/**
 * @file tick_timeout_bench.c
 * @brief Measure real SysTick processing when object waits expire together.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx.h"

#include "adversarial_registry.h"
#include "rtos.h"

enum {
    TTB_MAX_WAITERS = 16U,
    TTB_STACK_WORDS = 192U,
    TTB_WORKER_PRIORITY = 6U,
    TTB_LEAD_TICKS = 4U,
    TTB_SAMPLES = 48U,
    TTB_SYSTICK_EXCEPTION = 15U
};

typedef enum tick_bench_wait_kind {
    TTB_WAIT_SEM = 0,
    TTB_WAIT_QUEUE
} tick_bench_wait_kind_t;

typedef struct tick_bench_worker {
    rtos_tcb_t tcb;
    rtos_tick_t done_tick;
    rtos_status_t result;
} tick_bench_worker_t;

typedef struct tick_bench_capture {
    rtos_tick_t deadline;
    uint32_t cycles;
    bool is_armed;
    bool is_valid;
} tick_bench_capture_t;

static tick_bench_worker_t workers[TTB_MAX_WAITERS];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t
    worker_stacks[TTB_MAX_WAITERS][TTB_STACK_WORDS];
static rtos_mutex_t unrelated_mutexes[RTOS_CONFIG_MAX_MUTEXES];
static rtos_sem_t bench_sem;
static rtos_queue_t bench_queue;
static uint32_t queue_storage;
static tick_bench_wait_kind_t wait_kind;
static tick_bench_capture_t capture;
static uint32_t pass_count;
static uint32_t fail_count;

void __real_rtos_sched_tick(void);
void __wrap_rtos_sched_tick(void);

void __wrap_rtos_sched_tick(void)
{
    /* The port already holds the same critical section as an ordinary Tick. */
    if (!capture.is_armed || ((rtos_kernel.tick_count + 1U) != capture.deadline)) {
        __real_rtos_sched_tick();
        return;
    }

    const uint32_t nesting = rtos_port_get_critical_nesting();
    const uint32_t mask = rtos_port_get_interrupt_mask();
    const bool is_systick = (__get_IPSR() == TTB_SYSTICK_EXCEPTION);
    const uint32_t start = DWT->CYCCNT;
    __real_rtos_sched_tick();
    const uint32_t elapsed = DWT->CYCCNT - start;

    capture.cycles = elapsed;
    capture.is_valid = is_systick && (elapsed != 0U) && (nesting != 0U) && (mask != 0U)
        && (rtos_port_get_critical_nesting() == nesting)
        && (rtos_port_get_interrupt_mask() == mask);
    capture.is_armed = false;
}

static void run_waiter(void *argument)
{
    tick_bench_worker_t *const worker = (tick_bench_worker_t *)argument;
    const uint32_t guard = 0xDEADBEEFU;

    for (;;) {
        uint32_t deadline = 0U;
        uint32_t item = guard;
        rtos_status_t result = rtos_task_notify_wait(RTOS_WAIT_FOREVER, &deadline);
        if (result == RTOS_OK) {
            RTOS_PORT_ENTER_CRITICAL();
            const tick_bench_wait_kind_t kind = wait_kind;
            const rtos_tick_t remaining = deadline - rtos_kernel.tick_count;
            RTOS_PORT_EXIT_CRITICAL();
            if ((remaining != 0U) && (remaining <= TTB_LEAD_TICKS)) {
                if (kind == TTB_WAIT_SEM) {
                    result = rtos_sem_take(&bench_sem, remaining);
                } else {
                    result = rtos_queue_recv(&bench_queue, &item, remaining);
                }
            } else {
                result = RTOS_ERR_PARAM;
            }
        }
        RTOS_PORT_ENTER_CRITICAL();
        worker->done_tick = rtos_kernel.tick_count;
        worker->result = (item == guard) ? result : RTOS_ERR_PARAM;
        RTOS_PORT_EXIT_CRITICAL();
    }
}

static bool collect_sample(tick_bench_wait_kind_t kind, uint32_t count, uint32_t *cycles)
{
    /* Enrollment starts just after a real Tick. Exact wake ticks are checked
     * before arming the probe, so a late enrollment cannot alter the sample. */
    rtos_task_delay(1U);
    RTOS_PORT_ENTER_CRITICAL();
    const rtos_tick_t deadline = rtos_kernel.tick_count + TTB_LEAD_TICKS;
    wait_kind = kind;
    capture.deadline = deadline;
    capture.is_valid = false;
    capture.is_armed = false;
    for (uint32_t index = 0U; index < count; ++index) {
        workers[index].result = RTOS_ERR_PARAM;
    }
    RTOS_PORT_EXIT_CRITICAL();

    bool is_valid = true;
    for (uint32_t index = 0U; index < count; ++index) {
        if (rtos_task_notify(&workers[index].tcb, deadline, RTOS_NOTIFY_VALUE) != RTOS_OK) {
            is_valid = false;
        }
    }

    RTOS_PORT_ENTER_CRITICAL();
    const rtos_tick_t remaining = deadline - rtos_kernel.tick_count;
    void *const object = (kind == TTB_WAIT_SEM) ? (void *)&bench_sem : (void *)&bench_queue;
    rtos_wait_node_t **const head = (kind == TTB_WAIT_SEM)
        ? &bench_sem.wait_head : &bench_queue.recv_wait_head;
    for (uint32_t index = 0U; index < count; ++index) {
        const rtos_tcb_t *const task = &workers[index].tcb;
        if ((task->state != RTOS_TASK_DELAYED) || (task->wake_tick != deadline)
            || (task->blocked_on != object) || (task->wait_node.list_head != head)) {
            is_valid = false;
        }
    }
    capture.is_armed = is_valid && (remaining != 0U) && (remaining <= TTB_LEAD_TICKS);
    is_valid = capture.is_armed;
    RTOS_PORT_EXIT_CRITICAL();

    /* The coordinator expires one Tick after the sample, outside its window. */
    rtos_task_delay(is_valid ? remaining + 1U : TTB_LEAD_TICKS + 1U);

    RTOS_PORT_ENTER_CRITICAL();
    capture.is_armed = false;
    is_valid = is_valid && capture.is_valid && (*head == NULL)
        && (bench_sem.wait_bitmap == 0U) && (bench_sem.count == 0U)
        && (bench_queue.count == 0U);
    for (uint32_t index = 0U; index < count; ++index) {
        if ((workers[index].result != RTOS_ERR_TIMEOUT) || (workers[index].done_tick != deadline)) {
            is_valid = false;
        }
    }
    *cycles = capture.cycles;
    RTOS_PORT_EXIT_CRITICAL();
    return is_valid;
}

static void measure_case(tick_bench_wait_kind_t kind, uint32_t waiters, uint32_t mutexes)
{
    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0U;
    uint64_t sum = 0U;
    uint32_t samples = 0U;

    for (; samples < TTB_SAMPLES; ++samples) {
        uint32_t cycles;
        if (!collect_sample(kind, waiters, &cycles)) {
            break;
        }
        if (cycles < minimum) {
            minimum = cycles;
        }
        if (cycles > maximum) {
            maximum = cycles;
        }
        sum += cycles;
    }

    const bool is_valid = (samples == TTB_SAMPLES);
    if (is_valid) {
        ++pass_count;
    } else {
        ++fail_count;
    }
    (void)printf("TTB|CASE|%s|kind=%s|mutexes=%lu|waiters=%lu|n=%lu|avg=%lu|min=%lu|max=%lu\r\n",
                 is_valid ? "PASS" : "FAIL", (kind == TTB_WAIT_SEM) ? "sem" : "queue",
                 (unsigned long)mutexes, (unsigned long)waiters, (unsigned long)samples,
                 (unsigned long)((samples != 0U) ? (sum / samples) : 0U),
                 (unsigned long)((samples != 0U) ? minimum : 0U), (unsigned long)maximum);
}

static bool check_registry_count(uint32_t expected)
{
    RTOS_PORT_ENTER_CRITICAL();
    const bool is_expected = (rtos_internal_mutex_registry_count() == expected);
    RTOS_PORT_EXIT_CRITICAL();
    return is_expected;
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    static const uint32_t batch_sizes[] = {1U, 4U, 8U, TTB_MAX_WAITERS};

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    (void)printf("TTB|BEGIN|clock_hz=%lu|tick_hz=%lu\r\n", (unsigned long)SystemCoreClock,
                 (unsigned long)RTOS_CONFIG_TICK_RATE_HZ);

    const bool is_sem_created = (rtos_sem_init(&bench_sem, 0U, 1U) == RTOS_OK);
    const bool is_queue_created = (rtos_queue_init(&bench_queue, &queue_storage,
                                                  sizeof(queue_storage), 1U) == RTOS_OK);
    bool is_valid = is_sem_created && is_queue_created && check_registry_count(0U);
    uint32_t worker_count = 0U;
    while (is_valid && (worker_count < TTB_MAX_WAITERS)) {
        tick_bench_worker_t *const worker = &workers[worker_count];
        is_valid = rtos_task_create_static(&worker->tcb, worker_stacks[worker_count],
                                           TTB_STACK_WORDS, run_waiter, worker,
                                           TTB_WORKER_PRIORITY, "tick-bench") == RTOS_OK;
        if (is_valid) {
            ++worker_count;
        }
    }

    uint32_t mutex_count = 0U;
    for (uint32_t group = 0U; is_valid && (group < 2U); ++group) {
        const uint32_t target = (group == 0U) ? 0U : RTOS_CONFIG_MAX_MUTEXES;
        while (is_valid && (mutex_count < target)) {
            is_valid = rtos_mutex_init(&unrelated_mutexes[mutex_count]) == RTOS_OK;
            if (is_valid) {
                ++mutex_count;
            }
        }
        is_valid = is_valid && check_registry_count(target);
        const uint32_t batch_count = sizeof(batch_sizes) / sizeof(batch_sizes[0]);
        for (uint32_t index = 0U; is_valid && (index < batch_count); ++index) {
            measure_case(TTB_WAIT_SEM, batch_sizes[index], target);
            measure_case(TTB_WAIT_QUEUE, batch_sizes[index], target);
        }
    }
    if (!is_valid) {
        ++fail_count;
        (void)printf("TTB|setup|FAIL|workers=%lu|mutexes=%lu\r\n", (unsigned long)worker_count,
                     (unsigned long)mutex_count);
    }

    bool is_clean = true;
    for (uint32_t index = 0U; index < worker_count; ++index) {
        is_clean = (rtos_task_delete(&workers[index].tcb) == RTOS_OK) && is_clean;
    }
    for (uint32_t index = 0U; index < mutex_count; ++index) {
        is_clean = (rtos_mutex_deinit(&unrelated_mutexes[index]) == RTOS_OK) && is_clean;
    }
    if (is_queue_created) {
        is_clean = (rtos_queue_deinit(&bench_queue) == RTOS_OK) && is_clean;
    }
    if (is_sem_created) {
        is_clean = (rtos_sem_deinit(&bench_sem) == RTOS_OK) && is_clean;
    }
    if (!is_clean) {
        ++fail_count;
        (void)printf("TTB|cleanup|FAIL\r\n");
    }
    (void)printf("TTB|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)pass_count,
                 (unsigned long)fail_count);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

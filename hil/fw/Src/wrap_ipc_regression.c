/** @file wrap_ipc_regression.c
 *  @brief Exercise simultaneous object waits across the 32-bit SysTick rollover.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"

#include "rtos.h"

enum {
    WRAP_ROUNDS = 16U,
    WRAP_STACK_WORDS = 192U,
    WRAP_WORKER_PRIORITY = 6U,
    WRAP_START_LEAD = 32U,
    WRAP_FINISH_TICK = 8U,
    WRAP_SYSTICK_EXCEPTION = 15U,
    WRAP_EXTERNAL_EXCEPTION_OFFSET = 16U,
    WRAP_EVENT_KEEP = 1U << 0,
    WRAP_EVENT_TIMEOUT = 1U << 1,
    WRAP_EVENT_SIGNAL = 1U << 2
};

#define WRAP_GUARD UINT32_C(0x46A5ED90)
#define WRAP_QUEUE_ORIGINAL UINT32_C(0xF11E0000)
#define WRAP_QUEUE_SEND_VALUE UINT32_C(0x5E0D0000)
#define WRAP_QUEUE_RECV_VALUE UINT32_C(0x1A2C0000)
#define WRAP_NOTIFY_VALUE UINT32_C(0x60710000)
#define WRAP_NOTIFY_LATE_VALUE UINT32_C(0x1A7E0000)

typedef enum wrap_case_id {
    WRAP_SEM_BEFORE = 0,
    WRAP_SEM_ZERO,
    WRAP_SEM_IRQ,
    WRAP_QUEUE_RECV_ZERO,
    WRAP_QUEUE_RECV_IRQ,
    WRAP_QUEUE_SEND_ZERO,
    WRAP_QUEUE_SEND_IRQ,
    WRAP_EVENT_ZERO,
    WRAP_EVENT_IRQ,
    WRAP_NOTIFY_ZERO,
    WRAP_NOTIFY_IRQ,
    WRAP_MUTEX_AFTER,
    WRAP_CASE_COUNT
} wrap_case_id_t;

typedef struct wrap_case {
    const char *name;
    rtos_tick_t deadline;
    rtos_status_t expected_status;
} wrap_case_t;

typedef struct wrap_worker {
    rtos_tcb_t task;
    rtos_tick_t done_tick;
    uint32_t value;
    rtos_status_t status;
    rtos_status_t late_status;
} wrap_worker_t;

typedef struct wrap_trace {
    rtos_tick_t irq_tick;
    uint32_t systick_count;
    uint32_t irq_count;
    bool is_armed;
    bool is_systick_valid;
    bool is_irq_valid;
} wrap_trace_t;

static const wrap_case_t cases[WRAP_CASE_COUNT] = {
    [WRAP_SEM_BEFORE] = {"sem_before", UINT32_MAX, RTOS_ERR_TIMEOUT},
    [WRAP_SEM_ZERO] = {"sem_zero", 0U, RTOS_ERR_TIMEOUT},
    [WRAP_SEM_IRQ] = {"sem_irq", 3U, RTOS_OK},
    [WRAP_QUEUE_RECV_ZERO] = {"queue_recv_zero", 0U, RTOS_ERR_TIMEOUT},
    [WRAP_QUEUE_RECV_IRQ] = {"queue_recv_irq", 2U, RTOS_OK},
    [WRAP_QUEUE_SEND_ZERO] = {"queue_send_zero", 0U, RTOS_ERR_TIMEOUT},
    [WRAP_QUEUE_SEND_IRQ] = {"queue_send_irq", 4U, RTOS_OK},
    [WRAP_EVENT_ZERO] = {"event_zero", 0U, RTOS_ERR_TIMEOUT},
    [WRAP_EVENT_IRQ] = {"event_irq", 3U, RTOS_OK},
    [WRAP_NOTIFY_ZERO] = {"notify_zero", 0U, RTOS_ERR_TIMEOUT},
    [WRAP_NOTIFY_IRQ] = {"notify_irq", 3U, RTOS_OK},
    [WRAP_MUTEX_AFTER] = {"mutex_after", 1U, RTOS_ERR_TIMEOUT}
};

static wrap_worker_t workers[WRAP_CASE_COUNT];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t stacks[WRAP_CASE_COUNT][WRAP_STACK_WORDS];
static rtos_sem_t sem;
static rtos_queue_t recv_queue;
static rtos_queue_t send_queue;
static rtos_event_t event;
static rtos_mutex_t mutex;
static uint32_t recv_storage;
static uint32_t send_storage;
static uint32_t round_number;
static wrap_trace_t trace;

void __real_rtos_sched_tick(void);
void __wrap_rtos_sched_tick(void);
void EXTI1_IRQHandler(void);

void __wrap_rtos_sched_tick(void)
{
    const rtos_tick_t before = rtos_kernel.tick_count;
    __real_rtos_sched_tick();

    if (trace.is_armed && ((before >= (UINT32_MAX - 1U)) || (before == 0U))) {
        ++trace.systick_count;
        trace.is_systick_valid = trace.is_systick_valid && (__get_IPSR() == WRAP_SYSTICK_EXCEPTION)
            && (rtos_kernel.tick_count == before + 1U);
        if (before == UINT32_MAX) {
            /* Expire zero-deadline waiters first, then request a real syscall-domain IRQ. */
            NVIC_SetPendingIRQ(EXTI1_IRQn);
        }
    }
}

void EXTI1_IRQHandler(void)
{
    uint32_t original = WRAP_GUARD;
    ++trace.irq_count;
    trace.irq_tick = rtos_kernel.tick_count;
    const rtos_status_t sem_status = rtos_sem_give(&sem);
    const uint32_t value = WRAP_QUEUE_RECV_VALUE | round_number;
    const rtos_status_t send_status = rtos_queue_send(&recv_queue, &value, RTOS_NO_WAIT,
                                                     RTOS_FALSE);
    const rtos_status_t recv_status = rtos_queue_recv(&send_queue, &original, RTOS_NO_WAIT);
    const rtos_event_bits_t bits = rtos_event_set(&event, WRAP_EVENT_SIGNAL);
    const rtos_status_t notify_status = rtos_task_notify(&workers[WRAP_NOTIFY_IRQ].task,
        WRAP_NOTIFY_VALUE | round_number, RTOS_NOTIFY_VALUE);
    const rtos_status_t late_status = rtos_task_notify(&workers[WRAP_NOTIFY_ZERO].task,
        WRAP_NOTIFY_LATE_VALUE | round_number, RTOS_NOTIFY_VALUE);
    trace.is_irq_valid = trace.is_armed && (trace.irq_tick == 0U)
        && (__get_IPSR() == WRAP_EXTERNAL_EXCEPTION_OFFSET + (uint32_t)EXTI1_IRQn)
        && (sem_status == RTOS_OK) && (send_status == RTOS_OK) && (recv_status == RTOS_OK)
        && (original == (WRAP_QUEUE_ORIGINAL | round_number)) && (bits == WRAP_EVENT_KEEP)
        && (notify_status == RTOS_OK) && (late_status == RTOS_OK);
}

static void run_waiter(void *argument)
{
    wrap_worker_t *const worker = (wrap_worker_t *)argument;
    const wrap_case_id_t id = (wrap_case_id_t)(worker - workers);
    const rtos_tick_t remaining = cases[id].deadline - rtos_kernel.tick_count;
    rtos_status_t status = RTOS_ERR_PARAM;
    rtos_status_t late_status = RTOS_ERR_PARAM;
    uint32_t value = WRAP_GUARD;

    if ((remaining != 0U) && (remaining < WRAP_START_LEAD + WRAP_FINISH_TICK)) {
        switch (id) {
            case WRAP_SEM_BEFORE:
            case WRAP_SEM_ZERO:
            case WRAP_SEM_IRQ:
                status = rtos_sem_take(&sem, remaining);
                break;
            case WRAP_QUEUE_RECV_ZERO:
            case WRAP_QUEUE_RECV_IRQ:
                status = rtos_queue_recv(&recv_queue, &value, remaining);
                break;
            case WRAP_QUEUE_SEND_ZERO:
            case WRAP_QUEUE_SEND_IRQ:
                value = WRAP_QUEUE_SEND_VALUE | round_number;
                if (id == WRAP_QUEUE_SEND_ZERO) {
                    value ^= WRAP_GUARD;
                }
                status = rtos_queue_send(&send_queue, &value, remaining, RTOS_FALSE);
                break;
            case WRAP_EVENT_ZERO:
            case WRAP_EVENT_IRQ: {
                const rtos_event_bits_t mask = (id == WRAP_EVENT_ZERO)
                    ? WRAP_EVENT_TIMEOUT : WRAP_EVENT_SIGNAL;
                value = rtos_event_wait(&event, mask,
                                        RTOS_EVENT_WAIT_ALL | RTOS_EVENT_CLEAR_ON_EXIT, remaining);
                status = worker->task.wait_result;
                break;
            }
            case WRAP_NOTIFY_ZERO:
            case WRAP_NOTIFY_IRQ:
                status = rtos_task_notify_wait(remaining, &value);
                if (id == WRAP_NOTIFY_ZERO) {
                    value = WRAP_GUARD;
                    late_status = rtos_task_notify_wait(RTOS_NO_WAIT, &value);
                }
                break;
            case WRAP_MUTEX_AFTER:
                status = rtos_mutex_take(&mutex, remaining);
                break;
            default:
                break;
        }
    }

    RTOS_PORT_ENTER_CRITICAL();
    worker->done_tick = rtos_kernel.tick_count;
    worker->value = value;
    worker->status = status;
    worker->late_status = late_status;
    RTOS_PORT_EXIT_CRITICAL();

    /* Keep the TCB and its captured wait state available until the coordinator checks it. */
    if (rtos_task_suspend(NULL) != RTOS_OK) {
        for (;;) {
            rtos_task_delay(1U);
        }
    }
}

static uint32_t expected_value(wrap_case_id_t id)
{
    switch (id) {
        case WRAP_QUEUE_RECV_IRQ:
            return WRAP_QUEUE_RECV_VALUE | round_number;
        case WRAP_QUEUE_SEND_ZERO:
            return (WRAP_QUEUE_SEND_VALUE | round_number) ^ WRAP_GUARD;
        case WRAP_QUEUE_SEND_IRQ:
            return WRAP_QUEUE_SEND_VALUE | round_number;
        case WRAP_EVENT_ZERO:
            return WRAP_EVENT_KEEP;
        case WRAP_EVENT_IRQ:
            return WRAP_EVENT_KEEP | WRAP_EVENT_SIGNAL;
        case WRAP_NOTIFY_ZERO:
            return WRAP_NOTIFY_LATE_VALUE | round_number;
        case WRAP_NOTIFY_IRQ:
            return WRAP_NOTIFY_VALUE | round_number;
        default:
            return WRAP_GUARD;
    }
}

static bool check_waiter(wrap_case_id_t id)
{
    const wrap_worker_t *const worker = &workers[id];
    const wrap_case_t *const test_case = &cases[id];
    const bool has_timed_out = (test_case->expected_status == RTOS_ERR_TIMEOUT);
    const rtos_tick_t expected_tick = has_timed_out ? test_case->deadline : 0U;

    RTOS_PORT_ENTER_CRITICAL();
    const rtos_status_t status = worker->status;
    const rtos_status_t wait_result = worker->task.wait_result;
    const rtos_status_t late_status = worker->late_status;
    const rtos_tick_t done_tick = worker->done_tick;
    const uint32_t value = worker->value;
    const bool is_valid = (status == test_case->expected_status)
        && (wait_result == test_case->expected_status)
        && ((worker->task.wait_node.timed_out != 0U) == has_timed_out)
        && (worker->task.wait_node.is_mutex_wait == RTOS_FALSE)
        && (done_tick == expected_tick) && (value == expected_value(id))
        && ((id != WRAP_NOTIFY_ZERO) || (late_status == RTOS_OK))
        && (worker->task.state == RTOS_TASK_SUSPENDED) && (worker->task.blocked_on == NULL)
        && (worker->task.wait_node.list_head == NULL) && (worker->task.wait_node.next == NULL)
        && (worker->task.wait_node.prev == NULL);
    RTOS_PORT_EXIT_CRITICAL();

    if (!is_valid) {
        (void)printf("WRAP|FAIL|case=%s|status=%ld|wait=%ld|tick=%lu|value=%08lx|late=%ld\r\n",
                     test_case->name, (long)status, (long)wait_result, (unsigned long)done_tick,
                     (unsigned long)value, (long)late_status);
    }
    return is_valid;
}

static bool run_round(rtos_tcb_t *owner)
{
    const uint32_t original = WRAP_QUEUE_ORIGINAL | round_number;
    const rtos_prio_t base_priority = owner->base_priority;
    const rtos_status_t send_status = rtos_queue_send(&send_queue, &original, RTOS_NO_WAIT,
                                                     RTOS_FALSE);
    const rtos_status_t take_status = rtos_mutex_take(&mutex, RTOS_NO_WAIT);
    bool is_valid = (send_status == RTOS_OK) && (take_status == RTOS_OK);

    /* Begin just after a hardware Tick. Set the counter only while no finite wait is live. */
    rtos_task_delay(1U);
    RTOS_PORT_ENTER_CRITICAL();
    is_valid = (rtos_kernel.delay_head == NULL) && is_valid;
    if (is_valid) {
        rtos_kernel.tick_count = UINT32_MAX - WRAP_START_LEAD;
        trace = (wrap_trace_t){.is_systick_valid = true};
    }
    RTOS_PORT_EXIT_CRITICAL();

    uint32_t created = 0U;
    while (is_valid && (created < WRAP_CASE_COUNT)) {
        wrap_worker_t *const worker = &workers[created];
        worker->status = RTOS_ERR_PARAM;
        worker->late_status = RTOS_ERR_PARAM;
        is_valid = rtos_task_create_static(&worker->task, stacks[created], WRAP_STACK_WORDS,
            run_waiter, worker, WRAP_WORKER_PRIORITY, cases[created].name) == RTOS_OK;
        if (is_valid) {
            ++created;
        }
    }

    RTOS_PORT_ENTER_CRITICAL();
    const rtos_tick_t remaining = WRAP_FINISH_TICK - rtos_kernel.tick_count;
    is_valid = is_valid && (remaining > WRAP_FINISH_TICK)
        && (remaining <= WRAP_START_LEAD + WRAP_FINISH_TICK + 1U)
        && (owner->priority == WRAP_WORKER_PRIORITY) && (mutex.owner == owner);
    for (uint32_t index = 0U; index < created; ++index) {
        const rtos_tcb_t *const task = &workers[index].task;
        if ((task->state != RTOS_TASK_DELAYED) || (task->wake_tick != cases[index].deadline)
            || (task->blocked_on == NULL)) {
            is_valid = false;
        }
    }
    trace.is_armed = is_valid;
    RTOS_PORT_EXIT_CRITICAL();

    if (is_valid) {
        rtos_task_delay(remaining);
        RTOS_PORT_ENTER_CRITICAL();
        trace.is_armed = false;
        is_valid = (trace.systick_count == 3U) && trace.is_systick_valid
            && (trace.irq_count == 1U) && trace.is_irq_valid
            && (rtos_kernel.tick_count >= WRAP_FINISH_TICK) && (rtos_kernel.delay_head == NULL)
            && (sem.wait_head == NULL) && (sem.wait_bitmap == 0U) && (sem.count == 0U)
            && (recv_queue.recv_wait_head == NULL) && (recv_queue.count == 0U)
            && (send_queue.send_wait_head == NULL) && (send_queue.count == 1U)
            && (event.wait_head == NULL) && (event.bits == WRAP_EVENT_KEEP)
            && (mutex.wait_head == NULL) && (mutex.owner == owner)
            && (owner->priority == base_priority) && (owner->base_priority == base_priority);
        RTOS_PORT_EXIT_CRITICAL();

        for (uint32_t index = 0U; index < created; ++index) {
            is_valid = check_waiter((wrap_case_id_t)index) && is_valid;
        }
        uint32_t received = WRAP_GUARD;
        const rtos_status_t recv_status = rtos_queue_recv(&send_queue, &received, RTOS_NO_WAIT);
        is_valid = is_valid && (recv_status == RTOS_OK)
            && (received == (WRAP_QUEUE_SEND_VALUE | round_number)) && (send_queue.count == 0U);
    } else {
        (void)printf("WRAP|FAIL|phase=enroll|workers=%lu|remaining=%lu\r\n",
                     (unsigned long)created, (unsigned long)remaining);
    }

    for (uint32_t index = 0U; index < created; ++index) {
        is_valid = (rtos_task_delete(&workers[index].task) == RTOS_OK) && is_valid;
    }
    if (take_status == RTOS_OK) {
        is_valid = (rtos_mutex_give(&mutex) == RTOS_OK) && (mutex.owner == NULL) && is_valid;
    }
    return is_valid;
}

static bool initialize_objects(void)
{
    return (rtos_sem_init(&sem, 0U, 1U) == RTOS_OK)
        && (rtos_queue_init(&recv_queue, &recv_storage, sizeof(recv_storage), 1U) == RTOS_OK)
        && (rtos_queue_init(&send_queue, &send_storage, sizeof(send_storage), 1U) == RTOS_OK)
        && (rtos_event_init(&event) == RTOS_OK) && (rtos_mutex_init(&mutex) == RTOS_OK)
        && (rtos_event_set(&event, WRAP_EVENT_KEEP) == WRAP_EVENT_KEEP);
}

static bool deinitialize_objects(void)
{
    const bool is_mutex_clean = (mutex.is_initialized == 0U)
        || (rtos_mutex_deinit(&mutex) == RTOS_OK);
    const bool is_event_clean = (event.is_initialized == 0U)
        || (rtos_event_deinit(&event) == RTOS_OK);
    const bool is_send_queue_clean = (send_queue.is_initialized == 0U)
        || (rtos_queue_deinit(&send_queue) == RTOS_OK);
    const bool is_recv_queue_clean = (recv_queue.is_initialized == 0U)
        || (rtos_queue_deinit(&recv_queue) == RTOS_OK);
    const bool is_sem_clean = (sem.is_initialized == 0U) || (rtos_sem_deinit(&sem) == RTOS_OK);
    return is_mutex_clean && is_event_clean && is_send_queue_clean && is_recv_queue_clean
        && is_sem_clean;
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    (void)printf("WRAP|BEGIN|rounds=%u|workers=%u\r\n", (unsigned int)WRAP_ROUNDS,
                 (unsigned int)WRAP_CASE_COUNT);
    const uint32_t saved_priority = NVIC_GetPriority(EXTI1_IRQn);
    const bool is_irq_available = (NVIC_GetEnableIRQ(EXTI1_IRQn) == 0U)
        && (NVIC_GetPendingIRQ(EXTI1_IRQn) == 0U);
    uint32_t passes = 0U;
    uint32_t failures = 0U;

    if (is_irq_available && initialize_objects()) {
        NVIC_SetPriority(EXTI1_IRQn, RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY
                         >> (8U - __NVIC_PRIO_BITS));
        NVIC_EnableIRQ(EXTI1_IRQn);
        for (round_number = 1U; round_number <= WRAP_ROUNDS; ++round_number) {
            const bool is_valid = run_round(rtos_task_get_current());
            (void)printf("WRAP|ROUND|%s|n=%lu|systick=%lu|irq=%lu|irq_tick=%lu\r\n",
                         is_valid ? "PASS" : "FAIL", (unsigned long)round_number,
                         (unsigned long)trace.systick_count, (unsigned long)trace.irq_count,
                         (unsigned long)trace.irq_tick);
            if (!is_valid) {
                ++failures;
                break;
            }
            ++passes;
        }
        NVIC_DisableIRQ(EXTI1_IRQn);
        NVIC_ClearPendingIRQ(EXTI1_IRQn);
        NVIC_SetPriority(EXTI1_IRQn, saved_priority);
    } else {
        (void)printf("WRAP|FAIL|phase=init\r\n");
        ++failures;
    }

    if (!deinitialize_objects()) {
        (void)printf("WRAP|FAIL|phase=deinit\r\n");
        ++failures;
    }
    (void)printf("WRAP|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)passes, (unsigned long)failures);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

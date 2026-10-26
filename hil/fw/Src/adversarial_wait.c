/**
 * @file adversarial_wait.c
 * @brief Real-IRQ, multi-waiter handoff, timeout and object-lifetime regressions.
 *
 * Private objects have exact message/token oracles. Priority-five TIM7 performs
 * supported nonblocking ISR operations; priority-zero TIM2 is hardware-only.
 * Worker results are published under RTOS exclusion and printed only after a
 * phase, so UART latency cannot decide a waiter's deliberately short deadline.
 * Every object wait is finite. A host capture deadline remains the independent
 * watchdog if a broken scheduler cannot return from a finite delay at all.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "production_highirq.h"
#include "rtos.h"

#if !defined(FIRMWARE_ADVERSARIAL_WAIT) || !defined(FIRMWARE_AUDIT)
#error "adversarial_wait.c requires the dedicated audit-entry firmware"
#endif

#ifndef ADVERSARIAL_WAIT_ROUNDS
#define ADVERSARIAL_WAIT_ROUNDS (8U)
#endif

#define AW_WORKERS (4U)
#define AW_STACK_WORDS (384U)
#define AW_WAIT_TICKS (100U)
#define AW_SHORT_TICKS (8U)
#define AW_WAIT_BOUND_TICKS (300U)
#define AW_SENTINEL (0xA5C35AA5U)
#define AW_PREFIX (0x12345678U)
#define AW_SUFFIX (0x87654321U)
#define AW_ERROR_RECORDS (24U)

typedef enum aw_operation {
    AW_OP_RECV = 0,
    AW_OP_SEND,
    AW_OP_SEM,
    AW_OP_EVENT,
    AW_OP_NOTIFY,
    AW_OP_MUTEX,
    AW_OP_OWNER
} aw_operation_t;

typedef enum aw_irq_action {
    AW_IRQ_NONE = 0,
    AW_IRQ_SEND,
    AW_IRQ_RECV,
    AW_IRQ_GIVE,
    AW_IRQ_EVENT,
    AW_IRQ_NOTIFY_VALUE,
    AW_IRQ_NOTIFY_INCREMENT,
    AW_IRQ_QUEUE_DELETE
} aw_irq_action_t;

typedef struct aw_word {
    uint32_t prefix;
    uint32_t value;
    uint32_t suffix;
} aw_word_t;

typedef struct aw_worker {
    uint32_t id;
    aw_operation_t operation;
    uint32_t timeout;
    uint32_t input;
    uint32_t event_bits;
    uint32_t event_mode;
    uint32_t follow_notify;
} aw_worker_t;

typedef struct aw_result {
    int32_t status;
    int32_t secondary_status;
    uint32_t value;
    uint32_t secondary_value;
    uint32_t mask;
    uint32_t order;
    uint32_t guards;
    uint32_t *destination;
} aw_result_t;

typedef struct aw_error {
    const char *name;
    uint32_t round;
    uint32_t actual;
    uint32_t expected;
} aw_error_t;

static rtos_tcb_t workers[AW_WORKERS];
static rtos_stack_t stacks[AW_WORKERS][AW_STACK_WORDS] __attribute__((aligned(8)));
static aw_worker_t arguments[AW_WORKERS];
static aw_result_t results[AW_WORKERS];
static rtos_queue_t queue;
static uint32_t queue_storage;
static rtos_sem_t semaphore;
static rtos_sem_t owner_ready;
static rtos_sem_t owner_release;
static rtos_mutex_t mutex;
static rtos_event_t event;
static uint32_t created_mask;
static volatile uint32_t completed_mask;
static uint32_t completion_order;
static uint32_t pass_count;
static uint32_t fail_count;
static uint32_t round_number;
static uint32_t phase_count;
static aw_error_t errors[AW_ERROR_RECORDS];
static uint32_t recorded_errors;
static uint32_t printed_errors;
static volatile uint32_t irq_action;
static volatile uint32_t irq_value;
static volatile uint32_t irq_target;
static volatile uint32_t irq_sequence;
static volatile uint32_t irq_completed;
static volatile int32_t irq_status;
static volatile uint32_t irq_output;
static uint32_t saved_irq_priority;
static uint32_t irq_owned;

static void expect_value(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        pass_count++;
        return;
    }
    fail_count++;
    if (recorded_errors < AW_ERROR_RECORDS) {
        errors[recorded_errors].name = name;
        errors[recorded_errors].round = round_number;
        errors[recorded_errors].actual = actual;
        errors[recorded_errors].expected = expected;
        recorded_errors++;
    }
}

static void expect_status(const char *name, int32_t actual, rtos_status_t expected)
{
    expect_value(name, (uint32_t)actual, (uint32_t)(int32_t)expected);
}

static uint32_t thread_mask(void)
{
    return __get_BASEPRI() | __get_PRIMASK() | rtos_port_get_critical_nesting();
}

static uint32_t wait_completed(uint32_t mask)
{
    uint32_t tick = rtos_sched_get_tick_count();
    uint32_t started = DWT->CYCCNT;

    while ((completed_mask & mask) != mask) {
        if (((uint32_t)(rtos_sched_get_tick_count() - tick) >= AW_WAIT_BOUND_TICKS)
            || ((uint32_t)(DWT->CYCCNT - started) >= SystemCoreClock / 2U)) {
            return 0U;
        }
        rtos_task_delay(1U);
    }
    return 1U;
}

static void publish_result(uint32_t id, const aw_result_t *result)
{
    RTOS_PORT_ENTER_CRITICAL();
    results[id] = *result;
    results[id].order = completion_order;
    completion_order++;
    completed_mask |= 1U << id;
    RTOS_PORT_EXIT_CRITICAL();
}

static int32_t owner_work(void)
{
    rtos_status_t status = rtos_mutex_take(&mutex, RTOS_NO_WAIT);

    if (status != RTOS_OK) {
        return (int32_t)status;
    }
    status = rtos_sem_give(&owner_ready);
    if (status == RTOS_OK) {
        status = rtos_sem_take(&owner_release, AW_WAIT_BOUND_TICKS);
    }
    rtos_status_t release = rtos_mutex_give(&mutex);
    return (int32_t)(status == RTOS_OK ? release : status);
}

static void worker_entry(void *context)
{
    const aw_worker_t *worker = (const aw_worker_t *)context;
    aw_word_t word = {AW_PREFIX, AW_SENTINEL, AW_SUFFIX};
    aw_result_t result = {0};

    result.destination = &word.value;
    result.secondary_value = AW_SENTINEL;
    switch (worker->operation) {
        case AW_OP_RECV:
            result.status = (int32_t)rtos_queue_recv(&queue, &word.value, worker->timeout);
            break;
        case AW_OP_SEND:
            word.value = worker->input;
            result.status = (int32_t)rtos_queue_send(&queue, &word.value, worker->timeout,
                RTOS_FALSE);
            break;
        case AW_OP_SEM:
            result.status = (int32_t)rtos_sem_take(&semaphore, worker->timeout);
            break;
        case AW_OP_EVENT:
            result.value = rtos_event_wait(&event, worker->event_bits, worker->event_mode,
                worker->timeout);
            result.status = (int32_t)rtos_task_get_current()->wait_result;
            break;
        case AW_OP_NOTIFY:
            result.status = (int32_t)rtos_task_notify_wait(worker->timeout, &word.value);
            break;
        case AW_OP_MUTEX:
            result.status = (int32_t)rtos_mutex_take(&mutex, worker->timeout);
            if (result.status == RTOS_OK) {
                result.secondary_status = (int32_t)rtos_mutex_give(&mutex);
            }
            break;
        case AW_OP_OWNER:
            result.status = owner_work();
            break;
        default:
            result.status = (int32_t)RTOS_ERR_PARAM;
            break;
    }
    if (worker->operation != AW_OP_EVENT) {
        result.value = word.value;
    }
    if (worker->follow_notify != 0U) {
        result.secondary_status = (int32_t)rtos_task_notify_wait(RTOS_NO_WAIT,
            &result.secondary_value);
    }
    result.mask = thread_mask();
    result.guards = (word.prefix == AW_PREFIX) && (word.suffix == AW_SUFFIX);
    publish_result(worker->id, &result);
    (void)rtos_task_suspend(NULL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static uint32_t spawn_worker(uint32_t id, aw_operation_t operation, uint32_t timeout,
                             uint32_t priority)
{
    arguments[id].id = id;
    arguments[id].operation = operation;
    arguments[id].timeout = timeout;
    rtos_status_t status = rtos_task_create_static(&workers[id], stacks[id], AW_STACK_WORDS,
        worker_entry, &arguments[id], priority, "aw-worker");
    expect_status("worker_create", (int32_t)status, RTOS_OK);
    if (status != RTOS_OK) {
        return 0U;
    }
    created_mask |= 1U << id;
    return 1U;
}

static void clear_workers(void)
{
    for (uint32_t id = 0U; id < AW_WORKERS; id++) {
        if ((created_mask & (1U << id)) != 0U) {
            expect_status("worker_delete", (int32_t)rtos_task_delete(&workers[id]), RTOS_OK);
        }
    }
    created_mask = 0U;
}

static void begin_phase(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    memset(arguments, 0, sizeof(arguments));
    memset(results, 0, sizeof(results));
    completed_mask = 0U;
    completion_order = 0U;
    RTOS_PORT_EXIT_CRITICAL();
}

static void end_phase(const char *name)
{
    clear_workers();
    expect_value("controller_mask", thread_mask(), 0U);
    while (printed_errors < recorded_errors) {
        const aw_error_t *error = &errors[printed_errors];
        (void)printf("AW|CASE|FAIL|round=%lu name=%s actual=0x%08lX expected=0x%08lX\r\n",
            (unsigned long)error->round, error->name, (unsigned long)error->actual,
            (unsigned long)error->expected);
        printed_errors++;
    }
    phase_count++;
    (void)printf("AW|PHASE|name=%s round=%lu pass=%lu fail=%lu\r\n", name,
        (unsigned long)round_number, (unsigned long)pass_count, (unsigned long)fail_count);
}

static uint32_t isr_acquire(void)
{
    if ((NVIC_GetEnableIRQ(TIM7_IRQn) != 0U) || (NVIC_GetPendingIRQ(TIM7_IRQn) != 0U)
        || (NVIC_GetActive(TIM7_IRQn) != 0U)) {
        return 0U;
    }
    saved_irq_priority = NVIC_GetPriority(TIM7_IRQn);
    NVIC_SetPriority(TIM7_IRQn, 5U);
    NVIC_EnableIRQ(TIM7_IRQn);
    irq_owned = 1U;
    return 1U;
}

static void perform_irq_action(void)
{
    uint32_t value = irq_value;

    irq_status = (int32_t)RTOS_OK;
    switch ((aw_irq_action_t)irq_action) {
        case AW_IRQ_SEND:
            irq_status = (int32_t)rtos_queue_send(&queue, &value, RTOS_NO_WAIT, RTOS_FALSE);
            break;
        case AW_IRQ_RECV:
            irq_status = (int32_t)rtos_queue_recv(&queue, &value, RTOS_NO_WAIT);
            irq_output = value;
            break;
        case AW_IRQ_GIVE:
            irq_status = (int32_t)rtos_sem_give(&semaphore);
            break;
        case AW_IRQ_EVENT:
            irq_output = rtos_event_set(&event, value);
            break;
        case AW_IRQ_NOTIFY_VALUE:
            irq_status = (int32_t)rtos_task_notify(&workers[irq_target], value,
                RTOS_NOTIFY_VALUE);
            break;
        case AW_IRQ_NOTIFY_INCREMENT:
            for (uint32_t index = 0U; index < 3U; index++) {
                rtos_status_t status = rtos_task_notify(&workers[irq_target], 0U,
                    RTOS_NOTIFY_INCREMENT);
                if (status != RTOS_OK) {
                    irq_status = (int32_t)status;
                }
            }
            break;
        case AW_IRQ_QUEUE_DELETE:
            irq_status = (int32_t)rtos_queue_deinit(&queue);
            break;
        default:
            irq_status = (int32_t)RTOS_ERR_PARAM;
            break;
    }
}

/** @brief CMSIS vector for the fixture-owned, syscall-priority TIM7 request. */
void TIM7_IRQHandler(void)
{
    perform_irq_action();
    irq_completed = irq_sequence;
}

static void request_irq(aw_irq_action_t action, uint32_t value, uint32_t target)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t started = DWT->CYCCNT;

    __disable_irq();
    irq_action = (uint32_t)action;
    irq_value = value;
    irq_target = target;
    irq_sequence++;
    uint32_t sequence = irq_sequence;
    NVIC_SetPendingIRQ(TIM7_IRQn);
    __set_PRIMASK(primask);
    __DSB();
    __ISB();
    while ((irq_completed != sequence)
        && ((uint32_t)(DWT->CYCCNT - started) < SystemCoreClock / 10U)) {
        __NOP();
    }
    expect_value("real_irq_completed", irq_completed, sequence);
    expect_status("isr_api_status", irq_status, RTOS_OK);
}

static void check_worker(uint32_t id, rtos_status_t status, uint32_t value)
{
    expect_status("worker_status", results[id].status, status);
    expect_value("worker_value", results[id].value, value);
    expect_value("worker_mask", results[id].mask, 0U);
    expect_value("worker_guards", results[id].guards, 1U);
}

static void queue_receivers(void)
{
    begin_phase();
    expect_status("queue_init", (int32_t)rtos_queue_init(&queue, &queue_storage, 4U, 1U),
        RTOS_OK);
    if (spawn_worker(0U, AW_OP_RECV, AW_WAIT_TICKS, 4U)
        && spawn_worker(1U, AW_OP_RECV, AW_WAIT_TICKS, 5U)) {
        expect_value("two_receivers_pending", completed_mask, 0U);
        request_irq(AW_IRQ_SEND, 0x11110000U + round_number, 0U);
        expect_value("one_message_one_receiver", completed_mask, 1U);
        check_worker(0U, RTOS_OK, 0x11110000U + round_number);
        request_irq(AW_IRQ_SEND, 0x22220000U + round_number, 0U);
        expect_value("both_receivers_done", wait_completed(3U), 1U);
        check_worker(1U, RTOS_OK, 0x22220000U + round_number);
        expect_value("receiver_priority_order", results[0].order, 0U);
        expect_value("receiver_second_order", results[1].order, 1U);
        expect_value("queue_no_duplicate", queue.count, 0U);
        expect_value("receiver_list_empty", queue.recv_wait_head == NULL, 1U);
    }
    clear_workers();
    expect_status("queue_deinit", (int32_t)rtos_queue_deinit(&queue), RTOS_OK);
    end_phase("queue_receivers");
}

static void queue_senders(void)
{
    uint32_t value = 0x33330000U + round_number;

    begin_phase();
    expect_status("queue_init", (int32_t)rtos_queue_init(&queue, &queue_storage, 4U, 1U),
        RTOS_OK);
    expect_status("queue_fill", (int32_t)rtos_queue_send(&queue, &value, RTOS_NO_WAIT,
        RTOS_FALSE), RTOS_OK);
    arguments[0].input = 0x44440000U + round_number;
    arguments[1].input = 0x55550000U + round_number;
    if (spawn_worker(0U, AW_OP_SEND, AW_WAIT_TICKS, 4U)
        && spawn_worker(1U, AW_OP_SEND, AW_WAIT_TICKS, 5U)) {
        expect_value("two_senders_pending", completed_mask, 0U);
        request_irq(AW_IRQ_RECV, 0U, 0U);
        expect_value("original_item_first", irq_output, value);
        expect_value("one_slot_one_sender", completed_mask, 1U);
        request_irq(AW_IRQ_RECV, 0U, 0U);
        expect_value("higher_sender_item", irq_output, arguments[0].input);
        expect_value("both_senders_done", wait_completed(3U), 1U);
        request_irq(AW_IRQ_RECV, 0U, 0U);
        expect_value("lower_sender_item", irq_output, arguments[1].input);
        check_worker(0U, RTOS_OK, arguments[0].input);
        check_worker(1U, RTOS_OK, arguments[1].input);
        expect_value("sender_list_empty", queue.send_wait_head == NULL, 1U);
        expect_value("all_three_items_consumed", queue.count, 0U);
    }
    clear_workers();
    expect_status("queue_deinit", (int32_t)rtos_queue_deinit(&queue), RTOS_OK);
    end_phase("queue_senders");
}

static void semaphore_waiters(void)
{
    begin_phase();
    expect_status("sem_init", (int32_t)rtos_sem_init(&semaphore, 0U, 1U), RTOS_OK);
    if (spawn_worker(0U, AW_OP_SEM, AW_SHORT_TICKS, 4U)
        && spawn_worker(1U, AW_OP_SEM, AW_SHORT_TICKS, 5U)
        && spawn_worker(2U, AW_OP_SEM, AW_SHORT_TICKS, 6U)) {
        request_irq(AW_IRQ_GIVE, 0U, 0U);
        expect_value("one_give_one_completion", completed_mask, 1U);
        expect_value("all_sem_waiters_finished", wait_completed(7U), 1U);
        check_worker(0U, RTOS_OK, AW_SENTINEL);
        check_worker(1U, RTOS_ERR_TIMEOUT, AW_SENTINEL);
        check_worker(2U, RTOS_ERR_TIMEOUT, AW_SENTINEL);
        expect_value("sem_no_phantom_token", semaphore.count, 0U);
        expect_value("sem_wait_head_empty", semaphore.wait_head == NULL, 1U);
        expect_value("sem_bitmap_empty", semaphore.wait_bitmap, 0U);
        request_irq(AW_IRQ_GIVE, 0U, 0U);
        expect_value("late_give_becomes_stored_token", semaphore.count, 1U);
        expect_status("late_token_take_once", (int32_t)rtos_sem_take(&semaphore, RTOS_NO_WAIT),
            RTOS_OK);
        expect_status("late_token_not_duplicated", (int32_t)rtos_sem_take(&semaphore,
            RTOS_NO_WAIT), RTOS_ERR_TIMEOUT);
    }
    clear_workers();
    expect_status("sem_deinit", (int32_t)rtos_sem_deinit(&semaphore), RTOS_OK);
    end_phase("semaphore_waiters");
}

static void event_consumption(void)
{
    begin_phase();
    expect_status("event_init", (int32_t)rtos_event_init(&event), RTOS_OK);
    arguments[0].event_bits = 3U;
    arguments[0].event_mode = RTOS_EVENT_WAIT_ANY | RTOS_EVENT_CLEAR_ON_EXIT;
    arguments[1].event_bits = 3U;
    arguments[1].event_mode = RTOS_EVENT_WAIT_ALL | RTOS_EVENT_CLEAR_ON_EXIT;
    if (spawn_worker(0U, AW_OP_EVENT, AW_WAIT_TICKS, 4U)
        && spawn_worker(1U, AW_OP_EVENT, AW_WAIT_TICKS, 5U)) {
        request_irq(AW_IRQ_EVENT, 3U, 0U);
        expect_value("clear_consumed_by_highest_only", completed_mask, 1U);
        check_worker(0U, RTOS_OK, 3U);
        request_irq(AW_IRQ_EVENT, 1U, 0U);
        expect_value("all_waiter_not_satisfied_by_partial", completed_mask, 1U);
        request_irq(AW_IRQ_EVENT, 2U, 0U);
        expect_value("all_waiter_finished", wait_completed(3U), 1U);
        check_worker(1U, RTOS_OK, 3U);
        expect_value("event_bits_consumed", event.bits, 0U);
        expect_value("event_wait_list_empty", event.wait_head == NULL, 1U);
    }
    clear_workers();
    expect_status("event_deinit", (int32_t)rtos_event_deinit(&event), RTOS_OK);
    end_phase("event_consumption");
}

static void notification_channels(void)
{
    begin_phase();
    arguments[0].follow_notify = 1U;
    if (spawn_worker(0U, AW_OP_NOTIFY, AW_WAIT_TICKS, 4U)) {
        request_irq(AW_IRQ_NOTIFY_INCREMENT, 0U, 0U);
        expect_value("notify_waiter_finished", wait_completed(1U), 1U);
        check_worker(0U, RTOS_OK, 3U);
        expect_status("notification_consumed_once", results[0].secondary_status,
            RTOS_ERR_TIMEOUT);
    }
    end_phase("notify_increment");
    begin_phase();
    expect_status("event_init", (int32_t)rtos_event_init(&event), RTOS_OK);
    arguments[0].event_bits = 1U;
    arguments[0].event_mode = RTOS_EVENT_WAIT_ANY;
    arguments[0].follow_notify = 1U;
    if (spawn_worker(0U, AW_OP_EVENT, AW_WAIT_TICKS, 4U)) {
        request_irq(AW_IRQ_NOTIFY_VALUE, 0x76540000U + round_number, 0U);
        expect_value("notify_does_not_steal_event_wait", completed_mask, 0U);
        request_irq(AW_IRQ_EVENT, 1U, 0U);
        expect_value("event_then_notify_finished", wait_completed(1U), 1U);
        check_worker(0U, RTOS_OK, 1U);
        expect_status("notification_survived_other_wait", results[0].secondary_status, RTOS_OK);
        expect_value("notification_identity", results[0].secondary_value,
            0x76540000U + round_number);
    }
    clear_workers();
    expect_status("event_deinit", (int32_t)rtos_event_deinit(&event), RTOS_OK);
    end_phase("event_notify_independence");
}

static void queue_lifetime(void)
{
    begin_phase();
    expect_status("queue_init", (int32_t)rtos_queue_init(&queue, &queue_storage, 4U, 1U),
        RTOS_OK);
    if (spawn_worker(0U, AW_OP_RECV, AW_WAIT_TICKS, 4U)) {
        request_irq(AW_IRQ_QUEUE_DELETE, 0U, 0U);
        expect_value("deleted_object_wakes_receiver", wait_completed(1U), 1U);
        check_worker(0U, RTOS_ERR_DELETED, AW_SENTINEL);
        expect_value("deleted_queue_waiters_empty", queue.recv_wait_head == NULL, 1U);
    }
    end_phase("queue_object_delete");
    begin_phase();
    expect_status("queue_reinit", (int32_t)rtos_queue_init(&queue, &queue_storage, 4U, 1U),
        RTOS_OK);
    if (spawn_worker(0U, AW_OP_RECV, AW_SHORT_TICKS, 4U)) {
        expect_value("receiver_really_times_out", wait_completed(1U), 1U);
        check_worker(0U, RTOS_ERR_TIMEOUT, AW_SENTINEL);
        request_irq(AW_IRQ_SEND, 0x67890000U + round_number, 0U);
        expect_value("late_queue_message_buffered", queue.count, 1U);
        expect_value("timed_out_destination_unchanged", *results[0].destination, AW_SENTINEL);
        uint32_t value = 0U;
        expect_status("late_queue_message_received", (int32_t)rtos_queue_recv(&queue, &value,
            RTOS_NO_WAIT), RTOS_OK);
        expect_value("late_queue_identity", value, 0x67890000U + round_number);
    }
    clear_workers();
    expect_status("queue_deinit", (int32_t)rtos_queue_deinit(&queue), RTOS_OK);
    end_phase("queue_timeout_late_send");
}

static void mutex_timeout_inheritance(void)
{
    begin_phase();
    expect_status("mutex_init", (int32_t)rtos_mutex_init(&mutex), RTOS_OK);
    expect_status("owner_ready_init", (int32_t)rtos_sem_init(&owner_ready, 0U, 1U), RTOS_OK);
    expect_status("owner_release_init", (int32_t)rtos_sem_init(&owner_release, 0U, 1U), RTOS_OK);
    if (spawn_worker(0U, AW_OP_OWNER, AW_WAIT_TICKS, 12U)) {
        expect_status("owner_has_mutex", (int32_t)rtos_sem_take(&owner_ready,
            AW_WAIT_TICKS), RTOS_OK);
        if (spawn_worker(1U, AW_OP_MUTEX, AW_SHORT_TICKS, 4U)) {
            expect_value("mutex_inheritance_applied", workers[0].priority, 4U);
            expect_value("mutex_waiter_times_out", wait_completed(2U), 1U);
            check_worker(1U, RTOS_ERR_TIMEOUT, AW_SENTINEL);
            expect_value("mutex_priority_restored_after_timeout", workers[0].priority, 12U);
            expect_value("mutex_original_owner_preserved", mutex.owner == &workers[0], 1U);
            expect_status("owner_release_signal", (int32_t)rtos_sem_give(&owner_release),
                RTOS_OK);
            expect_value("owner_released_and_finished", wait_completed(3U), 1U);
            check_worker(0U, RTOS_OK, AW_SENTINEL);
            expect_value("mutex_released", mutex.owner == NULL, 1U);
            expect_value("mutex_wait_list_empty", mutex.wait_head == NULL, 1U);
        }
    }
    clear_workers();
    expect_status("mutex_deinit", (int32_t)rtos_mutex_deinit(&mutex), RTOS_OK);
    expect_status("owner_ready_deinit", (int32_t)rtos_sem_deinit(&owner_ready), RTOS_OK);
    expect_status("owner_release_deinit", (int32_t)rtos_sem_deinit(&owner_release), RTOS_OK);
    end_phase("mutex_timeout_inheritance");
}

/** @brief Record the finite real-IRQ scenario configuration before startup. */
void audit_pre_start(void)
{
    (void)printf("AW|META|rounds=%lu workers=%lu syscall_irq=TIM7 priority=5 "
        "highirq=TIM2 priority=0 finite_waits=1\r\n",
        (unsigned long)ADVERSARIAL_WAIT_ROUNDS, (unsigned long)AW_WORKERS);
}

/** @brief Execute private-object adversarial scenarios, then report AW|DONE.
 *  @param arg [in] Unused task argument; may be NULL.
 *  @return Never returns; waits after the result is emitted. */
void audit_regressions_task(void *arg)
{
    (void)arg;
    uint32_t highirq_started = production_highirq_start() == RTOS_OK;
    expect_value("highirq_start", highirq_started, 1U);
    expect_value("syscall_irq_ownership", isr_acquire(), 1U);
    if (irq_owned != 0U) {
        for (round_number = 0U; round_number < ADVERSARIAL_WAIT_ROUNDS; round_number++) {
            queue_receivers();
            queue_senders();
            semaphore_waiters();
            event_consumption();
            notification_channels();
            queue_lifetime();
            mutex_timeout_inheritance();
        }
        NVIC_DisableIRQ(TIM7_IRQn);
        NVIC_ClearPendingIRQ(TIM7_IRQn);
        NVIC_SetPriority(TIM7_IRQn, saved_irq_priority);
    }
    if (highirq_started != 0U) {
        expect_status("highirq_stop", (int32_t)production_highirq_stop(), RTOS_OK);
        production_highirq_snapshot_t snapshot;
        rtos_status_t status = production_highirq_snapshot(&snapshot);
        expect_status("highirq_snapshot", (int32_t)status, RTOS_OK);
        if (status == RTOS_OK) {
            expect_value("highirq_has_real_updates", snapshot.natural_updates != 0U, 1U);
            expect_value("highirq_no_errors", snapshot.errors, 0U);
            expect_value("highirq_restored", snapshot.restored, 1U);
            (void)printf("AW|HIGHIRQ|entries=%lu nonzero_samples=%lu errors=%lu restored=%lu\r\n",
                (unsigned long)snapshot.entries, (unsigned long)snapshot.nonzero_basepri_samples,
                (unsigned long)snapshot.errors, (unsigned long)snapshot.restored);
        }
    }
    end_phase("final_cleanup");
    (void)printf("AW|SUMMARY|pass=%lu fail=%lu rounds=%lu phases=%lu irq_requests=%lu\r\n",
        (unsigned long)pass_count, (unsigned long)fail_count,
        (unsigned long)ADVERSARIAL_WAIT_ROUNDS, (unsigned long)phase_count,
        (unsigned long)irq_sequence);
    (void)printf("AW|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

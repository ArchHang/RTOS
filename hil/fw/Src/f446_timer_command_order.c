/** @file f446_timer_command_order.c
 * @brief Exercise timer command ordering across an IRQ and a blocked callback.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"
#include "rtos.h"

#ifndef F446_TIMER_ORDER_ROUNDS
#define F446_TIMER_ORDER_ROUNDS 16U
#endif
#ifndef F446_TIMER_ORDER_TEST_DELETE
#define F446_TIMER_ORDER_TEST_DELETE 0
#endif

enum {
    TIMER_ORDER_PERIOD = 4U,
    TIMER_ORDER_WAIT_TICKS = 50U,
    TIMER_ORDER_SETTLE_TICKS = 4U * TIMER_ORDER_PERIOD,
    TIMER_ORDER_QUEUE_CAPACITY = 16U,
    TIMER_ORDER_FILL_PERIOD = 100U
};

_Static_assert(F446_TIMER_ORDER_ROUNDS > 0U && F446_TIMER_ORDER_ROUNDS <= 256U,
               "Use between one and 256 pairs of timer commands");

typedef enum timer_order_mode {
    TIMER_ORDER_IRQ_START,
    TIMER_ORDER_IRQ_STOP,
#if F446_TIMER_ORDER_TEST_DELETE
    TIMER_ORDER_IRQ_DELETE,
#endif
} timer_order_mode_t;

typedef struct timer_order_irq {
    _Atomic uint32_t entries;
    _Atomic uint32_t accepted;
    _Atomic rtos_status_t first_status;
    _Atomic rtos_status_t full_status;
    _Atomic rtos_status_t release_status;
    _Atomic bool is_expected_vector;
} timer_order_irq_t;

static rtos_timer_t s_timer;
static rtos_timer_t s_filler;
static rtos_sem_t s_entered;
static rtos_sem_t s_release;
static rtos_sem_t s_done;
static timer_order_irq_t s_irq;
static _Atomic timer_order_mode_t s_mode;
static _Atomic uint32_t s_fill_base = TIMER_ORDER_FILL_PERIOD;
static _Atomic(rtos_tcb_t *) s_callback_task;
static _Atomic uint32_t s_calls;
static _Atomic rtos_status_t s_wait_status;
static _Atomic rtos_status_t s_command_status;
static _Atomic rtos_tick_t s_command_ticks;
static _Atomic bool s_callback_error;
static _Atomic bool s_filler_called;
static uint32_t s_checks;
static uint32_t s_failures;

void f446_timer_command_order_run(void *argument);
void EXTI2_IRQHandler(void);

static bool record_check(const char *name, bool is_ok)
{
    ++s_checks;
    if (!is_ok) {
        ++s_failures;
        (void)printf("F4TI|CHECK|%s|FAIL\r\n", name);
    }
    return is_ok;
}

static void filler_callback(void *argument)
{
    (void)argument;
    s_filler_called = true;
}

static void timer_callback(void *argument)
{
    rtos_timer_t *timer = argument;
    if (RTOS_PORT_IN_ISR() || rtos_port_get_critical_nesting() != 0U ||
        __get_BASEPRI() != 0U || __get_PRIMASK() != 0U) {
        s_callback_error = true;
    }

    const uint32_t call = s_calls++;
    if (call != 0U) {
        if (rtos_timer_stop(timer) != RTOS_OK) {
            s_callback_error = true;
        }
        return;
    }

    s_callback_task = rtos_task_get_current();
    if (rtos_sem_give(&s_entered) != RTOS_OK) {
        s_callback_error = true;
    }
    s_wait_status = rtos_sem_take(&s_release, TIMER_ORDER_WAIT_TICKS);

    const rtos_tick_t before = rtos_sched_get_tick_count();
    s_command_status = (s_mode == TIMER_ORDER_IRQ_START)
        ? rtos_timer_stop(timer) : rtos_timer_start(timer);
    s_command_ticks = rtos_sched_get_tick_count() - before;
    if (rtos_sem_give(&s_done) != RTOS_OK) {
        s_callback_error = true;
    }
}

void EXTI2_IRQHandler(void)
{
    ++s_irq.entries;
    s_irq.is_expected_vector = __get_IPSR() == (uint32_t)EXTI2_IRQn + 16U;
    const timer_order_mode_t mode = s_mode;
#if F446_TIMER_ORDER_TEST_DELETE
    if (mode == TIMER_ORDER_IRQ_DELETE) {
        s_irq.first_status = rtos_timer_delete(&s_timer);
    } else
#endif
    {
        s_irq.first_status = (mode == TIMER_ORDER_IRQ_START)
            ? rtos_timer_start(&s_timer) : rtos_timer_stop(&s_timer);
    }
    const uint32_t fill_base = s_fill_base;
    uint32_t accepted = (s_irq.first_status == RTOS_OK) ? 1U : 0U;
    for (uint32_t index = 1U; index < TIMER_ORDER_QUEUE_CAPACITY; ++index) {
        if (rtos_timer_change_period(&s_filler, fill_base + index) == RTOS_OK) {
            ++accepted;
        }
    }
    s_irq.accepted = accepted;
    s_irq.full_status = rtos_timer_change_period(&s_filler, fill_base + TIMER_ORDER_QUEUE_CAPACITY);
    s_irq.release_status = rtos_sem_give(&s_release);
}

static bool is_callback_waiting(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    const rtos_tcb_t *task = s_callback_task;
    const bool is_waiting = task != NULL && task->state == RTOS_TASK_DELAYED &&
        task->blocked_on == &s_release && s_release.wait_head == &task->wait_node;
    RTOS_PORT_EXIT_CRITICAL();
    return is_waiting;
}

static bool check_order(timer_order_mode_t mode, uint32_t *observed_calls)
{
    s_mode = mode;
    s_fill_base += TIMER_ORDER_QUEUE_CAPACITY;
    s_calls = 0U;
    s_callback_task = NULL;
    s_wait_status = RTOS_ERR_TIMEOUT;
    s_command_status = RTOS_ERR_TIMEOUT;
    s_command_ticks = UINT32_MAX;
    s_irq.entries = 0U;
    s_irq.accepted = 0U;
    s_irq.first_status = RTOS_ERR_TIMEOUT;
    s_irq.full_status = RTOS_OK;
    s_irq.release_status = RTOS_ERR_TIMEOUT;
    s_irq.is_expected_vector = false;

    if (!record_check("timer_callback_enters",
                      rtos_timer_start(&s_timer) == RTOS_OK &&
                      rtos_sem_take(&s_entered, TIMER_ORDER_WAIT_TICKS) == RTOS_OK)) {
        return false;
    }
    rtos_task_delay(1U);
    if (!record_check("service_waits_on_callback_semaphore", is_callback_waiting())) {
        return false;
    }

    NVIC_SetPendingIRQ(EXTI2_IRQn);
    __DSB();
    __ISB();
    if (!record_check("callback_control_returns",
                      rtos_sem_take(&s_done, TIMER_ORDER_WAIT_TICKS) == RTOS_OK)) {
        return false;
    }
    (void)record_check("irq_burst_fills_queue_and_releases_callback",
                       s_irq.entries == 1U && s_irq.is_expected_vector &&
                       s_irq.first_status == RTOS_OK &&
                       s_irq.accepted == TIMER_ORDER_QUEUE_CAPACITY &&
                       s_irq.full_status == RTOS_ERR_TIMEOUT && s_irq.release_status == RTOS_OK);
    rtos_status_t expected_status = RTOS_OK;
    const char *status_name = "callback_command_is_synchronous";
#if F446_TIMER_ORDER_TEST_DELETE
    if (mode == TIMER_ORDER_IRQ_DELETE) {
        expected_status = RTOS_ERR_PARAM;
        status_name = "callback_start_rejects_earlier_delete";
    }
#endif
    (void)record_check(status_name, s_wait_status == RTOS_OK &&
                       s_command_status == expected_status && s_command_ticks <= 1U);

    /* The controller has higher priority, so block to let the service finish. */
    rtos_task_delay(TIMER_ORDER_SETTLE_TICKS);
    *observed_calls = s_calls;
    const uint32_t expected_calls = (mode == TIMER_ORDER_IRQ_STOP) ? 2U : 1U;
    const char *name = (mode == TIMER_ORDER_IRQ_START) ? "older_start_before_callback_stop"
                                                    : "older_stop_before_callback_start";
    bool has_expected_state = s_timer.is_active == 0U;
#if F446_TIMER_ORDER_TEST_DELETE
    if (mode == TIMER_ORDER_IRQ_DELETE) {
        name = "deleted_timer_remains_inactive";
        has_expected_state = has_expected_state && s_timer.is_initialized == 0U;
    }
#endif
    (void)record_check(name, *observed_calls == expected_calls && has_expected_state);
    const uint32_t final_period = s_fill_base + TIMER_ORDER_QUEUE_CAPACITY - 1U;
    (void)record_check("filler_queue_drains_in_order", s_filler.period == final_period &&
                       !s_filler_called);
    return true;
}

void f446_timer_command_order_run(void *argument)
{
    (void)argument;
    const bool setup =
        rtos_sem_init(&s_entered, 0U, 1U) == RTOS_OK &&
        rtos_sem_init(&s_release, 0U, 1U) == RTOS_OK &&
        rtos_sem_init(&s_done, 0U, 1U) == RTOS_OK &&
        rtos_timer_create(&s_timer, "order-probe", timer_callback, &s_timer,
                          TIMER_ORDER_PERIOD, RTOS_TIMER_PERIODIC) == RTOS_OK &&
        rtos_timer_create(&s_filler, "order-filler", filler_callback, NULL,
                          TIMER_ORDER_FILL_PERIOD, RTOS_TIMER_ONE_SHOT) == RTOS_OK;
    uint32_t completed = 0U;
    uint32_t stop_calls = 0U;
    uint32_t start_calls = 0U;
#if F446_TIMER_ORDER_TEST_DELETE
    uint32_t delete_calls = 0U;
#endif

    if (record_check("setup", setup)) {
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
        NVIC_SetPriority(EXTI2_IRQn, RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >>
                                     (8U - __NVIC_PRIO_BITS));
        NVIC_EnableIRQ(EXTI2_IRQn);
        for (uint32_t round = 0U; round < F446_TIMER_ORDER_ROUNDS; ++round) {
            uint32_t stop = 0U;
            uint32_t start = 0U;
            if (!check_order(TIMER_ORDER_IRQ_START, &stop) ||
                !check_order(TIMER_ORDER_IRQ_STOP, &start)) {
                break;
            }
            stop_calls += stop;
            start_calls += start;
            ++completed;
        }
#if F446_TIMER_ORDER_TEST_DELETE
        if (completed == F446_TIMER_ORDER_ROUNDS) {
            (void)check_order(TIMER_ORDER_IRQ_DELETE, &delete_calls);
        }
#endif
        NVIC_DisableIRQ(EXTI2_IRQn);
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
        (void)record_check("callback_context_and_mask_restored",
                           !s_callback_error && rtos_port_get_critical_nesting() == 0U &&
                           __get_BASEPRI() == 0U && __get_PRIMASK() == 0U);
        (void)record_check("all_rounds_completed", completed == F446_TIMER_ORDER_ROUNDS);
    }
#if F446_TIMER_ORDER_TEST_DELETE
    (void)printf("F4TI|DELETE|calls=%lu|command_status=%ld\r\n",
                 (unsigned long)delete_calls, (long)s_command_status);
#endif
    (void)printf("F4TI|DONE|status=%u|checks=%lu|failures=%lu|rounds=%lu"
                 "|stop_calls=%lu|start_calls=%lu\r\n", s_failures == 0U ? 0U : 1U,
                 (unsigned long)s_checks, (unsigned long)s_failures, (unsigned long)completed,
                 (unsigned long)stop_calls, (unsigned long)start_calls);
}

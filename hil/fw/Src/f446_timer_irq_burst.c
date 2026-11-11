/** @file f446_timer_irq_burst.c
 * @brief Measure the timer service's outer critical sections during IRQ command bursts.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"
#include "rtos.h"

#if !defined(RTOS_HIL_CRITICAL_TEST)
#error The timer IRQ burst fixture requires RTOS_HIL_CRITICAL_TEST
#endif

enum {
    TIMER_BURST_MAX_ACTIVE = 128U,
    TIMER_BURST_COMMANDS = 16U,
    TIMER_BURST_ROUNDS = 8U,
    TIMER_BURST_SETTLE_TICKS = 3U,
    TIMER_BURST_READY_TIMEOUT = 50U,
    TIMER_BURST_BASE_PERIOD = 0x10000000U,
    TIMER_BURST_PERIOD_STRIDE = 4096U
};

typedef struct timer_burst_metrics {
    _Atomic uint32_t entry_cycles;
    _Atomic uint32_t max_cycles;
    _Atomic uint32_t sections;
    _Atomic bool has_entry;
} timer_burst_metrics_t;

static rtos_timer_t s_timers[TIMER_BURST_MAX_ACTIVE];
static rtos_timer_t s_probe;
static rtos_sem_t s_probe_done;
static _Atomic(rtos_tcb_t *) s_service_task;
static rtos_tcb_t *s_sender_task;
static _Atomic bool s_measure;
static _Atomic bool s_saw_blocked_sender;
static _Atomic bool s_callback_error;
static _Atomic uint32_t s_irq_entries;
static _Atomic uint32_t s_accepted;
static _Atomic uint32_t s_rejected;
static _Atomic bool s_irq_error;
static _Atomic uint32_t s_first_target;
static _Atomic rtos_tick_t s_next_period;
static timer_burst_metrics_t s_metrics;
static uint32_t s_task_sends;
static uint32_t s_checks;
static uint32_t s_failures;

void f446_timer_irq_burst_run(void *argument);
void EXTI2_IRQHandler(void);

void rtos_port_critical_test_hook(uint32_t stage)
{
    if (!s_measure || __get_IPSR() != 0U || rtos_task_get_current() != s_service_task ||
        rtos_port_get_critical_nesting() != 0U) {
        return;
    }
    if (stage == RTOS_PORT_TEST_ENTER_MASKED) {
        s_metrics.entry_cycles = DWT->CYCCNT;
        s_metrics.has_entry = true;
        if (!s_saw_blocked_sender && s_sender_task->state == RTOS_TASK_DELAYED &&
            s_sender_task->blocked_on != NULL) {
            s_saw_blocked_sender = true;
        }
    } else if (stage == RTOS_PORT_TEST_EXIT_COUNTED && s_metrics.has_entry) {
        const uint32_t elapsed = DWT->CYCCNT - s_metrics.entry_cycles;
        if (elapsed > s_metrics.max_cycles) {
            s_metrics.max_cycles = elapsed;
        }
        ++s_metrics.sections;
        s_metrics.has_entry = false;
    }
}

void EXTI2_IRQHandler(void)
{
    if (__get_IPSR() != (uint32_t)EXTI2_IRQn + 16U) {
        s_irq_error = true;
    }
    ++s_irq_entries;
    const uint32_t first = s_first_target;
    const rtos_tick_t period = s_next_period;

    /* One command hands off to the waiting service and the rest fill the queue. */
    if (rtos_timer_change_period(&s_timers[first], period) == RTOS_OK) {
        ++s_accepted;
    }
    for (uint32_t index = 0U; index < TIMER_BURST_COMMANDS; ++index) {
        if (rtos_timer_change_period(&s_timers[first + index], period + index + 1U) == RTOS_OK) {
            ++s_accepted;
        }
    }
    if (rtos_timer_change_period(&s_timers[first], period + TIMER_BURST_COMMANDS + 1U) ==
        RTOS_ERR_TIMEOUT) {
        ++s_rejected;
    }
}

static bool record_check(const char *name, bool is_ok)
{
    ++s_checks;
    if (!is_ok) {
        ++s_failures;
        (void)printf("F4TB|CHECK|%s|FAIL\r\n", name);
    }
    return is_ok;
}

static void unexpected_callback(void *argument)
{
    (void)argument;
    s_callback_error = true;
}

static void identify_service(void *argument)
{
    (void)argument;
    s_service_task = rtos_task_get_current();
    if (rtos_sem_give(&s_probe_done) != RTOS_OK) {
        s_callback_error = true;
    }
}

static bool prepare_timers(void)
{
    if (rtos_sem_init(&s_probe_done, 0U, 1U) != RTOS_OK ||
        rtos_timer_create(&s_probe, "burst-service-probe", identify_service, NULL,
                          1U, RTOS_TIMER_ONE_SHOT) != RTOS_OK ||
        rtos_timer_start(&s_probe) != RTOS_OK ||
        rtos_sem_take(&s_probe_done, TIMER_BURST_READY_TIMEOUT) != RTOS_OK) {
        return false;
    }
    for (uint32_t index = 0U; index < TIMER_BURST_MAX_ACTIVE; ++index) {
        if (rtos_timer_create(&s_timers[index], "burst-active", unexpected_callback, NULL,
                              TIMER_BURST_BASE_PERIOD, RTOS_TIMER_ONE_SHOT) != RTOS_OK) {
            return false;
        }
    }
    return true;
}

static bool activate_timers(uint32_t previous, uint32_t active)
{
    for (uint32_t index = previous; index < active; ++index) {
        if (rtos_timer_start(&s_timers[index]) != RTOS_OK) {
            return false;
        }
    }
    rtos_task_delay(TIMER_BURST_SETTLE_TICKS);
    for (uint32_t index = 0U; index < active; ++index) {
        if (s_timers[index].is_active == 0U) {
            return false;
        }
    }
    return true;
}

static bool service_is_waiting(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    const rtos_tcb_t *task = s_service_task;
    const bool is_waiting = task != NULL && task->state == RTOS_TASK_DELAYED &&
        task->blocked_on != NULL && task->wait_node.list_head != NULL;
    RTOS_PORT_EXIT_CRITICAL();
    return is_waiting;
}

static bool run_burst(uint32_t first, rtos_tick_t period)
{
    const uint32_t expected_count = s_irq_entries + 1U;
    const uint32_t expected_accepted = s_accepted + TIMER_BURST_COMMANDS + 1U;
    const uint32_t expected_rejected = s_rejected + 1U;
    if (!service_is_waiting()) {
        return false;
    }

    s_first_target = first;
    s_next_period = period;
    s_saw_blocked_sender = false;
    s_measure = true;
    NVIC_SetPendingIRQ(EXTI2_IRQn);
    __DSB();
    __ISB();

    /* The service is ready at a lower priority. With its queue full, this
     * sender blocks and only enters the ring when the service pops a slot. */
    const rtos_tick_t sender_period = period + TIMER_BURST_COMMANDS + 2U;
    const rtos_status_t sent = rtos_timer_change_period(&s_timers[first], sender_period);
    rtos_task_delay(TIMER_BURST_SETTLE_TICKS);
    s_measure = false;

    if (s_irq_entries != expected_count || s_accepted != expected_accepted ||
        s_rejected != expected_rejected || s_irq_error || s_metrics.has_entry ||
        sent != RTOS_OK || !s_saw_blocked_sender) {
        return false;
    }
    ++s_task_sends;
    for (uint32_t index = 0U; index < TIMER_BURST_COMMANDS; ++index) {
        const rtos_timer_t *timer = &s_timers[first + index];
        const rtos_tick_t expected_period = index == 0U ? sender_period : period + index + 1U;
        if (timer->period != expected_period || timer->is_active == 0U) {
            return false;
        }
    }
    return true;
}

static bool run_population(uint32_t previous, uint32_t active, rtos_tick_t *period)
{
    if (!record_check("activate_population", activate_timers(previous, active))) {
        return false;
    }
    s_irq_entries = 0U;
    s_accepted = 0U;
    s_rejected = 0U;
    s_task_sends = 0U;
    s_metrics.max_cycles = 0U;
    s_metrics.sections = 0U;
    for (uint32_t round = 0U; round < TIMER_BURST_ROUNDS; ++round) {
        *period += TIMER_BURST_PERIOD_STRIDE;
        if (!record_check("irq_burst_drains_in_order",
                          run_burst(active - TIMER_BURST_COMMANDS, *period))) {
            return false;
        }
    }
    const uint32_t max_cycles = s_metrics.max_cycles;
    const uint32_t sections = s_metrics.sections;
    (void)printf("F4TB|LOAD|active=%lu|irq=%lu|accepted=%lu|full=%lu|task_sends=%lu"
                 "|sections=%lu|max_mask_cycles=%lu\r\n", (unsigned long)active,
                 (unsigned long)s_irq_entries, (unsigned long)s_accepted,
                 (unsigned long)s_rejected, (unsigned long)s_task_sends,
                 (unsigned long)sections, (unsigned long)max_cycles);
    return record_check("measured_timer_service_sections", sections > 0U && max_cycles > 0U);
}

void f446_timer_irq_burst_run(void *argument)
{
    (void)argument;
    static const uint32_t populations[] = {32U, 96U, TIMER_BURST_MAX_ACTIVE};
    uint32_t previous = 0U;
    rtos_tick_t period = TIMER_BURST_BASE_PERIOD;
    s_sender_task = rtos_task_get_current();
    const bool ready = s_sender_task != NULL && (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U &&
        prepare_timers();

    if (record_check("setup", ready)) {
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
        NVIC_SetPriority(EXTI2_IRQn, RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >>
                                     (8U - __NVIC_PRIO_BITS));
        NVIC_EnableIRQ(EXTI2_IRQn);
        for (uint32_t index = 0U; index < sizeof(populations) / sizeof(populations[0]); ++index) {
            if (!run_population(previous, populations[index], &period)) {
                break;
            }
            previous = populations[index];
        }
        NVIC_DisableIRQ(EXTI2_IRQn);
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
        (void)record_check("all_populations_completed", previous == TIMER_BURST_MAX_ACTIVE);
        (void)record_check("mask_restored_and_no_early_callback",
                           !s_callback_error && rtos_port_get_critical_nesting() == 0U &&
                           __get_BASEPRI() == 0U && __get_PRIMASK() == 0U);
    }
    (void)printf("F4TB|DONE|status=%u|checks=%lu|failures=%lu|clock_hz=%lu\r\n",
                 s_failures == 0U ? 0U : 1U, (unsigned long)s_checks, (unsigned long)s_failures,
                 (unsigned long)SystemCoreClock);
}

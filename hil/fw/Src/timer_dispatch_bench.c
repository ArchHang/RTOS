/**
 * @file timer_dispatch_bench.c
 * @brief Measure timer service dispatch from the command drain to its next wait.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"

#include "rtos.h"

enum {
    TIMER_DISPATCH_SAMPLES = 64U,
    TIMER_DISPATCH_RETRIES = 8U,
    TIMER_DISPATCH_SETTLE_TICKS = 2U,
    TIMER_DISPATCH_PERIOD_TICKS = 32U
};

typedef enum timer_dispatch_mode {
    TIMER_DISPATCH_INACTIVE = 0,
    TIMER_DISPATCH_FOREVER,
    TIMER_DISPATCH_DEADLINE
} timer_dispatch_mode_t;

typedef struct timer_dispatch_measurement {
    uint64_t sum;
    uint32_t samples;
    uint32_t skipped;
    uint32_t invalid;
    uint32_t minimum;
    uint32_t maximum;
    rtos_tick_t deadline;
} timer_dispatch_measurement_t;

/* The timer task owns the queue identity and start marker. Statistics, the
 * measurement mode and callback results are shared with the audit task under
 * the kernel critical section. */
static rtos_queue_t *timer_queue;
static uint32_t start_cycle;
static rtos_tick_t start_tick;
static bool is_measurement_pending;

static timer_dispatch_mode_t measurement_mode;
static timer_dispatch_measurement_t measurement;
static rtos_timer_t bench_timer;
static rtos_status_t setup_status;
static rtos_tick_t callback_tick;
static uint32_t callback_count;
static uint32_t pass_count;
static uint32_t fail_count;

/* These symbol names are the GNU linker --wrap protocol. */
rtos_status_t real_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout)
    __asm__("__real_rtos_queue_recv");
rtos_status_t wrap_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout)
    __asm__("__wrap_rtos_queue_recv");
rtos_status_t real_queue_recv_until(rtos_queue_t *queue, void *item, rtos_tick_t deadline)
    __asm__("__real_rtos_internal_queue_recv_until");
rtos_status_t wrap_queue_recv_until(rtos_queue_t *queue, void *item, rtos_tick_t deadline)
    __asm__("__wrap_rtos_internal_queue_recv_until");
uint32_t real_queue_count(const rtos_queue_t *queue) __asm__("__real_rtos_queue_get_count");
uint32_t wrap_queue_count(const rtos_queue_t *queue) __asm__("__wrap_rtos_queue_get_count");

static void discover_timer_queue(rtos_queue_t *queue)
{
    if (timer_queue == NULL) {
        const rtos_tcb_t *current = rtos_task_get_current();
        if ((current != NULL) && (strcmp(current->name, "TIMER") == 0)) {
            timer_queue = queue;
        }
    }
}

static void finish_measurement(timer_dispatch_mode_t observed_mode, rtos_tick_t deadline,
                               uint32_t end_cycle)
{
    if (!is_measurement_pending) {
        return;
    }
    is_measurement_pending = false;
    const rtos_tick_t end_tick = rtos_sched_get_tick_count();
    const bool is_unmasked = (rtos_port_get_interrupt_mask() == 0U)
        && (rtos_port_get_critical_nesting() == 0U);
    const uint32_t elapsed = end_cycle - start_cycle;

    RTOS_PORT_ENTER_CRITICAL();
    const rtos_tick_t remaining = deadline - end_tick;
    const bool is_deadline_valid = (bench_timer.is_active != 0U)
        && (bench_timer.expire_tick == deadline) && (remaining != 0U)
        && (remaining <= TIMER_DISPATCH_PERIOD_TICKS);
    if ((measurement_mode != observed_mode) || !is_unmasked || (elapsed == 0U)
        || ((observed_mode == TIMER_DISPATCH_DEADLINE) && !is_deadline_valid)
        || ((observed_mode == TIMER_DISPATCH_FOREVER) && (bench_timer.is_active != 0U))) {
        ++measurement.invalid;
    } else if (start_tick != end_tick) {
        ++measurement.skipped;
    } else {
        ++measurement.samples;
        measurement.sum += elapsed;
        if (elapsed < measurement.minimum) {
            measurement.minimum = elapsed;
        }
        if (elapsed > measurement.maximum) {
            measurement.maximum = elapsed;
        }
        measurement.deadline = deadline;
    }
    RTOS_PORT_EXIT_CRITICAL();
}

static inline void mark_empty_drain(void)
{
    if (measurement_mode != TIMER_DISPATCH_INACTIVE) {
        start_tick = rtos_sched_get_tick_count();
        is_measurement_pending = true;
        start_cycle = DWT->CYCCNT;
    }
}

rtos_status_t wrap_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout)
{
    const uint32_t end_cycle = DWT->CYCCNT;
    discover_timer_queue(queue);
    if ((queue == timer_queue) && (timeout == RTOS_WAIT_FOREVER)) {
        finish_measurement(TIMER_DISPATCH_FOREVER, 0U, end_cycle);
    }

    const rtos_status_t status = real_queue_recv(queue, item, timeout);
    /* Frozen implementations used an empty receive instead of a count snapshot. */
    if (queue == timer_queue && timeout == RTOS_NO_WAIT && status == RTOS_ERR_TIMEOUT) {
        mark_empty_drain();
    }
    return status;
}

uint32_t wrap_queue_count(const rtos_queue_t *queue)
{
    const uint32_t count = real_queue_count(queue);
    if (queue == timer_queue && count == 0U) {
        mark_empty_drain();
    }
    return count;
}

rtos_status_t wrap_queue_recv_until(rtos_queue_t *queue, void *item, rtos_tick_t deadline)
{
    const uint32_t end_cycle = DWT->CYCCNT;
    discover_timer_queue(queue);
    if (queue == timer_queue) {
        finish_measurement(TIMER_DISPATCH_DEADLINE, deadline, end_cycle);
    }
    return real_queue_recv_until(queue, item, deadline);
}

static void record_result(const char *name, bool is_valid)
{
    if (is_valid) {
        ++pass_count;
    } else {
        ++fail_count;
    }
    (void)printf("TDB|%s|%s\r\n", name, is_valid ? "PASS" : "FAIL");
}

static void on_expiry(void *argument)
{
    (void)argument;
    RTOS_PORT_ENTER_CRITICAL();
    callback_tick = rtos_sched_get_tick_count();
    ++callback_count;
    RTOS_PORT_EXIT_CRITICAL();
}

static bool measure_dispatch(timer_dispatch_mode_t mode, rtos_tick_t *deadline)
{
    timer_dispatch_measurement_t result = { 0 };
    bool is_valid = true;

    RTOS_PORT_ENTER_CRITICAL();
    measurement = (timer_dispatch_measurement_t){ .minimum = UINT32_MAX };
    measurement_mode = mode;
    RTOS_PORT_EXIT_CRITICAL();

    const uint32_t limit = TIMER_DISPATCH_SAMPLES + TIMER_DISPATCH_RETRIES;
    for (uint32_t attempt = 0U; attempt < limit; ++attempt) {
        const rtos_status_t status = (mode == TIMER_DISPATCH_DEADLINE)
            ? rtos_timer_start(&bench_timer) : rtos_timer_stop(&bench_timer);
        if (status != RTOS_OK) {
            is_valid = false;
            break;
        }
        rtos_task_delay(TIMER_DISPATCH_SETTLE_TICKS);

        RTOS_PORT_ENTER_CRITICAL();
        result = measurement;
        RTOS_PORT_EXIT_CRITICAL();
        if ((result.invalid != 0U) || ((result.samples + result.skipped) != (attempt + 1U))) {
            is_valid = false;
            break;
        }
        if (result.samples == TIMER_DISPATCH_SAMPLES) {
            break;
        }
    }

    RTOS_PORT_ENTER_CRITICAL();
    measurement_mode = TIMER_DISPATCH_INACTIVE;
    RTOS_PORT_EXIT_CRITICAL();
    *deadline = result.deadline;
    is_valid = is_valid && (result.samples == TIMER_DISPATCH_SAMPLES);
    const char *name = (mode == TIMER_DISPATCH_DEADLINE) ? "deadline" : "forever";
    const uint32_t average = (result.samples != 0U)
        ? (uint32_t)(result.sum / result.samples) : 0U;
    (void)printf("TDB|DISPATCH|%s|mode=%s|N=%lu|avg=%lu|min=%lu|max=%lu"
                 "|skipped=%lu|invalid=%lu\r\n",
                 is_valid ? "PASS" : "FAIL", name, (unsigned long)result.samples,
                 (unsigned long)average, (unsigned long)result.minimum,
                 (unsigned long)result.maximum, (unsigned long)result.skipped,
                 (unsigned long)result.invalid);
    return is_valid;
}

void audit_pre_start(void)
{
    setup_status = rtos_timer_create(&bench_timer, "dispatch", on_expiry, NULL,
                                     TIMER_DISPATCH_PERIOD_TICKS, RTOS_TIMER_ONE_SHOT);
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    const bool is_ready = (setup_status == RTOS_OK);
    rtos_tick_t deadline = 0U;
    record_result("SETUP", is_ready);
    record_result("FOREVER_DISPATCH",
                  is_ready && measure_dispatch(TIMER_DISPATCH_FOREVER, &deadline));
    const bool has_deadline = is_ready && measure_dispatch(TIMER_DISPATCH_DEADLINE, &deadline);
    record_result("DEADLINE_DISPATCH", has_deadline);

    RTOS_PORT_ENTER_CRITICAL();
    const bool has_no_early_callback = (callback_count == 0U);
    RTOS_PORT_EXIT_CRITICAL();
    record_result("NO_EARLY_CALLBACK", is_ready && has_no_early_callback);

    if (has_deadline) {
        rtos_task_delay(TIMER_DISPATCH_PERIOD_TICKS + TIMER_DISPATCH_SETTLE_TICKS);
    }
    RTOS_PORT_ENTER_CRITICAL();
    const bool has_exact_callback = (callback_count == 1U) && (callback_tick == deadline);
    RTOS_PORT_EXIT_CRITICAL();
    record_result("EXACT_DEADLINE_CALLBACK", has_deadline && has_exact_callback);

    const bool is_delete_queued = is_ready && (rtos_timer_delete(&bench_timer) == RTOS_OK);
    if (is_delete_queued) {
        rtos_task_delay(TIMER_DISPATCH_SETTLE_TICKS);
    }
    RTOS_PORT_ENTER_CRITICAL();
    const bool is_deleted = (bench_timer.is_initialized == 0U) && (bench_timer.is_active == 0U);
    RTOS_PORT_EXIT_CRITICAL();
    record_result("DELETED", is_delete_queued && is_deleted);
    (void)printf("TDB|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)pass_count,
                 (unsigned long)fail_count);

    for (;;) {
        rtos_task_delay(1000U);
    }
}

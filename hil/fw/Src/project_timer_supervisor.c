/**
 * @file project_timer_supervisor.c
 * @brief Exercise a renewable output lease and background software timers on F446.
 *
 * The NUCLEO LD2 GPIO represents an output controlled by a lease. A timer callback
 * posts expiry to its owner task. A link-time wrapper releases a higher-priority
 * task at the timer service's wait boundary to reproduce a precise preemption.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "rtos.h"

enum {
    OUTPUT_PRIORITY = 7U,
    LOAD_PRIORITY = 6U,
    WORKER_STACK_WORDS = 256U,
    MESSAGE_CAPACITY = 4U,
    LEASE_TICKS = 40U,
    LOAD_TICKS = 55U,
    DISPATCH_TICKS = 4U,
    RENEW_AFTER_TICKS = 18U,
    WAIT_TICKS = 200U,
    HOUSEKEEPING_TICKS = 7U,
    HOUSEKEEPING_LIMIT = 3U
};

typedef enum output_command {
    OUTPUT_RENEW,
    OUTPUT_EXPIRE
} output_command_t;

typedef struct output_report {
    rtos_tick_t tick;
    rtos_status_t status;
    GPIO_PinState pin;
    output_command_t command;
} output_report_t;

static rtos_timer_t lease_timer;
static rtos_timer_t housekeeping_timer;
static rtos_queue_t command_queue;
static rtos_queue_t report_queue;
static rtos_sem_t load_gate;
static output_command_t command_storage[MESSAGE_CAPACITY];
static output_report_t report_storage[MESSAGE_CAPACITY];
static rtos_tcb_t output_tcb;
static rtos_tcb_t load_tcb;
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t output_stack[WORKER_STACK_WORDS];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t load_stack[WORKER_STACK_WORDS];
static rtos_status_t setup_status;
static volatile bool is_load_armed;
static volatile uint32_t load_count;
static volatile rtos_status_t load_release_status;
static volatile rtos_tick_t load_start_tick;
static volatile rtos_tick_t load_end_tick;
static volatile uint32_t expiry_count;
static volatile uint32_t callback_errors;
static volatile uint32_t report_errors;
static volatile uint32_t housekeeping_count;
static volatile rtos_status_t housekeeping_stop_status = RTOS_ERR_SCHED;
static uint32_t pass_count;
static uint32_t fail_count;

rtos_status_t __real_rtos_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout);
rtos_status_t __wrap_rtos_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout);
rtos_status_t __real_rtos_internal_queue_recv_until(rtos_queue_t *queue, void *item,
                                                   rtos_tick_t deadline) __attribute__((weak));
rtos_status_t __wrap_rtos_internal_queue_recv_until(rtos_queue_t *queue, void *item,
                                                   rtos_tick_t deadline);

static void record_result(const char *name, bool is_valid, int32_t actual, int32_t expected)
{
    if (is_valid) {
        ++pass_count;
    } else {
        ++fail_count;
    }
    (void)printf("SUP|%s|%s|actual=%ld|expect=%ld\r\n", name,
                 is_valid ? "PASS" : "FAIL", (long)actual, (long)expected);
}

static void record_status(const char *name, rtos_status_t actual, rtos_status_t expected)
{
    record_result(name, actual == expected, (int32_t)actual, (int32_t)expected);
}

static void lease_expired(void *argument)
{
    (void)argument;
    const output_command_t command = OUTPUT_EXPIRE;
    ++expiry_count;
    if (rtos_queue_send(&command_queue, &command, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK) {
        ++callback_errors;
        HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
    }
}

static void run_housekeeping(void *argument)
{
    (void)argument;
    ++housekeeping_count;
    if (housekeeping_count == HOUSEKEEPING_LIMIT) {
        housekeeping_stop_status = rtos_timer_stop(&housekeeping_timer);
    }
}

static void run_output(void *argument)
{
    (void)argument;
    for (;;) {
        output_command_t command;
        if (rtos_queue_recv(&command_queue, &command, RTOS_WAIT_FOREVER) != RTOS_OK) {
            ++report_errors;
            rtos_task_delay(1U);
            continue;
        }

        output_report_t report = { .command = command, .status = RTOS_OK };
        GPIO_PinState pin = GPIO_PIN_RESET;
        if (command == OUTPUT_RENEW) {
            report.status = rtos_timer_start(&lease_timer);
            if (report.status == RTOS_OK) {
                pin = GPIO_PIN_SET;
            }
        }
        HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, pin);
        report.tick = rtos_sched_get_tick_count();
        report.pin = HAL_GPIO_ReadPin(LD2_GPIO_Port, LD2_Pin);
        if (rtos_queue_send(&report_queue, &report, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK) {
            ++report_errors;
        }
    }
}

static void run_cpu_load(void *argument)
{
    (void)argument;
    if (rtos_sem_take(&load_gate, WAIT_TICKS) != RTOS_OK) {
        return;
    }
    const rtos_tick_t start = rtos_sched_get_tick_count();
    load_start_tick = start;
    ++load_count;
    while ((rtos_sched_get_tick_count() - start) < LOAD_TICKS) {
        /* SysTick remains enabled while the higher-priority task owns the CPU. */
    }
    load_end_tick = rtos_sched_get_tick_count();
}

static void release_load_at_timer_wait(rtos_tick_t remaining)
{
    if (is_load_armed && (remaining > 0U) && (remaining <= LEASE_TICKS)) {
        const rtos_tcb_t *current = rtos_task_get_current();
        if ((current != NULL) && (strcmp(current->name, "TIMER") == 0)) {
            is_load_armed = false;
            load_release_status = rtos_sem_give(&load_gate);
        }
    }
}

/**
 * @brief Preempt a timer service that is about to receive with a relative wait.
 * @param[in] queue Real receiver's queue.
 * @param[out] item Real receiver's destination.
 * @param[in] timeout Real receiver's relative timeout in ticks.
 * @return The result from the real receive call.
 */
rtos_status_t __wrap_rtos_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout)
{
    release_load_at_timer_wait(timeout);
    return __real_rtos_queue_recv(queue, item, timeout);
}

/**
 * @brief Preempt a timer service that is about to receive with an absolute wait.
 * @param[in] queue Real receiver's queue.
 * @param[out] item Real receiver's destination.
 * @param[in] deadline Real receiver's absolute deadline in ticks.
 * @return The result from the real receive call.
 */
rtos_status_t __wrap_rtos_internal_queue_recv_until(rtos_queue_t *queue, void *item,
                                                   rtos_tick_t deadline)
{
    if (__real_rtos_internal_queue_recv_until == NULL) {
        return RTOS_ERR_SCHED;
    }
    release_load_at_timer_wait(deadline - rtos_sched_get_tick_count());
    return __real_rtos_internal_queue_recv_until(queue, item, deadline);
}

static rtos_status_t prepare_objects(void)
{
    rtos_status_t status = rtos_queue_init(&command_queue, command_storage,
                                          sizeof(command_storage[0]), MESSAGE_CAPACITY);
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_queue_init(&report_queue, report_storage, sizeof(report_storage[0]),
                             MESSAGE_CAPACITY);
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_sem_init(&load_gate, 0U, 1U);
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_timer_create(&lease_timer, "output-lease", lease_expired, NULL, LEASE_TICKS,
                               RTOS_TIMER_ONE_SHOT);
    if (status != RTOS_OK) {
        return status;
    }
    return rtos_timer_create(&housekeeping_timer, "supervisor-hk", run_housekeeping, NULL,
                             HOUSEKEEPING_TICKS, RTOS_TIMER_PERIODIC);
}

static rtos_status_t submit_renewal(output_report_t *report)
{
    const output_command_t command = OUTPUT_RENEW;
    rtos_status_t status = rtos_queue_send(&command_queue, &command, WAIT_TICKS, RTOS_FALSE);
    if (status == RTOS_OK) {
        status = rtos_queue_recv(&report_queue, report, WAIT_TICKS);
    }
    return status;
}

static bool is_output_enabled(const output_report_t *report)
{
    return (report->status == RTOS_OK) && (report->command == OUTPUT_RENEW)
           && (report->pin == GPIO_PIN_SET);
}

static bool is_output_expired(const output_report_t *report)
{
    return (report->command == OUTPUT_EXPIRE) && (report->pin == GPIO_PIN_RESET);
}

static void verify_expired_during_preemption(void)
{
    output_report_t enabled = { 0 };
    output_report_t expired = { 0 };
    is_load_armed = true;
    const rtos_status_t enable_status = submit_renewal(&enabled);
    const rtos_status_t expire_status = rtos_queue_recv(&report_queue, &expired, WAIT_TICKS);
    is_load_armed = false;

    record_status("grant_report", enable_status, RTOS_OK);
    record_result("grant_enables_gpio", is_output_enabled(&enabled), (int32_t)enabled.pin,
                  (int32_t)GPIO_PIN_SET);
    record_status("load_gate_released", load_release_status, RTOS_OK);
    record_result("load_runs_once", load_count == 1U, (int32_t)load_count, 1);
    const rtos_tick_t load_duration = load_end_tick - load_start_tick;
    record_result("preemption_crosses_deadline", load_duration >= LOAD_TICKS,
                  (int32_t)load_duration, LOAD_TICKS);
    record_status("expired_report", expire_status, RTOS_OK);
    record_result("expiry_disables_gpio", is_output_expired(&expired), (int32_t)expired.pin,
                  (int32_t)GPIO_PIN_RESET);
    if ((expire_status == RTOS_OK) && (load_count == 1U)) {
        const rtos_tick_t dispatch = expired.tick - load_end_tick;
        record_result("no_second_sleep_after_preemption", dispatch <= DISPATCH_TICKS,
                      (int32_t)dispatch, DISPATCH_TICKS);
    } else {
        record_result("no_second_sleep_after_preemption", false, -1, DISPATCH_TICKS);
    }
}

static void verify_renewal_and_housekeeping(void)
{
    output_report_t first = { 0 };
    output_report_t renewed = { 0 };
    output_report_t expired = { 0 };
    const rtos_status_t housekeeping_status = rtos_timer_start(&housekeeping_timer);
    const rtos_status_t first_status = submit_renewal(&first);
    rtos_task_delay(RENEW_AFTER_TICKS);
    const rtos_status_t renewal_status = submit_renewal(&renewed);
    const rtos_status_t old_deadline_status = rtos_queue_recv(&report_queue, &expired, LEASE_TICKS);
    const rtos_status_t new_deadline_status = (old_deadline_status == RTOS_OK)
        ? RTOS_OK : rtos_queue_recv(&report_queue, &expired, WAIT_TICKS);
    rtos_task_delay(HOUSEKEEPING_TICKS * 2U);

    record_status("housekeeping_start", housekeeping_status, RTOS_OK);
    record_status("initial_lease_granted", first_status, RTOS_OK);
    record_status("renewed_lease_granted", renewal_status, RTOS_OK);
    record_result("renew_keeps_gpio_high", is_output_enabled(&first) && is_output_enabled(&renewed),
                  (int32_t)renewed.pin, (int32_t)GPIO_PIN_SET);
    record_status("renewed_lease_expires", new_deadline_status, RTOS_OK);
    const rtos_tick_t lease_elapsed = expired.tick - renewed.tick;
    record_result("only_new_deadline_expires",
                  is_output_expired(&expired) && (lease_elapsed >= LEASE_TICKS)
                  && (lease_elapsed <= (LEASE_TICKS + DISPATCH_TICKS)),
                  (int32_t)lease_elapsed, LEASE_TICKS);
    record_status("housekeeping_self_stop", housekeeping_stop_status, RTOS_OK);
    record_result("housekeeping_runs_exactly_three_times", housekeeping_count == HOUSEKEEPING_LIMIT,
                  (int32_t)housekeeping_count, HOUSEKEEPING_LIMIT);
    record_result("no_lost_reports_or_extra_expiries",
                  (callback_errors == 0U) && (report_errors == 0U) && (expiry_count == 2U),
                  (int32_t)expiry_count, 2);
}

/** @brief Initialize the private test queues and GPIO state before scheduling. */
void audit_pre_start(void)
{
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
    setup_status = prepare_objects();
}

/**
 * @brief Run the actuator lease supervisor fixture and emit its UART results.
 * @param[in] argument Unused audit task argument.
 */
void audit_regressions_task(void *argument)
{
    (void)argument;
    record_status("setup", setup_status, RTOS_OK);
    if (setup_status == RTOS_OK) {
        const rtos_status_t output_status = rtos_task_create_static(
            &output_tcb, output_stack, WORKER_STACK_WORDS, run_output, NULL, OUTPUT_PRIORITY,
            "supervisor-out");
        const rtos_status_t load_status = rtos_task_create_static(
            &load_tcb, load_stack, WORKER_STACK_WORDS, run_cpu_load, NULL, LOAD_PRIORITY,
            "supervisor-load");
        record_status("output_task", output_status, RTOS_OK);
        record_status("load_task", load_status, RTOS_OK);
        if ((output_status == RTOS_OK) && (load_status == RTOS_OK)) {
            verify_expired_during_preemption();
            verify_renewal_and_housekeeping();
            record_status("delete_lease_accepted", rtos_timer_delete(&lease_timer), RTOS_OK);
            record_status("delete_housekeeping_accepted",
                          rtos_timer_delete(&housekeeping_timer), RTOS_OK);
        }
        if (output_status == RTOS_OK) {
            record_status("delete_output_task", rtos_task_delete(&output_tcb), RTOS_OK);
        }
    }
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
    (void)printf("SUP|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)pass_count,
                 (unsigned long)fail_count);
}

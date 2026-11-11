/** @file h743_boundary_stress.c
 *  @brief Exercise scheduler and software timer limits on Cortex-M7.
 */
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"

#include "rtos.h"

#ifndef H743_BOUNDARY_ROUNDS
#define H743_BOUNDARY_ROUNDS 32U
#endif

#define BOUNDARY_MAX_FINITE UINT32_C(0x7fffffff)

enum {
    BOUNDARY_WAIT_CASES = 4U,
    BOUNDARY_STACK_WORDS = 192U,
    BOUNDARY_SHORT_WAIT = 64U,
    BOUNDARY_PERIOD = 3U,
    BOUNDARY_PERIODIC_FIRES = 4U,
    BOUNDARY_CALLBACK_WAIT = 250U,
    BOUNDARY_IRQ_BURSTS = 4U,
    BOUNDARY_COMMANDS_PER_IRQ = 5U,
    BOUNDARY_COMMAND_CAPACITY = 16U,
    BOUNDARY_COMMAND_WAIT = 100U
};

_Static_assert(H743_BOUNDARY_ROUNDS > 0U &&
               H743_BOUNDARY_ROUNDS <= UINT32_MAX / (BOUNDARY_IRQ_BURSTS * BOUNDARY_COMMANDS_PER_IRQ),
               "Boundary round count must fit aggregate counters");

typedef struct boundary_wait_case {
    rtos_sem_t gate;
    rtos_tcb_t task;
    rtos_tick_t timeout;
    _Atomic rtos_tick_t start_tick;
    _Atomic rtos_prio_t ran_at;
    _Atomic rtos_status_t result;
} boundary_wait_case_t;

typedef struct boundary_irq_result {
    _Atomic uint32_t entries;
    _Atomic uint32_t accepted;
    _Atomic uint32_t full;
    _Atomic uint32_t unexpected;
} boundary_irq_result_t;

static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT)
    rtos_stack_t s_wait_stacks[BOUNDARY_WAIT_CASES][BOUNDARY_STACK_WORDS];
static boundary_wait_case_t s_wait_cases[BOUNDARY_WAIT_CASES];
static rtos_sem_t s_wait_done;
static rtos_sem_t s_callback_entered;
static rtos_sem_t s_callback_release;
static rtos_sem_t s_target_done;
static rtos_timer_t s_periodic;
static rtos_timer_t s_target;
static boundary_irq_result_t s_irq;
static _Atomic uint32_t s_periodic_calls;
static _Atomic uint32_t s_target_calls;
static _Atomic rtos_status_t s_callback_gate_status;
static _Atomic rtos_status_t s_self_stop_status;
static _Atomic bool s_bad_callback_context;
static _Atomic rtos_tick_t s_fire_ticks[BOUNDARY_PERIODIC_FIRES];
static uint32_t s_checks;
static uint32_t s_failures;
static uint32_t s_round;
static uint32_t s_total_irq_entries;
static uint32_t s_total_accepted;
static uint32_t s_total_full;
static uint32_t s_total_task_timeouts;

void h743_boundary_stress_run(void *argument);
void EXTI2_IRQHandler(void);

static bool record_check(const char *name, bool passed)
{
    ++s_checks;
    if (!passed) {
        ++s_failures;
        (void)printf("H7B|CHECK|%s|FAIL|round=%lu\r\n", name, (unsigned long)s_round);
    }
    return passed;
}

static void run_wait_case(void *argument)
{
    boundary_wait_case_t *probe = argument;
    probe->ran_at = rtos_kernel.current_tcb->priority;
    probe->start_tick = rtos_sched_get_tick_count();
    probe->result = rtos_sem_take(&probe->gate, probe->timeout);
    (void)rtos_sem_give(&s_wait_done);
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static bool wait_case_matches(boundary_wait_case_t *probe, uint32_t index, rtos_prio_t priority)
{
    RTOS_PORT_ENTER_CRITICAL();
    const rtos_tick_t elapsed_to_deadline = probe->task.wake_tick - probe->start_tick;
    const bool waiting = probe->task.blocked_on == &probe->gate &&
        probe->gate.wait_head == &probe->task.wait_node &&
        probe->gate.wait_bitmap == (UINT32_C(1) << priority);
    bool matches = probe->ran_at == priority;
    if (index == 0U) {
        matches = matches && !waiting && probe->task.blocked_on == NULL &&
            probe->gate.wait_head == NULL && probe->gate.wait_bitmap == 0U &&
            probe->result == RTOS_ERR_TIMEOUT;
    } else if (index == 1U) {
        matches = matches && waiting && probe->task.state == RTOS_TASK_DELAYED &&
            (elapsed_to_deadline == BOUNDARY_MAX_FINITE ||
             elapsed_to_deadline == BOUNDARY_MAX_FINITE + 1U);
    } else {
        matches = matches && waiting && probe->task.state == RTOS_TASK_BLOCKED &&
            probe->task.wake_tick == RTOS_WAIT_FOREVER;
    }
    RTOS_PORT_EXIT_CRITICAL();
    return matches;
}

static void check_scheduler_limits(void)
{
    const rtos_tick_t waits[BOUNDARY_WAIT_CASES] = {
        RTOS_NO_WAIT, BOUNDARY_MAX_FINITE, BOUNDARY_MAX_FINITE + 1U, RTOS_WAIT_FOREVER
    };
    const rtos_prio_t priorities[BOUNDARY_WAIT_CASES] = {
        0U, 1U, 3U, RTOS_CONFIG_MAX_PRIORITIES - 1U
    };
    const char *const names[BOUNDARY_WAIT_CASES] = {
        "priority_zero_no_wait", "max_finite_wait", "over_limit_wait", "lowest_priority_forever"
    };
    uint32_t created = 0U;

    if (!record_check("wait_completion_setup",
                      rtos_sem_init(&s_wait_done, 0U, BOUNDARY_WAIT_CASES) == RTOS_OK)) {
        return;
    }
    (void)record_check("invalid_task_priority",
                       rtos_task_create_static(&s_wait_cases[0].task, s_wait_stacks[0],
                                               BOUNDARY_STACK_WORDS, run_wait_case,
                                               &s_wait_cases[0], RTOS_CONFIG_MAX_PRIORITIES,
                                               "invalid-prio") == RTOS_ERR_PARAM);
    (void)record_check("idle_priority_fixed",
                       rtos_task_set_priority(rtos_kernel.idle_tcb, 0U) == RTOS_ERR_PARAM &&
                       rtos_kernel.idle_tcb->priority == RTOS_CONFIG_MAX_PRIORITIES - 1U);

    for (uint32_t index = 0U; index < BOUNDARY_WAIT_CASES; ++index) {
        boundary_wait_case_t *probe = &s_wait_cases[index];
        probe->timeout = waits[index];
        probe->ran_at = RTOS_CONFIG_MAX_PRIORITIES;
        if (!record_check("wait_case_setup", rtos_sem_init(&probe->gate, 0U, 1U) == RTOS_OK &&
                          rtos_task_create_static(&probe->task, s_wait_stacks[index],
                                                  BOUNDARY_STACK_WORDS, run_wait_case, probe,
                                                  priorities[index], names[index]) == RTOS_OK)) {
            break;
        }
        ++created;
    }

    /* Let the last worker run at the same priority as idle. */
    rtos_task_delay(5U);
    for (uint32_t index = 0U; index < created; ++index) {
        boundary_wait_case_t *probe = &s_wait_cases[index];
        (void)record_check(names[index], wait_case_matches(probe, index, priorities[index]));
        if (index != 0U) {
            (void)record_check("release_waiter", rtos_sem_give(&probe->gate) == RTOS_OK);
        }
    }
    for (uint32_t index = 0U; index < created; ++index) {
        (void)record_check("wait_case_completion",
                           rtos_sem_take(&s_wait_done, BOUNDARY_SHORT_WAIT) == RTOS_OK);
    }
    for (uint32_t index = 0U; index < created; ++index) {
        boundary_wait_case_t *probe = &s_wait_cases[index];
        (void)record_check("wait_result_and_unlink",
                           probe->result == (index == 0U ? RTOS_ERR_TIMEOUT : RTOS_OK) &&
                           probe->gate.wait_head == NULL && probe->gate.wait_bitmap == 0U);
        (void)record_check("wait_task_delete", rtos_task_delete(&probe->task) == RTOS_OK);
    }
}

static void check_callback_context(void)
{
    if (RTOS_PORT_IN_ISR() || rtos_port_get_critical_nesting() != 0U ||
        __get_BASEPRI() != 0U || __get_PRIMASK() != 0U) {
        s_bad_callback_context = true;
    }
}

static void record_periodic_fire(void *argument)
{
    rtos_timer_t *timer = argument;
    check_callback_context();
    const uint32_t index = s_periodic_calls++;
    if (index < BOUNDARY_PERIODIC_FIRES) {
        s_fire_ticks[index] = rtos_sched_get_tick_count();
    }
    if (index == BOUNDARY_PERIODIC_FIRES - 1U) {
        s_self_stop_status = rtos_timer_stop(timer);
        (void)rtos_sem_give(&s_callback_entered);
    }
}

static void hold_timer_service(void *argument)
{
    rtos_timer_t *timer = argument;
    check_callback_context();
    ++s_periodic_calls;
    (void)rtos_sem_give(&s_callback_entered);
    s_callback_gate_status = rtos_sem_take(&s_callback_release, BOUNDARY_CALLBACK_WAIT);
    s_self_stop_status = rtos_timer_stop(timer);
}

static void record_target_fire(void *argument)
{
    (void)argument;
    check_callback_context();
    ++s_target_calls;
    (void)rtos_sem_give(&s_target_done);
}

void EXTI2_IRQHandler(void)
{
    const uint32_t burst = s_irq.entries++;
    if (__get_IPSR() != (uint32_t)EXTI2_IRQn + 16U || burst >= BOUNDARY_IRQ_BURSTS) {
        ++s_irq.unexpected;
        return;
    }

    for (uint32_t index = 0U; index < BOUNDARY_COMMANDS_PER_IRQ; ++index) {
        const uint32_t command = burst * BOUNDARY_COMMANDS_PER_IRQ + index;
        const rtos_status_t status = command == 0U
            ? rtos_timer_delete(&s_target)
            : rtos_timer_change_period(&s_target, 31U + command);

        if (status == RTOS_OK) {
            ++s_irq.accepted;
        } else if (status == RTOS_ERR_TIMEOUT) {
            ++s_irq.full;
        } else {
            ++s_irq.unexpected;
        }
    }
}

static void check_periodic_limits(void)
{
    const bool rejects_periods =
        rtos_timer_create(&s_periodic, "invalid-timer", record_periodic_fire, &s_periodic,
                          0U, RTOS_TIMER_PERIODIC) == RTOS_ERR_PARAM &&
        rtos_timer_create(&s_periodic, "invalid-timer", record_periodic_fire, &s_periodic,
                          BOUNDARY_MAX_FINITE + 1U, RTOS_TIMER_PERIODIC) == RTOS_ERR_PARAM;
    (void)record_check("invalid_timer_periods", rejects_periods);
    if (!record_check("max_timer_period",
                      rtos_timer_create(&s_periodic, "periodic", record_periodic_fire,
                                        &s_periodic, BOUNDARY_MAX_FINITE,
                                        RTOS_TIMER_PERIODIC) == RTOS_OK &&
                      rtos_timer_start(&s_periodic) == RTOS_OK)) {
        return;
    }
    rtos_task_delay(4U);
    (void)record_check("max_timer_stays_active",
                       s_periodic.is_active != 0U && s_periodic_calls == 0U);
    (void)record_check("invalid_timer_change",
                       rtos_timer_change_period(&s_periodic, BOUNDARY_MAX_FINITE + 1U) ==
                       RTOS_ERR_PARAM && rtos_timer_change_period(&s_periodic, 0U) ==
                       RTOS_ERR_PARAM);
    (void)record_check("change_active_period",
                       rtos_timer_change_period(&s_periodic, BOUNDARY_PERIOD) == RTOS_OK);
    (void)record_check("periodic_callbacks_arrive",
                       rtos_sem_take(&s_callback_entered, BOUNDARY_SHORT_WAIT) == RTOS_OK);
    rtos_task_delay(3U * BOUNDARY_PERIOD);
    (void)record_check("periodic_self_stop",
                       s_periodic_calls == BOUNDARY_PERIODIC_FIRES &&
                       s_self_stop_status == RTOS_OK && s_periodic.is_active == 0U &&
                       !s_bad_callback_context);
    for (uint32_t index = 1U; index < BOUNDARY_PERIODIC_FIRES; ++index) {
        const rtos_tick_t interval = s_fire_ticks[index] - s_fire_ticks[index - 1U];
        (void)record_check("periodic_phase", interval == BOUNDARY_PERIOD);
    }
    (void)record_check("periodic_delete", rtos_timer_delete(&s_periodic) == RTOS_OK);
    rtos_task_delay(2U);
    (void)record_check("periodic_delete_consumed", s_periodic.is_initialized == 0U);
}

static bool fill_command_queue(void)
{
    s_irq.entries = 0U;
    s_irq.accepted = 0U;
    s_irq.full = 0U;
    s_irq.unexpected = 0U;
    for (uint32_t index = 0U; index < BOUNDARY_IRQ_BURSTS; ++index) {
        NVIC_SetPendingIRQ(EXTI2_IRQn);
        __DSB();
        __ISB();
        if (s_irq.entries != index + 1U) {
            /* Give a pending exception a bounded chance before reporting a mask leak. */
            rtos_task_delay(1U);
        }
    }
    s_total_irq_entries += s_irq.entries;
    s_total_accepted += s_irq.accepted;
    s_total_full += s_irq.full;
    const bool filled = s_irq.entries == BOUNDARY_IRQ_BURSTS &&
        s_irq.accepted == BOUNDARY_COMMAND_CAPACITY && s_irq.unexpected == 0U &&
        s_irq.full == BOUNDARY_IRQ_BURSTS * BOUNDARY_COMMANDS_PER_IRQ - BOUNDARY_COMMAND_CAPACITY;
    return record_check("timer_irq_queue_capacity", filled);
}

static void check_timer_round(void)
{
    s_periodic_calls = 0U;
    s_target_calls = 0U;
    s_bad_callback_context = false;
    s_callback_gate_status = RTOS_ERR_TIMEOUT;
    s_self_stop_status = RTOS_ERR_TIMEOUT;

    if (!record_check("create_round_timers",
                      rtos_timer_create(&s_periodic, "gated-timer", hold_timer_service,
                                        &s_periodic, BOUNDARY_PERIOD, RTOS_TIMER_PERIODIC) ==
                      RTOS_OK && rtos_timer_create(&s_target, "old-generation",
                                                  record_target_fire, NULL, 17U,
                                                  RTOS_TIMER_ONE_SHOT) == RTOS_OK &&
                      rtos_timer_start(&s_periodic) == RTOS_OK)) {
        return;
    }
    if (!record_check("timer_callback_gate_entered",
                      rtos_sem_take(&s_callback_entered, BOUNDARY_SHORT_WAIT) == RTOS_OK)) {
        return;
    }

    const bool filled = fill_command_queue();
    if (filled) {
        const rtos_tick_t start_tick = rtos_sched_get_tick_count();
        const rtos_status_t status = rtos_timer_start(&s_target);
        const rtos_tick_t waited = rtos_sched_get_tick_count() - start_tick;
        if (status == RTOS_ERR_TIMEOUT) {
            ++s_total_task_timeouts;
        }
        (void)record_check("timer_full_task_timeout", status == RTOS_ERR_TIMEOUT &&
                           waited >= BOUNDARY_COMMAND_WAIT && waited <= BOUNDARY_COMMAND_WAIT + 1U);
    }

    (void)record_check("timer_recreate_running_callback",
                       rtos_timer_create(&s_periodic, "invalid-recreate", hold_timer_service,
                                         &s_periodic, BOUNDARY_PERIOD, RTOS_TIMER_PERIODIC) ==
                       RTOS_ERR_ALREADY);
    const uint32_t old_generation = s_target.generation;
    const bool recreated = record_check("timer_recreate_inactive",
                                        rtos_timer_create(&s_target, "new-generation",
                                                          record_target_fire, NULL,
                                                          BOUNDARY_PERIOD,
                                                          RTOS_TIMER_ONE_SHOT) == RTOS_OK &&
                                        s_target.generation != old_generation);
    (void)record_check("timer_callback_gate_release",
                       rtos_sem_give(&s_callback_release) == RTOS_OK);
    if (recreated) {
        (void)record_check("timer_command_recovers", rtos_timer_start(&s_target) == RTOS_OK);
        (void)record_check("new_timer_fires",
                           rtos_sem_take(&s_target_done, BOUNDARY_SHORT_WAIT) == RTOS_OK);
    }

    rtos_task_delay(3U * BOUNDARY_PERIOD);
    (void)record_check("timer_self_stop_while_full", s_periodic_calls == 1U &&
                       s_callback_gate_status == RTOS_OK && s_self_stop_status == RTOS_OK &&
                       s_periodic.is_active == 0U);
    (void)record_check("stale_timer_commands_ignored", s_target_calls == 1U &&
                       s_target.is_active == 0U && s_target.is_initialized != 0U &&
                       s_target.period == BOUNDARY_PERIOD);
    (void)record_check("timer_context_restored", !s_bad_callback_context &&
                       rtos_port_get_critical_nesting() == 0U && __get_BASEPRI() == 0U &&
                       __get_PRIMASK() == 0U);
    (void)record_check("timer_delete_commands", rtos_timer_delete(&s_periodic) == RTOS_OK &&
                       rtos_timer_delete(&s_target) == RTOS_OK);
    rtos_task_delay(2U);
    (void)record_check("timer_delete_consumed",
                       s_periodic.is_initialized == 0U && s_target.is_initialized == 0U);
}

void h743_boundary_stress_run(void *argument)
{
    (void)argument;
    uint32_t completed_rounds = 0U;

    (void)printf("H7B|BEGIN|rounds=%lu\r\n", (unsigned long)H743_BOUNDARY_ROUNDS);
    check_scheduler_limits();
    if (record_check("timer_test_setup", rtos_sem_init(&s_callback_entered, 0U, 1U) == RTOS_OK &&
                     rtos_sem_init(&s_callback_release, 0U, 1U) == RTOS_OK &&
                     rtos_sem_init(&s_target_done, 0U, 1U) == RTOS_OK)) {
        check_periodic_limits();
        NVIC_SetPriority(EXTI2_IRQn, RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >>
                         (8U - __NVIC_PRIO_BITS));
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
        NVIC_EnableIRQ(EXTI2_IRQn);
        for (s_round = 1U; s_round <= H743_BOUNDARY_ROUNDS && s_failures == 0U; ++s_round) {
            check_timer_round();
            if (s_failures == 0U) {
                ++completed_rounds;
            }
            if (s_round % 32U == 0U) {
                (void)printf("H7B|PROGRESS|rounds=%lu|tick=%lu\r\n",
                             (unsigned long)completed_rounds,
                             (unsigned long)rtos_sched_get_tick_count());
            }
        }
        NVIC_DisableIRQ(EXTI2_IRQn);
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
    }
    (void)printf("H7B|DONE|status=%u|checks=%lu|failures=%lu|rounds=%lu|irq=%lu"
                 "|accepted=%lu|full=%lu|task_timeouts=%lu\r\n", s_failures != 0U ? 1U : 0U,
                 (unsigned long)s_checks, (unsigned long)s_failures,
                 (unsigned long)completed_rounds, (unsigned long)s_total_irq_entries,
                 (unsigned long)s_total_accepted,
                 (unsigned long)s_total_full, (unsigned long)s_total_task_timeouts);
}

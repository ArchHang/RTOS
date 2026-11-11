/** @file f446_stack_hook_irq.c
 * @brief Rebuild a reported task while Idle is delivering stack-guard callbacks.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"
#include "rtos.h"

#if RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW != 2 || !RTOS_CONFIG_USE_PERF_MONITOR
#error "This regression needs mode-2 stack guards and the performance registry"
#endif
#if !RTOS_CONFIG_USE_PREEMPTION
#error "This regression needs a preemptive Idle task"
#endif

enum {
    STACK_HOOK_TASK_COUNT = 2U,
    STACK_HOOK_WORKER_PRIORITY = 12U,
    STACK_HOOK_STACK_WORDS = 256U,
    STACK_HOOK_IRQ_PRIORITY = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >>
                              (8U - __NVIC_PRIO_BITS),
    STACK_HOOK_WAIT_TICKS = 1500U,
    STACK_HOOK_OBSERVE_TICKS = 20U
};

typedef struct stack_hook_trace {
    uint32_t reports;
    uint32_t stale_reports;
    uint32_t interrupt_count;
    uint32_t target;
    rtos_status_t signal_status;
    bool is_armed;
} stack_hook_trace_t;

static rtos_tcb_t s_tasks[STACK_HOOK_TASK_COUNT];
static _Alignas(8) rtos_stack_t s_stacks[STACK_HOOK_TASK_COUNT][STACK_HOOK_STACK_WORDS];
static rtos_stack_t s_canaries[STACK_HOOK_TASK_COUNT];
static uint32_t s_original_ids[STACK_HOOK_TASK_COUNT];
static rtos_sem_t s_rebuild;
static stack_hook_trace_t s_trace;

void f446_stack_hook_irq_run(void *argument);
void __wrap_rtos_stack_overflow_hook(rtos_tcb_t *task);
void EXTI3_IRQHandler(void);

void EXTI3_IRQHandler(void)
{
    if (rtos_port_isr_priority() == RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY) {
        ++s_trace.interrupt_count;
        s_trace.signal_status = rtos_sem_give(&s_rebuild);
    }
}

void __wrap_rtos_stack_overflow_hook(rtos_tcb_t *task)
{
    if (!s_trace.is_armed) {
        return;
    }
    for (uint32_t index = 0U; index < STACK_HOOK_TASK_COUNT; ++index) {
        if (task != &s_tasks[index]) {
            continue;
        }
        if (task->task_id != s_original_ids[index] || task->state == RTOS_TASK_DELETED) {
            ++s_trace.stale_reports;
        }
        ++s_trace.reports;
        if (s_trace.reports == 1U) {
            s_trace.target = (index == 0U) ? 1U : 0U;
            /* An ISR may wake the supervisor, which legally deletes and
             * recreates the other task on its existing static storage. */
            NVIC_SetPendingIRQ(EXTI3_IRQn);
        }
        return;
    }
}

static void park_worker(void *argument)
{
    (void)argument;
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static rtos_status_t create_worker(uint32_t index)
{
    return rtos_task_create_static(&s_tasks[index], s_stacks[index], STACK_HOOK_STACK_WORDS,
                                   park_worker, NULL, STACK_HOOK_WORKER_PRIORITY, "guard-probe");
}

static bool run_regression(void)
{
    s_trace = (stack_hook_trace_t){.target = STACK_HOOK_TASK_COUNT,
                                  .signal_status = RTOS_ERR_SCHED};
    uint32_t created = 0U;
    for (; created < STACK_HOOK_TASK_COUNT; ++created) {
        if (create_worker(created) != RTOS_OK) {
            break;
        }
        s_original_ids[created] = s_tasks[created].task_id;
        s_canaries[created] = s_stacks[created][0];
    }

    rtos_status_t waited = RTOS_ERR_SCHED;
    rtos_status_t deleted = RTOS_ERR_SCHED;
    rtos_status_t rebuilt = RTOS_ERR_SCHED;
    bool is_new_guard_intact = false;
    if (created == STACK_HOOK_TASK_COUNT) {
        RTOS_PORT_ENTER_CRITICAL();
        for (uint32_t index = 0U; index < STACK_HOOK_TASK_COUNT; ++index) {
            s_stacks[index][0] ^= 1U;
        }
        s_trace.is_armed = true;
        RTOS_PORT_EXIT_CRITICAL();

        waited = rtos_sem_take(&s_rebuild, STACK_HOOK_WAIT_TICKS);
        const uint32_t target = s_trace.target;
        if (waited == RTOS_OK && target < STACK_HOOK_TASK_COUNT) {
            deleted = rtos_task_delete(&s_tasks[target]);
            if (deleted == RTOS_OK) {
                rebuilt = create_worker(target);
                is_new_guard_intact = rebuilt == RTOS_OK &&
                                      s_tasks[target].task_id != s_original_ids[target] &&
                                      s_stacks[target][0] == s_canaries[target];
            }
            rtos_task_delay(STACK_HOOK_OBSERVE_TICKS);
        }
    }

    RTOS_PORT_ENTER_CRITICAL();
    s_trace.is_armed = false;
    const stack_hook_trace_t observed = s_trace;
    for (uint32_t index = 0U; index < created; ++index) {
        s_stacks[index][0] = s_canaries[index];
    }
    RTOS_PORT_EXIT_CRITICAL();

    bool cleaned = true;
    for (uint32_t index = 0U; index < created; ++index) {
        if (s_tasks[index].state != RTOS_TASK_DELETED) {
            const rtos_status_t status = rtos_task_delete(&s_tasks[index]);
            cleaned = (status == RTOS_OK) && cleaned;
        }
    }
    (void)printf("SGIR|RESULT|hooks=%lu|stale=%lu|irq=%lu|irq_status=%ld|wait=%ld|"
                 "delete=%ld|recreate=%ld|new_guard=%u|clean=%u\r\n",
                 (unsigned long)observed.reports, (unsigned long)observed.stale_reports,
                 (unsigned long)observed.interrupt_count, (long)observed.signal_status,
                 (long)waited, (long)deleted, (long)rebuilt,
                 (unsigned int)is_new_guard_intact, (unsigned int)cleaned);
    return waited == RTOS_OK && deleted == RTOS_OK && rebuilt == RTOS_OK && cleaned &&
           is_new_guard_intact && observed.reports >= 1U &&
           observed.reports <= STACK_HOOK_TASK_COUNT && observed.stale_reports == 0U &&
           observed.interrupt_count == 1U && observed.signal_status == RTOS_OK;
}

void f446_stack_hook_irq_run(void *argument)
{
    (void)argument;
    if (NVIC_GetEnableIRQ(EXTI3_IRQn) != 0U || NVIC_GetPendingIRQ(EXTI3_IRQn) != 0U ||
        rtos_sem_init(&s_rebuild, 0U, 1U) != RTOS_OK) {
        (void)printf("SGIR|SETUP|FAIL\r\nSGIR|DONE|pass=0|fail=1\r\n");
        return;
    }
    const uint32_t previous_priority = NVIC_GetPriority(EXTI3_IRQn);
    NVIC_SetPriority(EXTI3_IRQn, STACK_HOOK_IRQ_PRIORITY);
    NVIC_EnableIRQ(EXTI3_IRQn);

    const bool passed = run_regression();

    NVIC_DisableIRQ(EXTI3_IRQn);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    NVIC_SetPriority(EXTI3_IRQn, previous_priority);
    const bool is_deinitialized = rtos_sem_deinit(&s_rebuild) == RTOS_OK;
    (void)printf("SGIR|DONE|pass=%u|fail=%u\r\n", (passed && is_deinitialized) ? 1U : 0U,
                 (passed && is_deinitialized) ? 0U : 1U);
}

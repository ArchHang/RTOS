/** @file f446_cooperative_yield_irq.c
 * @brief Exercise a cooperative yield while a real IRQ changes its candidate.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"
#include "rtos.h"

#if RTOS_CONFIG_USE_PREEMPTION
#error "This regression requires cooperative scheduling"
#endif

enum {
    YIELD_IRQ_PRIORITY = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >> (8U - __NVIC_PRIO_BITS),
    YIELD_SUPERVISOR_PRIORITY = 4U,
    YIELD_PEER_PRIORITY = 12U,
    YIELD_URGENT_PRIORITY = 8U,
    YIELD_STACK_WORDS = 256U,
    YIELD_WAIT_TICKS = 80U
};

typedef enum yield_case {
    YIELD_CASE_WAKE = 0,
    YIELD_CASE_SUSPEND
} yield_case_t;

typedef enum yield_role {
    YIELD_ROLE_YIELDER = 0,
    YIELD_ROLE_CHOSEN,
    YIELD_ROLE_BACKUP,
    YIELD_ROLE_URGENT,
    YIELD_ROLE_COUNT
} yield_role_t;

typedef struct yield_trace {
    rtos_tcb_t *first_runner;
    rtos_status_t interrupt_status;
    rtos_status_t signal_status;
    uint32_t interrupt_count;
    bool is_armed;
    bool has_window;
} yield_trace_t;

static rtos_tcb_t s_tasks[YIELD_ROLE_COUNT];
static _Alignas(8) rtos_stack_t s_stacks[YIELD_ROLE_COUNT][YIELD_STACK_WORDS];
static rtos_sem_t s_completed;
static yield_case_t s_case;
static yield_trace_t s_trace;

void f446_cooperative_yield_irq_run(void *argument);
void __wrap_rtos_port_context_switch(void);
void __real_rtos_port_context_switch(void);
void EXTI2_IRQHandler(void);

void __wrap_rtos_port_context_switch(void)
{
    /* The caller is inside the scheduler critical section. Pend the real IRQ
     * so its RTOS API runs after unmasking and before the lower-priority PendSV. */
    if (s_trace.is_armed && rtos_kernel.current_tcb == &s_tasks[YIELD_ROLE_YIELDER] &&
        rtos_kernel.next_tcb == &s_tasks[YIELD_ROLE_CHOSEN]) {
        s_trace.is_armed = false;
        NVIC_SetPendingIRQ(EXTI2_IRQn);
    }
    __real_rtos_port_context_switch();
}

void EXTI2_IRQHandler(void)
{
    ++s_trace.interrupt_count;
    s_trace.has_window = rtos_kernel.current_tcb == &s_tasks[YIELD_ROLE_YIELDER] &&
                         rtos_kernel.current_tcb->state == RTOS_TASK_RUNNING &&
                         rtos_kernel.next_tcb == &s_tasks[YIELD_ROLE_CHOSEN] &&
                         rtos_port_isr_priority() == RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY &&
                         (SCB->ICSR & SCB_ICSR_PENDSVSET_Msk) != 0U;
    if (s_case == YIELD_CASE_WAKE) {
        s_trace.interrupt_status = rtos_task_resume(&s_tasks[YIELD_ROLE_URGENT]);
    } else {
        s_trace.interrupt_status = rtos_task_suspend(&s_tasks[YIELD_ROLE_CHOSEN]);
    }
}

static void run_peer(void *argument)
{
    rtos_tcb_t *const task = argument;
    if (task == &s_tasks[YIELD_ROLE_YIELDER]) {
        RTOS_PORT_ENTER_CRITICAL();
        s_trace.is_armed = true;
        RTOS_PORT_EXIT_CRITICAL();
        rtos_task_yield();
    }

    RTOS_PORT_ENTER_CRITICAL();
    if (s_trace.first_runner == NULL) {
        s_trace.first_runner = task;
        s_trace.signal_status = rtos_sem_give(&s_completed);
    }
    RTOS_PORT_EXIT_CRITICAL();

    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

static uint32_t read_role(const rtos_tcb_t *task)
{
    for (uint32_t role = 0U; role < (uint32_t)YIELD_ROLE_COUNT; ++role) {
        if (task == &s_tasks[role]) {
            return role;
        }
    }
    return (uint32_t)YIELD_ROLE_COUNT;
}

static bool run_case(yield_case_t kind)
{
    const rtos_status_t sem_status = rtos_sem_init(&s_completed, 0U, 1U);
    if (sem_status != RTOS_OK) {
        (void)printf("COY|SETUP|kind=%u|sem=%ld\r\n", (unsigned int)kind, (long)sem_status);
        return false;
    }

    s_case = kind;
    s_trace = (yield_trace_t){.interrupt_status = RTOS_ERR_SCHED, .signal_status = RTOS_ERR_SCHED};
    uint32_t created = 0U;
    for (; created < (uint32_t)YIELD_ROLE_COUNT; ++created) {
        const rtos_prio_t priority = (created == (uint32_t)YIELD_ROLE_URGENT)
            ? (rtos_prio_t)YIELD_URGENT_PRIORITY : (rtos_prio_t)YIELD_PEER_PRIORITY;
        if (rtos_task_create_static(&s_tasks[created], s_stacks[created], YIELD_STACK_WORDS,
                                    run_peer, &s_tasks[created], priority, "coop-yield") != RTOS_OK) {
            break;
        }
    }

    rtos_status_t waited = RTOS_ERR_SCHED;
    if (created == (uint32_t)YIELD_ROLE_COUNT &&
        rtos_task_suspend(&s_tasks[YIELD_ROLE_URGENT]) == RTOS_OK) {
        waited = rtos_sem_take(&s_completed, YIELD_WAIT_TICKS);
    }
    const uint32_t expected = (kind == YIELD_CASE_WAKE) ? (uint32_t)YIELD_ROLE_URGENT
                                                       : (uint32_t)YIELD_ROLE_BACKUP;
    const uint32_t first = read_role(s_trace.first_runner);
    const bool passed = waited == RTOS_OK && s_trace.signal_status == RTOS_OK &&
                        s_trace.interrupt_count == 1U && s_trace.has_window &&
                        s_trace.interrupt_status == RTOS_OK && first == expected;
    bool cleaned = true;
    for (uint32_t index = 0U; index < created; ++index) {
        const rtos_status_t status = rtos_task_delete(&s_tasks[index]);
        cleaned = (status == RTOS_OK) && cleaned;
    }
    const rtos_status_t deinit = rtos_sem_deinit(&s_completed);
    cleaned = (deinit == RTOS_OK) && cleaned;
    (void)printf("COY|CASE|kind=%u|first=%lu|expected=%lu|irq=%lu|window=%u|"
                 "irq_status=%ld|wait=%ld|clean=%u\r\n", (unsigned int)kind,
                 (unsigned long)first, (unsigned long)expected,
                 (unsigned long)s_trace.interrupt_count, (unsigned int)s_trace.has_window,
                 (long)s_trace.interrupt_status, (long)waited, (unsigned int)cleaned);
    return passed && cleaned;
}

void f446_cooperative_yield_irq_run(void *argument)
{
    (void)argument;
    if (rtos_task_get_priority(NULL) != (rtos_prio_t)YIELD_SUPERVISOR_PRIORITY) {
        (void)printf("COY|DONE|pass=0|fail=2\r\n");
        return;
    }
    if (NVIC_GetEnableIRQ(EXTI2_IRQn) != 0U || NVIC_GetPendingIRQ(EXTI2_IRQn) != 0U) {
        (void)printf("COY|SETUP|exti2_unavailable\r\nCOY|DONE|pass=0|fail=2\r\n");
        return;
    }

    const uint32_t previous_priority = NVIC_GetPriority(EXTI2_IRQn);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    NVIC_SetPriority(EXTI2_IRQn, YIELD_IRQ_PRIORITY);
    NVIC_EnableIRQ(EXTI2_IRQn);

    uint32_t passes = 0U;
    if (run_case(YIELD_CASE_WAKE)) {
        ++passes;
    }
    if (run_case(YIELD_CASE_SUSPEND)) {
        ++passes;
    }

    NVIC_DisableIRQ(EXTI2_IRQn);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    NVIC_SetPriority(EXTI2_IRQn, previous_priority);
    (void)printf("COY|DONE|pass=%lu|fail=%lu\r\n",
                 (unsigned long)passes, (unsigned long)(2U - passes));
}

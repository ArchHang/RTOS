/** @file h743_resume_regression.c
 * @brief Resume a suspended task from a real IRQ before PendSV commits a switch.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hil_target.h"

#include "rtos.h"

enum {
    H7_RESUME_STACK_WORDS = 192U,
    H7_RESUME_HIGH_PRIORITY = 12U,
    H7_RESUME_LOW_PRIORITY = 20U,
    H7_RESUME_ROUNDS = 32U
};

static _Alignas(8) rtos_stack_t g_high_stack[H7_RESUME_STACK_WORDS];
static _Alignas(8) rtos_stack_t g_low_stack[H7_RESUME_STACK_WORDS];
static rtos_tcb_t g_high_task;
static rtos_tcb_t g_low_task;
static rtos_tcb_t *g_controller;
static rtos_sem_t g_completion;
static volatile bool g_is_armed;
static volatile bool g_irq_observed_transition;
static volatile bool g_irq_selected_high;
static volatile rtos_status_t g_resume_status;
static volatile uint32_t g_irq_count;
static volatile uint32_t g_first_priority;

void h743_resume_regression_run(void *argument);
void EXTI2_IRQHandler(void);
void __real_rtos_port_context_switch(void);
void __wrap_rtos_port_context_switch(void);

void __wrap_rtos_port_context_switch(void)
{
    if (g_is_armed && rtos_kernel.current_tcb == g_controller &&
        rtos_kernel.next_tcb == &g_low_task) {
        g_is_armed = false;
        NVIC_SetPendingIRQ(EXTI2_IRQn);
    }
    __real_rtos_port_context_switch();
}

void EXTI2_IRQHandler(void)
{
    ++g_irq_count;
    g_irq_observed_transition = (__get_IPSR() == (uint32_t)EXTI2_IRQn + 16U) &&
        (rtos_kernel.current_tcb == g_controller) &&
        (g_controller->state == RTOS_TASK_DELAYED) && (rtos_kernel.next_tcb == &g_low_task);
    g_resume_status = rtos_task_resume(&g_high_task);
    g_irq_selected_high = rtos_kernel.next_tcb == &g_high_task;
}

static void child_run(void *argument)
{
    const uint32_t priority = *(const uint32_t *)argument;
    if (g_first_priority == UINT32_MAX) {
        g_first_priority = priority;
        (void)rtos_sem_give(&g_completion);
    }
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

void h743_resume_regression_run(void *argument)
{
    (void)argument;
    static const uint32_t high_priority = H7_RESUME_HIGH_PRIORITY;
    static const uint32_t low_priority = H7_RESUME_LOW_PRIORITY;
    g_controller = rtos_task_get_current();
    const uint32_t original_irq_priority = NVIC_GetPriority(EXTI2_IRQn);
    const bool is_irq_free = (NVIC_GetEnableIRQ(EXTI2_IRQn) == 0U) &&
        (NVIC_GetPendingIRQ(EXTI2_IRQn) == 0U);
    uint32_t valid = 0U;
    uint32_t selected = 0U;
    uint32_t high_first = 0U;
    uint32_t low_first = 0U;
    uint32_t clean = 0U;
    const bool is_initialized = rtos_sem_init(&g_completion, 0U, 1U) == RTOS_OK;
    if (is_irq_free && is_initialized) {
        const uint32_t irq_priority = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >>
                                      (8U - __NVIC_PRIO_BITS);
        NVIC_SetPriority(EXTI2_IRQn, irq_priority);
        NVIC_EnableIRQ(EXTI2_IRQn);
        for (uint32_t round = 0U; round < H7_RESUME_ROUNDS; ++round) {
            rtos_task_delay(1U);
            const rtos_status_t high_create = rtos_task_create_static(&g_high_task, g_high_stack,
                H7_RESUME_STACK_WORDS, child_run, (void *)&high_priority,
                H7_RESUME_HIGH_PRIORITY, "resume-high");
            if (high_create != RTOS_OK) {
                break;
            }
            const rtos_status_t suspend = rtos_task_suspend(&g_high_task);
            const rtos_status_t low_create = rtos_task_create_static(&g_low_task, g_low_stack,
                H7_RESUME_STACK_WORDS, child_run, (void *)&low_priority,
                H7_RESUME_LOW_PRIORITY, "resume-low");
            if ((suspend != RTOS_OK) || (low_create != RTOS_OK)) {
                (void)rtos_task_delete(&g_high_task);
                if (low_create == RTOS_OK) {
                    (void)rtos_task_delete(&g_low_task);
                }
                break;
            }
            g_irq_observed_transition = false;
            g_irq_selected_high = false;
            g_first_priority = UINT32_MAX;
            g_resume_status = RTOS_ERR_SCHED;
            g_is_armed = true;
            const rtos_status_t take = rtos_sem_take(&g_completion, 20U);
            g_is_armed = false;
            valid += (uint32_t)(g_irq_observed_transition && g_resume_status == RTOS_OK &&
                                take == RTOS_OK);
            selected += (uint32_t)g_irq_selected_high;
            high_first += (uint32_t)(g_first_priority == H7_RESUME_HIGH_PRIORITY);
            low_first += (uint32_t)(g_first_priority == H7_RESUME_LOW_PRIORITY);
            const rtos_status_t high_delete = rtos_task_delete(&g_high_task);
            const rtos_status_t low_delete = rtos_task_delete(&g_low_task);
            clean += (uint32_t)(high_delete == RTOS_OK && low_delete == RTOS_OK &&
                                rtos_sem_get_count(&g_completion) == 0U);
        }
        NVIC_DisableIRQ(EXTI2_IRQn);
        NVIC_ClearPendingIRQ(EXTI2_IRQn);
        NVIC_SetPriority(EXTI2_IRQn, original_irq_priority);
    }
    const bool is_deinitialized = is_initialized && rtos_sem_deinit(&g_completion) == RTOS_OK;
    (void)printf("H7RESUME|RESULT|rounds=%u|irq=%lu|valid=%lu|selected_high=%lu|"
                 "high_first=%lu|low_first=%lu|clean=%lu\r\n",
                 (unsigned)H7_RESUME_ROUNDS, (unsigned long)g_irq_count, (unsigned long)valid,
                 (unsigned long)selected, (unsigned long)high_first, (unsigned long)low_first,
                 (unsigned long)clean);
    const uint32_t passed = (uint32_t)(valid == H7_RESUME_ROUNDS && g_irq_count == valid) +
        (uint32_t)(selected == H7_RESUME_ROUNDS) + (uint32_t)(high_first == H7_RESUME_ROUNDS) +
        (uint32_t)(clean == H7_RESUME_ROUNDS && is_deinitialized);
    (void)printf("H7RESUME|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)passed,
                 (unsigned long)(4U - passed));
    for (;;) {
        rtos_task_delay(1000U);
    }
}

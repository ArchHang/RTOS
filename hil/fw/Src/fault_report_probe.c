/**
 * @file fault_report_probe.c
 * @brief Trigger a precise UsageFault on F446 with a known mask and stack context.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>

#include "main.h"
#include "rtos.h"

static void trigger_usage_fault(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    /* UDF must raise a hardware UsageFault before this critical section exits. */
    __asm__ __volatile__("udf #0" ::: "memory");
    RTOS_PORT_EXIT_CRITICAL();
}

#if defined(FAULT_DIAG_ISR)
void EXTI0_IRQHandler(void);

void EXTI0_IRQHandler(void)
{
    trigger_usage_fault();
}
#endif

void audit_pre_start(void)
{
    NVIC_SetPriority(UsageFault_IRQn, 0U);
}

void audit_regressions_task(void *arg)
{
    (void)arg;
#if defined(FAULT_DIAG_ISR)
    (void)printf("FDIAG|BEGIN|isr\r\n");
    NVIC_SetPriority(EXTI0_IRQn, 6U);
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_SetPendingIRQ(EXTI0_IRQn);
    __DSB();
    __ISB();
#else
    /* Keep a real frame on PSP so the upper/lower distance test is asymmetric. */
    volatile uint32_t stack_space[32U];
    stack_space[0] = 42U;
#if defined(FAULT_DIAG_FPU)
    (void)printf("FDIAG|BEGIN|fpu\r\n");
    volatile float operand = 1.25f;
    operand = operand * operand;
#else
    (void)printf("FDIAG|BEGIN|task\r\n");
#endif
    trigger_usage_fault();
    (void)stack_space[0];
#endif
    (void)printf("FDIAG|FAIL|fault_returned\r\n");
    for (;;) {
        __WFI();
    }
}

/** @file diagnostic_fault.c
 * @brief Intentional terminal UsageFault after trace history has been populated.
 */
#include "audit_regressions.h"

#include <stdio.h>

#include "rtos.h"

void audit_pre_start(void)
{
    (void)printf("DG|META|mode=fault diagnostics=%u capacity=%u\r\n",
        (unsigned int)RTOS_CONFIG_USE_DIAGNOSTICS,
        (unsigned int)RTOS_CONFIG_DIAGNOSTIC_CAPACITY);
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    (void)rtos_diag_reset();
    (void)rtos_diag_mark(0xA55A0001U, 0x12345678U);
    (void)rtos_task_delay(2U);
    (void)rtos_diag_mark(0xA55A0002U, 0x87654321U);
    (void)printf("DG|FAULT_ARMED|task=%lu code=udf\r\n",
        (unsigned long)rtos_task_get_current()->task_id);
    (void)fflush(stdout);
    __asm volatile("udf #0" ::: "memory");
    for (;;) {
    }
}

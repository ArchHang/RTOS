/**
 * @file robustness_runner.c
 * @brief Run focused regressions and report individual assertions over UART.
 */
#include "robustness.h"

#include <stdint.h>
#include <stdio.h>

#include "audit_regressions.h"
#include "robustness_core.h"
#include "robustness_memory.h"
#include "robustness_services.h"
#include "rtos.h"

static uint32_t pass_count;
static uint32_t fail_count;

#if defined(STM32H743xx)
void rtos_stack_overflow_hook(rtos_tcb_t *task)
{
    /* F446 already forwards its application hook to this fixture. */
    robustness_core_record_stack_overflow(task);
}
#endif

void robustness_check(const char *name, bool is_ok)
{
    if (is_ok) {
        ++pass_count;
    } else {
        ++fail_count;
    }
    (void)printf("RB|CHECK|%s|%s\r\n", name, is_ok ? "PASS" : "FAIL");
}

void audit_pre_start(void)
{
    (void)printf("RB|BEGIN|basepri=%u|assert=%u|preempt=%u|hotpath=%u\r\n",
                 (unsigned int)RTOS_CONFIG_USE_BASEPRI,
                 (unsigned int)RTOS_CONFIG_ASSERT_ENABLED,
                 (unsigned int)RTOS_CONFIG_USE_PREEMPTION,
                 (unsigned int)RTOS_CONFIG_PERF_HOTPATH_STATS);
}

void audit_regressions_task(void *arg)
{
    (void)arg;
    robustness_memory_run();
    robustness_core_run();
    robustness_services_run();
    (void)printf("RB|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)pass_count,
                 (unsigned long)fail_count);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

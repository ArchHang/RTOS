/**
 * @file robustness_msp.c
 * @brief 通过生产向量表测量首任务及切换过程中的 MSP。
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "main.h"
#include "rtos.h"

enum { MSP_ROUNDS = 32U };

/*
 * Pend an exception from Thread mode without a live C frame pointer.
 * Restore r7 explicitly so the old wrapper's clobber cannot hide the MSP sample.
 * C cannot express the register-preservation boundary around exception entry.
 */
__attribute__((naked)) static void pend_context_probe(void)
{
    __asm__ __volatile__(
        "push {r7, lr}           \n" /* Save the AAPCS frame pointer and return PC on PSP. */
        "ldr r0, =0xE000ED04     \n" /* r0 = SCB ICSR. */
        "mov.w r1, #0x10000000   \n" /* r1 = PENDSVSET. */
        "str r1, [r0]            \n" /* Request PendSV with the current task still ready. */
        "dsb                     \n" /* Complete the pending write. */
        "isb                     \n" /* Synchronize exception recognition before continuing. */
        "pop {r7, pc}            \n" /* Restore the caller after the probe. */
        ".ltorg                  \n"
    );
}

void audit_pre_start(void)
{
    (void)printf("MSP|BEGIN|rounds=%u\r\n", (unsigned int)MSP_ROUNDS);
}

void audit_regressions_task(void *arg)
{
    (void)arg;
    uint32_t initial_msp = __get_MSP();
    pend_context_probe();
    uint32_t probe_msp = __get_MSP();
    bool is_aligned = (initial_msp & 7U) == 0U;
    (void)printf("MSP|ENTRY|svc=0x%08lX|one_pendsv=0x%08lX\r\n",
                 (unsigned long)initial_msp, (unsigned long)probe_msp);
    (void)printf("MSP|svc_alignment|%s\r\n", is_aligned ? "PASS" : "FAIL");
    (void)printf("MSP|pendsv_probe_balance|%s\r\n", probe_msp == initial_msp ? "PASS" : "FAIL");

    rtos_tick_t initial_tick = rtos_sched_get_tick_count();
    for (uint32_t round = 0U; round < MSP_ROUNDS; ++round) {
        rtos_task_delay(1U);
    }
    uint32_t final_msp = __get_MSP();
    rtos_tick_t elapsed = rtos_sched_get_tick_count() - initial_tick;
    bool is_balanced = final_msp == probe_msp;
    bool has_progress = elapsed >= MSP_ROUNDS;

    (void)printf("MSP|SAMPLE|initial=0x%08lX|final=0x%08lX|ticks=%lu\r\n",
                 (unsigned long)initial_msp, (unsigned long)final_msp,
                 (unsigned long)elapsed);
    (void)printf("MSP|pendsv_balance|%s\r\n", is_balanced ? "PASS" : "FAIL");
    (void)printf("MSP|tick_progress|%s\r\n", has_progress ? "PASS" : "FAIL");
    (void)printf("MSP|DONE\r\n");

    for (;;) {
        rtos_task_delay(1000U);
    }
}

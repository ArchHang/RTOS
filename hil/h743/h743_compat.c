/** @file h743_compat.c
 *  @brief Exercise the Cortex-M7 exception, mask, and FPU contracts on the target.
 */
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32h743xx.h"

#include "rtos.h"

enum {
    COMPAT_STACK_WORDS = 256U,
    COMPAT_FP_WORDS = 16U,
    COMPAT_FP_ROUNDS = 64U,
    COMPAT_FP_TASK_PRIORITY = 10U,
    COMPAT_MSP_ROUNDS = 32U,
    COMPAT_IRQ_BUDGET = 20U,
    COMPAT_IRQ_REALTIME_PRIORITY = 1U,
    COMPAT_IRQ_STRICT_THRESHOLD = 3U
};

typedef struct compat_fp_probe {
    rtos_tcb_t task;
    uint32_t seed;
    uint32_t rounds;
    uint32_t errors;
} compat_fp_probe_t;

typedef struct compat_irq_trace {
    _Atomic uint32_t syscall_entries;
    _Atomic uint32_t realtime_entries;
    _Atomic uint32_t nested_entries;
    _Atomic bool is_syscall_active;
    _Atomic bool has_syscall_exception;
    _Atomic bool has_realtime_exception;
    _Atomic rtos_status_t give_status;
} compat_irq_trace_t;

static compat_fp_probe_t s_fp_probes[2];
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t s_fp_stacks[2][COMPAT_STACK_WORDS];
static rtos_tcb_t s_irq_task;
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t s_irq_stack[COMPAT_STACK_WORDS];
static rtos_sem_t s_irq_sem;
static compat_irq_trace_t s_irq_trace;
static uint32_t s_checks;
static uint32_t s_failures;

void h743_compat_run(void *argument);
void EXTI2_IRQHandler(void);
void EXTI3_IRQHandler(void);

static void record_check(const char *name, bool passed)
{
    ++s_checks;
    if (!passed) {
        ++s_failures;
    }
    (void)printf("H7C|CHECK|%s|%s\r\n", name, passed ? "PASS" : "FAIL");
}

static void pend_irq(IRQn_Type irq)
{
    NVIC_SetPendingIRQ(irq);
    __DSB();
    __ISB();
}

void EXTI3_IRQHandler(void)
{
    ++s_irq_trace.realtime_entries;
    if (s_irq_trace.is_syscall_active) {
        ++s_irq_trace.nested_entries;
    }
    s_irq_trace.has_realtime_exception = __get_IPSR() == (uint32_t)EXTI3_IRQn + 16U;
}

void EXTI2_IRQHandler(void)
{
    s_irq_trace.is_syscall_active = true;
    ++s_irq_trace.syscall_entries;
    s_irq_trace.has_syscall_exception = __get_IPSR() == (uint32_t)EXTI2_IRQn + 16U;
    pend_irq(EXTI3_IRQn);
    s_irq_trace.is_syscall_active = false;
    s_irq_trace.give_status = rtos_sem_give(&s_irq_sem);
}

static void run_irq_source(void *argument)
{
    (void)argument;
    pend_irq(EXTI2_IRQn);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void check_irq_mask(void)
{
    const uint32_t shift = 8U - __NVIC_PRIO_BITS;
    const uint32_t syscall_priority = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >> shift;
    const uint32_t strict_mask = (uint32_t)COMPAT_IRQ_STRICT_THRESHOLD << shift;
    const bool is_sem_ready = rtos_sem_init(&s_irq_sem, 0U, 1U) == RTOS_OK;
    record_check("irq_setup", is_sem_ready);
    if (!is_sem_ready) {
        return;
    }

    NVIC_SetPriority(EXTI2_IRQn, syscall_priority);
    NVIC_SetPriority(EXTI3_IRQn, COMPAT_IRQ_REALTIME_PRIORITY);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);

    __set_BASEPRI(strict_mask);
    __ISB();
    RTOS_PORT_ENTER_CRITICAL();
    RTOS_PORT_ENTER_CRITICAL();
    pend_irq(EXTI2_IRQn);
    pend_irq(EXTI3_IRQn);
    const bool syscall_blocked = (s_irq_trace.syscall_entries == 0U) &&
        (NVIC_GetPendingIRQ(EXTI2_IRQn) != 0U);
    const uint32_t realtime_inside = s_irq_trace.realtime_entries;
    const uint32_t inner_mask = __get_BASEPRI();
    const uint32_t inner_global = __get_PRIMASK();
    RTOS_PORT_EXIT_CRITICAL();
    RTOS_PORT_EXIT_CRITICAL();
    const bool caller_restored = (__get_BASEPRI() == strict_mask) && (__get_PRIMASK() == 0U) &&
        (rtos_port_get_critical_nesting() == 0U) && (s_irq_trace.syscall_entries == 0U);
    __set_BASEPRI(0U);
    __DSB();
    __ISB();

    record_check("nested_section_preserves_caller_basepri", syscall_blocked && caller_restored &&
                 (inner_mask == strict_mask));
#if RTOS_CONFIG_USE_BASEPRI
    record_check("mask_policy_inside_section", (realtime_inside == 1U) && (inner_global == 0U));
#else
    record_check("mask_policy_inside_section", (realtime_inside == 0U) && (inner_global != 0U));
#endif

    const rtos_status_t token = rtos_sem_take(&s_irq_sem, RTOS_NO_WAIT);
    record_check("deferred_irq_and_nested_irq", (token == RTOS_OK) &&
                 (s_irq_trace.give_status == RTOS_OK) && (s_irq_trace.syscall_entries == 1U) &&
                 (s_irq_trace.realtime_entries == 2U) && (s_irq_trace.nested_entries == 1U) &&
                 s_irq_trace.has_syscall_exception && s_irq_trace.has_realtime_exception);

    __disable_irq();
    const rtos_status_t blocked_primask = rtos_sem_take(&s_irq_sem, 2U);
    const bool primask_kept = __get_PRIMASK() != 0U;
    __enable_irq();
    __ISB();

    __set_BASEPRI(strict_mask);
    __ISB();
    const rtos_status_t blocked_basepri = rtos_sem_take(&s_irq_sem, 2U);
    const bool basepri_kept = __get_BASEPRI() == strict_mask;
    __set_BASEPRI(0U);
    __ISB();
    record_check("blocking_rejects_caller_masks", (blocked_primask == RTOS_ERR_SCHED) &&
                 (blocked_basepri == RTOS_ERR_SCHED) && primask_kept && basepri_kept);

    const rtos_status_t created = rtos_task_create_static(&s_irq_task, s_irq_stack,
        COMPAT_STACK_WORDS, run_irq_source, NULL, COMPAT_FP_TASK_PRIORITY, "h7-irq");
    rtos_status_t woke = RTOS_ERR_SCHED;
    bool cleaned = false;
    if (created == RTOS_OK) {
        woke = rtos_sem_take(&s_irq_sem, COMPAT_IRQ_BUDGET);
        cleaned = rtos_task_delete(&s_irq_task) == RTOS_OK;
    }
    record_check("actual_irq_releases_blocked_thread", (created == RTOS_OK) && (woke == RTOS_OK) &&
                 (s_irq_trace.syscall_entries == 2U) && (s_irq_trace.nested_entries == 2U) &&
                 (s_irq_trace.realtime_entries == 3U) && (s_irq_trace.give_status == RTOS_OK));

    NVIC_DisableIRQ(EXTI2_IRQn);
    NVIC_DisableIRQ(EXTI3_IRQn);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    const rtos_status_t deinit = rtos_sem_deinit(&s_irq_sem);
    record_check("irq_cleanup", cleaned && (deinit == RTOS_OK) &&
                 (__get_BASEPRI() == 0U) && (__get_PRIMASK() == 0U));
    (void)printf("H7C|IRQ|syscall=%lu|realtime=%lu|nested=%lu\r\n",
        (unsigned long)s_irq_trace.syscall_entries, (unsigned long)s_irq_trace.realtime_entries,
        (unsigned long)s_irq_trace.nested_entries);
}

/* Keep S16-S31 live across a real blocking dispatch. C cannot bind all sixteen
 * registers at the dispatch boundary. The helper preserves the caller's AAPCS
 * callee-saved registers and maintains 8-byte stack alignment before BL. */
__attribute__((naked)) static void switch_fp_context(
    const uint32_t *expected __attribute__((unused)), uint32_t *observed __attribute__((unused)))
{
    __asm__ __volatile__(
        "push {r4, r5, r6, lr}    \n" /* Preserve registers and return address. */
        "vpush {s16-s31}          \n" /* Preserve the caller's FP context. */
        "mov r4, r0               \n" /* Keep input array across the C call. */
        "mov r5, r1               \n" /* Keep output array across the C call. */
        "vldmia r4, {s16-s31}     \n" /* Install this task's high FP patterns. */
        "movs r0, #1              \n" /* Block for one Tick in Thread mode. */
        "bl rtos_task_delay       \n"
        "vstmia r5, {s16-s31}     \n" /* Capture the restored register bits. */
        "vpop {s16-s31}           \n" /* Restore the original AAPCS state. */
        "pop {r4, r5, r6, pc}     \n");
}

static void run_fp_probe(void *argument)
{
    compat_fp_probe_t *const probe = (compat_fp_probe_t *)argument;
    uint32_t expected[COMPAT_FP_WORDS];
    uint32_t observed[COMPAT_FP_WORDS];
    for (uint32_t round = 0U; round < COMPAT_FP_ROUNDS; ++round) {
        uint32_t errors = 0U;
        for (uint32_t index = 0U; index < COMPAT_FP_WORDS; ++index) {
            expected[index] = probe->seed + (round << 16U) + (index << 3U);
        }
        switch_fp_context(expected, observed);
        for (uint32_t index = 0U; index < COMPAT_FP_WORDS; ++index) {
            errors += expected[index] != observed[index] ? 1U : 0U;
        }
        RTOS_PORT_ENTER_CRITICAL();
        probe->errors += errors;
        probe->rounds = round + 1U;
        RTOS_PORT_EXIT_CRITICAL();
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void check_fp_contexts(void)
{
    s_fp_probes[0].seed = 0x3F800100U;
    s_fp_probes[1].seed = 0x40000200U;
    const bool first_ready = rtos_task_create_static(&s_fp_probes[0].task, s_fp_stacks[0],
        COMPAT_STACK_WORDS, run_fp_probe, &s_fp_probes[0], COMPAT_FP_TASK_PRIORITY, "h7-fpu-a") ==
        RTOS_OK;
    const bool second_ready = rtos_task_create_static(&s_fp_probes[1].task, s_fp_stacks[1],
        COMPAT_STACK_WORDS, run_fp_probe, &s_fp_probes[1], COMPAT_FP_TASK_PRIORITY, "h7-fpu-b") ==
        RTOS_OK;

    if (first_ready && second_ready) {
        rtos_task_delay(COMPAT_FP_ROUNDS * 2U);
    }
    record_check("fpu_lazy_enabled", ((SCB->CPACR & (15UL << 20U)) == (15UL << 20U)) &&
                 ((FPU->FPCCR & (FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk)) ==
                  (FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk)));

    /* Workers write under the kernel mask. This higher-priority coordinator is
     * not preempted by a worker while taking the final snapshot and deleting it. */
    record_check("fpu_distinct_context_a", first_ready &&
                 (s_fp_probes[0].rounds == COMPAT_FP_ROUNDS) && (s_fp_probes[0].errors == 0U));
    record_check("fpu_distinct_context_b", second_ready &&
                 (s_fp_probes[1].rounds == COMPAT_FP_ROUNDS) && (s_fp_probes[1].errors == 0U));
    (void)printf("H7C|FP|rounds_a=%lu|rounds_b=%lu|errors_a=%lu|errors_b=%lu\r\n",
        (unsigned long)s_fp_probes[0].rounds, (unsigned long)s_fp_probes[1].rounds,
        (unsigned long)s_fp_probes[0].errors, (unsigned long)s_fp_probes[1].errors);
    const bool first_deleted = first_ready && (rtos_task_delete(&s_fp_probes[0].task) == RTOS_OK);
    const bool second_deleted = second_ready && (rtos_task_delete(&s_fp_probes[1].task) == RTOS_OK);
    record_check("fpu_cleanup", first_deleted && second_deleted);
}

static void keep_core_awake(void *argument)
{
    (void)argument;
    for (;;) {
        rtos_task_yield();
    }
}

static void check_dispatch_and_clock(void)
{
    const uint32_t initial_msp = __get_MSP();
    const uint32_t max_priority = (1UL << __NVIC_PRIO_BITS) - 1U;
    const uint32_t clock_cycles = SystemCoreClock / RTOS_CONFIG_TICK_RATE_HZ;
    const uint32_t required_tick_ctrl = SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk |
        SysTick_CTRL_CLKSOURCE_Msk;
    record_check("svc_enters_psp_thread", (__get_IPSR() == 0U) &&
                 ((__get_CONTROL() & CONTROL_SPSEL_Msk) != 0U) && ((initial_msp & 7U) == 0U) &&
                 (__get_BASEPRI() == 0U) && (__get_PRIMASK() == 0U));
    record_check("vector_and_systick_configuration", (NVIC_GetPriority(PendSV_IRQn) == max_priority)
                 && (NVIC_GetPriority(SysTick_IRQn) == max_priority)
                 && (NVIC_GetPriority(SVCall_IRQn) == 0U) && (clock_cycles > 1U)
                 && ((SysTick->CTRL & required_tick_ctrl) == required_tick_ctrl)
                 && (SysTick->LOAD == clock_cycles - 1U));

    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    __DSB();
    __ISB();
    const uint32_t probe_msp = __get_MSP();
    record_check("pendsv_no_candidate_msp_balance", (initial_msp == probe_msp) &&
                 ((SCB->ICSR & SCB_ICSR_PENDSVSET_Msk) == 0U));

    /* A delayed coordinator otherwise lets idle enter WFI and gate CYCCNT.
     * Reuse the static IRQ source task's storage before that test starts. */
    const bool is_awake_task_ready = rtos_task_create_static(&s_irq_task, s_irq_stack,
        COMPAT_STACK_WORDS, keep_core_awake, NULL, COMPAT_FP_TASK_PRIORITY, "h7-awake") == RTOS_OK;
    const rtos_tick_t initial_tick = rtos_sched_get_tick_count();
    const uint32_t initial_cycles = DWT->CYCCNT;
    for (uint32_t round = 0U; round < COMPAT_MSP_ROUNDS; ++round) {
        rtos_task_delay(1U);
    }
    const uint32_t elapsed_cycles = DWT->CYCCNT - initial_cycles;
    const rtos_tick_t elapsed_ticks = rtos_sched_get_tick_count() - initial_tick;
    const uint32_t final_msp = __get_MSP();
    const bool is_awake_task_deleted = is_awake_task_ready &&
        (rtos_task_delete(&s_irq_task) == RTOS_OK);
    const uint64_t expected_cycles = (uint64_t)elapsed_ticks * clock_cycles;
    const uint64_t tolerance = (uint64_t)clock_cycles + 1024U;
    record_check("pendsv_blocking_msp_balance", probe_msp == final_msp);
    record_check("hardware_tick_and_dwt_agree", is_awake_task_deleted &&
                 (elapsed_ticks >= COMPAT_MSP_ROUNDS) && (elapsed_cycles != 0U) &&
                 ((uint64_t)elapsed_cycles + tolerance >= expected_cycles)
                 && ((uint64_t)elapsed_cycles <= expected_cycles + tolerance));
    (void)printf("H7C|STACK|entry=%08lX|probe=%08lX|final=%08lX|ticks=%lu|cycles=%lu\r\n",
        (unsigned long)initial_msp, (unsigned long)probe_msp, (unsigned long)final_msp,
        (unsigned long)elapsed_ticks, (unsigned long)elapsed_cycles);
}

void h743_compat_run(void *argument)
{
    (void)argument;
    check_dispatch_and_clock();
    check_irq_mask();
    check_fp_contexts();
    (void)printf("H7C|DONE|pass=%lu|fail=%lu\r\n",
        (unsigned long)(s_checks - s_failures), (unsigned long)s_failures);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

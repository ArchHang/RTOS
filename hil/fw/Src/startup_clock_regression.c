/**
 * @file startup_clock_regression.c
 * @brief Verify clock rejection, unchanged startup state and retry on Cortex-M.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hil_target.h"
#include "rtos.h"

#define STARTUP_PROBE_PRIORITY 8U
#define STARTUP_PROBE_STACK_WORDS 384U
#define STARTUP_PROBE_BASEPRI UINT32_C(0x20)
#define STARTUP_MAX_CYCLES_PER_TICK UINT32_C(0x01000000)
#define STARTUP_SYSTICK_CTRL_MASK (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk | \
                                   SysTick_CTRL_CLKSOURCE_Msk)

typedef struct startup_state {
    rtos_tcb_t *current_tcb;
    rtos_tcb_t *next_tcb;
    rtos_sched_state_t scheduler;
    rtos_task_state_t probe;
    uint32_t ready_bitmap;
    uint32_t systick_load;
    uint32_t systick_ctrl;
    uint32_t fpu_control;
    uint32_t critical_nesting;
    uint32_t critical_saved_mask;
    uint32_t basepri;
    uint32_t primask;
} startup_state_t;

static rtos_tcb_t probe_tcb;
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t probe_stack[STARTUP_PROBE_STACK_WORDS];
static uint32_t pass_count;
static uint32_t fail_count;
static uint32_t saved_clock;

static void record_result(const char *name, bool is_valid)
{
    if (is_valid) {
        ++pass_count;
    } else {
        ++fail_count;
    }
    (void)printf("STARTUP|%s|%s\r\n", name, is_valid ? "PASS" : "FAIL");
}

static void record_summary(void)
{
    (void)printf("STARTUP|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)pass_count,
                 (unsigned long)fail_count);
}

static startup_state_t read_startup_state(void)
{
    return (startup_state_t){
        .current_tcb = rtos_kernel.current_tcb,
        .next_tcb = rtos_kernel.next_tcb,
        .scheduler = rtos_sched_get_state(),
        .probe = probe_tcb.state,
        .ready_bitmap = rtos_kernel.ready_bitmap,
        .systick_load = SysTick->LOAD,
        .systick_ctrl = SysTick->CTRL & STARTUP_SYSTICK_CTRL_MASK,
        .fpu_control = FPU->FPCCR & (FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk),
        .critical_nesting = rtos_port_crit_nest,
        .critical_saved_mask = rtos_port_saved_mask,
        .basepri = __get_BASEPRI(),
        .primask = __get_PRIMASK(),
    };
}

static bool startup_state_equal(const startup_state_t *before, const startup_state_t *after)
{
    return (before->current_tcb == after->current_tcb) && (before->next_tcb == after->next_tcb) &&
           (before->scheduler == after->scheduler) && (before->probe == after->probe) &&
           (before->ready_bitmap == after->ready_bitmap) &&
           (before->systick_load == after->systick_load) &&
           (before->systick_ctrl == after->systick_ctrl) &&
           (before->fpu_control == after->fpu_control) &&
           (before->critical_nesting == after->critical_nesting) &&
           (before->critical_saved_mask == after->critical_saved_mask) &&
           (before->basepri == after->basepri) && (before->primask == after->primask);
}

static void check_invalid_clock(const char *name, uint32_t core_clock, bool is_primask_set)
{
    const uint32_t previous_primask = __get_PRIMASK();
    const uint32_t previous_basepri = __get_BASEPRI();
    if (is_primask_set) {
        __disable_irq();
    }
    __set_BASEPRI(STARTUP_PROBE_BASEPRI);
    __ISB();

    const startup_state_t before = read_startup_state();
    SystemCoreClock = core_clock;
    const rtos_status_t status = rtos_start();
    const startup_state_t after = read_startup_state();
    SystemCoreClock = saved_clock;

    __set_BASEPRI(previous_basepri);
    __set_PRIMASK(previous_primask);
    __ISB();
    record_result(name, (status == RTOS_ERR_PARAM) && startup_state_equal(&before, &after));
}

static void unexpected_start(void *arg)
{
    (void)arg;
    (void)printf("STARTUP|unexpected_start|clock=%lu|load=%lu\r\n",
                 (unsigned long)SystemCoreClock, (unsigned long)SysTick->LOAD);
    record_result("one_cycle", false);
    record_summary();
    for (;;) {
        __WFI();
    }
}

void audit_pre_start(void)
{
    saved_clock = SystemCoreClock;
    (void)printf("STARTUP|BEGIN|clock=%lu|tick_hz=%lu\r\n", (unsigned long)saved_clock,
                 (unsigned long)RTOS_CONFIG_TICK_RATE_HZ);
    const rtos_status_t create = rtos_task_create_static(&probe_tcb, probe_stack,
                                                       STARTUP_PROBE_STACK_WORDS, unexpected_start,
                                                       NULL, STARTUP_PROBE_PRIORITY, "startup");
    record_result("probe_create", create == RTOS_OK);
    if (create != RTOS_OK) {
        return;
    }

    check_invalid_clock("one_cycle", RTOS_CONFIG_TICK_RATE_HZ, false);
    check_invalid_clock("zero_clock", 0U, true);
#if RTOS_CONFIG_TICK_RATE_HZ > 1
    check_invalid_clock("less_than_one_cycle", RTOS_CONFIG_TICK_RATE_HZ - 1U, false);
#endif
    const uint64_t overflow_clock = ((uint64_t)STARTUP_MAX_CYCLES_PER_TICK + 1U) *
                                    (uint64_t)RTOS_CONFIG_TICK_RATE_HZ;
    if (overflow_clock <= UINT32_MAX) {
        check_invalid_clock("reload_overflow", (uint32_t)overflow_clock, false);
    } else {
        (void)printf("STARTUP|reload_overflow|SKIP|outside_uint32_clock\r\n");
    }

    record_result("probe_delete", rtos_task_delete(&probe_tcb) == RTOS_OK);
}

void audit_regressions_task(void *arg)
{
    (void)arg;
    record_result("valid_start", (rtos_sched_get_state() == RTOS_SCHED_RUNNING) &&
                                 (rtos_task_get_current() != NULL) &&
                                 (SysTick->LOAD == (saved_clock / RTOS_CONFIG_TICK_RATE_HZ) - 1U));
    record_result("init_repeated", rtos_init() == RTOS_ERR_ALREADY);
    record_result("start_repeated", rtos_start() == RTOS_ERR_ALREADY);
    record_result("version", strcmp(rtos_get_version(), RTOS_VERSION_STRING) == 0);
    const uint32_t initial_tick = rtos_sched_get_tick_count();
    rtos_task_delay(2U);
    record_result("valid_tick_progress", (rtos_sched_get_tick_count() - initial_tick) >= 2U);
    record_summary();
    for (;;) {
        rtos_task_delay(1000U);
    }
}

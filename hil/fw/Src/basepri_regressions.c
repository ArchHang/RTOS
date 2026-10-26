/**
 * @file basepri_regressions.c
 * @brief Deterministic BASEPRI interleavings on the real STM32F446 interrupt core.
 *
 * Test hooks only request a real TIM7 IRQ; no simulated ISR or register shadow is
 * used. High-priority IRQs exercise the low-level mask primitive, not IPC APIs:
 * their priority does not permit RTOS object access. Thread observations happen
 * after the complete outer pair. An IRQ observing a nonzero BASEPRI while it
 * preempts a critical body is expected, and is tested separately.
 *
 * After every observation the fixture explicitly repairs the original mask and
 * nesting state under PRIMASK, so an intentionally failing old kernel can print
 * its results. Such repair is evidence collection, never production recovery.
 * Shared 32-bit counters have one ISR writer; thread snapshots and resets use
 * PRIMASK. Volatile supplies visibility, not mutual exclusion.
 */
#include "basepri_regressions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "rtos.h"

#if !defined(RTOS_HIL_CRITICAL_TEST) || !defined(FIRMWARE_AUDIT)
#error "basepri_regressions.c requires the dedicated basepri HIL variant"
#endif

#if !defined(STM32F446xx) || !defined(__ARM_ARCH_7EM__)
#error "basepri_regressions.c requires the STM32F446 Cortex-M4 target"
#endif

#define BP_ARRAY_SIZE(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))
#define BP_IRQ_WAIT_CYCLES (SystemCoreClock / 100U)
#define BP_IRQ_WAIT_POLLS (1000000U)
#define BP_PEER_ROUNDS (1000U)
#define BP_STRESS_PAIRS (1000000U)
#define BP_STRESS_SECONDS (10U)
#define BP_TIMER_CLOCK_HZ (84000000U)
#define BP_COUNTER_CLOCK_HZ (1000000U)
#define BP_PEER_STACK_WORDS (256U)

typedef struct bp_timer_state {
    uint32_t clock;
    uint32_t cr1;
    uint32_t cr2;
    uint32_t dier;
    uint32_t psc;
    uint32_t arr;
    uint32_t cnt;
    uint32_t priority;
    uint32_t is_enabled;
    uint32_t is_pending;
} bp_timer_state_t;

typedef struct bp_case_config {
    uint32_t stage;
    uint32_t priority;
    uint32_t basepri;
    uint32_t primask;
    uint32_t depth;
} bp_case_config_t;

typedef struct bp_case_result {
    uint32_t basepri;
    uint32_t primask;
    uint32_t nesting;
    uint32_t hook_seen;
    uint32_t hook_primask;
    uint32_t irq_in_hook;
    uint32_t irq_before_repair;
    uint32_t irq_count;
    uint32_t irq_errors;
    uint32_t irq_basepri;
    uint32_t irq_nesting;
    uint32_t irq_ipsr;
    uint32_t irq_priority;
} bp_case_result_t;

typedef struct bp_irq_state {
    volatile uint32_t count;
    volatile uint32_t errors;
    volatile uint32_t entry_basepri;
    volatile uint32_t entry_nesting;
    volatile uint32_t entry_ipsr;
    volatile uint32_t entry_priority;
    volatile uint32_t is_periodic;
    uint32_t random_state;
} bp_irq_state_t;

static bp_timer_state_t saved_timer;
static bp_irq_state_t irq_state;
static uint32_t timer_is_owned;
static uint32_t pass_count;
static uint32_t fail_count;
static uint32_t matrix_count;
static uint32_t matrix_fail_count;
static volatile uint32_t hook_is_armed;
static volatile uint32_t hook_stage;
static volatile uint32_t hook_seen;
static volatile uint32_t hook_primask;
static volatile uint32_t hook_irq_count;
static rtos_sem_t peer_request;
static rtos_sem_t peer_reply;
static rtos_tcb_t peer_tcb;
static rtos_stack_t peer_stack[BP_PEER_STACK_WORDS] __attribute__((aligned(8)));
static volatile uint32_t peer_completed;
static volatile uint32_t peer_errors;

static void check_result(const char *name, uint32_t is_valid)
{
    if (is_valid != 0U) {
        pass_count++;
        (void)printf("BP|CHECK|%s|PASS\r\n", name);
    } else {
        fail_count++;
        (void)printf("BP|CHECK|%s|FAIL\r\n", name);
    }
}

/* CYCCNT subtraction intentionally uses modulo-2^32 arithmetic. Every bounded
 * interval is shorter than one wrap (about 25 seconds at 168 MHz). */
static uint32_t elapsed_cycles(uint32_t started)
{
    return (uint32_t)(DWT->CYCCNT - started);
}

static uint32_t wait_for_irq(void)
{
    uint32_t started = DWT->CYCCNT;

    for (uint32_t poll = 0U; poll < BP_IRQ_WAIT_POLLS; poll++) {
        if (irq_state.count != 0U) {
            return 1U;
        }
        if (elapsed_cycles(started) >= BP_IRQ_WAIT_CYCLES) {
            break;
        }
        __NOP();
    }
    return 0U;
}

static void repair_thread_state(void)
{
    __disable_irq();
    hook_is_armed = 0U;
    rtos_port_crit_nest = 0U;
    rtos_port_saved_mask = 0U;
    __set_BASEPRI(0U);
    __set_PRIMASK(0U);
    __ISB();
}

static void reset_irq_state(void)
{
    memset((void *)&irq_state, 0, sizeof(irq_state));
    irq_state.random_state = 0xC001D00DU;
    hook_is_armed = 0U;
    hook_seen = 0U;
    hook_primask = 0U;
    hook_irq_count = 0U;
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    TIM7->SR = 0U;
}

static uint32_t timer_acquire(void)
{
    uint32_t primask = __get_PRIMASK();

    if ((primask != 0U) || (__get_BASEPRI() != 0U)
        || (rtos_port_get_critical_nesting() != 0U)
        || (HAL_RCC_GetPCLK1Freq() != BP_TIMER_CLOCK_HZ / 2U)
        || ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV4)
        || ((RCC->DCKCFGR & RCC_DCKCFGR_TIMPRE) != 0U)
        || (NVIC_GetPriorityGrouping() != 3U)) {
        return 0U;
    }
    __disable_irq();
    saved_timer.clock = RCC->APB1ENR & RCC_APB1ENR_TIM7EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM7EN;
    (void)RCC->APB1ENR;
    __DSB();
    if (((TIM7->CR1 & TIM_CR1_CEN) != 0U) || (TIM7->DIER != 0U)
        || (TIM7->SR != 0U) || (NVIC_GetEnableIRQ(TIM7_IRQn) != 0U)
        || (NVIC_GetPendingIRQ(TIM7_IRQn) != 0U)
        || (NVIC_GetActive(TIM7_IRQn) != 0U)) {
        RCC->APB1ENR = (RCC->APB1ENR & ~RCC_APB1ENR_TIM7EN) | saved_timer.clock;
        __set_PRIMASK(primask);
        return 0U;
    }
    saved_timer.cr1 = TIM7->CR1;
    saved_timer.cr2 = TIM7->CR2;
    saved_timer.dier = TIM7->DIER;
    saved_timer.psc = TIM7->PSC;
    saved_timer.arr = TIM7->ARR;
    saved_timer.cnt = TIM7->CNT;
    saved_timer.priority = NVIC_GetPriority(TIM7_IRQn);
    saved_timer.is_enabled = NVIC_GetEnableIRQ(TIM7_IRQn);
    saved_timer.is_pending = NVIC_GetPendingIRQ(TIM7_IRQn);
    TIM7->CR1 = 0U;
    TIM7->CR2 = 0U;
    TIM7->DIER = 0U;
    TIM7->PSC = BP_TIMER_CLOCK_HZ / BP_COUNTER_CLOCK_HZ - 1U;
    TIM7->ARR = 36U;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0U;
    TIM7->CNT = 0U;
    timer_is_owned = 1U;
    NVIC_SetPriority(TIM7_IRQn, 0U);
    NVIC_EnableIRQ(TIM7_IRQn);
    __set_PRIMASK(primask);
    __ISB();
    return 1U;
}

static uint32_t timer_release(void)
{
    uint32_t is_valid;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    TIM7->CR1 = 0U;
    TIM7->DIER = 0U;
    NVIC_DisableIRQ(TIM7_IRQn);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    TIM7->CR2 = saved_timer.cr2;
    TIM7->PSC = saved_timer.psc;
    TIM7->ARR = saved_timer.arr;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0U;
    TIM7->CNT = saved_timer.cnt;
    TIM7->CR1 = saved_timer.cr1;
    TIM7->DIER = saved_timer.dier;
    NVIC_SetPriority(TIM7_IRQn, saved_timer.priority);
    if (saved_timer.is_pending != 0U) {
        NVIC_SetPendingIRQ(TIM7_IRQn);
    }
    if (saved_timer.is_enabled != 0U) {
        NVIC_EnableIRQ(TIM7_IRQn);
    }
    is_valid = (TIM7->CR1 == saved_timer.cr1) && (TIM7->CR2 == saved_timer.cr2)
        && (TIM7->DIER == saved_timer.dier) && (TIM7->PSC == saved_timer.psc)
        && (TIM7->ARR == saved_timer.arr) && (TIM7->CNT == saved_timer.cnt)
        && (TIM7->SR == 0U) && (NVIC_GetPriority(TIM7_IRQn) == saved_timer.priority)
        && (NVIC_GetEnableIRQ(TIM7_IRQn) == saved_timer.is_enabled)
        && (NVIC_GetPendingIRQ(TIM7_IRQn) == saved_timer.is_pending);
    RCC->APB1ENR = (RCC->APB1ENR & ~RCC_APB1ENR_TIM7EN) | saved_timer.clock;
    __DSB();
    is_valid = is_valid && ((RCC->APB1ENR & RCC_APB1ENR_TIM7EN) == saved_timer.clock);
    timer_is_owned = 0U;
    __set_PRIMASK(primask);
    __ISB();
    return is_valid;
}

void TIM7_IRQHandler(void)
{
    uint32_t saved_basepri = __get_BASEPRI();
    uint32_t saved_primask = __get_PRIMASK();
    uint32_t saved_nesting = rtos_port_crit_nest;
    uint32_t depth;

    TIM7->SR = ~(uint32_t)TIM_SR_UIF;
    irq_state.entry_basepri = saved_basepri;
    irq_state.entry_nesting = saved_nesting;
    irq_state.entry_ipsr = __get_IPSR();
    irq_state.entry_priority = NVIC_GetPriority(TIM7_IRQn);
    depth = (irq_state.count & 1U) + 1U;
    for (uint32_t index = 0U; index < depth; index++) {
        RTOS_PORT_ENTER_CRITICAL();
    }
    for (uint32_t index = 0U; index < depth; index++) {
        RTOS_PORT_EXIT_CRITICAL();
    }
    if ((__get_BASEPRI() != saved_basepri) || (__get_PRIMASK() != saved_primask)
        || (rtos_port_crit_nest != saved_nesting)) {
        irq_state.errors++;
    }
    irq_state.count++;
    if (irq_state.is_periodic != 0U) {
        uint32_t random_state = irq_state.random_state;

        random_state ^= random_state << 13U;
        random_state ^= random_state >> 17U;
        random_state ^= random_state << 5U;
        irq_state.random_state = random_state;
        /* Vary the next real timer period from 37 to 73 microseconds. */
        TIM7->ARR = 36U + random_state % 37U;
    }
}

void rtos_port_critical_test_hook(uint32_t stage)
{
    if ((hook_is_armed == 0U) || (stage != hook_stage)) {
        return;
    }
    if (((stage == RTOS_PORT_TEST_EXIT_COUNTED) || (stage == RTOS_PORT_TEST_EXIT_RESTORED))
        && (rtos_port_crit_nest != 0U)) {
        return;
    }
    hook_is_armed = 0U;
    hook_seen = 1U;
    hook_primask = __get_PRIMASK();
    NVIC_SetPendingIRQ(TIM7_IRQn);
    __DSB();
    __ISB();
    hook_irq_count = irq_state.count;
}

static void capture_case(bp_case_result_t *result)
{
    result->basepri = __get_BASEPRI();
    result->primask = __get_PRIMASK();
    result->nesting = rtos_port_crit_nest;
    result->hook_seen = hook_seen;
    result->hook_primask = hook_primask;
    result->irq_in_hook = hook_irq_count;
    result->irq_before_repair = irq_state.count;
}

static void run_case(const bp_case_config_t *config, bp_case_result_t *result)
{
    __disable_irq();
    reset_irq_state();
    NVIC_SetPriority(TIM7_IRQn, config->priority);
    hook_stage = config->stage;
    hook_is_armed = 1U;
    __set_BASEPRI(config->basepri);
    __set_PRIMASK(config->primask);
    __ISB();
    for (uint32_t index = 0U; index < config->depth; index++) {
        RTOS_PORT_ENTER_CRITICAL();
    }
    for (uint32_t index = 0U; index < config->depth; index++) {
        RTOS_PORT_EXIT_CRITICAL();
    }
    capture_case(result);
    /* Snapshot first; only then unmask the baseline so evidence can be printed. */
    repair_thread_state();
    (void)wait_for_irq();
    __disable_irq();
    result->irq_count = irq_state.count;
    result->irq_errors = irq_state.errors;
    result->irq_basepri = irq_state.entry_basepri;
    result->irq_nesting = irq_state.entry_nesting;
    result->irq_ipsr = irq_state.entry_ipsr;
    result->irq_priority = irq_state.entry_priority;
    __set_PRIMASK(0U);
    __ISB();
}

static void report_case(const bp_case_config_t *config, const bp_case_result_t *result)
{
    uint32_t is_valid = (result->basepri == config->basepri)
        && (result->primask == config->primask) && (result->nesting == 0U)
        && (result->hook_seen == 1U) && (result->irq_count == 1U)
        && (result->irq_errors == 0U)
        && (result->irq_ipsr == (uint32_t)TIM7_IRQn + 16U)
        && (result->irq_priority == config->priority);

    matrix_count++;
    if (is_valid != 0U) {
        pass_count++;
    } else {
        fail_count++;
        matrix_fail_count++;
    }
    (void)printf("BP|CASE|%s|id=%lu stage=%lu prio=%lu in_bp=0x%02lX "
                 "in_pm=%lu depth=%lu out_bp=0x%02lX out_pm=%lu nest=%lu "
                 "hook=%lu hook_pm=%lu in_hook=%lu before_repair=%lu "
                 "irq=%lu irq_errors=%lu irq_bp=0x%02lX irq_nest=%lu\r\n",
                 is_valid != 0U ? "PASS" : "FAIL", (unsigned long)matrix_count,
                 (unsigned long)config->stage, (unsigned long)config->priority,
                 (unsigned long)config->basepri, (unsigned long)config->primask,
                 (unsigned long)config->depth, (unsigned long)result->basepri,
                 (unsigned long)result->primask, (unsigned long)result->nesting,
                 (unsigned long)result->hook_seen, (unsigned long)result->hook_primask,
                 (unsigned long)result->irq_in_hook, (unsigned long)result->irq_before_repair,
                 (unsigned long)result->irq_count, (unsigned long)result->irq_errors,
                 (unsigned long)result->irq_basepri, (unsigned long)result->irq_nesting);
}

static void run_matrix_masks(uint32_t stage, uint32_t priority)
{
    static const uint32_t masks[] = {0U, 0x20U, 0x50U, 0x80U, 0xF0U};
    static const uint32_t depths[] = {1U, 2U, 8U};
    bp_case_config_t config = {stage, priority, 0U, 0U, 1U};
    bp_case_result_t result;

    for (uint32_t mask = 0U; mask < BP_ARRAY_SIZE(masks); mask++) {
        config.basepri = masks[mask];
        for (uint32_t primask = 0U; primask <= 1U; primask++) {
            config.primask = primask;
            for (uint32_t depth = 0U; depth < BP_ARRAY_SIZE(depths); depth++) {
                config.depth = depths[depth];
                run_case(&config, &result);
                report_case(&config, &result);
            }
        }
    }
}

static void run_matrix(void)
{
    static const uint32_t priorities[] = {0U, 4U, 5U, 15U};

    for (uint32_t stage = RTOS_PORT_TEST_ENTER_MASKED;
         stage <= RTOS_PORT_TEST_EXIT_RESTORED; stage++) {
        for (uint32_t priority = 0U; priority < BP_ARRAY_SIZE(priorities); priority++) {
            run_matrix_masks(stage, priorities[priority]);
        }
    }
    (void)printf("BP|MATRIX|cases=%lu fail=%lu stages=5 priorities=4 masks=5 "
                 "primasks=2 depths=3|repair=explicit_after_snapshot\r\n",
                 (unsigned long)matrix_count, (unsigned long)matrix_fail_count);
    check_result("matrix_complete", matrix_count == 600U);
}

static void run_body_irq_check(uint32_t priority)
{
    uint32_t ran_in_body;
    uint32_t body_basepri;
    uint32_t entry_basepri;
    uint32_t final_basepri;
    uint32_t final_nesting;
    uint32_t irq_count;
    uint32_t is_expected_in_body;

    __disable_irq();
    reset_irq_state();
    NVIC_SetPriority(TIM7_IRQn, priority);
    __set_PRIMASK(0U);
    RTOS_PORT_ENTER_CRITICAL();
    body_basepri = __get_BASEPRI();
    NVIC_SetPendingIRQ(TIM7_IRQn);
    __DSB();
    __ISB();
    ran_in_body = irq_state.count;
    entry_basepri = irq_state.entry_basepri;
    RTOS_PORT_EXIT_CRITICAL();
    final_basepri = __get_BASEPRI();
    final_nesting = rtos_port_crit_nest;
    (void)wait_for_irq();
    __disable_irq();
    irq_count = irq_state.count;
    __set_PRIMASK(0U);
#if RTOS_CONFIG_USE_BASEPRI
    is_expected_in_body = ((priority << (8U - __NVIC_PRIO_BITS))
        < RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY) ? 1U : 0U;
#else
    is_expected_in_body = 0U;
#endif
    (void)printf("BP|BODY|prio=%lu ran_inside=%lu expected=%lu body_bp=0x%02lX "
                 "irq_bp=0x%02lX final_bp=0x%02lX nest=%lu irq=%lu\r\n",
                 (unsigned long)priority, (unsigned long)ran_in_body,
                 (unsigned long)is_expected_in_body, (unsigned long)body_basepri,
                 (unsigned long)entry_basepri, (unsigned long)final_basepri,
                 (unsigned long)final_nesting, (unsigned long)irq_count);
    check_result("body_irq_delivery", (ran_in_body == is_expected_in_body)
        && (irq_count == 1U) && (final_basepri == 0U) && (final_nesting == 0U));
    if (is_expected_in_body != 0U) {
        check_result("nonzero_irq_sample_is_expected",
            entry_basepri == RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY);
    }
    repair_thread_state();
}

static void run_pair_measurement(void)
{
    uint32_t started = DWT->CYCCNT;
    uint32_t cycles;

    for (uint32_t pair = 0U; pair < 10000U; pair++) {
        RTOS_PORT_ENTER_CRITICAL();
        RTOS_PORT_EXIT_CRITICAL();
    }
    cycles = elapsed_cycles(started);
    (void)printf("BP|TIMING|pairs=10000 cycles=%lu|hooks=compiled_in "
                 "includes_interrupts=1 production_benchmark=0\r\n",
                 (unsigned long)cycles);
    check_result("pair_state_balanced", (__get_BASEPRI() == 0U)
        && (__get_PRIMASK() == 0U) && (rtos_port_crit_nest == 0U));
}

static void run_periodic_stress(void)
{
    uint32_t started;
    uint32_t completed = 0U;
    uint32_t leaks = 0U;
    uint32_t tick_before = rtos_sched_get_tick_count();
    uint32_t entries;
    uint32_t errors;

    __disable_irq();
    reset_irq_state();
    irq_state.is_periodic = 1U;
    NVIC_SetPriority(TIM7_IRQn, 0U);
    TIM7->CNT = 0U;
    TIM7->ARR = 36U;
    TIM7->DIER = TIM_DIER_UIE;
    TIM7->CR1 = TIM_CR1_CEN;
    __set_PRIMASK(0U);
    started = DWT->CYCCNT;
    for (uint32_t pair = 0U; pair < BP_STRESS_PAIRS; pair++) {
        RTOS_PORT_ENTER_CRITICAL();
        if ((pair & 7U) == 0U) {
            RTOS_PORT_ENTER_CRITICAL();
            RTOS_PORT_EXIT_CRITICAL();
        }
        RTOS_PORT_EXIT_CRITICAL();
        completed++;
        if ((__get_BASEPRI() != 0U) || (__get_PRIMASK() != 0U)
            || (rtos_port_crit_nest != 0U)) {
            leaks++;
            repair_thread_state();
        }
        if (elapsed_cycles(started) > SystemCoreClock * BP_STRESS_SECONDS) {
            break;
        }
    }
    __disable_irq();
    TIM7->CR1 = 0U;
    TIM7->DIER = 0U;
    TIM7->SR = 0U;
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    irq_state.is_periodic = 0U;
    entries = irq_state.count;
    errors = irq_state.errors;
    __set_PRIMASK(0U);
    __ISB();
    (void)printf("BP|STRESS|pairs=%lu requested=%lu irq=%lu irq_errors=%lu "
                 "thread_leaks=%lu ticks=%lu seed=0xC001D00D "
                 "irq_period_us=37..73 repair=explicit_on_failure\r\n",
                 (unsigned long)completed, (unsigned long)BP_STRESS_PAIRS,
                 (unsigned long)entries, (unsigned long)errors, (unsigned long)leaks,
                 (unsigned long)(rtos_sched_get_tick_count() - tick_before));
    check_result("million_pairs_complete", completed == BP_STRESS_PAIRS);
    check_result("periodic_real_irq_coverage", entries >= 1000U);
    check_result("periodic_no_mask_leak", (leaks == 0U) && (errors == 0U));
    check_result("systick_progress_under_irq", rtos_sched_get_tick_count() != tick_before);
}

static void peer_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (rtos_sem_take(&peer_request, RTOS_WAIT_FOREVER) != RTOS_OK) {
            peer_errors++;
            continue;
        }
        if ((__get_BASEPRI() != 0U) || (__get_PRIMASK() != 0U)
            || (rtos_port_crit_nest != 0U)) {
            peer_errors++;
        }
        peer_completed++;
        if (rtos_sem_give(&peer_reply) != RTOS_OK) {
            peer_errors++;
        }
    }
}

static uint32_t wait_for_peer_reply(void)
{
    uint32_t started = DWT->CYCCNT;

    for (uint32_t poll = 0U; poll < BP_IRQ_WAIT_POLLS; poll++) {
        if (rtos_sem_take(&peer_reply, RTOS_NO_WAIT) == RTOS_OK) {
            return 1U;
        }
        if (elapsed_cycles(started) >= BP_IRQ_WAIT_CYCLES) {
            break;
        }
    }
    return 0U;
}

static void run_peer_progress(void)
{
    uint32_t completed = 0U;
    uint32_t switches_before = rtos_kernel.switch_count;
    uint32_t request_ready = rtos_sem_init(&peer_request, 0U, 1U) == RTOS_OK;
    uint32_t reply_ready = rtos_sem_init(&peer_reply, 0U, 1U) == RTOS_OK;
    uint32_t peer_ready = 0U;

    peer_completed = 0U;
    peer_errors = 0U;
    if ((request_ready != 0U) && (reply_ready != 0U)) {
        peer_ready = rtos_task_create_static(&peer_tcb, peer_stack, BP_PEER_STACK_WORDS,
            peer_task, NULL, 8U, "bp-peer") == RTOS_OK;
    }
    check_result("peer_setup", peer_ready);
    if (peer_ready != 0U) {
        for (uint32_t round = 0U; round < BP_PEER_ROUNDS; round++) {
            if ((rtos_sem_give(&peer_request) != RTOS_OK) || (wait_for_peer_reply() == 0U)) {
                break;
            }
            completed++;
        }
        (void)printf("BP|PROGRESS|requests=%lu replies=%lu peer_errors=%lu switches=%lu\r\n",
                     (unsigned long)completed, (unsigned long)peer_completed,
                     (unsigned long)peer_errors,
                     (unsigned long)(rtos_kernel.switch_count - switches_before));
        check_result("peer_ipc_progress", (completed == BP_PEER_ROUNDS)
            && (peer_completed == BP_PEER_ROUNDS) && (peer_errors == 0U));
        check_result("pendsv_progress", rtos_kernel.switch_count > switches_before);
        check_result("peer_delete", rtos_task_delete(&peer_tcb) == RTOS_OK);
    }
    if (request_ready != 0U) {
        check_result("peer_request_cleanup", rtos_sem_deinit(&peer_request) == RTOS_OK);
    }
    if (reply_ready != 0U) {
        check_result("peer_reply_cleanup", rtos_sem_deinit(&peer_reply) == RTOS_OK);
    }
    check_result("controller_mask_after_switch", (__get_BASEPRI() == 0U)
        && (__get_PRIMASK() == 0U) && (rtos_port_crit_nest == 0U));
}

void audit_pre_start(void)
{
    (void)printf("BP|META|stm32f446 basepri_regressions|use_basepri=%lu asserts=%lu "
                 "threshold=0x%02lX|prestart_only=1\r\n",
                 (unsigned long)RTOS_CONFIG_USE_BASEPRI,
                 (unsigned long)RTOS_CONFIG_ASSERT_ENABLED,
                 (unsigned long)RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY);
}

void audit_regressions_task(void *arg)
{
    (void)arg;
    (void)printf("BP|BEGIN|matrix_expected=600|irq=TIM7|repair=explicit_after_snapshot\r\n");
    check_result("dwt_running", (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
    check_result("timer_ownership", timer_acquire());
    if (timer_is_owned != 0U) {
        run_matrix();
        run_body_irq_check(0U);
        run_body_irq_check(4U);
        run_body_irq_check(5U);
        run_body_irq_check(15U);
        run_pair_measurement();
        run_periodic_stress();
        check_result("timer_state_restored", timer_release());
        run_peer_progress();
    }
    (void)printf("BP|SUMMARY|pass=%lu fail=%lu cases=%lu matrix_fail=%lu\r\n",
                 (unsigned long)pass_count, (unsigned long)fail_count,
                 (unsigned long)matrix_count, (unsigned long)matrix_fail_count);
    (void)printf("BP|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

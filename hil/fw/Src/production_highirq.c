/**
 * @file production_highirq.c
 * @brief Priority-zero hardware IRQ interference without any kernel API use.
 *
 * TIM2 alternates 37/71-us periods and can preempt both syscall IRQs and tasks.
 * Active TIM6/TIM7 NVIC bits prove actual nesting; a pending request issued by
 * the nested TIM7 handler provides deterministic three-level IRQ coverage.
 * BASEPRI samples are observations, not leak verdicts. Only thread safe points
 * outside every caller-owned critical section can diagnose a leaked mask.
 *
 * Single-core ownership and thread snapshots use saved PRIMASK. The handler has
 * one writer per statistic, and never accesses IPC, scheduler state, a critical
 * primitive, dynamic memory, logging, or floating-point state. Volatile supplies
 * register-width visibility; PRIMASK supplies exclusion for the other writers.
 */
#include "production_highirq.h"

#include <stdint.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#if (!defined(FIRMWARE_PRODUCTION_STRESS) || (FIRMWARE_PRODUCTION_STRESS != 1)) \
    && !defined(FIRMWARE_BASEPRI_BLOCKING) && !defined(FIRMWARE_ADVERSARIAL_WAIT)
#error "production_highirq.c requires a dedicated stress or blocking-test firmware"
#endif

#if !defined(STM32F446xx) || !defined(__ARM_ARCH_7EM__)
#error "production_highirq.c requires the STM32F446 Cortex-M4 target"
#endif

#define PRODUCTION_HIGHIRQ_TIMER_CLOCK_HZ (84000000U)
#define PRODUCTION_HIGHIRQ_COUNTER_CLOCK_HZ (1000000U)
#define PRODUCTION_HIGHIRQ_SHORT_PERIOD_US (37U)
#define PRODUCTION_HIGHIRQ_LONG_PERIOD_US (71U)
#define PRODUCTION_HIGHIRQ_RELOAD_POLLS (1024U)

typedef struct production_highirq_timer {
    uint32_t clock;
    uint32_t cr1;
    uint32_t cr2;
    uint32_t dier;
    uint32_t cnt;
    uint32_t psc;
    uint32_t arr;
    uint32_t priority;
    uint32_t is_enabled;
    uint32_t is_pending;
} production_highirq_timer_t;

static production_highirq_timer_t saved_timer;
static volatile production_highirq_snapshot_t highirq_state;
static volatile uint32_t forced_pending;
static uint32_t is_owned;

static rtos_status_t check_start_context(void)
{
    if (__get_IPSR() != 0U) {
        return RTOS_ERR_ISR;
    }
    if (((__get_CONTROL() & CONTROL_nPRIV_Msk) != 0U) || (__get_BASEPRI() != 0U)
        || (__get_FAULTMASK() != 0U)) {
        return RTOS_ERR_SCHED;
    }
    if ((SystemCoreClock != 168000000U)
        || (HAL_RCC_GetPCLK1Freq() != PRODUCTION_HIGHIRQ_TIMER_CLOCK_HZ / 2U)
        || ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV4)
        || ((RCC->DCKCFGR & RCC_DCKCFGR_TIMPRE) != 0U)
        || (NVIC_GetPriorityGrouping() != 3U)) {
        return RTOS_ERR_PARAM;
    }
    return RTOS_OK;
}

static uint32_t timer_is_idle(void)
{
    /* Slave triggers, enabled channels and IRQ/DMA requests indicate another
     * owner even when its counter happens to be stopped at this instant. */
    return ((TIM2->CR1 & TIM_CR1_CEN) == 0U) && (TIM2->DIER == 0U)
        && (TIM2->SR == 0U) && (TIM2->CCER == 0U) && (TIM2->SMCR == 0U)
        && ((TIM2->CR2 & TIM_CR2_MMS) == 0U)
        && (NVIC_GetEnableIRQ(TIM2_IRQn) == 0U)
        && (NVIC_GetPendingIRQ(TIM2_IRQn) == 0U)
        && (NVIC_GetActive(TIM2_IRQn) == 0U);
}

static void save_timer(void)
{
    saved_timer.cr1 = TIM2->CR1;
    saved_timer.cr2 = TIM2->CR2;
    saved_timer.dier = TIM2->DIER;
    saved_timer.cnt = TIM2->CNT;
    saved_timer.psc = TIM2->PSC;
    saved_timer.arr = TIM2->ARR;
    saved_timer.priority = NVIC_GetPriority(TIM2_IRQn);
    saved_timer.is_enabled = NVIC_GetEnableIRQ(TIM2_IRQn);
    saved_timer.is_pending = NVIC_GetPendingIRQ(TIM2_IRQn);
}

static uint32_t reload_timer(uint32_t *poll_count)
{
    /* URS/UDIS must be zero at both callers. A pre-existing UIF cannot serve
     * as acknowledgement of this UG, so clear and synchronize it first.
     * DSB completes the APB write, not the timer's internal update pipeline.
     * Polling UIF acknowledges the event that transfers PSC/ARR preloads.
     * The fixed bound also terminates when the peripheral clock is broken. */
    TIM2->SR = 0U;
    (void)TIM2->SR;
    __DSB();
    TIM2->EGR = TIM_EGR_UG;
    __DSB();
    for (uint32_t poll = 0U; poll < PRODUCTION_HIGHIRQ_RELOAD_POLLS; poll++) {
        if ((TIM2->SR & TIM_SR_UIF) != 0U) {
            *poll_count = poll + 1U;
            return 1U;
        }
    }
    *poll_count = PRODUCTION_HIGHIRQ_RELOAD_POLLS;
    highirq_state.reload_timeouts++;
    return 0U;
}

static uint32_t configure_timer(void)
{
    uint32_t poll_count;
    uint32_t updated;

    TIM2->CR1 = TIM_CR1_ARPE;
    TIM2->CR2 = 0U;
    TIM2->DIER = 0U;
    TIM2->PSC = PRODUCTION_HIGHIRQ_TIMER_CLOCK_HZ / PRODUCTION_HIGHIRQ_COUNTER_CLOCK_HZ
        - 1U;
    TIM2->ARR = PRODUCTION_HIGHIRQ_SHORT_PERIOD_US - 1U;
    TIM2->CNT = 0U;
    updated = reload_timer(&poll_count);
    highirq_state.configure_reload_polls = poll_count;
    TIM2->SR = 0U;
    (void)TIM2->SR;
    __DSB();
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    __DSB();
    if (updated == 0U) {
        return 0U;
    }
    NVIC_SetPriority(TIM2_IRQn, 0U);
    TIM2->DIER = TIM_DIER_UIE;
    NVIC_EnableIRQ(TIM2_IRQn);
    return 1U;
}

rtos_status_t production_highirq_start(void)
{
    uint32_t primask;
    rtos_status_t status = check_start_context();

    if (status != RTOS_OK) {
        return status;
    }
    primask = __get_PRIMASK();
    __disable_irq();
    if (is_owned != 0U) {
        __set_PRIMASK(primask);
        return RTOS_ERR_ALREADY;
    }
    saved_timer.clock = RCC->APB1ENR & RCC_APB1ENR_TIM2EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    (void)RCC->APB1ENR;
    __DSB();
    if (timer_is_idle() == 0U) {
        RCC->APB1ENR = (RCC->APB1ENR & ~RCC_APB1ENR_TIM2EN) | saved_timer.clock;
        __DSB();
        __set_PRIMASK(primask);
        return RTOS_ERR_ALREADY;
    }
    save_timer();
    memset((void *)&highirq_state, 0, sizeof(highirq_state));
    forced_pending = 0U;
    highirq_state.work_checksum = 0x13579BDFU;
    highirq_state.is_running = 1U;
    is_owned = 1U;
    if (configure_timer() == 0U) {
        highirq_state.errors++;
        /* Ownership was acquired before configuration, so the same stop path
         * restores every register even when the update handshake failed. */
        status = production_highirq_stop();
        __set_PRIMASK(primask);
        __ISB();
        return status == RTOS_OK ? RTOS_ERR_PARAM : status;
    }
    TIM2->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
    __DSB();
    __set_PRIMASK(primask);
    __ISB();
    return RTOS_OK;
}

static void quiesce_timer(void)
{
    /* An asserted peripheral source can re-pend even a disabled NVIC line.
     * Complete the APB source deassertion before clearing the NVIC latch. */
    TIM2->DIER = 0U;
    TIM2->CR1 &= ~(uint32_t)TIM_CR1_CEN;
    TIM2->SR = 0U;
    (void)TIM2->DIER;
    (void)TIM2->CR1;
    (void)TIM2->SR;
    __DSB();
    NVIC_DisableIRQ(TIM2_IRQn);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    __DSB();
    __ISB();
}

static uint32_t timer_restore_mismatch(void)
{
    uint32_t mismatch = 0U;

    if (TIM2->CR1 != saved_timer.cr1) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_CR1;
    }
    if (TIM2->CR2 != saved_timer.cr2) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_CR2;
    }
    if (TIM2->DIER != saved_timer.dier) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_DIER;
    }
    if (TIM2->PSC != saved_timer.psc) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_PSC;
    }
    if (TIM2->ARR != saved_timer.arr) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_ARR;
    }
    if (TIM2->CNT != saved_timer.cnt) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_CNT;
    }
    highirq_state.restore_sr_actual = TIM2->SR;
    if (highirq_state.restore_sr_actual != 0U) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_SR;
    }
    if (NVIC_GetPriority(TIM2_IRQn) != saved_timer.priority) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_PRIORITY;
    }
    if (NVIC_GetEnableIRQ(TIM2_IRQn) != saved_timer.is_enabled) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_ENABLE;
    }
    highirq_state.restore_pending_after = NVIC_GetPendingIRQ(TIM2_IRQn);
    if (highirq_state.restore_pending_after != saved_timer.is_pending) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_PENDING;
    }
    return mismatch;
}

static uint32_t restore_timer(void)
{
    uint32_t poll_count;
    uint32_t updated;
    uint32_t mismatch;

    TIM2->CR1 = 0U;
    TIM2->CR2 = saved_timer.cr2;
    TIM2->PSC = saved_timer.psc;
    TIM2->ARR = saved_timer.arr;
    updated = reload_timer(&poll_count);
    highirq_state.restore_reload_polls = poll_count;
    TIM2->CNT = saved_timer.cnt;
    TIM2->CR1 = saved_timer.cr1;
    TIM2->DIER = saved_timer.dier;
    /* Clear after both the acknowledged update and the final CNT write, so
     * update/capture-compare flags generated by restoration are not retained. */
    __DSB();
    TIM2->SR = 0U;
    (void)TIM2->SR;
    __DSB();
    NVIC_SetPriority(TIM2_IRQn, saved_timer.priority);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    __DSB();
    if (saved_timer.is_pending != 0U) {
        NVIC_SetPendingIRQ(TIM2_IRQn);
    }
    if (saved_timer.is_enabled != 0U) {
        NVIC_EnableIRQ(TIM2_IRQn);
    }
    __DSB();
    __ISB();
    mismatch = timer_restore_mismatch();
    if (updated == 0U) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_UPDATE_TIMEOUT;
    }
    return mismatch;
}

rtos_status_t production_highirq_stop(void)
{
    uint32_t primask;
    uint32_t mismatch;

    if (__get_IPSR() != 0U) {
        return RTOS_ERR_ISR;
    }
    primask = __get_PRIMASK();
    __disable_irq();
    if (is_owned == 0U) {
        __set_PRIMASK(primask);
        return RTOS_ERR_ALREADY;
    }
    highirq_state.stop_pending_before = NVIC_GetPendingIRQ(TIM2_IRQn);
    quiesce_timer();
    highirq_state.stop_pending_after = NVIC_GetPendingIRQ(TIM2_IRQn);
    if (forced_pending != 0U) {
        highirq_state.errors++;
    }
    forced_pending = 0U;
    highirq_state.is_running = 0U;
    mismatch = restore_timer();
    RCC->APB1ENR = (RCC->APB1ENR & ~RCC_APB1ENR_TIM2EN) | saved_timer.clock;
    __DSB();
    if ((RCC->APB1ENR & RCC_APB1ENR_TIM2EN) != saved_timer.clock) {
        mismatch |= PRODUCTION_HIGHIRQ_RESTORE_CLOCK;
    }
    highirq_state.restore_mismatch = mismatch;
    highirq_state.restored = mismatch == 0U ? 1U : 0U;
    if (mismatch != 0U) {
        highirq_state.errors++;
    }
    is_owned = 0U;
    __set_PRIMASK(primask);
    __ISB();
    return highirq_state.errors == 0U ? RTOS_OK : RTOS_ERR_PARAM;
}

rtos_status_t production_highirq_snapshot(production_highirq_snapshot_t *snapshot)
{
    uint32_t primask;

    if (snapshot == NULL) {
        return RTOS_ERR_NULL;
    }
    if (__get_IPSR() != 0U) {
        return RTOS_ERR_ISR;
    }
    primask = __get_PRIMASK();
    __disable_irq();
    *snapshot = highirq_state;
    __set_PRIMASK(primask);
    __ISB();
    return RTOS_OK;
}

void production_highirq_force_nested(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    if ((highirq_state.is_running == 0U)
        || (__get_IPSR() != (uint32_t)TIM7_IRQn + 16U)
        || (NVIC_GetActive(TIM6_DAC_IRQn) == 0U)) {
        highirq_state.errors++;
    } else if (forced_pending == 0U) {
        forced_pending = 1U;
        highirq_state.forced_requests++;
        NVIC_SetPendingIRQ(TIM2_IRQn);
    }
    __DSB();
    __set_PRIMASK(primask);
    __ISB();
}

void TIM2_IRQHandler(void)
{
    uint32_t natural;
    uint32_t forced;
    uint32_t active_low;
    uint32_t active_medium;
    uint32_t work;

    if (highirq_state.is_running == 0U) {
        /* No ownership: never acknowledge an unrelated owner's timer status. */
        return;
    }
    natural = (TIM2->SR & TIM_SR_UIF) != 0U ? 1U : 0U;
    TIM2->SR = ~(uint32_t)TIM_SR_UIF;
    forced = forced_pending;
    forced_pending = 0U;
    highirq_state.entries++;
    highirq_state.natural_updates += natural;
    highirq_state.forced_observed += forced;
    highirq_state.last_basepri = __get_BASEPRI();
    highirq_state.last_ipsr = __get_IPSR();
    highirq_state.priority = NVIC_GetPriority(TIM2_IRQn);
    if (highirq_state.last_basepri != 0U) {
        highirq_state.nonzero_basepri_samples++;
    }
    active_low = NVIC_GetActive(TIM6_DAC_IRQn);
    active_medium = NVIC_GetActive(TIM7_IRQn);
    if ((SCB->ICSR & SCB_ICSR_RETTOBASE_Msk) == 0U) {
        highirq_state.nested_entries++;
    }
    if ((active_low != 0U) && (active_medium != 0U)) {
        highirq_state.triple_nested_entries++;
    }
    if ((natural | forced) == 0U) {
        highirq_state.spurious_entries++;
    }
    if ((highirq_state.last_ipsr != (uint32_t)TIM2_IRQn + 16U)
        || (highirq_state.priority != 0U) || (__get_PRIMASK() != 0U)) {
        highirq_state.errors++;
    }
    /* The fixed eight-step checksum deliberately uses unsigned modular math. */
    work = highirq_state.work_checksum ^ highirq_state.entries;
    for (uint32_t index = 0U; index < 8U; index++) {
        work = ((work << 5U) | (work >> 27U)) ^ (0x9E3779B9U + index);
    }
    highirq_state.work_checksum = work;
    if (natural != 0U) {
        /* ARPE avoids shrinking ARR beneath an already advanced counter. */
        TIM2->ARR = ((highirq_state.natural_updates & 1U) != 0U
            ? PRODUCTION_HIGHIRQ_LONG_PERIOD_US : PRODUCTION_HIGHIRQ_SHORT_PERIOD_US) - 1U;
    }
}

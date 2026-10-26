/**
 * @file production_irq.c
 * @brief Real TIM6/TIM7 interrupts driving shared production stress IPC objects.
 *
 * TIM6 uses an 84 MHz APB1 timer clock, 1 MHz counter, 1000 us period, priority 7.
 * TIM7 uses the same clock and a 701 us period, priority 6. Both priorities are
 * within the kernel syscall range. Vector names retain the CMSIS startup ABI.
 * The fixture rejects occupied timers/DAC IRQ use rather than taking them over.
 * ISR statistics use short kernel critical sections. Preemption is deliberately
 * allowed between IPC calls, but never inside an unprotected counter update.
 * Independent TIM2 priority-zero load preempts these handlers without using the
 * RTOS. Its counters use their own saved-PRIMASK snapshots and lifetime control.
 */
#include "production_irq.h"

#include <stdint.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "production_stress.h"
#include "rtos_event.h"
#include "rtos_port.h"
#include "rtos_queue.h"
#include "rtos_sched.h"
#include "rtos_sem.h"

#if !defined(FIRMWARE_PRODUCTION_STRESS) || (FIRMWARE_PRODUCTION_STRESS != 1)
#error "production_irq.c requires FIRMWARE_PRODUCTION_STRESS=1"
#endif

#if !defined(STM32F446xx) || !defined(__ARM_ARCH_7EM__)
#error "production_irq.c supports only STM32F446 Cortex-M4 stress firmware"
#endif

#define PRODUCTION_IRQ_TIMER_CLOCK_HZ (84000000U)
#define PRODUCTION_IRQ_COUNTER_CLOCK_HZ (1000000U)
#define PRODUCTION_IRQ_FORCE_INTERVAL (128U)
#define PRODUCTION_IRQ_CLOCK_MASK ((uint32_t)(RCC_APB1ENR_TIM6EN | RCC_APB1ENR_TIM7EN))

typedef struct production_timer_state {
    uint32_t cr1;
    uint32_t cr2;
    uint32_t dier;
    uint32_t cnt;
    uint32_t psc;
    uint32_t arr;
    uint32_t priority;
} production_timer_state_t;

static TIM_TypeDef *const timer_instances[2] = {TIM6, TIM7};
static const IRQn_Type timer_irq_numbers[2] = {TIM6_DAC_IRQn, TIM7_IRQn};
static const uint32_t timer_priorities[2] = {7U, 6U};
static const uint32_t timer_periods_us[2] = {1000U, 701U};
static production_irq_config_t irq_config;
static production_irq_snapshot_t irq_snapshot;
static production_timer_state_t saved_timers[2];
static uint32_t saved_clock_mask;
static uint32_t irq_depth;
static uint32_t forced_pending;
static uint32_t has_started;

static rtos_status_t check_thread_context(void)
{
    if (__get_IPSR() != 0U) {
        return RTOS_ERR_ISR;
    }
    if (((__get_CONTROL() & CONTROL_nPRIV_Msk) != 0U) || (__get_PRIMASK() != 0U)
        || (__get_BASEPRI() != 0U) || (__get_FAULTMASK() != 0U)
        || (rtos_port_get_critical_nesting() != 0U)
        || (rtos_sched_get_state() != RTOS_SCHED_RUNNING)) {
        return RTOS_ERR_SCHED;
    }
    return RTOS_OK;
}

static rtos_status_t check_config(const production_irq_config_t *config)
{
    if ((config == NULL) || (config->queue == NULL) || (config->sem == NULL)
        || (config->event == NULL)) {
        return RTOS_ERR_NULL;
    }
    if ((config->queue->is_initialized == 0U) || (config->sem->is_initialized == 0U)
        || (config->event->is_initialized == 0U)
        || (config->queue->item_size != (uint32_t)sizeof(production_packet_t))) {
        return RTOS_ERR_PARAM;
    }
    /* TIMPRE=0 doubles PCLK1 when APB1 is divided; this variant requires /4. */
    if ((HAL_RCC_GetPCLK1Freq() != (PRODUCTION_IRQ_TIMER_CLOCK_HZ / 2U))
        || (SystemCoreClock != 168000000U)
        || ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV4)
        || ((RCC->DCKCFGR & RCC_DCKCFGR_TIMPRE) != 0U)
        || (NVIC_GetPriorityGrouping() != 3U)
        || ((timer_priorities[1] << (8U - __NVIC_PRIO_BITS))
            < RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY)) {
        return RTOS_ERR_PARAM;
    }
    return RTOS_OK;
}

static rtos_bool_t are_timers_idle(void)
{
    /* TIM6 shares its vector with DAC underrun: no other producer may own it. */
    if (((RCC->APB1ENR & RCC_APB1ENR_DACEN) != 0U)
        && ((DAC->CR & (DAC_CR_DMAUDRIE1 | DAC_CR_DMAUDRIE2)) != 0U)) {
        return RTOS_FALSE;
    }
    for (uint32_t index = 0U; index < 2U; index++) {
        TIM_TypeDef *timer = timer_instances[index];
        IRQn_Type irq_number = timer_irq_numbers[index];

        if (((timer->CR1 & TIM_CR1_CEN) != 0U) || (timer->DIER != 0U)
            || (timer->SR != 0U) || (NVIC_GetEnableIRQ(irq_number) != 0U)
            || (NVIC_GetPendingIRQ(irq_number) != 0U)
            || (NVIC_GetActive(irq_number) != 0U)) {
            return RTOS_FALSE;
        }
    }
    return RTOS_TRUE;
}

static void save_timer_state(uint32_t index)
{
    TIM_TypeDef *timer = timer_instances[index];
    production_timer_state_t *state = &saved_timers[index];

    state->cr1 = timer->CR1;
    state->cr2 = timer->CR2;
    state->dier = timer->DIER;
    state->cnt = timer->CNT;
    state->psc = timer->PSC;
    state->arr = timer->ARR;
    state->priority = NVIC_GetPriority(timer_irq_numbers[index]);
}

static void configure_timer(uint32_t index)
{
    TIM_TypeDef *timer = timer_instances[index];
    IRQn_Type irq_number = timer_irq_numbers[index];

    timer->CR1 = 0U;
    timer->CR2 = 0U;
    timer->DIER = 0U;
    timer->PSC = (PRODUCTION_IRQ_TIMER_CLOCK_HZ / PRODUCTION_IRQ_COUNTER_CLOCK_HZ) - 1U;
    timer->ARR = timer_periods_us[index] - 1U;
    timer->CNT = 0U;
    timer->EGR = TIM_EGR_UG;
    timer->SR = 0U;
    NVIC_ClearPendingIRQ(irq_number);
    NVIC_SetPriority(irq_number, timer_priorities[index]);
    timer->DIER = TIM_DIER_UIE;
    NVIC_EnableIRQ(irq_number);
}

static rtos_bool_t restore_timer_state(uint32_t index)
{
    TIM_TypeDef *timer = timer_instances[index];
    const production_timer_state_t *state = &saved_timers[index];
    IRQn_Type irq_number = timer_irq_numbers[index];

    timer->CR1 = 0U;
    timer->CR2 = state->cr2;
    timer->PSC = state->psc;
    timer->ARR = state->arr;
    /* Load the original prescaler with interrupts disabled, then restore CNT. */
    timer->EGR = TIM_EGR_UG;
    timer->SR = 0U;
    timer->CNT = state->cnt;
    timer->CR1 = state->cr1;
    timer->DIER = state->dier;
    NVIC_ClearPendingIRQ(irq_number);
    NVIC_SetPriority(irq_number, state->priority);
    if ((timer->CR1 != state->cr1) || (timer->CR2 != state->cr2)
        || (timer->DIER != state->dier) || (timer->CNT != state->cnt)
        || (timer->PSC != state->psc) || (timer->ARR != state->arr)
        || (timer->SR != 0U) || (NVIC_GetEnableIRQ(irq_number) != 0U)
        || (NVIC_GetPendingIRQ(irq_number) != 0U)
        || (NVIC_GetPriority(irq_number) != state->priority)) {
        return RTOS_FALSE;
    }
    return RTOS_TRUE;
}

rtos_status_t production_irq_start(const production_irq_config_t *config)
{
    uint32_t primask;
    rtos_status_t status = check_thread_context();

    if (status != RTOS_OK) {
        return status;
    }
    status = check_config(config);
    if (status != RTOS_OK) {
        return status;
    }
    primask = __get_PRIMASK();
    __disable_irq();
    if (irq_snapshot.is_running != 0U) {
        __set_PRIMASK(primask);
        return RTOS_ERR_ALREADY;
    }
    saved_clock_mask = RCC->APB1ENR & PRODUCTION_IRQ_CLOCK_MASK;
    RCC->APB1ENR |= PRODUCTION_IRQ_CLOCK_MASK;
    /* Complete the RCC clock write before reading the timer peripheral. */
    (void)RCC->APB1ENR;
    __DSB();
    if (are_timers_idle() == RTOS_FALSE) {
        RCC->APB1ENR = (RCC->APB1ENR & ~PRODUCTION_IRQ_CLOCK_MASK) | saved_clock_mask;
        __DSB();
        __set_PRIMASK(primask);
        return RTOS_ERR_ALREADY;
    }
    status = production_highirq_start();
    if (status != RTOS_OK) {
        /* TIM6/TIM7 have only had their clocks enabled, not been configured. */
        RCC->APB1ENR = (RCC->APB1ENR & ~PRODUCTION_IRQ_CLOCK_MASK) | saved_clock_mask;
        __DSB();
        __set_PRIMASK(primask);
        return status;
    }
    irq_config = *config;
    memset(&irq_snapshot, 0, sizeof(irq_snapshot));
    irq_depth = 0U;
    forced_pending = 0U;
    for (uint32_t index = 0U; index < 2U; index++) {
        save_timer_state(index);
        configure_timer(index);
    }
    has_started = 1U;
    irq_snapshot.is_running = 1U;
    TIM6->CR1 = TIM_CR1_CEN;
    TIM7->CR1 = TIM_CR1_CEN;
    __DSB();
    __set_PRIMASK(primask);
    __ISB();
    return RTOS_OK;
}

rtos_status_t production_irq_stop(void)
{
    uint32_t primask;
    rtos_status_t status = check_thread_context();

    if (status != RTOS_OK) {
        return status;
    }
    primask = __get_PRIMASK();
    __disable_irq();
    if (irq_snapshot.is_running == 0U) {
        __set_PRIMASK(primask);
        return RTOS_ERR_ALREADY;
    }
    /* A thread cannot overlap an active handler on this single-core target. */
    for (uint32_t index = 0U; index < 2U; index++) {
        timer_instances[index]->DIER = 0U;
        timer_instances[index]->CR1 &= ~(uint32_t)TIM_CR1_CEN;
        NVIC_DisableIRQ(timer_irq_numbers[index]);
        timer_instances[index]->SR = 0U;
        NVIC_ClearPendingIRQ(timer_irq_numbers[index]);
    }
    __DSB();
    irq_snapshot.is_running = 0U;
    forced_pending = 0U;
    if (irq_depth != 0U) {
        irq_snapshot.api_error_count++;
        status = RTOS_ERR_PARAM;
    }
    if (production_highirq_stop() != RTOS_OK) {
        irq_snapshot.api_error_count++;
        status = RTOS_ERR_PARAM;
    }
    for (uint32_t index = 0U; index < 2U; index++) {
        if (restore_timer_state(index) == RTOS_FALSE) {
            irq_snapshot.api_error_count++;
            status = RTOS_ERR_PARAM;
        }
    }
    RCC->APB1ENR = (RCC->APB1ENR & ~PRODUCTION_IRQ_CLOCK_MASK) | saved_clock_mask;
    __DSB();
    if ((RCC->APB1ENR & PRODUCTION_IRQ_CLOCK_MASK) != saved_clock_mask) {
        irq_snapshot.api_error_count++;
        status = RTOS_ERR_PARAM;
    }
    __set_PRIMASK(primask);
    __ISB();
    return status;
}

rtos_status_t production_irq_snapshot(production_irq_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return RTOS_ERR_NULL;
    }
    if (__get_IPSR() != 0U) {
        return RTOS_ERR_ISR;
    }
    RTOS_PORT_ENTER_CRITICAL();
    *snapshot = irq_snapshot;
    RTOS_PORT_EXIT_CRITICAL();
    return production_highirq_snapshot(&snapshot->highirq);
}

rtos_status_t production_irq_status(void)
{
    rtos_status_t status = RTOS_OK;
    production_highirq_snapshot_t highirq;

    if (__get_IPSR() != 0U) {
        return RTOS_ERR_ISR;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (has_started == 0U) {
        status = RTOS_ERR_ALREADY;
    } else if (irq_snapshot.api_error_count != 0U) {
        status = RTOS_ERR_PARAM;
    }
    RTOS_PORT_EXIT_CRITICAL();
    if (status == RTOS_OK) {
        status = production_highirq_snapshot(&highirq);
        if ((status == RTOS_OK) && (highirq.errors != 0U)) {
            status = RTOS_ERR_PARAM;
        }
    }
    return status;
}

static rtos_bool_t capture_irq_entry(uint32_t index, rtos_bool_t *should_force)
{
    TIM_TypeDef *timer = timer_instances[index];
    uint32_t natural;
    uint32_t forced = 0U;
    rtos_bool_t should_deliver = RTOS_FALSE;

    *should_force = RTOS_FALSE;
    RTOS_PORT_ENTER_CRITICAL();
    natural = ((timer->SR & TIM_SR_UIF) != 0U) ? 1U : 0U;
    timer->SR = ~(uint32_t)TIM_SR_UIF;
    if (index == 1U) {
        forced = forced_pending;
        forced_pending = 0U;
    }
    irq_depth++;
    irq_snapshot.entries[index]++;
    irq_snapshot.natural[index] += natural;
    irq_snapshot.forced[index] += forced;
    irq_snapshot.ipsr[index] = __get_IPSR();
    irq_snapshot.priority[index] = NVIC_GetPriority(timer_irq_numbers[index]);
    if (irq_depth > irq_snapshot.depth_max) {
        irq_snapshot.depth_max = irq_depth;
    }
    if (irq_depth > 1U) {
        irq_snapshot.nesting_count++;
        irq_snapshot.forced_observed += forced;
    }
    if ((irq_snapshot.ipsr[index] != ((uint32_t)timer_irq_numbers[index] + 16U))
        || (irq_snapshot.priority[index] != timer_priorities[index])) {
        irq_snapshot.api_error_count++;
    }
    if ((irq_snapshot.is_running != 0U) && ((natural | forced) != 0U)) {
        should_deliver = RTOS_TRUE;
        if ((index == 0U) && (natural != 0U)
            && ((irq_snapshot.natural[0] % PRODUCTION_IRQ_FORCE_INTERVAL) == 0U)) {
            *should_force = RTOS_TRUE;
        }
    } else {
        irq_snapshot.spurious[index]++;
    }
    RTOS_PORT_EXIT_CRITICAL();
    return should_deliver;
}

static void request_forced_irq(void)
{
    /* Publish reason and pending IRQ atomically. Preemption occurs after exit. */
    RTOS_PORT_ENTER_CRITICAL();
    if (forced_pending == 0U) {
        forced_pending = 1U;
        irq_snapshot.forced_requests++;
        NVIC_SetPendingIRQ(TIM7_IRQn);
    }
    RTOS_PORT_EXIT_CRITICAL();
    __DSB();
    __ISB();
}

static void deliver_irq_ipc(uint32_t index)
{
    production_packet_t packet;
    rtos_status_t queue_status;
    rtos_status_t sem_status;

    RTOS_PORT_ENTER_CRITICAL();
    packet.source = index + 4U;
    packet.sequence = irq_snapshot.sent[index];
    RTOS_PORT_EXIT_CRITICAL();
    packet.payload = production_stress_payload(packet.source, packet.sequence, irq_config.seed);
    packet.checksum = production_stress_checksum(&packet);
    queue_status = rtos_queue_send(irq_config.queue, &packet, RTOS_NO_WAIT, RTOS_FALSE);
    sem_status = rtos_sem_give(irq_config.sem);
    /* A zero return can be valid when CLEAR_ON_EXIT waiters consume the bit. */
    (void)rtos_event_set(irq_config.event, (rtos_event_bits_t)(1U << index));

    RTOS_PORT_ENTER_CRITICAL();
    if (queue_status == RTOS_OK) {
        irq_snapshot.sent[index]++;
    } else if ((queue_status == RTOS_ERR_OVERFLOW) || (queue_status == RTOS_ERR_TIMEOUT)) {
        irq_snapshot.rejected[index]++;
    } else {
        irq_snapshot.api_error_count++;
    }
    if (sem_status == RTOS_OK) {
        irq_snapshot.sem_give_success++;
    } else if (sem_status == RTOS_ERR_OVERFLOW) {
        irq_snapshot.sem_give_overflow++;
    } else {
        irq_snapshot.api_error_count++;
    }
    irq_snapshot.event_set_count++;
    RTOS_PORT_EXIT_CRITICAL();
}

static void handle_timer_irq(uint32_t index)
{
    rtos_bool_t should_force;
    rtos_bool_t should_deliver = capture_irq_entry(index, &should_force);

    if (should_force == RTOS_TRUE) {
        request_forced_irq();
    }
    if ((index == 1U) && (irq_depth >= 2U)) {
        production_highirq_force_nested();
    }
    if (should_deliver == RTOS_TRUE) {
        deliver_irq_ipc(index);
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (irq_depth == 0U) {
        irq_snapshot.api_error_count++;
    } else {
        irq_depth--;
    }
    RTOS_PORT_EXIT_CRITICAL();
}

/** @brief CMSIS TIM6/DAC vector; DAC interrupt ownership is rejected at start. */
void TIM6_DAC_IRQHandler(void)
{
    handle_timer_irq(0U);
}

/** @brief CMSIS TIM7 vector; serves real updates and recorded software pending. */
void TIM7_IRQHandler(void)
{
    handle_timer_irq(1U);
}

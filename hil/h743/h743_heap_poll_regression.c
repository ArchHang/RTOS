/** @file h743_heap_poll_regression.c
 *  @brief Verify optimized reads of TIM6-updated heap statistics on STM32F446/H743.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "production_timer_target.h"
#include "rtos.h"
#include "rtos_heap.h"

#if defined(STM32F446xx)
#define HEAP_POLL_ENTRY f446_heap_poll_regression_run
#define HEAP_POLL_RECORD "F4MP"
#else
#define HEAP_POLL_ENTRY h743_heap_poll_regression_run
#define HEAP_POLL_RECORD "H7MP"
#endif

enum {
    HEAP_POLL_COUNTER_HZ = 100000U,
    HEAP_POLL_TIMER_RELOAD = 499U, /* 5 ms */
    HEAP_POLL_EARLIEST_MS = 4U,
    HEAP_POLL_DEADLINE_MS = 50U,
    HEAP_POLL_REQUEST_BYTES = 8U,
};

typedef enum heap_poll_kind {
    HEAP_POLL_ALLOC_COUNT = 0,
    HEAP_POLL_FREE_COUNT,
    HEAP_POLL_FREE_BYTES,
} heap_poll_kind_t;

static _Atomic uint32_t s_kind;
static _Atomic uint32_t s_irq_succeeded;
static void *s_block;

void TIM6_DAC_IRQHandler(void);
void HEAP_POLL_ENTRY(void *argument);

/** @brief Apply the published heap operation on a TIM6 update. */
void TIM6_DAC_IRQHandler(void)
{
    if ((TIM6->SR & TIM_SR_UIF) == 0U) {
        return;
    }
    TIM6->SR = 0U;

    const uint32_t kind = atomic_load_explicit(&s_kind, memory_order_acquire);
    if (kind == HEAP_POLL_ALLOC_COUNT) {
        s_block = rtos_heap_alloc(HEAP_POLL_REQUEST_BYTES);
        if (s_block == NULL) {
            return;
        }
    } else {
        if (s_block == NULL) {
            return;
        }
        rtos_heap_free(s_block);
        s_block = NULL;
    }
    atomic_store_explicit(&s_irq_succeeded, 1U, memory_order_release);
}

static void stop_timer(void)
{
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
    TIM6->DIER = 0U;
    TIM6->CR1 = 0U;
    TIM6->SR = 0U;
    NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
    __DSB();
}

static void arm_timer(heap_poll_kind_t kind)
{
    stop_timer();
    atomic_store_explicit(&s_irq_succeeded, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_kind, (uint32_t)kind, memory_order_release);
    TIM6->CR1 = TIM_CR1_URS;
    TIM6->PSC = (PRODUCTION_TIMER_CLOCK_HZ / HEAP_POLL_COUNTER_HZ) - 1U;
    TIM6->ARR = HEAP_POLL_TIMER_RELOAD;
    TIM6->EGR = TIM_EGR_UG;
    TIM6->SR = 0U;
    TIM6->DIER = TIM_DIER_UIE;
    NVIC_EnableIRQ(TIM6_DAC_IRQn);
    TIM6->CR1 = TIM_CR1_URS | TIM_CR1_OPM | TIM_CR1_CEN;
}

static uint32_t read_heap_stat(heap_poll_kind_t kind)
{
    switch (kind) {
    case HEAP_POLL_ALLOC_COUNT:
        return rtos_heap_get_alloc_count();
    case HEAP_POLL_FREE_COUNT:
        return rtos_heap_get_free_count();
    case HEAP_POLL_FREE_BYTES:
        return rtos_heap_get_free();
    default:
        return 0U;
    }
}

static bool poll_until_irq(heap_poll_kind_t kind, uint32_t expected)
{
    const uint32_t cycles_limit = (SystemCoreClock / 1000U) * HEAP_POLL_DEADLINE_MS;
    const uint32_t before = read_heap_stat(kind);
    uint32_t observed;
    arm_timer(kind);
    const uint32_t start = DWT->CYCCNT;
    do {
        observed = read_heap_stat(kind);
    } while ((observed != expected) && (DWT->CYCCNT - start < cycles_limit));
    const uint32_t elapsed = DWT->CYCCNT - start;
    stop_timer();

    /* A completion load inside the loop could mask a stale statistic. */
    const uint32_t succeeded = atomic_load_explicit(&s_irq_succeeded, memory_order_acquire);
    const uint32_t earliest = (SystemCoreClock / 1000U) * HEAP_POLL_EARLIEST_MS;
    const bool is_passed = (before != expected) && (observed == expected) &&
                           (elapsed >= earliest) && (elapsed < cycles_limit) && (succeeded == 1U);
    (void)printf(HEAP_POLL_RECORD "|CASE|kind=%u|before=%" PRIu32 "|observed=%" PRIu32
                 "|expected=%" PRIu32 "|irq=%" PRIu32 "|cycles=%" PRIu32 "|pass=%u\r\n",
                 (unsigned int)kind, before, observed, expected, succeeded, elapsed,
                 is_passed ? 1U : 0U);
    return is_passed;
}

static bool timer_is_available(void)
{
    if (NVIC_GetPriorityGrouping() != 3U || NVIC_GetEnableIRQ(TIM6_DAC_IRQn) != 0U ||
        NVIC_GetPendingIRQ(TIM6_DAC_IRQn) != 0U || NVIC_GetActive(TIM6_DAC_IRQn) != 0U) {
        return false;
    }
    if ((PRODUCTION_TIMER_APB_ENABLE & PRODUCTION_TIMER_DAC_ENABLE) != 0U &&
        (PRODUCTION_TIMER_DAC->CR & (DAC_CR_DMAUDRIE1 | DAC_CR_DMAUDRIE2)) != 0U) {
        return false;
    }
    return (PRODUCTION_TIMER_APB_ENABLE & PRODUCTION_TIMER_TIM6_ENABLE) == 0U;
}

/** @brief Run three heap observations with the board's reserved TIM6.
 *  @param[in] argument Unused task argument.
 */
void HEAP_POLL_ENTRY(void *argument)
{
    (void)argument;
    uint32_t pass_count = 0U;
    const uint32_t capacity = rtos_heap_get_max_alloc();
    const uint32_t initial_alloc = rtos_heap_get_alloc_count();
    const uint32_t initial_free = rtos_heap_get_free_count();
    const uint32_t priority = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >>
                              (8U - __NVIC_PRIO_BITS);
    if (!PRODUCTION_TIMER_PROFILE_VALID() || !timer_is_available() ||
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
        /* The fixture cannot establish its time base or claim the vector. */
        (void)printf(HEAP_POLL_RECORD "|SETUP|FAIL\r\n"
                     HEAP_POLL_RECORD "|DONE|pass=0|fail=4\r\n");
        return;
    }
    const uint32_t previous_priority = NVIC_GetPriority(TIM6_DAC_IRQn);
    PRODUCTION_TIMER_APB_ENABLE |= PRODUCTION_TIMER_TIM6_ENABLE;
    (void)PRODUCTION_TIMER_APB_ENABLE;
    __DSB();
    NVIC_SetPriority(TIM6_DAC_IRQn, priority);
    (void)printf(HEAP_POLL_RECORD "|BEGIN|core_hz=%" PRIu32 "|timer_hz=%" PRIu32
                 "|irq_priority=%" PRIu32 "\r\n", SystemCoreClock,
                 (uint32_t)PRODUCTION_TIMER_CLOCK_HZ, priority);
    if (poll_until_irq(HEAP_POLL_ALLOC_COUNT, initial_alloc + 1U)) {
        ++pass_count;
    }
    if (poll_until_irq(HEAP_POLL_FREE_COUNT, initial_free + 1U)) {
        ++pass_count;
    }
    s_block = rtos_heap_alloc(capacity);
    if ((s_block != NULL) && poll_until_irq(HEAP_POLL_FREE_BYTES, capacity)) {
        ++pass_count;
    }
    if (s_block != NULL) {
        rtos_heap_free(s_block);
        s_block = NULL;
    }
    if ((rtos_heap_get_max_alloc() == capacity) &&
        (rtos_heap_get_alloc_count() == initial_alloc + 2U) &&
        (rtos_heap_get_free_count() == initial_free + 2U)) {
        ++pass_count;
    }
    NVIC_SetPriority(TIM6_DAC_IRQn, previous_priority);
    PRODUCTION_TIMER_APB_ENABLE &= ~(uint32_t)PRODUCTION_TIMER_TIM6_ENABLE;
    (void)printf(HEAP_POLL_RECORD "|DONE|pass=%" PRIu32 "|fail=%" PRIu32 "\r\n",
                 pass_count, 4U - pass_count);
}

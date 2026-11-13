/** @file f446_heap_irq_latency.c
 *  @brief Measure real TIM5 compare response and instrumented heap critical sections.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx.h"
#include "rtos_port.h"

#ifndef INDUSTRY_BACKEND
#define INDUSTRY_BACKEND unknown
#endif

#ifndef HEAP_IRQ_INSTRUMENTED
#define HEAP_IRQ_INSTRUMENTED 0
#endif

#define IRQ_STRING_IMPL(value) #value
#define IRQ_STRING(value) IRQ_STRING_IMPL(value)

enum {
    IRQ_MASKED_PRIORITY = 6U,
    IRQ_HIGH_PRIORITY = 4U,
    IRQ_SAMPLES = 96U,
    IRQ_HOLES = 128U,
    IRQ_SLOT_COUNT = 512U,
    IRQ_TELEMETRY_DEPTH = 16U,
    IRQ_TELEMETRY_ROUNDS = 2U,
    IRQ_FRAME_COUNT = 96U,
    IRQ_KNOWN_HOLD = 840U,
    IRQ_TIMEOUT = 168000U,
    IRQ_PHASE_MIN = 18U,
    IRQ_BACKGROUND_PERIOD = 2053U,
    IRQ_COMPARE_MIN_LEAD = 12U,
    IRQ_CASES = 6U
};

typedef enum irq_case {
    IRQ_IDLE = 0,
    IRQ_EMPTY,
    IRQ_CONTROL,
    IRQ_COLD,
    IRQ_CACHED,
    IRQ_TELEMETRY
} irq_case_t;

typedef struct irq_stat {
    uint32_t count;
    uint32_t min;
    uint32_t max;
    uint32_t sum;
} irq_stat_t;

typedef struct irq_result {
    irq_stat_t response;
    irq_stat_t api;
    irq_stat_t in_api_response;
    irq_stat_t inside_mask;
    uint32_t line;
    uint32_t isr_masked;
} irq_result_t;

typedef struct irq_payload {
    uint8_t *address;
    uint32_t size;
} irq_payload_t;

bool industry_heap_init(void);
void *industry_heap_alloc(uint32_t size);
void industry_heap_free(void *address);

void TIM5_IRQHandler(void);
void f446_heap_irq_latency_run(void *argument);
void heap_irq_mask_begin(void);
void heap_irq_mask_end(void);
bool heap_irq_background_start(void);
bool heap_irq_background_stop(const char *phase);

static _Atomic uint32_t s_irq_completed;
static _Atomic uint32_t s_irq_error;
static _Atomic uint32_t s_irq_expected;
static uint32_t s_irq_response;
static uint32_t s_irq_basepri;
static uint32_t s_irq_wait_status;
static uint32_t s_mask_start;
static uint32_t s_mask_ticks;
static uint32_t s_mask_entries;
static uint32_t s_mask_exits;
static bool s_trace_mask;
static _Atomic bool s_background;
static uint32_t s_background_mask_max;
static uint32_t s_background_overruns;
static irq_stat_t s_background_response;
static irq_payload_t s_payloads[IRQ_SLOT_COUNT];

static void add_sample(irq_stat_t *stats, uint32_t value);

#define IRQ_CHECK(result, condition)                         \
    do {                                                     \
        if (!(condition)) {                                  \
            (result)->line = (uint32_t)__LINE__;             \
            return false;                                    \
        }                                                     \
    } while (0)

static bool masks_clear(void)
{
    return __get_BASEPRI() == 0U && __get_PRIMASK() == 0U &&
           rtos_port_get_critical_nesting() == 0U;
}

/** @brief Record a timestamp after the heap has applied its local mask. */
void heap_irq_mask_begin(void)
{
    if (s_trace_mask) {
        ++s_mask_entries;
        s_mask_start = DWT->CYCCNT;
    }
}

/** @brief Record elapsed cycles before the heap begins restoring its mask. */
void heap_irq_mask_end(void)
{
    if (s_trace_mask) {
        s_mask_ticks = DWT->CYCCNT - s_mask_start;
        ++s_mask_exits;
        if (atomic_load_explicit(&s_background, memory_order_relaxed) &&
            s_mask_ticks > s_background_mask_max) {
            s_background_mask_max = s_mask_ticks;
        }
    }
}

/** @brief Read the compare delay before acknowledging the hardware event. */
void TIM5_IRQHandler(void)
{
    const uint32_t arrival = TIM5->CNT;
    if ((TIM5->SR & TIM_SR_CC1IF) == 0U) {
        atomic_fetch_add_explicit(&s_irq_error, 1U, memory_order_relaxed);
        return;
    }

    if (__get_IPSR() != (uint32_t)TIM5_IRQn + 16U) {
        atomic_fetch_add_explicit(&s_irq_error, 1U, memory_order_relaxed);
    }
    const uint32_t expected = atomic_load_explicit(&s_irq_expected, memory_order_relaxed);
    if (atomic_load_explicit(&s_background, memory_order_relaxed)) {
        add_sample(&s_background_response, arrival - expected);
        if (__get_BASEPRI() != 0U) {
            atomic_fetch_add_explicit(&s_irq_error, 1U, memory_order_relaxed);
        }
        TIM5->SR = 0U;
        const uint32_t now = TIM5->CNT;
        uint32_t next = expected + IRQ_BACKGROUND_PERIOD;
        while ((int32_t)(next - now) < (int32_t)IRQ_COMPARE_MIN_LEAD) {
            next += IRQ_BACKGROUND_PERIOD;
            ++s_background_overruns;
        }
        atomic_store_explicit(&s_irq_expected, next, memory_order_relaxed);
        TIM5->CCR1 = next;
        __DSB();
        return;
    }

    TIM5->DIER = 0U;
    TIM5->SR = 0U;
    (void)TIM5->SR;
    s_irq_response = arrival - expected;
    s_irq_basepri = __get_BASEPRI();
    atomic_fetch_add_explicit(&s_irq_completed, 1U, memory_order_release);
    __DSB();
}

static void add_sample(irq_stat_t *stats, uint32_t value)
{
    if (stats->count == 0U || value < stats->min) {
        stats->min = value;
    }
    if (value > stats->max) {
        stats->max = value;
    }
    stats->sum += value;
    ++stats->count;
}

static bool enable_clocks(void)
{
    /* Default TIMPRE and APB1 /4 clock TIM5 at half the core clock. */
    if (SystemCoreClock != 168000000U ||
        (RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV4 ||
        (RCC->DCKCFGR & RCC_DCKCFGR_TIMPRE) != 0U || !masks_clear()) {
        return false;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U) {
        return false;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    RCC->APB1ENR |= RCC_APB1ENR_TIM5EN;
    (void)RCC->APB1ENR;
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM5RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM5RST;
    TIM5->PSC = 0U;
    TIM5->ARR = UINT32_MAX;
    TIM5->EGR = TIM_EGR_UG;
    TIM5->SR = 0U;
    TIM5->CR1 = TIM_CR1_CEN;

    const uint32_t clock_start = DWT->CYCCNT;
    const uint32_t timer_start = TIM5->CNT;
    while (DWT->CYCCNT - clock_start < 4096U) {
        if (TIM5->CNT - timer_start > 4000U) {
            return false;
        }
    }
    const uint32_t timer_delta = TIM5->CNT - timer_start;
    return timer_delta > 1900U && timer_delta < 2240U;
}

static void disable_timer(void)
{
    NVIC_DisableIRQ(TIM5_IRQn);
    TIM5->DIER = 0U;
    TIM5->CR1 = 0U;
    TIM5->SR = 0U;
    NVIC_ClearPendingIRQ(TIM5_IRQn);
}

static uint32_t arm_compare(uint32_t lead)
{
    /* Do not let SysTick stop the thread between reading CNT and programming CCR1. */
    const uint32_t old_primask = __get_PRIMASK();
    __disable_irq();
    TIM5->DIER = 0U;
    TIM5->SR = 0U;
    (void)TIM5->SR;
    NVIC_ClearPendingIRQ(TIM5_IRQn);
    const uint32_t prior = atomic_load_explicit(&s_irq_completed, memory_order_relaxed);
    const uint32_t expected = TIM5->CNT + lead;
    atomic_store_explicit(&s_irq_expected, expected, memory_order_relaxed);
    TIM5->CCR1 = expected;
    TIM5->DIER = TIM_DIER_CC1IE;
    __set_PRIMASK(old_primask);
    return prior;
}

/** @brief Start a periodic TIM5 compare source during a business trace. */
bool heap_irq_background_start(void)
{
    if (atomic_load_explicit(&s_background, memory_order_relaxed) || !enable_clocks()) {
        return false;
    }
    atomic_store_explicit(&s_background, true, memory_order_relaxed);
    s_background_response = (irq_stat_t){0};
    s_background_mask_max = 0U;
    s_background_overruns = 0U;
    s_mask_entries = 0U;
    s_mask_exits = 0U;
    s_trace_mask = HEAP_IRQ_INSTRUMENTED != 0;
    atomic_store_explicit(&s_irq_error, 0U, memory_order_relaxed);
    NVIC_SetPriority(TIM5_IRQn, IRQ_MASKED_PRIORITY);
    (void)arm_compare(IRQ_BACKGROUND_PERIOD);
    NVIC_EnableIRQ(TIM5_IRQn);
    return true;
}

/**
 * @brief Stop periodic measurement and emit one record.
 * @param phase NUL-terminated label for the finished business trace.
 * @return True if samples and allocator mask exits were consistent.
 */
bool heap_irq_background_stop(const char *phase)
{
    if (!atomic_load_explicit(&s_background, memory_order_relaxed)) {
        return false;
    }
    disable_timer();
    __DSB();
    __ISB();
    atomic_store_explicit(&s_background, false, memory_order_relaxed);
    s_trace_mask = false;
    const bool passed = s_background_response.count > 0U && s_mask_entries == s_mask_exits &&
                        atomic_load_explicit(&s_irq_error, memory_order_relaxed) == 0U &&
                        (HEAP_IRQ_INSTRUMENTED == 0 || s_mask_entries > 0U) &&
                        s_background_response.min > 0U && masks_clear();
    (void)printf("HILIRQ|BG|case=%s|p=6|n=%lu|min=%lu|max=%lu|sum=%lu|lost=%lu"
                 "|mask_n=%lu|mask_max=%lu|%s\r\n",
                 phase, (unsigned long)s_background_response.count,
                 (unsigned long)s_background_response.min,
                 (unsigned long)s_background_response.max,
                 (unsigned long)s_background_response.sum, (unsigned long)s_background_overruns,
                 (unsigned long)s_mask_entries, (unsigned long)s_background_mask_max,
                 passed ? "PASS" : "FAIL");
    return passed;
}

static bool wait_compare(uint32_t prior)
{
    const uint32_t start = DWT->CYCCNT;
    while (atomic_load_explicit(&s_irq_completed, memory_order_acquire) == prior) {
        if (DWT->CYCCNT - start >= IRQ_TIMEOUT) {
            s_irq_wait_status = 1U;
            return false;
        }
    }
    s_irq_wait_status = 0U;
    if (atomic_load_explicit(&s_irq_completed, memory_order_relaxed) != prior + 1U) {
        s_irq_wait_status |= 2U;
    }
    if (atomic_load_explicit(&s_irq_error, memory_order_relaxed) != 0U) {
        s_irq_wait_status |= 4U;
    }
    if (s_irq_response >= IRQ_TIMEOUT / 2U) {
        s_irq_wait_status |= 8U;
    }
    if (!masks_clear()) {
        s_irq_wait_status |= 16U;
    }
    return s_irq_wait_status == 0U;
}

static uint32_t phase_lead(irq_case_t kind, uint32_t sequence)
{
    if (kind == IRQ_COLD || kind == IRQ_CACHED) {
        /* Dense early samples plus a coarse sweep over the expected cold scan. */
        return (sequence < 48U) ? IRQ_PHASE_MIN + sequence :
               96U + (sequence - 48U) * 96U;
    }
    return IRQ_PHASE_MIN + (sequence % 40U);
}

static void begin_sample(void)
{
    s_mask_entries = 0U;
    s_mask_exits = 0U;
    s_mask_ticks = 0U;
    s_trace_mask = HEAP_IRQ_INSTRUMENTED != 0;
}

static bool record_sample(irq_result_t *result, irq_case_t kind, uint32_t prior,
                          uint32_t first, uint32_t last, uint32_t elapsed)
{
    s_trace_mask = false;
    IRQ_CHECK(result, wait_compare(prior));
    add_sample(&result->response, s_irq_response);
    add_sample(&result->api, elapsed);
    const uint32_t expected = atomic_load_explicit(&s_irq_expected, memory_order_relaxed);
    if (expected - first <= last - first) {
        add_sample(&result->in_api_response, s_irq_response);
    }
    if (s_irq_basepri != 0U) {
        ++result->isr_masked;
    }
    if (HEAP_IRQ_INSTRUMENTED != 0 && kind != IRQ_IDLE) {
        IRQ_CHECK(result, s_mask_entries == 1U && s_mask_exits == 1U && s_mask_ticks > 0U);
        add_sample(&result->inside_mask, s_mask_ticks);
    }
    return true;
}

static void run_control(irq_case_t kind)
{
    const uint32_t mask = RTOS_PORT_ENTER_LOCAL_CRITICAL();
    heap_irq_mask_begin();
    if (kind == IRQ_CONTROL) {
        const uint32_t start = DWT->CYCCNT;
        while (DWT->CYCCNT - start < IRQ_KNOWN_HOLD) {
            atomic_signal_fence(memory_order_seq_cst);
        }
    }
    heap_irq_mask_end();
    RTOS_PORT_EXIT_LOCAL_CRITICAL(mask);
}

static bool measure_control(irq_result_t *result, irq_case_t kind)
{
    for (uint32_t sample = 0U; sample < IRQ_SAMPLES; ++sample) {
        begin_sample();
        const uint32_t prior = arm_compare(phase_lead(kind, sample));
        const uint32_t first = TIM5->CNT;
        const uint32_t start = DWT->CYCCNT;
        if (kind != IRQ_IDLE) {
            run_control(kind);
        }
        const uint32_t elapsed = DWT->CYCCNT - start;
        const uint32_t last = TIM5->CNT;
        if (!record_sample(result, kind, prior, first, last, elapsed)) {
            return false;
        }
    }
    return true;
}

static bool measure_allocation(irq_result_t *result, irq_case_t kind, uint32_t size,
                               void **address)
{
    begin_sample();
    const uint32_t prior = arm_compare(phase_lead(kind, result->response.count));
    const uint32_t first = TIM5->CNT;
    const uint32_t start = DWT->CYCCNT;
    *address = industry_heap_alloc(size);
    const uint32_t elapsed = DWT->CYCCNT - start;
    const uint32_t last = TIM5->CNT;
    return record_sample(result, kind, prior, first, last, elapsed);
}

static bool measure_release(irq_result_t *result, void *address)
{
    begin_sample();
    const uint32_t prior = arm_compare(phase_lead(IRQ_TELEMETRY, result->response.count));
    const uint32_t first = TIM5->CNT;
    const uint32_t start = DWT->CYCCNT;
    industry_heap_free(address);
    const uint32_t elapsed = DWT->CYCCNT - start;
    const uint32_t last = TIM5->CNT;
    return record_sample(result, IRQ_TELEMETRY, prior, first, last, elapsed);
}

static bool assign_payload(uint32_t index, uint8_t *address, uint32_t size)
{
    if (address == NULL || ((uintptr_t)address & 7U) != 0U) {
        return false;
    }
    s_payloads[index] = (irq_payload_t){address, size};
    address[0] = (uint8_t)index;
    address[size - 1U] = (uint8_t)(index ^ 0xA5U);
    return true;
}

static bool release_payload(irq_result_t *result, uint32_t index, bool timed)
{
    irq_payload_t *const payload = &s_payloads[index];
    IRQ_CHECK(result, payload->address != NULL && payload->address[0] == (uint8_t)index &&
              payload->address[payload->size - 1U] == (uint8_t)(index ^ 0xA5U));
    if (timed) {
        IRQ_CHECK(result, measure_release(result, payload->address));
    } else {
        industry_heap_free(payload->address);
    }
    *payload = (irq_payload_t){0};
    return true;
}

static bool prepare_fragments(irq_result_t *result, uint32_t *tail)
{
    for (uint32_t hole = 0U; hole < IRQ_HOLES; ++hole) {
        const uint32_t index = hole * 2U;
        const uint32_t size = 24U + (hole % 4U) * 8U;
        IRQ_CHECK(result, assign_payload(index, industry_heap_alloc(size), size));
        IRQ_CHECK(result, assign_payload(index + 1U, industry_heap_alloc(80U), 80U));
    }
    uint32_t next = IRQ_HOLES * 2U;
    const uint32_t fill_sizes[] = {256U, 32U};
    for (uint32_t group = 0U; group < 2U; ++group) {
        const uint32_t size = fill_sizes[group];
        for (;;) {
            IRQ_CHECK(result, next < IRQ_SLOT_COUNT);
            void *const address = industry_heap_alloc(size);
            if (address == NULL) {
                break;
            }
            IRQ_CHECK(result, assign_payload(next, address, size));
            ++next;
        }
    }
    for (uint32_t hole = 0U; hole < IRQ_HOLES; ++hole) {
        if (!release_payload(result, hole * 2U, false)) {
            return false;
        }
    }
    *tail = next;
    return true;
}

static bool measure_fragments(irq_result_t *result, irq_case_t kind)
{
    uint32_t tail = 0U;
    IRQ_CHECK(result, industry_heap_init());
    if (!prepare_fragments(result, &tail)) {
        return false;
    }
    if (kind == IRQ_CACHED) {
        IRQ_CHECK(result, industry_heap_alloc(80U) == NULL);
    }

    for (uint32_t sample = 0U; sample < IRQ_SAMPLES; ++sample) {
        if (kind == IRQ_COLD) {
            /* Allocate and release one existing hole to invalidate a whole-heap result.
             * The out-of-window mutation cannot make an 80-byte span available. */
            void *const invalidation = industry_heap_alloc(24U);
            IRQ_CHECK(result, invalidation != NULL);
            industry_heap_free(invalidation);
        }
        void *address = NULL;
        if (!measure_allocation(result, kind, 80U, &address)) {
            return false;
        }
        IRQ_CHECK(result, address == NULL);
    }
    for (uint32_t index = tail; index > 0U; --index) {
        if (s_payloads[index - 1U].address != NULL &&
            !release_payload(result, index - 1U, false)) {
            return false;
        }
    }
    return true;
}

static bool measure_telemetry(irq_result_t *result)
{
    const uint32_t sizes[] = {64U, 128U, 192U, 256U, 384U, 512U, 768U, 1024U, 1536U};
    enum { SIZE_COUNT = sizeof(sizes) / sizeof(sizes[0]), ANCHORS = 48U };
    IRQ_CHECK(result, industry_heap_init());
    for (uint32_t index = 0U; index < ANCHORS; ++index) {
        IRQ_CHECK(result, assign_payload(index, industry_heap_alloc(64U), 64U));
    }
    for (uint32_t index = 0U; index < ANCHORS; index += 2U) {
        if (!release_payload(result, index, false)) {
            return false;
        }
    }
    for (uint32_t round = 0U; round < IRQ_TELEMETRY_ROUNDS; ++round) {
        for (uint32_t frame = 0U; frame < IRQ_FRAME_COUNT; ++frame) {
            const uint32_t index = ANCHORS + frame % IRQ_TELEMETRY_DEPTH;
            if (frame >= IRQ_TELEMETRY_DEPTH && !release_payload(result, index, true)) {
                return false;
            }
            const uint32_t sequence = round * IRQ_FRAME_COUNT + frame;
            const uint32_t size = sizes[(sequence * 5U + round * 3U) % SIZE_COUNT];
            void *address = NULL;
            if (!measure_allocation(result, IRQ_TELEMETRY, size, &address)) {
                return false;
            }
            IRQ_CHECK(result, assign_payload(index, address, size));
        }
        for (uint32_t frame = IRQ_FRAME_COUNT - IRQ_TELEMETRY_DEPTH;
             frame < IRQ_FRAME_COUNT; ++frame) {
            if (!release_payload(result, ANCHORS + frame % IRQ_TELEMETRY_DEPTH, true)) {
                return false;
            }
        }
    }
    for (uint32_t index = 1U; index < ANCHORS; index += 2U) {
        if (!release_payload(result, index, false)) {
            return false;
        }
    }
    return true;
}

static bool check_case(irq_result_t *result, irq_case_t kind, uint32_t priority)
{
    if (kind <= IRQ_CONTROL) {
        if (!measure_control(result, kind)) {
            return false;
        }
    } else if (kind == IRQ_TELEMETRY) {
        if (!measure_telemetry(result)) {
            return false;
        }
    } else if (!measure_fragments(result, kind)) {
        return false;
    }

    const uint32_t count = kind == IRQ_TELEMETRY ?
                           2U * IRQ_TELEMETRY_ROUNDS * IRQ_FRAME_COUNT : IRQ_SAMPLES;
    IRQ_CHECK(result, result->api.count == count && result->response.count == count &&
              result->response.min > 0U && result->response.max < IRQ_TIMEOUT / 2U);
    if (priority == IRQ_MASKED_PRIORITY) {
        IRQ_CHECK(result, result->isr_masked == 0U);
        if (kind == IRQ_CONTROL) {
            IRQ_CHECK(result, result->in_api_response.count > 0U &&
                      result->in_api_response.max >= 300U);
        }
    } else if (kind == IRQ_CONTROL) {
        IRQ_CHECK(result, result->isr_masked > 0U && result->in_api_response.count > 0U &&
                  result->in_api_response.max < 300U);
    }
    return true;
}

static void print_case(const char *name, uint32_t priority, const irq_result_t *result,
                       bool passed)
{
    const irq_stat_t *const response = &result->response;
    (void)printf("HILIRQ|RSP|case=%s|p=%lu|n=%lu|min=%lu|max=%lu|sum=%lu\r\n",
                 name, (unsigned long)priority, (unsigned long)response->count,
                 (unsigned long)response->min, (unsigned long)response->max,
                 (unsigned long)response->sum);
    (void)printf("HILIRQ|TIME|case=%s|p=%lu|api_max=%lu|mask_n=%lu|mask_max=%lu\r\n",
                 name, (unsigned long)priority, (unsigned long)result->api.max,
                 (unsigned long)result->inside_mask.count, (unsigned long)result->inside_mask.max);
    (void)printf("HILIRQ|CASE|case=%s|p=%lu|in_api=%lu|in_api_max=%lu"
                 "|isr_masked=%lu|line=%lu|%s\r\n",
                 name, (unsigned long)priority, (unsigned long)result->in_api_response.count,
                 (unsigned long)result->in_api_response.max, (unsigned long)result->isr_masked,
                 (unsigned long)result->line, passed ? "PASS" : "FAIL");
}

/** @brief Run the TIM5 hardware compare fixture in the isolated audit firmware. */
void f446_heap_irq_latency_run(void *argument)
{
    (void)argument;
    const char *const names[IRQ_CASES] = {"idle", "empty", "known", "cold", "cached", "telemetry"};
    const uint32_t priorities[] = {IRQ_MASKED_PRIORITY, IRQ_HIGH_PRIORITY};
    (void)printf("HILIRQ|START|backend=%s|instrumented=%u|cpu_hz=%lu|tim_hz=84000000\r\n",
                 IRQ_STRING(INDUSTRY_BACKEND), (unsigned int)HEAP_IRQ_INSTRUMENTED,
                 (unsigned long)SystemCoreClock);
    if (!enable_clocks()) {
        (void)printf("HILIRQ|FAIL|clock\r\nHILIRQ|DONE|status=1|checks=12|failures=12\r\n");
        return;
    }

    uint32_t failures = 0U;
    for (uint32_t group = 0U; group < 2U; ++group) {
        const uint32_t priority = priorities[group];
        NVIC_SetPriority(TIM5_IRQn, priority);
        NVIC_EnableIRQ(TIM5_IRQn);
        for (uint32_t index = 0U; index < IRQ_CASES; ++index) {
            irq_result_t result = {0};
            const bool passed = check_case(&result, (irq_case_t)index, priority);
            print_case(names[index], priority, &result, passed);
            if (!passed) {
                (void)printf("HILIRQ|DETAIL|wait=%lu|sr=%lx|dier=%lx|basepri=%lx|primask=%lx\r\n",
                             (unsigned long)s_irq_wait_status, (unsigned long)TIM5->SR,
                             (unsigned long)TIM5->DIER, (unsigned long)__get_BASEPRI(),
                             (unsigned long)__get_PRIMASK());
                ++failures;
            }
        }
        NVIC_DisableIRQ(TIM5_IRQn);
    }
    disable_timer();
    (void)printf("HILIRQ|DONE|status=%u|checks=12|failures=%lu\r\n",
                 (unsigned int)(failures != 0U), (unsigned long)failures);
}

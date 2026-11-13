/** @file f446_heap_industry.c
 *  @brief Replay identical application inputs on F446 allocator implementations.
 */
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"
#include "rtos_port.h"
#include "rtos_sched.h"

#ifndef INDUSTRY_BACKEND
#define INDUSTRY_BACKEND unknown
#endif
#ifndef INDUSTRY_FIXED_ONLY
#define INDUSTRY_FIXED_ONLY 0
#endif
#define INDUSTRY_STRING_IMPL(value) #value
#define INDUSTRY_STRING(value) INDUSTRY_STRING_IMPL(value)

enum {
    HIB_CAPACITY = 32768U,
    HIB_SLOTS = 512U,
    HIB_FIRST_FRAME = 48U,
    HIB_ROUNDS = 24U,
    HIB_FRAME_COUNT = 96U,
    HIB_HOLES = 128U,
    HIB_FIXED_COUNT = 256U
};

typedef enum hib_op {
    HIB_ALLOC = 0,
    HIB_FREE,
    HIB_MISS,
    HIB_OPERATION_COUNT
} hib_op_t;

typedef struct hib_sample {
    uint32_t count;
    uint32_t sum;
    uint32_t min;
    uint32_t max;
    uint32_t crossings;
} hib_sample_t;

typedef struct hib_slot {
    uint8_t *address;
    uint32_t size;
    uint8_t marker;
} hib_slot_t;

typedef struct hib_result {
    hib_sample_t samples[HIB_OPERATION_COUNT];
    uint32_t completed;
    uint32_t retried;
    uint32_t peak;
    uint32_t live;
    uint32_t initial_largest;
    uint32_t restored_largest;
    uint32_t rejected_at_live;
    uint32_t input_hash;
    uint32_t failure_line;
} hib_result_t;

static hib_slot_t s_slots[HIB_SLOTS];

bool industry_heap_init(void);
void *industry_heap_alloc(uint32_t size);
void industry_heap_free(void *address);
void f446_heap_industry_run(void *argument);
#if defined(INDUSTRY_HEAP_FINISH)
void industry_heap_finish(void);
#endif
#if defined(HEAP_IRQ_BACKGROUND)
bool heap_irq_background_start(void);
bool heap_irq_background_stop(const char *phase);
#endif

#define HIB_CHECK(result, condition)                       \
    do {                                                   \
        if (!(condition)) {                                \
            (result)->failure_line = (uint32_t)__LINE__;   \
            return false;                                  \
        }                                                  \
    } while (0)

static bool is_mask_clear(void)
{
    return __get_BASEPRI() == 0U && __get_PRIMASK() == 0U &&
           rtos_port_get_critical_nesting() == 0U;
}

static bool add_sample(hib_sample_t *sample, uint32_t cycles, rtos_tick_t tick)
{
    if (cycles == 0U || cycles > UINT32_MAX - sample->sum || !is_mask_clear()) {
        return false;
    }
    if (sample->count == 0U || cycles < sample->min) {
        sample->min = cycles;
    }
    if (cycles > sample->max) {
        sample->max = cycles;
    }
    sample->sum += cycles;
    ++sample->count;
    if (rtos_sched_get_tick_count() != tick) {
        ++sample->crossings;
    }
    return true;
}

static bool is_slot_intact(const hib_slot_t *slot)
{
    return slot->address != NULL && slot->address[0] == slot->marker &&
           slot->address[slot->size / 2U] == (uint8_t)(slot->marker ^ 0x55U) &&
           slot->address[slot->size - 1U] == (uint8_t)(slot->marker ^ 0xAAU);
}

static bool take_slot(hib_result_t *result, uint32_t index, uint32_t size, bool timed)
{
    hib_slot_t *const slot = &s_slots[index];
    HIB_CHECK(result, slot->address == NULL);
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    uint8_t *const address = industry_heap_alloc(size);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    if (timed) {
        const hib_op_t operation = address != NULL ? HIB_ALLOC : HIB_MISS;
        HIB_CHECK(result, add_sample(&result->samples[operation], cycles, tick));
    }
    if (address == NULL) {
        return true;
    }

    HIB_CHECK(result, ((uintptr_t)address & 7U) == 0U);
    const uintptr_t low = (uintptr_t)address;
    for (uint32_t i = 0U; i < HIB_SLOTS; ++i) {
        const hib_slot_t *const active = &s_slots[i];
        if (active->address != NULL) {
            const uintptr_t other = (uintptr_t)active->address;
            HIB_CHECK(result, low + size <= other || other + active->size <= low);
        }
    }
    const uint8_t marker = (uint8_t)(index + result->completed);
    *slot = (hib_slot_t){address, size, marker};
    address[0] = marker;
    address[size / 2U] = (uint8_t)(marker ^ 0x55U);
    address[size - 1U] = (uint8_t)(marker ^ 0xAAU);
    result->live += size;
    if (result->live > result->peak) {
        result->peak = result->live;
    }
    return true;
}

static bool release_slot(hib_result_t *result, uint32_t index, bool timed)
{
    hib_slot_t *const slot = &s_slots[index];
    HIB_CHECK(result, is_slot_intact(slot) && result->live >= slot->size);
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    industry_heap_free(slot->address);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    if (timed) {
        HIB_CHECK(result, add_sample(&result->samples[HIB_FREE], cycles, tick));
    }
    result->live -= slot->size;
    *slot = (hib_slot_t){0};
    return true;
}

static uint32_t probe_largest(void)
{
    uint32_t lo = 0U;
    uint32_t hi = HIB_CAPACITY / 8U + 1U;
    while (hi - lo > 1U) {
        const uint32_t mid = lo + (hi - lo) / 2U;
        void *const address = industry_heap_alloc(mid * 8U);
        if (address != NULL) {
            industry_heap_free(address);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo * 8U;
}

static void record_input(hib_result_t *result, uint32_t size)
{
    result->input_hash = (result->input_hash ^ size) * UINT32_C(16777619);
}

static bool fixed_packets(hib_result_t *result)
{
    for (uint32_t i = 0U; i < HIB_FIXED_COUNT; ++i) {
        record_input(result, 64U);
        HIB_CHECK(result, take_slot(result, i, 64U, true) && s_slots[i].address != NULL);
        ++result->completed;
    }
    for (uint32_t i = 0U; i < HIB_FIXED_COUNT; i += 2U) {
        HIB_CHECK(result, release_slot(result, i, true));
    }
    for (uint32_t i = 0U; i < HIB_FIXED_COUNT; i += 2U) {
        record_input(result, 64U);
        HIB_CHECK(result, take_slot(result, i, 64U, true) && s_slots[i].address != NULL);
        ++result->completed;
    }
    for (uint32_t i = HIB_FIXED_COUNT; i != 0U; --i) {
        HIB_CHECK(result, release_slot(result, i - 1U, true));
    }
    return true;
}

static bool telemetry(hib_result_t *result, bool burst)
{
    const uint32_t sizes[] = {64U, 128U, 192U, 256U, 384U, 512U, 768U, 1024U, 1536U};
    const uint32_t size_count = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
    for (uint32_t anchor = 0U; anchor < HIB_FIRST_FRAME; ++anchor) {
        record_input(result, 64U);
        HIB_CHECK(result, take_slot(result, anchor, 64U, true) &&
                  s_slots[anchor].address != NULL);
    }
    for (uint32_t anchor = 0U; anchor < HIB_FIRST_FRAME; anchor += 2U) {
        HIB_CHECK(result, release_slot(result, anchor, true));
    }

    for (uint32_t round = 0U; round < HIB_ROUNDS; ++round) {
        uint32_t head = 0U;
        for (uint32_t frame = 0U; frame < HIB_FRAME_COUNT; ++frame) {
            if (!burst && frame - head >= 16U) {
                HIB_CHECK(result, release_slot(result, HIB_FIRST_FRAME + head, true));
                ++head;
            }
            const uint32_t index = HIB_FIRST_FRAME + frame;
            const uint32_t sequence = round * HIB_FRAME_COUNT + frame;
            const uint32_t size = sizes[(sequence * 5U + round * 3U) % size_count];
            record_input(result, size);
            HIB_CHECK(result, take_slot(result, index, size, true));
            if (s_slots[index].address == NULL) {
                ++result->retried;
                do {
                    HIB_CHECK(result, head < frame);
                    HIB_CHECK(result, release_slot(result, HIB_FIRST_FRAME + head, true));
                    ++head;
                    HIB_CHECK(result, take_slot(result, index, size, true));
                } while (s_slots[index].address == NULL);
            }
            ++result->completed;
            if (burst && frame >= 48U && (frame & 3U) == 3U) {
                HIB_CHECK(result, release_slot(result, HIB_FIRST_FRAME + head, true));
                ++head;
            }
        }
        while (head < HIB_FRAME_COUNT) {
            HIB_CHECK(result, release_slot(result, HIB_FIRST_FRAME + head, true));
            ++head;
        }
    }
    for (uint32_t anchor = 1U; anchor < HIB_FIRST_FRAME; anchor += 2U) {
        HIB_CHECK(result, release_slot(result, anchor, true));
    }
    HIB_CHECK(result, result->completed == HIB_ROUNDS * HIB_FRAME_COUNT);
    HIB_CHECK(result, burst || result->retried == 0U);
    return true;
}

static bool protocol_sessions(hib_result_t *result)
{
    const uint32_t sizes[] = {40U, 72U, 120U, 256U, 528U, 984U, 1472U, 2112U};
    enum { SESSION_COUNT = 28U };
    for (uint32_t i = 0U; i < SESSION_COUNT; ++i) {
        const uint32_t size = sizes[i % 8U];
        record_input(result, size);
        HIB_CHECK(result, take_slot(result, i, size, true) && s_slots[i].address != NULL);
    }
    for (uint32_t i = 0U; i < 1536U; ++i) {
        const uint32_t index = (i * 11U + 3U) % SESSION_COUNT;
        const uint32_t size = sizes[(i * 5U + i / SESSION_COUNT) % 8U];
        const uint32_t scratch = 96U + ((i >> 2U) & 7U) * 32U;
        record_input(result, size);
        record_input(result, scratch);
        HIB_CHECK(result, release_slot(result, index, true));
        HIB_CHECK(result, take_slot(result, index, size, true) && s_slots[index].address != NULL);
        HIB_CHECK(result, take_slot(result, SESSION_COUNT, scratch, true) &&
                  s_slots[SESSION_COUNT].address != NULL);
        HIB_CHECK(result, release_slot(result, SESSION_COUNT, true));
        ++result->completed;
    }
    for (uint32_t i = 0U; i < SESSION_COUNT; ++i) {
        HIB_CHECK(result, release_slot(result, (i * 11U) % SESSION_COUNT, true));
    }
    return true;
}

static bool fragment_recovery(hib_result_t *result)
{
    for (uint32_t index = 0U; index < HIB_HOLES * 2U; index += 2U) {
        const uint32_t size = 24U + ((index / 2U) % 4U) * 8U;
        record_input(result, size);
        record_input(result, 80U);
        HIB_CHECK(result, take_slot(result, index, size, false) &&
                  s_slots[index].address != NULL);
        HIB_CHECK(result, take_slot(result, index + 1U, 80U, false) &&
                  s_slots[index + 1U].address != NULL);
    }
    uint32_t tail = HIB_HOLES * 2U;
    const uint32_t tail_sizes[] = {256U, 32U};
    for (uint32_t group = 0U; group < 2U; ++group) {
        for (;;) {
            HIB_CHECK(result, tail < HIB_SLOTS - 1U);
            HIB_CHECK(result, take_slot(result, tail, tail_sizes[group], false));
            if (s_slots[tail].address == NULL) {
                break;
            }
            ++tail;
        }
    }
    for (uint32_t index = 0U; index < HIB_HOLES * 2U; index += 2U) {
        HIB_CHECK(result, release_slot(result, index, false));
    }
    result->rejected_at_live = result->live;
    record_input(result, 80U);
    HIB_CHECK(result, take_slot(result, tail, 80U, true) && s_slots[tail].address == NULL);
    for (uint32_t attempt = 0U; attempt < 64U; ++attempt) {
        record_input(result, 80U);
        HIB_CHECK(result, take_slot(result, tail, 80U, true) && s_slots[tail].address == NULL);
    }
    HIB_CHECK(result, release_slot(result, 1U, true));
    record_input(result, 80U);
    HIB_CHECK(result, take_slot(result, tail, 80U, true) && s_slots[tail].address != NULL);
    HIB_CHECK(result, release_slot(result, tail, true));
    for (uint32_t index = tail; index != 0U; --index) {
        if (s_slots[index - 1U].address != NULL) {
            HIB_CHECK(result, release_slot(result, index - 1U, false));
        }
    }
    result->completed = 1U;
    return true;
}

static bool run_case(uint32_t index, hib_result_t *result)
{
    HIB_CHECK(result, industry_heap_init());
    (void)memset(s_slots, 0, sizeof(s_slots));
    result->initial_largest = probe_largest();
    /* Start the trace from a fresh arena because the capacity probe changes allocator state. */
    HIB_CHECK(result, result->initial_largest >= 64U && industry_heap_init());
    result->input_hash = UINT32_C(2166136261);
    bool passed;
    switch (index) {
        case 0U:
            passed = fixed_packets(result);
            break;
        case 1U:
            passed = telemetry(result, false);
            break;
        case 2U:
            passed = telemetry(result, true);
            break;
        case 3U:
            passed = protocol_sessions(result);
            break;
        default:
            passed = fragment_recovery(result);
            break;
    }
    if (!passed) {
        return false;
    }
    result->restored_largest = probe_largest();
    HIB_CHECK(result, result->live == 0U && is_mask_clear());
    HIB_CHECK(result, result->restored_largest == result->initial_largest);
    HIB_CHECK(result, result->samples[HIB_ALLOC].count + (index == 4U ? 1U : 0U) ==
              result->samples[HIB_FREE].count);
    return true;
}

static void print_result(const char *name, const hib_result_t *result, bool passed)
{
    const char *const names[] = {"alloc", "free", "miss"};
    for (uint32_t i = 0U; i < HIB_OPERATION_COUNT; ++i) {
        const hib_sample_t *const s = &result->samples[i];
        (void)printf("HIB|STAT|case=%s|op=%s|n=%lu|sum=%lu|min=%lu|max=%lu|ticks=%lu\r\n",
                     name, names[i], (unsigned long)s->count, (unsigned long)s->sum,
                     (unsigned long)s->min, (unsigned long)s->max, (unsigned long)s->crossings);
    }
    (void)printf("HIB|CASE|case=%s|completed=%lu|retried=%lu|peak=%lu|miss_live=%lu"
                 "|initial=%lu|restored=%lu|input=%08lx|line=%lu|%s\r\n", name,
                 (unsigned long)result->completed, (unsigned long)result->retried,
                 (unsigned long)result->peak, (unsigned long)result->rejected_at_live,
                 (unsigned long)result->initial_largest, (unsigned long)result->restored_largest,
                 (unsigned long)result->input_hash, (unsigned long)result->failure_line,
                 passed ? "PASS" : "FAIL");
}

/** @brief Run deterministic application traces in the F446 audit task. */
void f446_heap_industry_run(void *argument)
{
    (void)argument;
    const char *const cases[] = {"fixed", "telemetry", "burst", "sessions", "fragment"};
    const uint32_t count = INDUSTRY_FIXED_ONLY ? 1U : (uint32_t)(sizeof(cases) / sizeof(cases[0]));
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    (void)printf("HIB|START|backend=%s|hz=%lu|budget=%u|cases=%lu\r\n",
                 INDUSTRY_STRING(INDUSTRY_BACKEND), (unsigned long)SystemCoreClock,
                 (unsigned int)HIB_CAPACITY, (unsigned long)count);
    uint32_t failures = 0U;
    for (uint32_t index = 0U; index < count; ++index) {
        hib_result_t result = {0};
#if defined(HEAP_IRQ_BACKGROUND)
        const bool timer_started = heap_irq_background_start();
        const bool workload_passed = timer_started && run_case(index, &result);
        const bool timer_passed = timer_started && heap_irq_background_stop(cases[index]);
        const bool passed = workload_passed && timer_passed;
#else
        const bool passed = run_case(index, &result);
#endif
#if defined(INDUSTRY_HEAP_FINISH)
        industry_heap_finish();
#endif
        print_result(cases[index], &result, passed);
        if (!passed) {
            ++failures;
        }
    }
    (void)printf("HIB|DONE|status=%u|checks=%lu|failures=%lu\r\n", (unsigned int)(failures != 0U),
                 (unsigned long)count, (unsigned long)failures);
}

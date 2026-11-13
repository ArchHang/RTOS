/**
 * @file f446_heap_failure.c
 * @brief Verify failure scans, first-fit recovery and accounting on a fragmented heap.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx.h"

#include "rtos_heap.h"
#include "rtos_port.h"
#include "rtos_sched.h"

enum {
    HFL_ALIGNMENT = 8U,
    HFL_HEADER = 8U,
    HFL_HOLE_COUNT = 128U,
    HFL_HOLE_MIN = 24U,
    HFL_HOLE_MAX = 48U,
    HFL_GUARD_SIZE = 32U,
    HFL_MISS_SIZE = 56U,
    HFL_RECOVER_SIZE = 80U,
    HFL_REPETITIONS = 32U,
    HFL_CHECKS = 5U
};

typedef struct hfl_samples {
    uint32_t count;
    uint32_t sum;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t tick_crossings;
} hfl_samples_t;

static uint8_t *s_holes[HFL_HOLE_COUNT];
static uint8_t *s_guards[HFL_HOLE_COUNT];
static uint8_t *s_tail;
static uint8_t *s_joined;
static uint32_t s_tail_size;
static uint32_t s_capacity;
static uint32_t s_fragmented_free;
static uint32_t s_allocations;
static uint32_t s_releases;

void f446_heap_failure_run(void *argument);

static bool enable_cycles(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U) {
        return false;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const uint32_t start = DWT->CYCCNT;
    for (uint32_t attempt = 0U; attempt < 16U; ++attempt) {
        if (DWT->CYCCNT != start) {
            return true;
        }
    }
    return false;
}

static bool check_stats(uint32_t expected_free)
{
    const uint32_t free_bytes = rtos_heap_get_free();
    const uint32_t allocations = rtos_heap_get_alloc_count();
    const uint32_t releases = rtos_heap_get_free_count();
    if (free_bytes != expected_free || allocations != s_allocations || releases != s_releases) {
        (void)printf("HFL|ERROR|free=%lu|expected_free=%lu|alloc=%lu|expected_alloc=%lu"
                     "|release=%lu|expected_release=%lu\r\n",
                     (unsigned long)free_bytes, (unsigned long)expected_free,
                     (unsigned long)allocations, (unsigned long)s_allocations,
                     (unsigned long)releases, (unsigned long)s_releases);
        return false;
    }
    return __get_BASEPRI() == 0U && __get_PRIMASK() == 0U &&
        rtos_port_get_critical_nesting() == 0U;
}

static uint32_t hole_size(uint32_t index)
{
    return HFL_HOLE_MIN + (index % 4U) * HFL_ALIGNMENT;
}

static uint8_t *allocate(uint32_t size)
{
    uint8_t *const address = rtos_heap_alloc(size);
    if (address != NULL) {
        ++s_allocations;
    }
    return address;
}

static void release(uint8_t *address)
{
    rtos_heap_free(address);
    ++s_releases;
}

static bool prepare_layout(void)
{
    s_capacity = rtos_heap_get_max_alloc();
    s_allocations = rtos_heap_get_alloc_count();
    s_releases = rtos_heap_get_free_count();
    const uint32_t minimum = HFL_HOLE_COUNT * (HFL_HOLE_MAX + HFL_GUARD_SIZE + 2U * HFL_HEADER);
    if (!enable_cycles() || s_capacity < minimum + 64U || !check_stats(s_capacity)) {
        return false;
    }

    s_tail_size = s_capacity;
    for (uint32_t index = 0U; index < HFL_HOLE_COUNT; ++index) {
        const uint32_t size = hole_size(index);
        s_holes[index] = allocate(size);
        s_guards[index] = allocate(HFL_GUARD_SIZE);
        if (s_holes[index] == NULL || s_guards[index] == NULL ||
            (uintptr_t)s_guards[index] != (uintptr_t)s_holes[index] + size + HFL_HEADER) {
            return false;
        }
        if (index != 0U &&
            (uintptr_t)s_holes[index] != (uintptr_t)s_guards[index - 1U] + HFL_GUARD_SIZE +
                HFL_HEADER) {
            return false;
        }
        s_guards[index][0] = (uint8_t)index;
        s_guards[index][HFL_GUARD_SIZE - 1U] = (uint8_t)(index ^ 0xA5U);
        s_tail_size -= size + HFL_GUARD_SIZE + 2U * HFL_HEADER;
        s_fragmented_free += size;
    }
    s_tail = allocate(s_tail_size);
    if (s_tail == NULL ||
        (uintptr_t)s_tail != (uintptr_t)s_guards[HFL_HOLE_COUNT - 1U] + HFL_GUARD_SIZE +
            HFL_HEADER) {
        return false;
    }
    s_tail[0] = 0x41U;
    s_tail[s_tail_size - 1U] = 0x62U;
    return check_stats(0U);
}

static bool add_sample(hfl_samples_t *samples, uint32_t cycles, rtos_tick_t tick)
{
    if (cycles == 0U || cycles > UINT32_MAX - samples->sum) {
        return false;
    }
    if (samples->count == 0U || cycles < samples->minimum) {
        samples->minimum = cycles;
    }
    if (cycles > samples->maximum) {
        samples->maximum = cycles;
    }
    if (rtos_sched_get_tick_count() != tick) {
        ++samples->tick_crossings;
    }
    ++samples->count;
    samples->sum += cycles;
    return true;
}

static bool measure_miss(uint32_t size, uint32_t expected_free, hfl_samples_t *samples)
{
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    const void *const address = rtos_heap_alloc(size);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    return address == NULL && add_sample(samples, cycles, tick) && check_stats(expected_free);
}

static bool measure_max(hfl_samples_t *samples)
{
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t largest = rtos_heap_get_max_alloc();
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    return largest == HFL_HOLE_MAX && add_sample(samples, cycles, tick) &&
        check_stats(s_fragmented_free);
}

static void print_samples(const char *operation, const hfl_samples_t *samples)
{
    (void)printf("HFL|STAT|op=%s|n=%lu|sum=%lu|min=%lu|max=%lu|tick_crossings=%lu\r\n",
                 operation, (unsigned long)samples->count, (unsigned long)samples->sum,
                 (unsigned long)samples->minimum, (unsigned long)samples->maximum,
                 (unsigned long)samples->tick_crossings);
}

static bool check_exhausted(void)
{
    hfl_samples_t exhausted = {0};
    const bool passed = measure_miss(HFL_ALIGNMENT, 0U, &exhausted) &&
        rtos_heap_get_max_alloc() == 0U;
    print_samples("exhausted", &exhausted);
    return passed;
}

static bool check_fragmented(void)
{
    hfl_samples_t first = {0};
    hfl_samples_t same = {0};
    hfl_samples_t larger = {0};
    hfl_samples_t largest = {0};
    hfl_samples_t cached_largest = {0};
    for (uint32_t index = 0U; index < HFL_HOLE_COUNT; ++index) {
        release(s_holes[index]);
    }
    if (!check_stats(s_fragmented_free) ||
        !measure_miss(HFL_MISS_SIZE, s_fragmented_free, &first) || !measure_max(&largest)) {
        return false;
    }
    for (uint32_t index = 0U; index < HFL_REPETITIONS; ++index) {
        if (!measure_miss(HFL_MISS_SIZE, s_fragmented_free, &same) ||
            !measure_miss(HFL_RECOVER_SIZE, s_fragmented_free, &larger)) {
            return false;
        }
    }
    if (!measure_max(&cached_largest)) {
        return false;
    }
    print_samples("first_miss", &first);
    print_samples("same_miss", &same);
    print_samples("larger_miss", &larger);
    print_samples("max_after_miss", &largest);
    print_samples("max_after_query", &cached_largest);
    return true;
}

static bool check_recovery(void)
{
    uint8_t *const smaller = allocate(HFL_HOLE_MIN);
    if (smaller != s_holes[0] || !check_stats(s_fragmented_free - HFL_HOLE_MIN) ||
        rtos_heap_get_max_alloc() != HFL_HOLE_MAX) {
        return false;
    }
    release(smaller);
    if (rtos_heap_alloc(HFL_RECOVER_SIZE) != NULL || !check_stats(s_fragmented_free) ||
        s_guards[0][0] != 0U || s_guards[0][HFL_GUARD_SIZE - 1U] != 0xA5U) {
        return false;
    }

    /* Guard zero borders the first two holes. Its release reclaims one right header. */
    release(s_guards[0]);
    const uint32_t joined_free = s_fragmented_free + HFL_GUARD_SIZE + HFL_HEADER;
    if (!check_stats(joined_free)) {
        return false;
    }
    s_joined = allocate(HFL_RECOVER_SIZE);
    if (s_joined != s_holes[0] || !check_stats(joined_free - HFL_RECOVER_SIZE)) {
        return false;
    }
    return rtos_heap_get_max_alloc() == HFL_HOLE_MAX;
}

static bool check_capacity(void)
{
    if (s_tail[0] != 0x41U || s_tail[s_tail_size - 1U] != 0x62U) {
        return false;
    }
    release(s_tail);
    for (uint32_t index = HFL_HOLE_COUNT - 1U; index != 0U; --index) {
        if (s_guards[index][0] != (uint8_t)index ||
            s_guards[index][HFL_GUARD_SIZE - 1U] != (uint8_t)(index ^ 0xA5U)) {
            return false;
        }
        release(s_guards[index]);
    }
    release(s_joined);
    if (rtos_heap_get_max_alloc() != s_capacity) {
        return false;
    }
    uint8_t *const whole = allocate(s_capacity);
    if (whole != s_holes[0] || !check_stats(0U) || rtos_heap_get_max_alloc() != 0U) {
        return false;
    }
    release(whole);
    return check_stats(s_capacity) && rtos_heap_get_max_alloc() == s_capacity;
}

static bool print_case(const char *name, bool passed, uint32_t *failures)
{
    (void)printf("HFL|CASE|case=%s|%s\r\n", name, passed ? "PASS" : "FAIL");
    if (!passed) {
        ++*failures;
    }
    return passed;
}

/** @brief Run the failure scan suite from an isolated F446 HIL task. */
void f446_heap_failure_run(void *argument)
{
    (void)argument;
    uint32_t failures = 0U;
    const bool layout = prepare_layout();
    (void)printf("HFL|START|capacity=%lu|holes=%u|raw_fragmented=%lu|max_fragmented=%u\r\n",
                 (unsigned long)s_capacity, (unsigned int)HFL_HOLE_COUNT,
                 (unsigned long)s_fragmented_free, (unsigned int)HFL_HOLE_MAX);
    (void)print_case("layout", layout, &failures);
    const bool exhausted = print_case("exhausted", layout && check_exhausted(), &failures);
    const bool fragmented = print_case("fragmented", exhausted && check_fragmented(), &failures);
    const bool recovered = print_case("recovery", fragmented && check_recovery(), &failures);
    (void)print_case("capacity", recovered && check_capacity(), &failures);
    (void)printf("HFL|DONE|status=%u|checks=%u|failures=%lu\r\n",
                 (unsigned int)(failures != 0U), (unsigned int)HFL_CHECKS,
                 (unsigned long)failures);
}

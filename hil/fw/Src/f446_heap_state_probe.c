/**
 * @file f446_heap_state_probe.c
 * @brief Exercise heap search hints through the public API on an isolated F446 task.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx.h"

#include "rtos_heap.h"
#include "rtos_port.h"
#include "rtos_sched.h"

enum {
    HSP_LIVE_COUNT = 48U,
    HSP_HOLE_COUNT = 24U,
    HSP_HEADER_SIZE = 8U,
    HSP_LIVE_SIZE = 24U,
    HSP_HOLE_SIZE = 32U,
    HSP_PROBE_SIZE = 40U,
    HSP_HIGH_INDEX = HSP_LIVE_COUNT + 2U * HSP_HOLE_COUNT,
    HSP_TAIL_INDEX = HSP_HIGH_INDEX + 2U,
    HSP_BLOCK_COUNT = HSP_TAIL_INDEX + 1U,
    HSP_ROUNDS = 128U,
    HSP_CHECKS = 5U
};

typedef struct hsp_block {
    uint8_t *address;
    uint32_t size;
    uint8_t seed;
} hsp_block_t;

typedef struct hsp_samples {
    uint32_t count;
    uint32_t sum;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t tick_crossings;
} hsp_samples_t;

/* The fixture task is the sole owner of the layout and sample state. */
static hsp_block_t s_blocks[HSP_BLOCK_COUNT];
static uint32_t s_capacity;
static uint32_t s_alloc_count;
static uint32_t s_free_count;
static uint32_t s_expected_free;

void f446_heap_state_probe_run(void *argument);

static bool check_stats(void)
{
    return (rtos_heap_get_free() == s_expected_free)
        && (rtos_heap_get_alloc_count() == s_alloc_count)
        && (rtos_heap_get_free_count() == s_free_count)
        && (__get_BASEPRI() == 0U)
        && (__get_PRIMASK() == 0U)
        && (rtos_port_get_critical_nesting() == 0U);
}

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

static void mark_block(const hsp_block_t *block)
{
    block->address[0] = block->seed;
    block->address[block->size - 1U] = (uint8_t)(block->seed ^ 0xA5U);
}

static bool check_block(const hsp_block_t *block)
{
    return (block->address[0] == block->seed)
        && (block->address[block->size - 1U] == (uint8_t)(block->seed ^ 0xA5U));
}

static bool add_sample(hsp_samples_t *samples, uint32_t cycles, rtos_tick_t tick)
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

static bool take_exact(const hsp_block_t *block, hsp_samples_t *samples)
{
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    const void *const address = rtos_heap_alloc(block->size);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;

    if ((address != block->address) || (s_expected_free < block->size)) {
        return false;
    }
    mark_block(block);
    ++s_alloc_count;
    s_expected_free -= block->size;
    return check_stats() && ((samples == NULL) || add_sample(samples, cycles, tick));
}

static bool release_block(const hsp_block_t *block, uint32_t merged_header_size)
{
    if (!check_block(block)) {
        return false;
    }
    rtos_heap_free(block->address);
    ++s_free_count;
    s_expected_free += block->size + merged_header_size;
    return check_stats();
}

static bool build_block(uint32_t index, uintptr_t *cursor, uint32_t size)
{
    uint8_t *const address = rtos_heap_alloc(size);
    if ((address == NULL) || ((uintptr_t)address != *cursor)) {
        return false;
    }
    hsp_block_t *const block = &s_blocks[index];
    *block = (hsp_block_t){address, size, (uint8_t)index};
    mark_block(block);
    ++s_alloc_count;
    *cursor += size + HSP_HEADER_SIZE;
    return true;
}

static hsp_block_t *get_hole(uint32_t index)
{
    return &s_blocks[HSP_LIVE_COUNT + 2U * index];
}

static bool build_layout(void)
{
    const uint32_t used_prefix = HSP_LIVE_COUNT * (HSP_LIVE_SIZE + HSP_HEADER_SIZE)
        + HSP_HOLE_COUNT * (HSP_HOLE_SIZE + HSP_LIVE_SIZE + 2U * HSP_HEADER_SIZE)
        + HSP_PROBE_SIZE + HSP_LIVE_SIZE + 2U * HSP_HEADER_SIZE;
    s_capacity = rtos_heap_get_max_alloc();
    s_expected_free = s_capacity;
    s_alloc_count = rtos_heap_get_alloc_count();
    s_free_count = rtos_heap_get_free_count();
    if (!enable_cycles() || !check_stats() || (s_capacity < used_prefix + HSP_LIVE_SIZE)) {
        return false;
    }

    void *const whole = rtos_heap_alloc(s_capacity);
    if (whole == NULL) {
        return false;
    }
    uintptr_t cursor = (uintptr_t)whole;
    rtos_heap_free(whole);
    ++s_alloc_count;
    ++s_free_count;
    if (!check_stats()) {
        return false;
    }

    for (uint32_t index = 0U; index < HSP_LIVE_COUNT; ++index) {
        if (!build_block(index, &cursor, HSP_LIVE_SIZE)) {
            return false;
        }
    }
    for (uint32_t index = HSP_LIVE_COUNT; index < HSP_HIGH_INDEX; index += 2U) {
        if (!build_block(index, &cursor, HSP_HOLE_SIZE)
            || !build_block(index + 1U, &cursor, HSP_LIVE_SIZE)) {
            return false;
        }
    }
    if (!build_block(HSP_HIGH_INDEX, &cursor, HSP_PROBE_SIZE)
        || !build_block(HSP_HIGH_INDEX + 1U, &cursor, HSP_LIVE_SIZE)
        || !build_block(HSP_TAIL_INDEX, &cursor, s_capacity - used_prefix)) {
        return false;
    }

    s_expected_free = 0U;
    if (!check_stats() || !release_block(&s_blocks[HSP_HIGH_INDEX], 0U)) {
        return false;
    }
    /* Hole zero stays live until the explicit checkpoint invalidation case. */
    for (uint32_t index = 1U; index < HSP_HOLE_COUNT; ++index) {
        if (!release_block(get_hole(index), 0U)) {
            return false;
        }
    }
    return true;
}

static bool prime_high(void)
{
    const hsp_block_t *const high = &s_blocks[HSP_HIGH_INDEX];
    return take_exact(high, NULL) && release_block(high, 0U);
}

static bool measure_skip(hsp_samples_t *samples)
{
    if (!prime_high()) {
        return false;
    }
    const hsp_block_t *const high = &s_blocks[HSP_HIGH_INDEX];
    for (uint32_t round = 0U; round < HSP_ROUNDS; ++round) {
        if (!take_exact(high, samples) || !release_block(high, 0U)) {
            return false;
        }
    }
    return samples->count == HSP_ROUNDS;
}

static bool measure_equal(hsp_samples_t *samples)
{
    const hsp_block_t *const first_free = get_hole(1U);
    for (uint32_t round = 0U; round < HSP_ROUNDS; ++round) {
        /* The request equals the largest low-address hole and must return it. */
        if (!prime_high() || !take_exact(first_free, samples) || !release_block(first_free, 0U)) {
            return false;
        }
    }
    return samples->count == HSP_ROUNDS;
}

static bool measure_invalidated(hsp_samples_t *samples)
{
    const hsp_block_t *const first = get_hole(0U);
    for (uint32_t round = 0U; round < HSP_ROUNDS; ++round) {
        /* Release below the checkpoint while leaving the preceding live blocks in place. */
        if (!prime_high() || !release_block(first, 0U) || !take_exact(first, samples)) {
            return false;
        }
    }
    return samples->count == HSP_ROUNDS;
}

static bool restore_layout(void)
{
    for (uint32_t index = 1U; index < HSP_HOLE_COUNT; ++index) {
        if (!take_exact(get_hole(index), NULL)) {
            return false;
        }
    }
    if (!take_exact(&s_blocks[HSP_HIGH_INDEX], NULL)
        || !release_block(&s_blocks[HSP_TAIL_INDEX], 0U)) {
        return false;
    }
    for (uint32_t index = HSP_TAIL_INDEX; index != 0U; --index) {
        if (!release_block(&s_blocks[index - 1U], HSP_HEADER_SIZE)) {
            return false;
        }
    }
    return (s_expected_free == s_capacity) && (rtos_heap_get_max_alloc() == s_capacity);
}

static void print_case(const char *name, bool passed, const hsp_samples_t *samples)
{
    /* Diagnostic output follows the timed operations. */
    if (samples != NULL) {
        (void)printf("HSP|STAT|case=%s|op=alloc|n=%lu|sum=%lu|min=%lu|max=%lu"
                     "|tick_crossings=%lu\r\n", name, (unsigned long)samples->count,
                     (unsigned long)samples->sum, (unsigned long)samples->minimum,
                     (unsigned long)samples->maximum, (unsigned long)samples->tick_crossings);
    }
    (void)printf("HSP|CASE|case=%s|%s\r\n", name, passed ? "PASS" : "FAIL");
}

/**
 * @brief Measure directed first-fit cases using only the public RTOS heap API.
 * @param argument Unused portable fixture argument.
 */
void f446_heap_state_probe_run(void *argument)
{
    (void)argument;
    hsp_samples_t skip = {0};
    hsp_samples_t equal = {0};
    hsp_samples_t invalidated = {0};

    const bool layout_passed = build_layout();
    const bool skip_passed = layout_passed && measure_skip(&skip);
    const bool equal_passed = skip_passed && measure_equal(&equal);
    const bool invalidated_passed = equal_passed && measure_invalidated(&invalidated);
    const bool restored = invalidated_passed && restore_layout();

    (void)printf("HSP|START|capacity=%lu|live_prefix_blocks=%u|holes=%u|rounds=%u\r\n",
                 (unsigned long)s_capacity, (unsigned int)HSP_LIVE_COUNT,
                 (unsigned int)HSP_HOLE_COUNT, (unsigned int)HSP_ROUNDS);
    print_case("layout", layout_passed, NULL);
    print_case("skip_gt", skip_passed, &skip);
    print_case("equal", equal_passed, &equal);
    print_case("invalidated", invalidated_passed, &invalidated);
    print_case("restored", restored, NULL);
    const uint32_t passed = (uint32_t)layout_passed + (uint32_t)skip_passed
        + (uint32_t)equal_passed + (uint32_t)invalidated_passed + (uint32_t)restored;
    (void)printf("HSP|DONE|status=%u|checks=%u|failures=%lu\r\n",
                 (unsigned int)(passed != HSP_CHECKS), (unsigned int)HSP_CHECKS,
                 (unsigned long)(HSP_CHECKS - passed));
}

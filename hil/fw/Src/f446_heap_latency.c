/**
 * @file f446_heap_latency.c
 * @brief Compare heap API latency on identical F446 allocation workloads.
 *
 * DWT measures the API calls in task context with normal interrupt handling.
 * Setup, payload checks, aggregate statistics and UART output are outside
 * the timed interval. Each case warms its own allocation pattern.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx.h"

#include "rtos.h"
#include "rtos_heap.h"

enum {
    HEAP_LATENCY_SAMPLES = 256U,
    HEAP_LATENCY_WARMUP = 8U,
    HEAP_LATENCY_FRAGMENT_BLOCKS = 256U,
    HEAP_LATENCY_FRAGMENT_SIZE = 32U,
    HEAP_LATENCY_FRAGMENT_PROBE = 1024U,
    HEAP_LATENCY_FEW_BLOCKS = 4U,
    HEAP_LATENCY_FEW_SIZE = 96U,
    HEAP_LATENCY_FEW_PROBE = 160U,
    HEAP_LATENCY_TASK_SLOTS = 4U,
    HEAP_LATENCY_MIN_STACK_WORDS = 128U,
    HEAP_LATENCY_STACK_STEP_WORDS = 64U,
    HEAP_LATENCY_ALIGNMENT = 8U
};

enum {
    HEAP_BUCKET_HEADER_SIZE = 8U,
    HEAP_BUCKET_SMALL_SIZE = 120U,
    HEAP_BUCKET_LARGE_SIZE = 144U,
    HEAP_BUCKET_GUARD_SIZE = 24U,
    HEAP_BUCKET_JOINED_SIZE = HEAP_BUCKET_SMALL_SIZE + HEAP_BUCKET_LARGE_SIZE +
                             HEAP_BUCKET_GUARD_SIZE + 2U * HEAP_BUCKET_HEADER_SIZE
};

typedef enum heap_bucket_block {
    HEAP_BUCKET_SMALL = 0,
    HEAP_BUCKET_SEPARATOR,
    HEAP_BUCKET_LARGE,
    HEAP_BUCKET_TAIL_GUARD,
    HEAP_BUCKET_TAIL,
    HEAP_BUCKET_BLOCK_COUNT
} heap_bucket_block_t;

typedef struct heap_latency_stats {
    uint32_t samples;
    uint32_t sum;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t tick_crossings;
    uint32_t invalid;
} heap_latency_stats_t;

typedef struct heap_latency_pattern {
    const char *name;
    uint32_t block_count;
    uint32_t block_size;
    uint32_t probe_size;
} heap_latency_pattern_t;

typedef struct heap_latency_task_pair {
    uint8_t *tcb;
    uint8_t *stack;
    uint32_t stack_size;
    uint8_t marker;
} heap_latency_task_pair_t;

typedef struct heap_latency_task_stats {
    heap_latency_stats_t tcb_alloc;
    heap_latency_stats_t tcb_free;
    heap_latency_stats_t stack_alloc;
    heap_latency_stats_t stack_free;
} heap_latency_task_stats_t;

static uint8_t *s_blocks[HEAP_LATENCY_FRAGMENT_BLOCKS];

void f446_heap_latency_run(void *argument);

static bool enable_cycle_counter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U) {
        return false;
    }
    /* Keep the performance monitor's existing counter origin. */
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const uint32_t first = DWT->CYCCNT;
    for (uint32_t retry = 0U; retry < 16U; ++retry) {
        if (DWT->CYCCNT != first) {
            return true;
        }
    }
    return false;
}

static bool is_interrupt_state_clear(void)
{
    return __get_BASEPRI() == 0U && __get_PRIMASK() == 0U &&
        rtos_port_get_critical_nesting() == 0U;
}

static void add_sample(heap_latency_stats_t *stats, uint32_t elapsed, bool crossed_tick)
{
    if (stats == NULL) {
        return;
    }
    if (elapsed == 0U || elapsed > UINT32_MAX - stats->sum || !is_interrupt_state_clear()) {
        ++stats->invalid;
        return;
    }
    if (stats->samples == 0U || elapsed < stats->minimum) {
        stats->minimum = elapsed;
    }
    if (elapsed > stats->maximum) {
        stats->maximum = elapsed;
    }
    stats->sum += elapsed;
    ++stats->samples;
    if (crossed_tick) {
        ++stats->tick_crossings;
    }
}

static void *sample_alloc(uint32_t size, heap_latency_stats_t *stats)
{
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    void *const block = rtos_heap_alloc(size);
    atomic_signal_fence(memory_order_seq_cst);
    /* Unsigned subtraction gives the elapsed cycles across one wrap. */
    const uint32_t elapsed = DWT->CYCCNT - start;
    add_sample(stats, elapsed, rtos_sched_get_tick_count() != tick);
    return block;
}

static void sample_free(void *block, heap_latency_stats_t *stats)
{
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    rtos_heap_free(block);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t elapsed = DWT->CYCCNT - start;
    add_sample(stats, elapsed, rtos_sched_get_tick_count() != tick);
}

static bool print_stats(const char *name, const char *operation, const heap_latency_stats_t *stats)
{
    const uint32_t mean = (stats->samples != 0U) ? stats->sum / stats->samples : 0U;
    (void)printf("HLAT|STAT|case=%s|op=%s|n=%lu|sum=%lu|min=%lu|mean=%lu|max=%lu"
                 "|tick_crossings=%lu|invalid=%lu\r\n",
                 name, operation, (unsigned long)stats->samples, (unsigned long)stats->sum,
                 (unsigned long)stats->minimum, (unsigned long)mean,
                 (unsigned long)stats->maximum, (unsigned long)stats->tick_crossings,
                 (unsigned long)stats->invalid);
    return stats->samples == HEAP_LATENCY_SAMPLES && stats->invalid == 0U;
}

static bool is_aligned_block(const void *block)
{
    return block != NULL && ((uintptr_t)block % HEAP_LATENCY_ALIGNMENT) == 0U;
}

static void mark_block(uint8_t *block, uint32_t size, uint8_t marker)
{
    block[0] = marker;
    block[size - 1U] = (uint8_t)~marker;
}

static bool check_block(const uint8_t *block, uint32_t size, uint8_t marker)
{
    const uint8_t last = (uint8_t)~marker;
    return block[0] == marker && block[size - 1U] == last;
}

static void sort_blocks(uint32_t block_count)
{
    /* Establish the physical order independently of the allocator's search policy. */
    for (uint32_t index = 1U; index < block_count; ++index) {
        uint8_t *const block = s_blocks[index];
        uint32_t position = index;
        while (position > 0U && (uintptr_t)s_blocks[position - 1U] > (uintptr_t)block) {
            s_blocks[position] = s_blocks[position - 1U];
            --position;
        }
        s_blocks[position] = block;
    }
}

static bool prepare_holes(const heap_latency_pattern_t *pattern)
{
    for (uint32_t index = 0U; index < pattern->block_count; ++index) {
        s_blocks[index] = rtos_heap_alloc(pattern->block_size);
        if (!is_aligned_block(s_blocks[index])) {
            return false;
        }
    }
    sort_blocks(pattern->block_count);

    const uintptr_t stride = (uintptr_t)s_blocks[1] - (uintptr_t)s_blocks[0];
    /* Verify the addresses before calling the result a fragmented layout. */
    if (stride < pattern->block_size || stride >= pattern->probe_size) {
        return false;
    }
    for (uint32_t index = 1U; index < pattern->block_count; ++index) {
        if ((uintptr_t)s_blocks[index] - (uintptr_t)s_blocks[index - 1U] != stride) {
            return false;
        }
    }

    /* The final block stays live, keeping the last small hole away from the tail. */
    for (uint32_t index = 0U; index < pattern->block_count; index += 2U) {
        rtos_heap_free(s_blocks[index]);
        s_blocks[index] = NULL;
        mark_block(s_blocks[index + 1U], pattern->block_size, (uint8_t)index);
    }
    return true;
}

static bool measure_holes(const heap_latency_pattern_t *pattern)
{
    heap_latency_stats_t allocations = { 0 };
    heap_latency_stats_t releases = { 0 };
    const bool is_layout_valid = prepare_holes(pattern);
    bool is_valid = is_layout_valid;
    const uintptr_t last_guard = (uintptr_t)s_blocks[pattern->block_count - 1U];
    const uintptr_t probe_minimum = last_guard + pattern->block_size;

    for (uint32_t sample = 0U; is_valid && sample < HEAP_LATENCY_WARMUP + HEAP_LATENCY_SAMPLES;
         ++sample) {
        const bool is_warmup = sample < HEAP_LATENCY_WARMUP;
        void *const block = sample_alloc(pattern->probe_size, is_warmup ? NULL : &allocations);
        is_valid = is_aligned_block(block) && (uintptr_t)block >= probe_minimum;
        if (block != NULL) {
            sample_free(block, is_warmup ? NULL : &releases);
        }
    }
    for (uint32_t remaining = pattern->block_count; remaining > 0U; --remaining) {
        const uint32_t index = remaining - 1U;
        if (s_blocks[index] != NULL) {
            if (is_layout_valid &&
                !check_block(s_blocks[index], pattern->block_size, (uint8_t)(index - 1U))) {
                is_valid = false;
            }
            rtos_heap_free(s_blocks[index]);
            s_blocks[index] = NULL;
        }
    }
    (void)printf("HLAT|LAYOUT|case=%s|blocks=%lu|holes=%lu|block_size=%lu|probe_size=%lu"
                 "|verified=%lu\r\n",
                 pattern->name, (unsigned long)pattern->block_count,
                 (unsigned long)(pattern->block_count / 2U), (unsigned long)pattern->block_size,
                 (unsigned long)pattern->probe_size, (unsigned long)is_layout_valid);
    const bool are_allocations_valid = print_stats(pattern->name, "alloc", &allocations);
    const bool are_releases_valid = print_stats(pattern->name, "free", &releases);
    return is_valid && are_allocations_valid && are_releases_valid;
}

static bool allocate_task_pair(uint32_t sample, heap_latency_task_stats_t *stats,
                              heap_latency_task_pair_t *pair)
{
    const uint32_t words = HEAP_LATENCY_MIN_STACK_WORDS +
        ((sample / HEAP_LATENCY_TASK_SLOTS + sample) % HEAP_LATENCY_TASK_SLOTS) *
        HEAP_LATENCY_STACK_STEP_WORDS;
    pair->stack_size = words * (uint32_t)sizeof(rtos_stack_t);
    pair->marker = (uint8_t)sample;
    pair->tcb = sample_alloc((uint32_t)sizeof(rtos_tcb_t),
                             (stats != NULL) ? &stats->tcb_alloc : NULL);
    if (!is_aligned_block(pair->tcb)) {
        return false;
    }
    pair->stack = sample_alloc(pair->stack_size, (stats != NULL) ? &stats->stack_alloc : NULL);
    if (!is_aligned_block(pair->stack)) {
        return false;
    }
    mark_block(pair->tcb, (uint32_t)sizeof(rtos_tcb_t), pair->marker);
    mark_block(pair->stack, pair->stack_size, (uint8_t)~pair->marker);
    return true;
}

static void release_task_pair(heap_latency_task_pair_t *pair, heap_latency_task_stats_t *stats)
{
    if (pair->stack != NULL) {
        sample_free(pair->stack, (stats != NULL) ? &stats->stack_free : NULL);
        pair->stack = NULL;
    }
    if (pair->tcb != NULL) {
        sample_free(pair->tcb, (stats != NULL) ? &stats->tcb_free : NULL);
        pair->tcb = NULL;
    }
}

static bool check_task_pair(const heap_latency_task_pair_t *pair)
{
    return pair->tcb != NULL && pair->stack != NULL &&
        check_block(pair->tcb, (uint32_t)sizeof(rtos_tcb_t), pair->marker) &&
        check_block(pair->stack, pair->stack_size, (uint8_t)~pair->marker);
}

static bool measure_task_churn(void)
{
    heap_latency_task_pair_t pairs[HEAP_LATENCY_TASK_SLOTS] = { 0 };
    heap_latency_task_stats_t stats = { 0 };
    bool is_valid = true;
    for (uint32_t slot = 0U; is_valid && slot < HEAP_LATENCY_TASK_SLOTS; ++slot) {
        is_valid = allocate_task_pair(slot, NULL, &pairs[slot]);
    }

    for (uint32_t sample = 0U; is_valid && sample < HEAP_LATENCY_WARMUP + HEAP_LATENCY_SAMPLES;
         ++sample) {
        heap_latency_task_pair_t *const pair = &pairs[sample % HEAP_LATENCY_TASK_SLOTS];
        if (!check_task_pair(pair)) {
            is_valid = false;
            break;
        }
        heap_latency_task_stats_t *const measured = (sample < HEAP_LATENCY_WARMUP) ? NULL : &stats;
        release_task_pair(pair, measured);
        is_valid = allocate_task_pair(sample + HEAP_LATENCY_TASK_SLOTS, measured, pair);
    }
    for (uint32_t slot = 0U; slot < HEAP_LATENCY_TASK_SLOTS; ++slot) {
        is_valid = is_valid && check_task_pair(&pairs[slot]);
        release_task_pair(&pairs[slot], NULL);
    }

    const bool is_tcb_alloc_valid = print_stats("task", "tcb_alloc", &stats.tcb_alloc);
    const bool is_tcb_free_valid = print_stats("task", "tcb_free", &stats.tcb_free);
    const bool is_stack_alloc_valid = print_stats("task", "stack_alloc", &stats.stack_alloc);
    const bool is_stack_free_valid = print_stats("task", "stack_free", &stats.stack_free);
    return is_valid && is_tcb_alloc_valid && is_tcb_free_valid &&
        is_stack_alloc_valid && is_stack_free_valid;
}

static uint32_t live_allocations(void)
{
    /* Both counters are modular. Their difference gives the current live count. */
    return rtos_heap_get_alloc_count() - rtos_heap_get_free_count();
}

static bool normalize_layout(uint32_t initial_maximum, uint32_t initial_live)
{
    if (rtos_heap_get_max_alloc() != initial_maximum || live_allocations() != initial_live) {
        return false;
    }
    /* Restore the common large span before setting up the next pattern. This
     * also coalesces the baseline allocator's adjacent free blocks. */
    void *const block = rtos_heap_alloc(initial_maximum);
    if (block == NULL) {
        return false;
    }
    const bool is_aligned = is_aligned_block(block);
    rtos_heap_free(block);
    return is_aligned && live_allocations() == initial_live && is_interrupt_state_clear();
}

static bool prepare_wide_bucket(uint8_t *blocks[HEAP_BUCKET_BLOCK_COUNT])
{
    static const uint32_t payload_sizes[HEAP_BUCKET_TAIL] = {
        [HEAP_BUCKET_SMALL] = HEAP_BUCKET_SMALL_SIZE,
        [HEAP_BUCKET_SEPARATOR] = HEAP_BUCKET_GUARD_SIZE,
        [HEAP_BUCKET_LARGE] = HEAP_BUCKET_LARGE_SIZE,
        [HEAP_BUCKET_TAIL_GUARD] = HEAP_BUCKET_GUARD_SIZE
    };
    uintptr_t expected = 0U;
    for (uint32_t index = 0U; index < HEAP_BUCKET_TAIL; ++index) {
        blocks[index] = rtos_heap_alloc(payload_sizes[index]);
        const uintptr_t address = (uintptr_t)blocks[index];
        if (!is_aligned_block(blocks[index]) || (index != 0U && address != expected)) {
            return false;
        }
        expected = address + payload_sizes[index] + HEAP_BUCKET_HEADER_SIZE;
    }
    const uint32_t tail_size = rtos_heap_get_max_alloc();
    if (tail_size == 0U) {
        return false;
    }
    blocks[HEAP_BUCKET_TAIL] = rtos_heap_alloc(tail_size);
    if (!is_aligned_block(blocks[HEAP_BUCKET_TAIL]) ||
        (uintptr_t)blocks[HEAP_BUCKET_TAIL] != expected || rtos_heap_get_max_alloc() != 0U) {
        return false;
    }
    mark_block(blocks[HEAP_BUCKET_SEPARATOR], HEAP_BUCKET_GUARD_SIZE, UINT8_C(0xA5));
    mark_block(blocks[HEAP_BUCKET_TAIL_GUARD], HEAP_BUCKET_GUARD_SIZE, UINT8_C(0x5A));
    return true;
}

static bool reject_above_wide_bucket_max(void)
{
    const uint32_t free_before = rtos_heap_get_free();
    const uint32_t alloc_before = rtos_heap_get_alloc_count();
    const uint32_t release_before = rtos_heap_get_free_count();
    void *const excess = rtos_heap_alloc(HEAP_BUCKET_LARGE_SIZE + 1U);
    if (excess != NULL) {
        rtos_heap_free(excess);
        return false;
    }
    return rtos_heap_get_free() == free_before && rtos_heap_get_alloc_count() == alloc_before &&
        rtos_heap_get_free_count() == release_before &&
        rtos_heap_get_max_alloc() == HEAP_BUCKET_LARGE_SIZE;
}

static bool check_wide_bucket(void)
{
    uint8_t *blocks[HEAP_BUCKET_BLOCK_COUNT] = { 0 };
    const bool is_layout_valid = prepare_wide_bucket(blocks);
    uint32_t holes_max = 0U;
    uint32_t remaining_max = 0U;
    uint32_t joined_max = 0U;
    bool is_rejected = false;
    bool is_deep_fit = false;
    bool are_guards_valid = false;
    bool is_joined_fit = false;
    if (is_layout_valid) {
        const uintptr_t small = (uintptr_t)blocks[HEAP_BUCKET_SMALL];
        const uintptr_t large = (uintptr_t)blocks[HEAP_BUCKET_LARGE];
        /* Physical sizes 128 and 152 are in the same [128, 160) bucket.
         * Returning the smaller block last puts it at the bucket head. */
        rtos_heap_free(blocks[HEAP_BUCKET_LARGE]);
        blocks[HEAP_BUCKET_LARGE] = NULL;
        rtos_heap_free(blocks[HEAP_BUCKET_SMALL]);
        blocks[HEAP_BUCKET_SMALL] = NULL;
        holes_max = rtos_heap_get_max_alloc();
        is_rejected = reject_above_wide_bucket_max();

        void *const deep = rtos_heap_alloc(HEAP_BUCKET_LARGE_SIZE);
        is_deep_fit = (uintptr_t)deep == large;
        remaining_max = rtos_heap_get_max_alloc();
        if (deep != NULL) {
            rtos_heap_free(deep);
        }
        are_guards_valid =
            check_block(blocks[HEAP_BUCKET_SEPARATOR], HEAP_BUCKET_GUARD_SIZE, UINT8_C(0xA5)) &&
            check_block(blocks[HEAP_BUCKET_TAIL_GUARD], HEAP_BUCKET_GUARD_SIZE, UINT8_C(0x5A));
        if (are_guards_valid) {
            /* The separator now has free physical neighbors on both sides. */
            rtos_heap_free(blocks[HEAP_BUCKET_SEPARATOR]);
            blocks[HEAP_BUCKET_SEPARATOR] = NULL;
            joined_max = rtos_heap_get_max_alloc();
            void *const joined = rtos_heap_alloc(HEAP_BUCKET_JOINED_SIZE);
            is_joined_fit = (uintptr_t)joined == small;
            if (joined != NULL) {
                rtos_heap_free(joined);
            }
            are_guards_valid = check_block(blocks[HEAP_BUCKET_TAIL_GUARD],
                                           HEAP_BUCKET_GUARD_SIZE, UINT8_C(0x5A));
        }
    }
    for (uint32_t count = HEAP_BUCKET_BLOCK_COUNT; count > 0U; --count) {
        rtos_heap_free(blocks[count - 1U]);
    }
    (void)printf("HLAT|BUCKET|layout=%lu|holes_max=%lu|rejected=%lu|deep_fit=%lu"
                 "|remaining_max=%lu|joined_max=%lu|joined_fit=%lu|guards=%lu\r\n",
                 (unsigned long)is_layout_valid, (unsigned long)holes_max,
                 (unsigned long)is_rejected, (unsigned long)is_deep_fit,
                 (unsigned long)remaining_max, (unsigned long)joined_max,
                 (unsigned long)is_joined_fit, (unsigned long)are_guards_valid);
    return is_layout_valid && holes_max == HEAP_BUCKET_LARGE_SIZE && is_rejected && is_deep_fit &&
        remaining_max == HEAP_BUCKET_SMALL_SIZE && joined_max == HEAP_BUCKET_JOINED_SIZE &&
        is_joined_fit && are_guards_valid;
}

#if defined(HEAP_LATENCY_SIZE_CASES) && (HEAP_LATENCY_SIZE_CASES != 0)
enum {
    HEAP_LATENCY_EXACT_SIZE = 32U,
    HEAP_LATENCY_LARGE_SIZE = 4096U,
    HEAP_LATENCY_SIZE_STRIDE = HEAP_LATENCY_EXACT_SIZE + HEAP_BUCKET_HEADER_SIZE,
    HEAP_LATENCY_SIZE_MIN_TAIL = 24U
};

typedef struct heap_latency_size_case {
    const char *name;
    uint32_t request;
    bool uses_exact_hole;
} heap_latency_size_case_t;

typedef struct heap_latency_size_layout {
    uint8_t *guard;
    uintptr_t hole_address;
    uintptr_t tail_address;
    uint32_t tail_max;
    uint32_t free_bytes;
} heap_latency_size_layout_t;

static bool prepare_size_layout(uint32_t initial_maximum, uint32_t initial_live,
                                heap_latency_size_layout_t *layout)
{
    const uint32_t required = 2U * HEAP_LATENCY_SIZE_STRIDE + HEAP_LATENCY_LARGE_SIZE +
        HEAP_BUCKET_HEADER_SIZE + HEAP_LATENCY_SIZE_MIN_TAIL;
    if (initial_maximum < required || !normalize_layout(initial_maximum, initial_live)) {
        return false;
    }

    void *const hole = rtos_heap_alloc(HEAP_LATENCY_EXACT_SIZE);
    if (!is_aligned_block(hole)) {
        rtos_heap_free(hole);
        return false;
    }
    layout->hole_address = (uintptr_t)hole;
    layout->guard = rtos_heap_alloc(HEAP_LATENCY_EXACT_SIZE);
    if (!is_aligned_block(layout->guard) ||
        (uintptr_t)layout->guard != layout->hole_address + HEAP_LATENCY_SIZE_STRIDE) {
        rtos_heap_free(hole);
        return false;
    }
    layout->tail_address = (uintptr_t)layout->guard + HEAP_LATENCY_SIZE_STRIDE;
    mark_block(layout->guard, HEAP_LATENCY_EXACT_SIZE, UINT8_C(0xB7));
    rtos_heap_free(hole);

    /* The live guard separates one exact-size hole from the large tail. */
    layout->tail_max = rtos_heap_get_max_alloc();
    layout->free_bytes = rtos_heap_get_free();
    return layout->tail_max == initial_maximum - 2U * HEAP_LATENCY_SIZE_STRIDE &&
        layout->free_bytes == layout->tail_max + HEAP_LATENCY_EXACT_SIZE;
}

static bool check_size_probe(const heap_latency_size_case_t *pattern,
                             const heap_latency_size_layout_t *layout, uint32_t *allocated_max)
{
    const uintptr_t expected = pattern->uses_exact_hole
        ? layout->hole_address : layout->tail_address;
    const uint32_t cost = pattern->request +
        (pattern->uses_exact_hole ? 0U : HEAP_BUCKET_HEADER_SIZE);
    const uint32_t expected_max = pattern->uses_exact_hole
        ? layout->tail_max : layout->tail_max - cost;
    void *const block = rtos_heap_alloc(pattern->request);
    *allocated_max = rtos_heap_get_max_alloc();
    const bool is_fit = is_aligned_block(block) && (uintptr_t)block == expected &&
        *allocated_max == expected_max && rtos_heap_get_free() == layout->free_bytes - cost;
    rtos_heap_free(block);
    return is_fit && rtos_heap_get_max_alloc() == layout->tail_max &&
        rtos_heap_get_free() == layout->free_bytes;
}

static bool measure_size_case(const heap_latency_size_case_t *pattern, uint32_t initial_maximum,
                              uint32_t initial_live)
{
    heap_latency_size_layout_t layout = { 0 };
    heap_latency_stats_t allocations = { 0 };
    heap_latency_stats_t releases = { 0 };
    uint32_t allocated_max = 0U;
    const bool is_layout_valid = prepare_size_layout(initial_maximum, initial_live, &layout);
    const bool is_probe_valid = is_layout_valid &&
        check_size_probe(pattern, &layout, &allocated_max);
    const uintptr_t expected = pattern->uses_exact_hole ? layout.hole_address : layout.tail_address;
    bool is_valid = is_probe_valid;

    for (uint32_t sample = 0U; is_valid && sample < HEAP_LATENCY_WARMUP + HEAP_LATENCY_SAMPLES;
         ++sample) {
        const bool is_warmup = sample < HEAP_LATENCY_WARMUP;
        void *const block = sample_alloc(pattern->request, is_warmup ? NULL : &allocations);
        is_valid = is_aligned_block(block) && (uintptr_t)block == expected;
        if (block != NULL) {
            sample_free(block, is_warmup ? NULL : &releases);
        }
    }
    const bool is_layout_restored = is_layout_valid &&
        check_block(layout.guard, HEAP_LATENCY_EXACT_SIZE, UINT8_C(0xB7)) &&
        rtos_heap_get_max_alloc() == layout.tail_max && rtos_heap_get_free() == layout.free_bytes;
    rtos_heap_free(layout.guard);
    const bool is_heap_restored = rtos_heap_get_max_alloc() == initial_maximum &&
        live_allocations() == initial_live && is_interrupt_state_clear();
    (void)printf("HLAT|SIZE|case=%s|request=%lu|hole_size=%lu|tail_max=%lu|allocated_max=%lu"
                 "|layout=%lu|probe=%lu|layout_restored=%lu|heap_restored=%lu\r\n",
                 pattern->name, (unsigned long)pattern->request,
                 (unsigned long)HEAP_LATENCY_EXACT_SIZE, (unsigned long)layout.tail_max,
                 (unsigned long)allocated_max, (unsigned long)is_layout_valid,
                 (unsigned long)is_probe_valid, (unsigned long)is_layout_restored,
                 (unsigned long)is_heap_restored);
    const bool are_allocations_valid = print_stats(pattern->name, "alloc", &allocations);
    const bool are_releases_valid = print_stats(pattern->name, "free", &releases);
    return is_valid && is_layout_restored && is_heap_restored &&
        are_allocations_valid && are_releases_valid;
}
#endif

static bool check_recovery(uint32_t initial_maximum, uint32_t initial_live, uint32_t initial_free)
{
    const uint32_t restored_maximum = rtos_heap_get_max_alloc();
    const uint32_t restored_live = live_allocations();
    const uint32_t restored_free = rtos_heap_get_free();
    void *probe = NULL;
    if (initial_maximum != 0U && restored_maximum == initial_maximum) {
        probe = rtos_heap_alloc(initial_maximum);
    }
    const bool is_exact_probe_valid = is_aligned_block(probe);
    if (probe != NULL) {
        rtos_heap_free(probe);
    }
    const uint32_t final_maximum = rtos_heap_get_max_alloc();
    const uint32_t final_live = live_allocations();
    (void)printf("HLAT|RECOVERY|initial_max=%lu|restored_max=%lu|final_max=%lu"
                 "|initial_live=%lu|restored_live=%lu|final_live=%lu"
                 "|initial_free=%lu|restored_free=%lu|exact_probe=%lu\r\n",
                 (unsigned long)initial_maximum, (unsigned long)restored_maximum,
                 (unsigned long)final_maximum, (unsigned long)initial_live,
                 (unsigned long)restored_live, (unsigned long)final_live,
                 (unsigned long)initial_free, (unsigned long)restored_free,
                 (unsigned long)is_exact_probe_valid);
    /* Lazy coalescing can retain extra free-block headers after cleanup. */
    return is_exact_probe_valid && final_maximum == initial_maximum &&
        restored_live == initial_live && final_live == initial_live && is_interrupt_state_clear();
}

static void print_case(const char *name, bool is_valid, uint32_t *failures)
{
    if (!is_valid) {
        ++(*failures);
    }
    (void)printf("HLAT|CASE|case=%s|%s\r\n", name, is_valid ? "PASS" : "FAIL");
}

/**
 * @brief Run the one-shot benchmark in the isolated F446 audit task.
 * @param[in] argument Unused audit task argument.
 */
void f446_heap_latency_run(void *argument)
{
    (void)argument;
    static const heap_latency_pattern_t few = {
        "few", HEAP_LATENCY_FEW_BLOCKS, HEAP_LATENCY_FEW_SIZE, HEAP_LATENCY_FEW_PROBE
    };
    static const heap_latency_pattern_t fragmented = {
        "fragmented", HEAP_LATENCY_FRAGMENT_BLOCKS, HEAP_LATENCY_FRAGMENT_SIZE,
        HEAP_LATENCY_FRAGMENT_PROBE
    };
    const uint32_t initial_maximum = rtos_heap_get_max_alloc();
    const uint32_t initial_live = live_allocations();
    const uint32_t initial_free = rtos_heap_get_free();
    const bool is_ready = initial_maximum != 0U && is_interrupt_state_clear() &&
        enable_cycle_counter() && normalize_layout(initial_maximum, initial_live);
    uint32_t failures = 0U;
    (void)printf("HLAT|START|clock_hz=%lu|samples=%lu|warmup=%lu|heap_size=%lu|tcb_size=%lu"
                 "|stack_words=128,192,256,320|task_slots=%lu\r\n",
                 (unsigned long)SystemCoreClock, (unsigned long)HEAP_LATENCY_SAMPLES,
                 (unsigned long)HEAP_LATENCY_WARMUP, (unsigned long)RTOS_CONFIG_HEAP_SIZE,
                 (unsigned long)sizeof(rtos_tcb_t), (unsigned long)HEAP_LATENCY_TASK_SLOTS);
    print_case("setup", is_ready, &failures);
    const bool is_few_valid = is_ready && measure_holes(&few) &&
        normalize_layout(initial_maximum, initial_live);
    print_case("few", is_few_valid, &failures);
    const bool is_fragmented_valid = is_few_valid && measure_holes(&fragmented) &&
        normalize_layout(initial_maximum, initial_live);
    print_case("fragmented", is_fragmented_valid, &failures);
    const bool is_task_valid = is_fragmented_valid && measure_task_churn();
    print_case("task", is_task_valid, &failures);
    print_case("wide_bucket", is_task_valid && normalize_layout(initial_maximum, initial_live) &&
               check_wide_bucket(), &failures);
#if defined(HEAP_LATENCY_SIZE_CASES) && (HEAP_LATENCY_SIZE_CASES != 0)
    static const heap_latency_size_case_t exact = { "exact32", HEAP_LATENCY_EXACT_SIZE, true };
    static const heap_latency_size_case_t large = { "split4096", HEAP_LATENCY_LARGE_SIZE, false };
    const bool is_exact_valid = failures == 0U &&
        measure_size_case(&exact, initial_maximum, initial_live);
    print_case("exact32", is_exact_valid, &failures);
    print_case("split4096", is_exact_valid && measure_size_case(&large, initial_maximum,
               initial_live), &failures);
#endif
    print_case("recovery", check_recovery(initial_maximum, initial_live, initial_free), &failures);
#if defined(HEAP_LATENCY_SIZE_CASES) && (HEAP_LATENCY_SIZE_CASES != 0)
    (void)printf("HLAT|DONE|status=%lu|checks=8|failures=%lu\r\n",
                 (unsigned long)(failures != 0U), (unsigned long)failures);
#else
    (void)printf("HLAT|DONE|status=%lu|checks=6|failures=%lu\r\n",
                 (unsigned long)(failures != 0U), (unsigned long)failures);
#endif
}

/**
 * @file f446_heap_checkpoint_mixed.c
 * @brief Check first-fit and measure mixed heap calls with forced prefix releases.
 *
 * This fixture runs alone in a task. Its ordered span model owns no production
 * heap metadata. Model operations, canaries and UART output are outside timing.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"

#include "rtos_heap.h"
#include "rtos_port.h"
#include "rtos_sched.h"

enum {
    CHECKPOINT_ALIGNMENT = 8U,
    CHECKPOINT_HEADER_SIZE = 8U,
    CHECKPOINT_MIN_TAIL = 24U,
    CHECKPOINT_FEW_ROUNDS = 128U,
    CHECKPOINT_MIXED_ROUNDS = 192U,
    CHECKPOINT_MIXED_SLOTS = 24U,
    CHECKPOINT_PROBE_SLOT = CHECKPOINT_MIXED_SLOTS,
    CHECKPOINT_SLOT_COUNT = CHECKPOINT_MIXED_SLOTS + 1U,
    CHECKPOINT_SPAN_LIMIT = 256U,
    CHECKPOINT_CHECKS = 3U
};

typedef struct checkpoint_span {
    uint32_t size;
    uint32_t owner;
} checkpoint_span_t;

typedef struct checkpoint_slot {
    uint8_t *address;
    uint32_t size;
    uint32_t stamp;
} checkpoint_slot_t;

typedef struct checkpoint_model {
    uintptr_t base;
    uint32_t capacity;
    uint32_t span_count;
    uint32_t allocations;
    uint32_t releases;
    uint32_t last_checkpoint;
    checkpoint_span_t spans[CHECKPOINT_SPAN_LIMIT];
    checkpoint_slot_t slots[CHECKPOINT_SLOT_COUNT];
} checkpoint_model_t;

typedef struct checkpoint_samples {
    uint32_t count;
    uint32_t sum;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t tick_crossings;
    uint32_t invalid;
} checkpoint_samples_t;

typedef struct checkpoint_trace {
    checkpoint_samples_t alloc;
    checkpoint_samples_t cold_alloc;
    checkpoint_samples_t release;
    checkpoint_samples_t prefix_release;
    uint32_t intended_invalidations;
    bool pending_cold_alloc;
} checkpoint_trace_t;

static checkpoint_model_t s_model;
static const uint32_t s_request_sizes[] = {8U, 17U, 32U, 47U, 64U, 95U, 128U, 149U,
                                          216U, 257U, 320U, 511U};

void f446_heap_checkpoint_mixed_run(void *argument);

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

static bool is_interrupt_state_clear(void)
{
    return __get_BASEPRI() == 0U && __get_PRIMASK() == 0U &&
        rtos_port_get_critical_nesting() == 0U;
}

static uint32_t physical_size(uint32_t size)
{
    return ((size + CHECKPOINT_ALIGNMENT - 1U) / CHECKPOINT_ALIGNMENT) * CHECKPOINT_ALIGNMENT +
        CHECKPOINT_HEADER_SIZE;
}

static uint32_t request_at(uint32_t sequence)
{
    return s_request_sizes[sequence % (sizeof(s_request_sizes) / sizeof(s_request_sizes[0]))];
}

static uint8_t canary(uint32_t stamp, uint32_t offset)
{
    return (uint8_t)((stamp * 71U) ^ (offset * 13U) ^ 0xA5U);
}

static void fill_slot(checkpoint_slot_t *slot)
{
    for (uint32_t index = 0U; index < slot->size; ++index) {
        slot->address[index] = canary(slot->stamp, index);
    }
}

static bool check_slot(const checkpoint_slot_t *slot)
{
    for (uint32_t index = 0U; index < slot->size; ++index) {
        if (slot->address[index] != canary(slot->stamp, index)) {
            return false;
        }
    }
    return true;
}

static bool check_model(void)
{
    uint32_t cursor = 0U;
    uint32_t free_bytes = 0U;
    uint32_t run = 0U;
    uint32_t largest = 0U;
    for (uint32_t index = 0U; index < s_model.span_count; ++index) {
        const checkpoint_span_t span = s_model.spans[index];
        if (span.owner == 0U) {
            free_bytes += span.size - CHECKPOINT_HEADER_SIZE;
            run += span.size;
            if (run > largest) {
                largest = run;
            }
        } else {
            const checkpoint_slot_t *slot = &s_model.slots[span.owner - 1U];
            if ((uintptr_t)slot->address != s_model.base + cursor || !check_slot(slot)) {
                return false;
            }
            run = 0U;
        }
        cursor += span.size;
    }
    const uint32_t max_payload = (largest != 0U) ? largest - CHECKPOINT_HEADER_SIZE : 0U;
    return cursor == s_model.capacity + CHECKPOINT_HEADER_SIZE &&
        rtos_heap_get_free() == free_bytes && rtos_heap_get_max_alloc() == max_payload &&
        rtos_heap_get_alloc_count() == s_model.allocations &&
        rtos_heap_get_free_count() == s_model.releases && is_interrupt_state_clear();
}

static bool assign_span(uint32_t start, uint32_t stop, uint32_t length, uint32_t need,
                        uint32_t owner)
{
    const uint32_t tail = length - need;
    const bool split = tail >= CHECKPOINT_MIN_TAIL;
    const uint32_t destination = start + (split ? 2U : 1U);
    const uint32_t remaining = s_model.span_count - stop;
    if (destination + remaining > CHECKPOINT_SPAN_LIMIT) {
        return false;
    }
    if (remaining != 0U) {
        memmove(&s_model.spans[destination], &s_model.spans[stop],
                remaining * sizeof(s_model.spans[0]));
    }
    s_model.spans[start] = (checkpoint_span_t){split ? need : length, owner};
    if (split) {
        s_model.spans[start + 1U] = (checkpoint_span_t){tail, 0U};
    }
    s_model.span_count = destination + remaining;
    return true;
}

static bool model_allocate(uint32_t slot, uint32_t request, uint32_t *offset)
{
    const uint32_t need = physical_size(request);
    uint32_t cursor = 0U;
    uint32_t index = 0U;
    while (index < s_model.span_count) {
        if (s_model.spans[index].owner != 0U) {
            cursor += s_model.spans[index].size;
            ++index;
            continue;
        }
        const uint32_t start = index;
        uint32_t span = 0U;
        while (index < s_model.span_count && s_model.spans[index].owner == 0U && span < need) {
            span += s_model.spans[index].size;
            ++index;
        }
        if (span >= need) {
            *offset = cursor;
            return assign_span(start, index, span, need, slot + 1U);
        }
        cursor += span;
    }
    return false;
}

static bool model_release(uint32_t slot, uint32_t *offset)
{
    uint32_t cursor = 0U;
    for (uint32_t index = 0U; index < s_model.span_count; ++index) {
        checkpoint_span_t *const span = &s_model.spans[index];
        if (span->owner == slot + 1U) {
            *offset = cursor;
            span->owner = 0U;
            const uint32_t next = index + 1U;
            if (next < s_model.span_count && s_model.spans[next].owner == 0U) {
                span->size += s_model.spans[next].size;
                --s_model.span_count;
                const uint32_t remaining = s_model.span_count - next;
                if (remaining != 0U) {
                    memmove(&s_model.spans[next], &s_model.spans[next + 1U],
                            remaining * sizeof(s_model.spans[0]));
                }
            }
            return true;
        }
        cursor += span->size;
    }
    return false;
}

static void add_sample(checkpoint_samples_t *samples, uint32_t cycles, bool crossed_tick)
{
    if (cycles == 0U || cycles > UINT32_MAX - samples->sum || !is_interrupt_state_clear()) {
        ++samples->invalid;
        return;
    }
    if (samples->count == 0U || cycles < samples->minimum) {
        samples->minimum = cycles;
    }
    if (cycles > samples->maximum) {
        samples->maximum = cycles;
    }
    ++samples->count;
    samples->sum += cycles;
    if (crossed_tick) {
        ++samples->tick_crossings;
    }
}

static bool take_slot(uint32_t index, uint32_t request, checkpoint_trace_t *trace)
{
    checkpoint_slot_t *const slot = &s_model.slots[index];
    uint32_t offset;
    if (slot->address != NULL || !model_allocate(index, request, &offset)) {
        return false;
    }
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    uint8_t *const address = rtos_heap_alloc(request);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    if (trace != NULL) {
        const bool crossed_tick = rtos_sched_get_tick_count() != tick;
        add_sample(&trace->alloc, cycles, crossed_tick);
        if (trace->pending_cold_alloc) {
            add_sample(&trace->cold_alloc, cycles, crossed_tick);
            trace->pending_cold_alloc = false;
        }
    }
    if (address == NULL || (uintptr_t)address != s_model.base + offset) {
        (void)printf("HCPM|ERROR|op=alloc|size=%lu|offset=%lu\r\n",
                     (unsigned long)request, (unsigned long)offset);
        return false;
    }
    ++s_model.allocations;
    s_model.last_checkpoint = offset;
    *slot = (checkpoint_slot_t){address, request, s_model.allocations};
    fill_slot(slot);
    return check_model();
}

static bool return_slot(uint32_t index, checkpoint_trace_t *trace)
{
    checkpoint_slot_t *const slot = &s_model.slots[index];
    uint32_t offset;
    if (slot->address == NULL || !check_slot(slot) || !model_release(index, &offset) ||
        (uintptr_t)slot->address != s_model.base + offset) {
        return false;
    }
    const bool is_prefix = offset < s_model.last_checkpoint;
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    rtos_heap_free(slot->address);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    if (trace != NULL) {
        const bool crossed_tick = rtos_sched_get_tick_count() != tick;
        add_sample(&trace->release, cycles, crossed_tick);
        if (is_prefix) {
            add_sample(&trace->prefix_release, cycles, crossed_tick);
            ++trace->intended_invalidations;
            trace->pending_cold_alloc = true;
        }
    }
    if (is_prefix) {
        s_model.last_checkpoint = 0U;
    }
    ++s_model.releases;
    *slot = (checkpoint_slot_t){0};
    return check_model();
}

static bool initialize_model(void)
{
    if (!enable_cycles() || !is_interrupt_state_clear()) {
        return false;
    }
    const uint32_t capacity = rtos_heap_get_max_alloc();
    if (capacity < 16384U) {
        return false;
    }
    /* Consuming the largest extent proves that no other free extent exists. */
    void *const whole = rtos_heap_alloc(capacity);
    if (whole == NULL || ((uintptr_t)whole % CHECKPOINT_ALIGNMENT) != 0U ||
        rtos_heap_get_free() != 0U || rtos_heap_get_max_alloc() != 0U) {
        return false;
    }
    s_model.base = (uintptr_t)whole;
    rtos_heap_free(whole);
    s_model.capacity = capacity;
    s_model.span_count = 1U;
    s_model.spans[0] = (checkpoint_span_t){capacity + CHECKPOINT_HEADER_SIZE, 0U};
    s_model.allocations = rtos_heap_get_alloc_count();
    s_model.releases = rtos_heap_get_free_count();
    return check_model();
}

static bool run_few(checkpoint_trace_t *trace)
{
    for (uint32_t round = 0U; round < CHECKPOINT_FEW_ROUNDS; ++round) {
        const uint32_t first = request_at(round * 5U);
        const uint32_t second = request_at(round * 7U + 1U);
        const uint32_t high = request_at(round * 11U + 3U);
        const uint32_t joined = physical_size(first) + physical_size(second) -
                                CHECKPOINT_HEADER_SIZE;
        /* Low-to-high release leaves two adjacent free spans. The live high
         * allocation ensures the joined request must reclaim the interior header. */
        if (!take_slot(0U, first, trace) || !take_slot(1U, second, trace) ||
            !take_slot(2U, high, trace) || !return_slot(0U, trace) ||
            !return_slot(1U, trace) || !take_slot(0U, joined, trace) ||
            !return_slot(2U, trace) || !return_slot(0U, trace) ||
            trace->intended_invalidations != round + 1U || s_model.span_count != 1U) {
            return false;
        }
    }
    return true;
}

static bool tail_probe_size(uint32_t sequence, uint32_t *request)
{
    uint32_t interior = 0U;
    uint32_t tail = 0U;
    for (uint32_t index = 0U; index < s_model.span_count; ++index) {
        const checkpoint_span_t span = s_model.spans[index];
        if (span.owner == 0U) {
            tail += span.size;
        } else {
            if (tail > interior) {
                interior = tail;
            }
            tail = 0U;
        }
    }
    const uint32_t chosen = physical_size(request_at(sequence));
    const uint32_t need = (chosen > interior) ? chosen : interior + CHECKPOINT_ALIGNMENT;
    if (need > tail) {
        return false;
    }
    *request = need - CHECKPOINT_HEADER_SIZE;
    return true;
}

static bool run_mixed(checkpoint_trace_t *trace)
{
    for (uint32_t slot = 0U; slot < CHECKPOINT_MIXED_SLOTS; ++slot) {
        if (!take_slot(slot, request_at(slot * 5U + 1U), NULL)) {
            return false;
        }
    }
    for (uint32_t round = 0U; round < CHECKPOINT_MIXED_ROUNDS; ++round) {
        const uint32_t first = (round * 11U + 3U) % CHECKPOINT_MIXED_SLOTS;
        const uint32_t second = (first + 7U) % CHECKPOINT_MIXED_SLOTS;
        uint32_t probe;
        /* A tail-only request makes the next lower release a prefix invalidation. */
        if (!tail_probe_size(round * 13U + 7U, &probe) ||
            !take_slot(CHECKPOINT_PROBE_SLOT, probe, trace) || !return_slot(first, trace) ||
            !return_slot(second, trace) || !return_slot(CHECKPOINT_PROBE_SLOT, trace) ||
            !take_slot(first, request_at(round * 7U + 2U), trace) ||
            !take_slot(second, request_at(round * 11U + 9U), trace) ||
            trace->intended_invalidations != round + 1U) {
            return false;
        }
    }
    for (uint32_t count = CHECKPOINT_MIXED_SLOTS; count > 0U; --count) {
        if (!return_slot(count - 1U, NULL)) {
            return false;
        }
    }
    return take_slot(0U, s_model.capacity, NULL) && return_slot(0U, NULL) &&
        s_model.span_count == 1U;
}

static bool print_samples(const char *name, const char *operation,
                          const checkpoint_samples_t *samples, uint32_t expected)
{
    (void)printf("HCPM|STAT|case=%s|op=%s|n=%lu|sum=%lu|min=%lu|max=%lu"
                 "|tick_crossings=%lu|invalid=%lu\r\n",
                 name, operation, (unsigned long)samples->count, (unsigned long)samples->sum,
                 (unsigned long)samples->minimum, (unsigned long)samples->maximum,
                 (unsigned long)samples->tick_crossings, (unsigned long)samples->invalid);
    return samples->count == expected && samples->invalid == 0U;
}

static bool print_trace(const char *name, const checkpoint_trace_t *trace, uint32_t rounds,
                        uint32_t operations)
{
    (void)printf("HCPM|TRACE|case=%s|rounds=%lu|intended_invalidations=%lu\r\n", name,
                 (unsigned long)rounds, (unsigned long)trace->intended_invalidations);
    const bool alloc = print_samples(name, "alloc", &trace->alloc, rounds * operations);
    const bool cold = print_samples(name, "cold_alloc", &trace->cold_alloc, rounds);
    const bool release = print_samples(name, "free", &trace->release, rounds * operations);
    const bool prefix = print_samples(name, "prefix_free", &trace->prefix_release, rounds);
    return alloc && cold && release && prefix && trace->intended_invalidations == rounds &&
           !trace->pending_cold_alloc;
}

static void print_result(const char *name, bool passed, uint32_t *failures)
{
    if (!passed) {
        ++*failures;
    }
    (void)printf("HCPM|CASE|case=%s|%s\r\n", name, passed ? "PASS" : "FAIL");
}

/**
 * @brief Run both deterministic traces in an isolated portable fixture task.
 * @param[in] argument Unused audit task argument.
 */
void f446_heap_checkpoint_mixed_run(void *argument)
{
    (void)argument;
    checkpoint_trace_t few = {0};
    checkpoint_trace_t mixed = {0};
    uint32_t failures = 0U;
    const bool setup = initialize_model();
    (void)printf("HCPM|START|capacity=%lu|few=%u|mixed=%u|slots=%u\r\n",
                 (unsigned long)s_model.capacity, (unsigned int)CHECKPOINT_FEW_ROUNDS,
                 (unsigned int)CHECKPOINT_MIXED_ROUNDS, (unsigned int)CHECKPOINT_MIXED_SLOTS);
    print_result("setup", setup, &failures);
    const bool few_pass = setup && run_few(&few);
    const bool few_stats = print_trace("few", &few, CHECKPOINT_FEW_ROUNDS, 4U);
    print_result("few", few_pass && few_stats, &failures);
    const bool mixed_pass = few_pass && run_mixed(&mixed);
    const bool mixed_stats = print_trace("mixed", &mixed, CHECKPOINT_MIXED_ROUNDS, 3U);
    print_result("mixed", mixed_pass && mixed_stats, &failures);
    (void)printf("HCPM|DONE|status=%lu|checks=%u|failures=%lu\r\n",
                 (unsigned long)(failures != 0U), (unsigned int)CHECKPOINT_CHECKS,
                 (unsigned long)failures);
}

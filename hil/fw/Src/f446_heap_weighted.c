/**
 * @file f446_heap_weighted.c
 * @brief Measure complete heap API costs on deterministic telemetry lifecycles.
 *
 * An independent ordered span model checks first-fit and capacity after each
 * operation. Only application-requested diagnostic queries occur inside a
 * lifecycle. Payload checks, the oracle and UART output are outside DWT windows.
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"

#include "rtos_config.h"
#include "rtos_heap.h"
#include "rtos_port.h"
#include "rtos_sched.h"

#define HWB_REQUIRE(condition)                                              \
    do {                                                                   \
        if (!(condition)) {                                                \
            (void)printf("HWB|ERROR|line=%u\r\n", (unsigned int)__LINE__);   \
            return false;                                                  \
        }                                                                  \
    } while (0)

enum {
    HWB_HEADER_SIZE = 8U,
    HWB_MIN_TAIL = 24U,
    HWB_ANCHORS = 48U,
    HWB_ANCHOR_SIZE = 64U,
    HWB_FIFO_CAPACITY = 128U,
    HWB_SLOT_COUNT = HWB_ANCHORS + HWB_FIFO_CAPACITY,
    HWB_SPAN_LIMIT = RTOS_CONFIG_HEAP_SIZE / 16U + 1U,
    HWB_ROUNDS = 32U,
    HWB_FRAMES_PER_ROUND = 96U,
    HWB_NORMAL_DEPTH = 16U,
    HWB_BURST_DEPTH = 48U,
    HWB_CASE_COUNT = 3U
};

typedef enum hwb_operation {
    HWB_ALLOC_OK,
    HWB_ALLOC_FAIL,
    HWB_FREE,
    HWB_QUERY,
    HWB_OPERATION_COUNT
} hwb_operation_t;

typedef enum hwb_policy {
    HWB_NORMAL,
    HWB_BURST,
    HWB_DIAGNOSED
} hwb_policy_t;

typedef struct hwb_samples {
    uint64_t sum;
    uint32_t count;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t tick_crossings;
    uint32_t invalid;
} hwb_samples_t;

typedef struct hwb_span {
    uint32_t size;
    uint32_t owner;
} hwb_span_t;

typedef struct hwb_slot {
    uint8_t *address;
    uint32_t size;
    uint32_t stamp;
} hwb_slot_t;

typedef struct hwb_model {
    uintptr_t base;
    uint32_t capacity;
    uint32_t span_count;
    uint32_t allocations;
    uint32_t releases;
    hwb_span_t spans[HWB_SPAN_LIMIT];
    hwb_slot_t slots[HWB_SLOT_COUNT];
} hwb_model_t;

typedef struct hwb_result {
    hwb_samples_t operations[HWB_OPERATION_COUNT];
    hwb_samples_t initial_ok;
    hwb_samples_t retry_ok;
    hwb_samples_t first_fail;
    hwb_samples_t repeated_fail;
    uint64_t alloc_chain_cycles;
    uint64_t recovery_free_cycles;
    uint64_t recovery_query_cycles;
    uint32_t retried;
    uint32_t recovery_frees;
    uint32_t recovery_queries;
    uint32_t frames;
    uint32_t hash;
    uint32_t raw_free;
    uint32_t maximum;
    uint32_t restored_free;
} hwb_result_t;

typedef struct hwb_fifo {
    uint32_t head;
    uint32_t count;
} hwb_fifo_t;

static const uint32_t s_request_sizes[] = {
    64U, 128U, 192U, 256U, 384U, 512U, 768U, 1024U, 1536U
};
static hwb_model_t s_model;
static hwb_result_t s_results[HWB_CASE_COUNT];

void f446_heap_weighted_run(void *argument);

static bool is_interrupt_state_clear(void)
{
    return __get_BASEPRI() == 0U && __get_PRIMASK() == 0U &&
        rtos_port_get_critical_nesting() == 0U;
}

static bool enable_counter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U) {
        return false;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const uint32_t first = DWT->CYCCNT;
    for (uint32_t attempt = 0U; attempt < 16U; ++attempt) {
        if (DWT->CYCCNT != first) {
            return is_interrupt_state_clear();
        }
    }
    return false;
}

static uint32_t physical_size(uint32_t request)
{
    return ((request + 7U) & ~UINT32_C(7)) + HWB_HEADER_SIZE;
}

static uint8_t canary(uint32_t stamp, uint32_t index)
{
    return (uint8_t)((stamp * 71U) ^ (index * 13U) ^ 0xA5U);
}

static bool check_slot(const hwb_slot_t *slot)
{
    for (uint32_t index = 0U; index < slot->size; ++index) {
        if (slot->address[index] != canary(slot->stamp, index)) {
            return false;
        }
    }
    return true;
}

static void fill_slot(const hwb_slot_t *slot)
{
    for (uint32_t index = 0U; index < slot->size; ++index) {
        slot->address[index] = canary(slot->stamp, index);
    }
}

static void hash_value(hwb_result_t *result, uint32_t value)
{
    result->hash = (result->hash ^ value) * UINT32_C(16777619);
}

static uint32_t model_maximum(void)
{
    uint32_t largest = 0U;
    uint32_t run = 0U;
    for (uint32_t index = 0U; index < s_model.span_count; ++index) {
        if (s_model.spans[index].owner == 0U) {
            run += s_model.spans[index].size;
            if (run > largest) {
                largest = run;
            }
        } else {
            run = 0U;
        }
    }
    return largest != 0U ? largest - HWB_HEADER_SIZE : 0U;
}

/* Statistics getters leave the allocator's search hints untouched. */
static bool check_model(void)
{
    uint32_t cursor = 0U;
    uint32_t free_bytes = 0U;
    for (uint32_t index = 0U; index < s_model.span_count; ++index) {
        const hwb_span_t span = s_model.spans[index];
        if (span.owner == 0U) {
            free_bytes += span.size - HWB_HEADER_SIZE;
        } else {
            const hwb_slot_t *const slot = &s_model.slots[span.owner - 1U];
            if ((uintptr_t)slot->address != s_model.base + cursor) {
                return false;
            }
        }
        cursor += span.size;
    }
    return cursor == s_model.capacity + HWB_HEADER_SIZE &&
        rtos_heap_get_free() == free_bytes &&
        rtos_heap_get_alloc_count() == s_model.allocations &&
        rtos_heap_get_free_count() == s_model.releases && is_interrupt_state_clear();
}

static bool assign_span(uint32_t start, uint32_t stop, uint32_t length, uint32_t need,
                        uint32_t owner)
{
    const uint32_t tail = length - need;
    const bool split = tail >= HWB_MIN_TAIL;
    const uint32_t destination = start + (split ? 2U : 1U);
    const uint32_t remaining = s_model.span_count - stop;
    HWB_REQUIRE(destination + remaining <= HWB_SPAN_LIMIT);
    if (remaining != 0U) {
        (void)memmove(&s_model.spans[destination], &s_model.spans[stop],
                      remaining * sizeof(s_model.spans[0]));
    }
    s_model.spans[start] = (hwb_span_t){split ? need : length, owner};
    if (split) {
        s_model.spans[start + 1U] = (hwb_span_t){tail, 0U};
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
        hwb_span_t *const span = &s_model.spans[index];
        if (span->owner == slot + 1U) {
            *offset = cursor;
            span->owner = 0U;
            const uint32_t next = index + 1U;
            if (next < s_model.span_count && s_model.spans[next].owner == 0U) {
                span->size += s_model.spans[next].size;
                --s_model.span_count;
                const uint32_t remaining = s_model.span_count - next;
                if (remaining != 0U) {
                    (void)memmove(&s_model.spans[next], &s_model.spans[next + 1U],
                                  remaining * sizeof(s_model.spans[0]));
                }
            }
            return true;
        }
        cursor += span->size;
    }
    return false;
}

static void add_sample(hwb_samples_t *samples, uint32_t cycles, bool crossed_tick)
{
    if (cycles == 0U || !is_interrupt_state_clear()) {
        ++samples->invalid;
        return;
    }
    if (samples->count == 0U || cycles < samples->minimum) {
        samples->minimum = cycles;
    }
    if (cycles > samples->maximum) {
        samples->maximum = cycles;
    }
    samples->sum += cycles;
    ++samples->count;
    if (crossed_tick) {
        ++samples->tick_crossings;
    }
}

static bool try_allocate(uint32_t index, uint32_t request, hwb_result_t *result,
                         uint32_t *cycles_out, bool *allocated, bool is_frame,
                         bool is_retry)
{
    hwb_slot_t *const slot = &s_model.slots[index];
    HWB_REQUIRE(slot->address == NULL);
    uint32_t offset = 0U;
    const bool expected = model_allocate(index, request, &offset);
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    uint8_t *const address = rtos_heap_alloc(request);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    const bool crossed_tick = rtos_sched_get_tick_count() != tick;
    const bool success = address != NULL;
    add_sample(&result->operations[success ? HWB_ALLOC_OK : HWB_ALLOC_FAIL], cycles,
                crossed_tick);
    if (is_frame) {
        hwb_samples_t *const detail = success ?
            (is_retry ? &result->retry_ok : &result->initial_ok) :
            (is_retry ? &result->repeated_fail : &result->first_fail);
        add_sample(detail, cycles, crossed_tick);
    }
    hash_value(result, success ? HWB_ALLOC_OK : HWB_ALLOC_FAIL);
    hash_value(result, index);
    hash_value(result, request);
    hash_value(result, success ? offset : UINT32_MAX);
    HWB_REQUIRE(success == expected);
    if (success) {
        HWB_REQUIRE((uintptr_t)address == s_model.base + offset);
        ++s_model.allocations;
        *slot = (hwb_slot_t){address, request, s_model.allocations};
        fill_slot(slot);
    }
    *cycles_out = cycles;
    *allocated = success;
    return check_model();
}

static bool release_slot(uint32_t index, hwb_result_t *result, bool is_recovery)
{
    hwb_slot_t *const slot = &s_model.slots[index];
    HWB_REQUIRE(slot->address != NULL && check_slot(slot));
    uint32_t offset;
    HWB_REQUIRE(model_release(index, &offset));
    HWB_REQUIRE((uintptr_t)slot->address == s_model.base + offset);
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    rtos_heap_free(slot->address);
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    add_sample(&result->operations[HWB_FREE], cycles, rtos_sched_get_tick_count() != tick);
    if (is_recovery) {
        result->recovery_free_cycles += cycles;
        ++result->recovery_frees;
    }
    hash_value(result, HWB_FREE);
    hash_value(result, index);
    hash_value(result, offset);
    ++s_model.releases;
    *slot = (hwb_slot_t){0};
    return check_model();
}

static bool measure_query(hwb_result_t *result, uint32_t *maximum)
{
    const uint32_t expected = model_maximum();
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t actual = rtos_heap_get_max_alloc();
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    add_sample(&result->operations[HWB_QUERY], cycles, rtos_sched_get_tick_count() != tick);
    result->recovery_query_cycles += cycles;
    ++result->recovery_queries;
    hash_value(result, HWB_QUERY);
    hash_value(result, actual);
    HWB_REQUIRE(actual == expected);
    *maximum = actual;
    return check_model();
}

static bool release_oldest(hwb_fifo_t *fifo, hwb_result_t *result, bool is_recovery)
{
    HWB_REQUIRE(fifo->count != 0U);
    const uint32_t slot = HWB_ANCHORS + fifo->head;
    HWB_REQUIRE(release_slot(slot, result, is_recovery));
    fifo->head = (fifo->head + 1U) % HWB_FIFO_CAPACITY;
    --fifo->count;
    return true;
}

static bool produce_frame(hwb_fifo_t *fifo, uint32_t request, hwb_policy_t policy,
                          hwb_result_t *result)
{
    HWB_REQUIRE(fifo->count < HWB_FIFO_CAPACITY);
    const uint32_t tail = (fifo->head + fifo->count) % HWB_FIFO_CAPACITY;
    const uint32_t slot = HWB_ANCHORS + tail;
    uint64_t chain = 0U;
    bool retry = false;
    for (uint32_t attempt = 0U; attempt <= HWB_FIFO_CAPACITY; ++attempt) {
        uint32_t cycles;
        bool allocated;
        HWB_REQUIRE(try_allocate(slot, request, result, &cycles, &allocated, true, retry));
        chain += cycles;
        if (allocated) {
            ++fifo->count;
            ++result->frames;
            if (retry) {
                result->alloc_chain_cycles += chain;
            }
            return true;
        }
        if (!retry) {
            ++result->retried;
            retry = true;
        }
        if (policy == HWB_DIAGNOSED) {
            uint32_t maximum;
            HWB_REQUIRE(measure_query(result, &maximum));
            while (maximum < request) {
                HWB_REQUIRE(release_oldest(fifo, result, true));
                HWB_REQUIRE(measure_query(result, &maximum));
            }
        } else {
            HWB_REQUIRE(release_oldest(fifo, result, true));
        }
    }
    HWB_REQUIRE(false);
}

/* Normalize the free arena before the measured lifecycle, then establish an oracle. */
static bool initialize_model(void)
{
    const uint32_t capacity = rtos_heap_get_max_alloc();
    HWB_REQUIRE(capacity >= 24576U);
    uint8_t *const whole = rtos_heap_alloc(capacity);
    HWB_REQUIRE(whole != NULL && rtos_heap_get_free() == 0U);
    rtos_heap_free(whole);
    (void)memset(&s_model, 0, sizeof(s_model));
    s_model.base = (uintptr_t)whole;
    s_model.capacity = capacity;
    s_model.span_count = 1U;
    s_model.spans[0] = (hwb_span_t){capacity + HWB_HEADER_SIZE, 0U};
    s_model.allocations = rtos_heap_get_alloc_count();
    s_model.releases = rtos_heap_get_free_count();
    return check_model();
}

static bool run_case(hwb_policy_t policy, hwb_result_t *result)
{
    HWB_REQUIRE(initialize_model());
    result->hash = UINT32_C(2166136261);
    const uint32_t allocations = s_model.allocations;
    const uint32_t releases = s_model.releases;
    for (uint32_t index = 0U; index < HWB_ANCHORS; ++index) {
        uint32_t cycles;
        bool allocated;
        HWB_REQUIRE(try_allocate(index, HWB_ANCHOR_SIZE, result, &cycles, &allocated,
                                 false, false) && allocated);
    }
    for (uint32_t index = 0U; index < HWB_ANCHORS; index += 2U) {
        HWB_REQUIRE(release_slot(index, result, false));
    }
    hwb_fifo_t fifo = {0};
    for (uint32_t round = 0U; round < HWB_ROUNDS; ++round) {
        for (uint32_t frame = 0U; frame < HWB_FRAMES_PER_ROUND; ++frame) {
            if (policy == HWB_NORMAL && fifo.count >= HWB_NORMAL_DEPTH) {
                HWB_REQUIRE(release_oldest(&fifo, result, false));
            }
            const uint32_t sequence = round * HWB_FRAMES_PER_ROUND + frame;
            const uint32_t size_index = sequence %
                (sizeof(s_request_sizes) / sizeof(s_request_sizes[0]));
            HWB_REQUIRE(produce_frame(&fifo, s_request_sizes[size_index], policy, result));
            if (policy != HWB_NORMAL && frame >= HWB_BURST_DEPTH && (frame & 3U) == 3U) {
                HWB_REQUIRE(release_oldest(&fifo, result, false));
            }
        }
        while (fifo.count != 0U) {
            HWB_REQUIRE(release_oldest(&fifo, result, false));
        }
    }
    for (uint32_t index = 1U; index < HWB_ANCHORS; index += 2U) {
        HWB_REQUIRE(release_slot(index, result, false));
    }
    result->raw_free = rtos_heap_get_free();
    result->maximum = rtos_heap_get_max_alloc();
    HWB_REQUIRE(result->maximum == s_model.capacity && check_model());
    HWB_REQUIRE(s_model.allocations - allocations ==
                result->operations[HWB_ALLOC_OK].count);
    HWB_REQUIRE(s_model.releases - releases == result->operations[HWB_FREE].count);
    HWB_REQUIRE(result->operations[HWB_ALLOC_OK].count == result->operations[HWB_FREE].count);
    HWB_REQUIRE(result->frames == HWB_ROUNDS * HWB_FRAMES_PER_ROUND);
    HWB_REQUIRE(result->initial_ok.count + result->retry_ok.count == result->frames &&
                result->retried == result->retry_ok.count &&
                result->retried == result->first_fail.count &&
                result->first_fail.count + result->repeated_fail.count ==
                result->operations[HWB_ALLOC_FAIL].count);
    HWB_REQUIRE(result->alloc_chain_cycles == result->first_fail.sum +
                result->repeated_fail.sum + result->retry_ok.sum);
    HWB_REQUIRE(result->recovery_queries == result->operations[HWB_QUERY].count &&
                result->recovery_query_cycles == result->operations[HWB_QUERY].sum &&
                result->recovery_frees <= result->operations[HWB_FREE].count &&
                result->recovery_free_cycles <= result->operations[HWB_FREE].sum);
    if (policy == HWB_NORMAL) {
        HWB_REQUIRE(result->retried == 0U && result->operations[HWB_QUERY].count == 0U);
    } else {
        HWB_REQUIRE(result->retried != 0U && result->recovery_frees != 0U);
    }
    if (policy == HWB_DIAGNOSED) {
        HWB_REQUIRE(result->recovery_queries == result->operations[HWB_QUERY].count &&
                    result->recovery_queries != 0U);
    }
    for (uint32_t operation = 0U; operation < HWB_OPERATION_COUNT; ++operation) {
        HWB_REQUIRE(result->operations[operation].invalid == 0U);
    }
    void *const whole = rtos_heap_alloc(s_model.capacity);
    HWB_REQUIRE(whole != NULL && rtos_heap_get_free() == 0U);
    rtos_heap_free(whole);
    result->restored_free = rtos_heap_get_free();
    HWB_REQUIRE(result->restored_free == s_model.capacity);
    return true;
}

static void print_sample(const char *prefix, const char *name, const char *operation,
                          const hwb_samples_t *samples)
{
    (void)printf("HWB|%s|case=%s|op=%s|n=%lu|sum_hi=%lu|sum_lo=%lu|min=%lu|max=%lu"
                 "|tick_crossings=%lu|invalid=%lu\r\n", prefix, name, operation,
                 (unsigned long)samples->count, (unsigned long)(samples->sum >> 32U),
                 (unsigned long)(uint32_t)samples->sum, (unsigned long)samples->minimum,
                 (unsigned long)samples->maximum, (unsigned long)samples->tick_crossings,
                 (unsigned long)samples->invalid);
}

static void print_result(const char *name, const hwb_result_t *result, bool passed)
{
    static const char *const operations[HWB_OPERATION_COUNT] = {
        "alloc_ok", "alloc_fail", "free", "query"
    };
    for (uint32_t operation = 0U; operation < HWB_OPERATION_COUNT; ++operation) {
        print_sample("STAT", name, operations[operation], &result->operations[operation]);
    }
    print_sample("SUBSTAT", name, "initial_ok", &result->initial_ok);
    print_sample("SUBSTAT", name, "retry_ok", &result->retry_ok);
    print_sample("SUBSTAT", name, "first_fail", &result->first_fail);
    print_sample("SUBSTAT", name, "repeat_fail", &result->repeated_fail);
    (void)printf("HWB|RETRY|case=%s|requests=%lu|retried=%lu|fail_attempts=%lu"
                 "|retry_success=%lu|alloc_chain_hi=%lu|alloc_chain_lo=%lu"
                 "|recovery_frees=%lu|recovery_free_hi=%lu|recovery_free_lo=%lu"
                 "|recovery_queries=%lu|recovery_query_hi=%lu|recovery_query_lo=%lu\r\n",
                 name, (unsigned long)result->frames, (unsigned long)result->retried,
                 (unsigned long)result->operations[HWB_ALLOC_FAIL].count,
                 (unsigned long)result->retry_ok.count,
                 (unsigned long)(result->alloc_chain_cycles >> 32U),
                 (unsigned long)(uint32_t)result->alloc_chain_cycles,
                 (unsigned long)result->recovery_frees,
                 (unsigned long)(result->recovery_free_cycles >> 32U),
                 (unsigned long)(uint32_t)result->recovery_free_cycles,
                 (unsigned long)result->recovery_queries,
                 (unsigned long)(result->recovery_query_cycles >> 32U),
                 (unsigned long)(uint32_t)result->recovery_query_cycles);
    uint64_t cycles = 0U;
    uint32_t calls = 0U;
    for (uint32_t operation = 0U; operation < HWB_OPERATION_COUNT; ++operation) {
        cycles += result->operations[operation].sum;
        calls += result->operations[operation].count;
    }
    (void)printf("HWB|TOTAL|case=%s|frames=%lu|anchor_allocations=%u|allocations=%lu"
                 "|frees=%lu|calls=%lu|cycles_hi=%lu|cycles_lo=%lu|hash=%lu"
                 "|raw_free=%lu|max_alloc=%lu|restored_free=%lu|setup_calls=3|verify_calls=3"
                 "\r\n", name,
                 (unsigned long)result->frames, (unsigned int)HWB_ANCHORS,
                 (unsigned long)result->operations[HWB_ALLOC_OK].count,
                 (unsigned long)result->operations[HWB_FREE].count, (unsigned long)calls,
                 (unsigned long)(cycles >> 32U), (unsigned long)(uint32_t)cycles,
                 (unsigned long)result->hash, (unsigned long)result->raw_free,
                 (unsigned long)result->maximum, (unsigned long)result->restored_free);
    (void)printf("HWB|CASE|case=%s|%s\r\n", name, passed ? "PASS" : "FAIL");
}

void f446_heap_weighted_run(void *argument)
{
    (void)argument;
    static const char *const names[HWB_CASE_COUNT] = {"normal", "burst", "diagnosed"};
    const bool is_ready = enable_counter();
    const uint32_t capacity = rtos_heap_get_max_alloc();
    (void)printf("HWB|START|clock_hz=%lu|capacity=%lu|rounds=%u|frames_per_round=%u"
                 "|anchors=%u|basepri=%u\r\n", (unsigned long)SystemCoreClock,
                 (unsigned long)capacity, (unsigned int)HWB_ROUNDS,
                 (unsigned int)HWB_FRAMES_PER_ROUND, (unsigned int)HWB_ANCHORS,
                 (unsigned int)RTOS_CONFIG_USE_BASEPRI);
    uint32_t failures = 0U;
    for (uint32_t index = 0U; index < HWB_CASE_COUNT; ++index) {
        const bool passed = is_ready && failures == 0U &&
            run_case((hwb_policy_t)index, &s_results[index]);
        print_result(names[index], &s_results[index], passed);
        if (!passed) {
            ++failures;
        }
    }
    (void)printf("HWB|DONE|status=%u|checks=%u|failures=%lu\r\n",
                 (unsigned int)(failures != 0U), (unsigned int)HWB_CASE_COUNT,
                 (unsigned long)failures);
}

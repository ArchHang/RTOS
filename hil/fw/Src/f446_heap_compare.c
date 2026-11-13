/**
 * @file f446_heap_compare.c
 * @brief Replay identical F446 traces against the RTOS heap and newlib-nano.
 */
#include <errno.h>
#include <reent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "stm32f4xx.h"

#include "rtos_heap.h"
#include "rtos_port.h"
#include "rtos_sched.h"

#ifndef HCB_PROTECTED
#define HCB_PROTECTED 1
#endif

#ifndef HCB_RTOS_ONLY
#define HCB_RTOS_ONLY 0
#endif

#ifndef HCB_FAILURE_ONLY
#define HCB_FAILURE_ONLY 0
#endif

#ifndef HCB_LIBC_FIRST
#define HCB_LIBC_FIRST 0U
#endif

enum {
    HCB_SAMPLES = 512U,
    HCB_WARMUP = 16U,
    HCB_MIXED_SLOTS = 16U,
    HCB_FRAGMENT_BLOCKS = 80U,
    HCB_FRAGMENT_SIZE = 32U,
    HCB_FRAGMENT_PROBE = 512U,
    HCB_SLOT_COUNT = HCB_FRAGMENT_BLOCKS + 1U,
    HCB_FAILURE_HOLES = 128U,
    HCB_FAILURE_REPEATS = 32U,
    HCB_HEADER_SIZE = 8U,
    HCB_GUARD_SIZE = 32U
};

typedef enum hcb_backend {
    HCB_RTOS = 0,
    HCB_LIBC,
    HCB_BACKEND_COUNT = HCB_RTOS_ONLY ? 1 : 2
} hcb_backend_t;

typedef struct hcb_samples {
    uint32_t count;
    uint32_t sum;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t crossings;
    uint32_t invalid;
} hcb_samples_t;

typedef struct hcb_result {
    hcb_samples_t allocations;
    hcb_samples_t releases;
    uint32_t live;
    uint32_t peak;
    uint32_t stride_min;
    uint32_t stride_max;
    uint32_t probe_below;
    bool passed;
} hcb_result_t;

typedef struct hcb_slot {
    uint8_t *address;
    uint32_t size;
    uint8_t seed;
} hcb_slot_t;

typedef struct hcb_failure {
    hcb_samples_t exhausted;
    hcb_samples_t first;
    hcb_samples_t repeated;
    hcb_samples_t larger;
    hcb_samples_t query;
    hcb_samples_t repeated_query;
    bool passed;
} hcb_failure_t;

static hcb_slot_t s_slots[HCB_SLOT_COUNT];
static hcb_result_t s_results[HCB_BACKEND_COUNT][3];
static hcb_failure_t s_failures[HCB_BACKEND_COUNT];
static uint8_t *s_holes[HCB_FAILURE_HOLES];
static uint8_t *s_guards[HCB_FAILURE_HOLES];
static uintptr_t s_libc_limit;

void *__real__sbrk(ptrdiff_t increment);
void *__wrap__sbrk(ptrdiff_t increment);
void f446_heap_compare_run(void *argument);

#if HCB_PROTECTED
static uint32_t s_malloc_depth;
static uint32_t s_malloc_saved_mask;

/* These names are the ABI declared by newlib's reent.h. Only IRQ priorities
 * allowed to take the RTOS heap lock may enter this benchmark's allocator. */
void __malloc_lock(struct _reent *context)
{
    (void)context;
    const uint32_t mask = RTOS_PORT_ENTER_LOCAL_CRITICAL();
    if (s_malloc_depth == 0U) {
        s_malloc_saved_mask = mask;
    }
    ++s_malloc_depth;
}

void __malloc_unlock(struct _reent *context)
{
    (void)context;
    --s_malloc_depth;
    if (s_malloc_depth == 0U) {
        RTOS_PORT_EXIT_LOCAL_CRITICAL(s_malloc_saved_mask);
    }
}
#endif

/* GNU ld redirects library _sbrk calls here only in the HIL image. */
void *__wrap__sbrk(ptrdiff_t increment)
{
    if (increment > 0 && s_libc_limit != 0U) {
        const uintptr_t current = (uintptr_t)__real__sbrk(0);
        if (current > s_libc_limit || (uintptr_t)increment > s_libc_limit - current) {
            errno = ENOMEM;
            return (void *)-1;
        }
    }
    return __real__sbrk(increment);
}

static bool is_interrupt_state_clear(void)
{
    return __get_BASEPRI() == 0U && __get_PRIMASK() == 0U &&
           rtos_port_get_critical_nesting() == 0U;
}

#if HCB_PROTECTED && !HCB_RTOS_ONLY
static bool verify_newlib_lock(void)
{
    const uint32_t threshold = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY;
    const uint32_t stronger = threshold >> 1U & (0xFFU << (8U - __NVIC_PRIO_BITS));
    __malloc_lock(_REENT);
    bool is_valid = __get_BASEPRI() == threshold && __get_PRIMASK() == 0U;
    __malloc_lock(_REENT);
    is_valid = is_valid && __get_BASEPRI() == threshold && s_malloc_depth == 2U;
    __malloc_unlock(_REENT);
    is_valid = is_valid && __get_BASEPRI() == threshold && s_malloc_depth == 1U;
    __malloc_unlock(_REENT);
    is_valid = is_valid && is_interrupt_state_clear() && s_malloc_depth == 0U;

    __set_BASEPRI(stronger);
    __disable_irq();
    void *const address = malloc(16U);
    is_valid = is_valid && address != NULL && __get_BASEPRI() == stronger &&
               __get_PRIMASK() == 1U && s_malloc_depth == 0U;
    free(address);
    is_valid = is_valid && __get_BASEPRI() == stronger && __get_PRIMASK() == 1U &&
               s_malloc_depth == 0U;
    __set_BASEPRI(0U);
    __enable_irq();
    return is_valid && is_interrupt_state_clear();
}
#endif

static bool enable_counter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U) {
        return false;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const uint32_t first = DWT->CYCCNT;
    for (uint32_t retry = 0U; retry < 16U; ++retry) {
        if (DWT->CYCCNT != first) {
            return is_interrupt_state_clear();
        }
    }
    return false;
}

static void add_sample(hcb_samples_t *samples, uint32_t cycles, rtos_tick_t tick)
{
    if (samples == NULL) {
        return;
    }
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
    if (rtos_sched_get_tick_count() != tick) {
        ++samples->crossings;
    }
    ++samples->count;
    samples->sum += cycles;
}

static void *measure_alloc(hcb_backend_t backend, uint32_t size, hcb_samples_t *samples)
{
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    void *address;
    uint32_t cycles;
    if (backend == HCB_RTOS) {
        const uint32_t start = DWT->CYCCNT;
        atomic_signal_fence(memory_order_seq_cst);
        address = rtos_heap_alloc(size);
        atomic_signal_fence(memory_order_seq_cst);
        cycles = DWT->CYCCNT - start;
    } else {
        const uint32_t start = DWT->CYCCNT;
        atomic_signal_fence(memory_order_seq_cst);
        address = malloc((size_t)size);
        atomic_signal_fence(memory_order_seq_cst);
        cycles = DWT->CYCCNT - start;
    }
    add_sample(samples, cycles, tick);
    return address;
}

static void measure_free(hcb_backend_t backend, void *address, hcb_samples_t *samples)
{
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    uint32_t cycles;
    if (backend == HCB_RTOS) {
        const uint32_t start = DWT->CYCCNT;
        atomic_signal_fence(memory_order_seq_cst);
        rtos_heap_free(address);
        atomic_signal_fence(memory_order_seq_cst);
        cycles = DWT->CYCCNT - start;
    } else {
        const uint32_t start = DWT->CYCCNT;
        atomic_signal_fence(memory_order_seq_cst);
        free(address);
        atomic_signal_fence(memory_order_seq_cst);
        cycles = DWT->CYCCNT - start;
    }
    add_sample(samples, cycles, tick);
}

static bool check_slot(const hcb_slot_t *slot)
{
    return slot->address != NULL && slot->address[0] == slot->seed &&
           slot->address[slot->size / 2U] == (uint8_t)(slot->seed ^ 0x5AU) &&
           slot->address[slot->size - 1U] == (uint8_t)(slot->seed ^ 0xA5U);
}

static bool take_slot(hcb_backend_t backend, uint32_t index, uint32_t size, uint8_t seed,
                      hcb_result_t *result, bool is_measured)
{
    hcb_slot_t *const slot = &s_slots[index];
    if (slot->address != NULL) {
        return false;
    }
    hcb_samples_t *const samples = is_measured ? &result->allocations : NULL;
    uint8_t *const address = (uint8_t *)measure_alloc(backend, size, samples);
    if (address == NULL || ((uintptr_t)address & 7U) != 0U) {
        return false;
    }
    const uintptr_t start = (uintptr_t)address;
    for (uint32_t other = 0U; other < HCB_SLOT_COUNT; ++other) {
        if (s_slots[other].address == NULL) {
            continue;
        }
        const uintptr_t existing = (uintptr_t)s_slots[other].address;
        if ((start >= existing && start - existing < s_slots[other].size) ||
            (existing > start && existing - start < size)) {
            return false;
        }
    }
    *slot = (hcb_slot_t){address, size, seed};
    slot->address[0] = seed;
    slot->address[size / 2U] = (uint8_t)(seed ^ 0x5AU);
    slot->address[size - 1U] = (uint8_t)(seed ^ 0xA5U);
    result->live += size;
    if (result->live > result->peak) {
        result->peak = result->live;
    }
    return true;
}

static bool return_slot(hcb_backend_t backend, uint32_t index, hcb_result_t *result,
                        bool is_measured)
{
    hcb_slot_t *const slot = &s_slots[index];
    if (!check_slot(slot) || result->live < slot->size) {
        return false;
    }
    hcb_samples_t *const samples = is_measured ? &result->releases : NULL;
    measure_free(backend, slot->address, samples);
    result->live -= slot->size;
    *slot = (hcb_slot_t){0};
    return true;
}

static bool release_all(hcb_backend_t backend, hcb_result_t *result)
{
    bool is_valid = true;
    for (uint32_t remaining = HCB_SLOT_COUNT; remaining != 0U; --remaining) {
        const uint32_t index = remaining - 1U;
        if (s_slots[index].address != NULL && !return_slot(backend, index, result, false)) {
            is_valid = false;
        }
    }
    return is_valid && result->live == 0U;
}

static bool measure_reuse(hcb_backend_t backend, hcb_result_t *result)
{
    for (uint32_t index = 0U; index < HCB_WARMUP + HCB_SAMPLES; ++index) {
        const bool is_measured = index >= HCB_WARMUP;
        if (!take_slot(backend, 0U, 64U, (uint8_t)index, result, is_measured) ||
            !return_slot(backend, 0U, result, is_measured)) {
            return false;
        }
    }
    return result->peak == 64U;
}

static bool measure_mixed(hcb_backend_t backend, hcb_result_t *result)
{
    const uint32_t sizes[] = {24U, 33U, 64U, 95U, 128U, 255U, 384U, 512U};
    uint32_t expected_live = 0U;
    for (uint32_t index = 0U; index < HCB_MIXED_SLOTS; ++index) {
        const uint32_t size = sizes[index % 8U];
        if (!take_slot(backend, index, size, (uint8_t)index, result, false)) {
            return false;
        }
        expected_live += size;
    }
    for (uint32_t index = 0U; index < HCB_WARMUP + HCB_SAMPLES; ++index) {
        const bool is_measured = index >= HCB_WARMUP;
        const uint32_t slot = (index * 5U + 3U) % HCB_MIXED_SLOTS;
        const uint32_t size = sizes[(index * 7U + index / HCB_MIXED_SLOTS) % 8U];
        expected_live -= s_slots[slot].size;
        if (!return_slot(backend, slot, result, is_measured) ||
            !take_slot(backend, slot, size, (uint8_t)(index + 0x53U), result, is_measured)) {
            return false;
        }
        expected_live += size;
        if (result->live != expected_live) {
            return false;
        }
    }
    return true;
}

static bool prepare_fragments(hcb_backend_t backend, hcb_result_t *result)
{
    for (uint32_t index = 0U; index < HCB_FRAGMENT_BLOCKS; ++index) {
        if (!take_slot(backend, index, HCB_FRAGMENT_SIZE, (uint8_t)index, result, false)) {
            return false;
        }
        const hcb_slot_t block = s_slots[index];
        uint32_t position = index;
        while (position > 0U && (uintptr_t)s_slots[position - 1U].address >
                                (uintptr_t)block.address) {
            s_slots[position] = s_slots[position - 1U];
            --position;
        }
        s_slots[position] = block;
    }
    result->stride_min = UINT32_MAX;
    for (uint32_t index = 1U; index < HCB_FRAGMENT_BLOCKS; ++index) {
        const uintptr_t stride = (uintptr_t)s_slots[index].address -
                                 (uintptr_t)s_slots[index - 1U].address;
        if (stride < HCB_FRAGMENT_SIZE || stride >= HCB_FRAGMENT_PROBE / 2U) {
            return false;
        }
        if (stride < result->stride_min) {
            result->stride_min = (uint32_t)stride;
        }
        if (stride > result->stride_max) {
            result->stride_max = (uint32_t)stride;
        }
    }
    for (uint32_t index = 0U; index < HCB_FRAGMENT_BLOCKS; index += 2U) {
        if (!return_slot(backend, index, result, false)) {
            return false;
        }
    }
    return result->live == HCB_FRAGMENT_BLOCKS / 2U * HCB_FRAGMENT_SIZE;
}

static bool measure_fragments(hcb_backend_t backend, hcb_result_t *result)
{
    if (!prepare_fragments(backend, result)) {
        return false;
    }
    const uintptr_t first_guard = (uintptr_t)s_slots[1].address;
    const uintptr_t last_end = (uintptr_t)s_slots[HCB_FRAGMENT_BLOCKS - 1U].address +
                               HCB_FRAGMENT_SIZE;
    for (uint32_t index = 0U; index < HCB_WARMUP + HCB_SAMPLES; ++index) {
        const bool is_measured = index >= HCB_WARMUP;
        if (!take_slot(backend, HCB_FRAGMENT_BLOCKS, HCB_FRAGMENT_PROBE,
                       (uint8_t)index, result, is_measured)) {
            return false;
        }
        const uintptr_t probe = (uintptr_t)s_slots[HCB_FRAGMENT_BLOCKS].address;
        if (probe < last_end) {
            if (probe >= first_guard || first_guard - probe < HCB_FRAGMENT_PROBE) {
                return false;
            }
            if (is_measured) {
                ++result->probe_below;
            }
        }
        if (!return_slot(backend, HCB_FRAGMENT_BLOCKS, result, is_measured)) {
            return false;
        }
    }
    return result->live == HCB_FRAGMENT_BLOCKS / 2U * HCB_FRAGMENT_SIZE;
}

static bool check_samples(const hcb_samples_t *samples)
{
    return samples->count == HCB_SAMPLES && samples->invalid == 0U;
}

static bool run_backend(hcb_backend_t backend)
{
    bool (*const scenarios[])(hcb_backend_t, hcb_result_t *) = {
        measure_reuse, measure_mixed, measure_fragments
    };
    for (uint32_t index = 0U; index < sizeof(scenarios) / sizeof(scenarios[0]); ++index) {
        hcb_result_t *const result = &s_results[backend][index];
        const bool workload_passed = scenarios[index](backend, result);
        const bool cleanup_passed = release_all(backend, result);
        result->passed = workload_passed && cleanup_passed &&
                         check_samples(&result->allocations) && check_samples(&result->releases);
        if (!result->passed) {
            return false;
        }
    }
    return true;
}

static bool measure_miss(hcb_backend_t backend, uint32_t size, hcb_samples_t *samples)
{
    void *const address = measure_alloc(backend, size, samples);
    if (address != NULL) {
        measure_free(backend, address, NULL);
        return false;
    }
    return is_interrupt_state_clear() && samples->invalid == 0U;
}

static bool measure_largest(hcb_samples_t *samples)
{
    const rtos_tick_t tick = rtos_sched_get_tick_count();
    const uint32_t start = DWT->CYCCNT;
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t largest = rtos_heap_get_max_alloc();
    atomic_signal_fence(memory_order_seq_cst);
    const uint32_t cycles = DWT->CYCCNT - start;
    add_sample(samples, cycles, tick);
    return largest == 48U && samples->invalid == 0U;
}

static uint32_t get_hole_size(uint32_t index)
{
    return 24U + (index % 4U) * 8U;
}

static bool measure_failure(hcb_backend_t backend, uint32_t capacity)
{
    hcb_failure_t *const result = &s_failures[backend];
    if (backend == HCB_LIBC) {
        const uintptr_t current = (uintptr_t)__real__sbrk(0);
        if (current == UINTPTR_MAX || (current & 7U) != 0U) {
            return false;
        }
        s_libc_limit = current + capacity + HCB_HEADER_SIZE;
    }
    const uint32_t start_live = rtos_heap_get_alloc_count() - rtos_heap_get_free_count();
    uint32_t remaining = capacity;
    uint32_t free_bytes = 0U;
    for (uint32_t index = 0U; index < HCB_FAILURE_HOLES; ++index) {
        const uint32_t size = get_hole_size(index);
        s_holes[index] = measure_alloc(backend, size, NULL);
        s_guards[index] = measure_alloc(backend, HCB_GUARD_SIZE, NULL);
        if (s_holes[index] == NULL || s_guards[index] == NULL ||
            (uintptr_t)s_guards[index] != (uintptr_t)s_holes[index] + size + HCB_HEADER_SIZE ||
            (index != 0U &&
             (uintptr_t)s_holes[index] !=
                 (uintptr_t)s_guards[index - 1U] + HCB_GUARD_SIZE + HCB_HEADER_SIZE)) {
            return false;
        }
        s_guards[index][0] = (uint8_t)index;
        s_guards[index][HCB_GUARD_SIZE - 1U] = (uint8_t)(index ^ 0xA5U);
        remaining -= size + HCB_GUARD_SIZE + 2U * HCB_HEADER_SIZE;
        free_bytes += size;
    }
    uint8_t *const tail = measure_alloc(backend, remaining, NULL);
    if (tail == NULL ||
        (uintptr_t)tail !=
            (uintptr_t)s_guards[HCB_FAILURE_HOLES - 1U] + HCB_GUARD_SIZE + HCB_HEADER_SIZE ||
        (backend == HCB_LIBC && (uintptr_t)__real__sbrk(0) != s_libc_limit) ||
        !measure_miss(backend, 8U, &result->exhausted)) {
        return false;
    }
    tail[0] = 0x41U;
    tail[remaining - 1U] = 0x62U;
    for (uint32_t index = 0U; index < HCB_FAILURE_HOLES; ++index) {
        measure_free(backend, s_holes[index], NULL);
    }
    if ((backend == HCB_RTOS && rtos_heap_get_free() != free_bytes) ||
        !measure_miss(backend, 56U, &result->first) ||
        (backend == HCB_RTOS && !measure_largest(&result->query))) {
        return false;
    }
    for (uint32_t index = 0U; index < HCB_FAILURE_REPEATS; ++index) {
        if (!measure_miss(backend, 56U, &result->repeated) ||
            !measure_miss(backend, 80U, &result->larger)) {
            return false;
        }
    }
    if (backend == HCB_RTOS && !measure_largest(&result->repeated_query)) {
        return false;
    }
    void *const small = measure_alloc(backend, get_hole_size(0U), NULL);
    if (small != s_holes[0]) {
        return false;
    }
    measure_free(backend, small, NULL);
    for (uint32_t index = 0U; index < HCB_FAILURE_HOLES; ++index) {
        if (s_guards[index][0] != (uint8_t)index ||
            s_guards[index][HCB_GUARD_SIZE - 1U] != (uint8_t)(index ^ 0xA5U)) {
            return false;
        }
    }
    measure_free(backend, s_guards[0], NULL);
    void *const recovered = measure_alloc(backend, 80U, NULL);
    if (recovered != s_holes[0] || tail[0] != 0x41U || tail[remaining - 1U] != 0x62U) {
        return false;
    }
    measure_free(backend, tail, NULL);
    for (uint32_t index = HCB_FAILURE_HOLES - 1U; index != 0U; --index) {
        measure_free(backend, s_guards[index], NULL);
    }
    measure_free(backend, recovered, NULL);
    void *const whole = measure_alloc(backend, capacity, NULL);
    if (whole != s_holes[0] || (backend == HCB_RTOS && rtos_heap_get_free() != 0U)) {
        return false;
    }
    measure_free(backend, whole, NULL);
    if (backend == HCB_RTOS &&
        (rtos_heap_get_max_alloc() != capacity || rtos_heap_get_free() != capacity ||
         rtos_heap_get_alloc_count() - rtos_heap_get_free_count() != start_live)) {
        return false;
    }
    result->passed = true;
    return true;
}

static void print_samples(const char *backend, const char *name, const char *operation,
                          const hcb_samples_t *samples)
{
    (void)printf("HCB|STAT|backend=%s|case=%s|op=%s|n=%lu|sum=%lu|min=%lu|max=%lu"
                 "|tick_crossings=%lu|invalid=%lu\r\n", backend, name, operation,
                 (unsigned long)samples->count, (unsigned long)samples->sum,
                 (unsigned long)samples->minimum, (unsigned long)samples->maximum,
                 (unsigned long)samples->crossings, (unsigned long)samples->invalid);
}

static uint32_t print_results(void)
{
    const char *const backends[] = {"rtos", "libc"};
    const char *const names[] = {"reuse64", "mixed", "fragmented"};
    uint32_t failures = 0U;
    for (uint32_t backend = 0U; backend < HCB_BACKEND_COUNT; ++backend) {
        for (uint32_t index = 0U; index < 3U; ++index) {
            const hcb_result_t *const result = &s_results[backend][index];
            print_samples(backends[backend], names[index], "alloc", &result->allocations);
            print_samples(backends[backend], names[index], "free", &result->releases);
            (void)printf("HCB|CASE|backend=%s|case=%s|peak=%lu|live=%lu|%s\r\n",
                         backends[backend], names[index], (unsigned long)result->peak,
                         (unsigned long)result->live, result->passed ? "PASS" : "FAIL");
            if (!result->passed) {
                ++failures;
            }
        }
        const hcb_result_t *const fragments = &s_results[backend][2];
        (void)printf("HCB|LAYOUT|backend=%s|blocks=%u|stride_min=%lu|stride_max=%lu"
                     "|probe_below=%lu\r\n", backends[backend],
                     (unsigned int)HCB_FRAGMENT_BLOCKS, (unsigned long)fragments->stride_min,
                     (unsigned long)fragments->stride_max, (unsigned long)fragments->probe_below);
    }
    return failures;
}

static uint32_t print_failures(void)
{
    uint32_t failures = 0U;
    for (uint32_t backend = 0U; backend < HCB_BACKEND_COUNT; ++backend) {
        const char *const name = backend == HCB_RTOS ? "rtos" : "libc";
        const hcb_failure_t *const result = &s_failures[backend];
        print_samples(name, "failure", "exhausted", &result->exhausted);
        print_samples(name, "failure", "first_miss", &result->first);
        print_samples(name, "failure", "same_miss", &result->repeated);
        print_samples(name, "failure", "larger_miss", &result->larger);
        if (backend == HCB_RTOS) {
            print_samples(name, "failure", "max_after_miss", &result->query);
            print_samples(name, "failure", "max_after_query", &result->repeated_query);
        }
        (void)printf("HCB|CASE|backend=%s|case=failure|%s\r\n", name,
                     result->passed ? "PASS" : "FAIL");
        if (!result->passed) {
            ++failures;
        }
    }
    return failures;
}

/** @brief Run the F446 heap trace in its dedicated HIL task. */
void f446_heap_compare_run(void *argument)
{
    (void)argument;
    const uint32_t start_max = rtos_heap_get_max_alloc();
    const uint32_t start_free = rtos_heap_get_free();
    const uint32_t start_live = rtos_heap_get_alloc_count() - rtos_heap_get_free_count();
    const hcb_backend_t first = HCB_LIBC_FIRST && !HCB_RTOS_ONLY ? HCB_LIBC : HCB_RTOS;
    const hcb_backend_t second = first == HCB_RTOS ? HCB_LIBC : HCB_RTOS;
    (void)printf("HCB|START|protected=%u|failure_only=%u|libc_first=%u|samples=%u|warmup=%u\r\n",
                 (unsigned int)HCB_PROTECTED, (unsigned int)HCB_FAILURE_ONLY,
                 (unsigned int)(first == HCB_LIBC), (unsigned int)HCB_SAMPLES,
                 (unsigned int)HCB_WARMUP);
    const void *const libc_begin = __real__sbrk(0);
    const bool is_ready = enable_counter() && start_max >= 16384U && libc_begin != (void *)-1;
    if (HCB_FAILURE_ONLY && is_ready && measure_failure(first, start_max) && !HCB_RTOS_ONLY) {
        (void)measure_failure(second, start_max);
    }
    if (!HCB_FAILURE_ONLY && is_ready && run_backend(first) && !HCB_RTOS_ONLY) {
        (void)run_backend(second);
    }
    const void *const libc_end = __real__sbrk(0);
    s_libc_limit = 0U;
    bool is_lock_valid = true;
#if HCB_PROTECTED && !HCB_RTOS_ONLY
    is_lock_valid = verify_newlib_lock();
#endif
    (void)printf("HCB|MASK|checked=%u|valid=%u\r\n",
                 (unsigned int)(HCB_PROTECTED && !HCB_RTOS_ONLY), (unsigned int)is_lock_valid);
    const uint32_t end_max = rtos_heap_get_max_alloc();
    const uint32_t end_free = rtos_heap_get_free();
    const uint32_t end_live = rtos_heap_get_alloc_count() - rtos_heap_get_free_count();
    bool is_capacity_valid = end_max == start_max && end_live == start_live &&
                             libc_begin != (void *)-1 && libc_end != (void *)-1 &&
                             is_interrupt_state_clear();
    if (is_capacity_valid) {
        /* A full round trip rejoins any adjacent free spans left by right-only release. */
        void *whole = rtos_heap_alloc(start_max);
        is_capacity_valid = whole != NULL && rtos_heap_get_free() == 0U;
        rtos_heap_free(whole);
        is_capacity_valid = is_capacity_valid && rtos_heap_get_free() == start_free &&
                            rtos_heap_get_max_alloc() == start_max &&
                            rtos_heap_get_alloc_count() - rtos_heap_get_free_count() == start_live;
    }
    const uint32_t restored_free = rtos_heap_get_free();
    (void)printf("HCB|CAPACITY|rtos_start_max=%lu|rtos_end_max=%lu|rtos_start_free=%lu"
                 "|rtos_end_free=%lu|rtos_restored_free=%lu|rtos_live=%lu"
                 "|libc_begin=%p|libc_end=%p|valid=%u\r\n",
                 (unsigned long)start_max, (unsigned long)end_max, (unsigned long)start_free,
                 (unsigned long)end_free, (unsigned long)restored_free, (unsigned long)end_live,
                 libc_begin, libc_end, (unsigned int)is_capacity_valid);
    uint32_t failures = HCB_FAILURE_ONLY ? print_failures() : print_results();
    if (!is_capacity_valid || !is_lock_valid) {
        ++failures;
    }
    (void)printf("HCB|DONE|status=%u|checks=%u|failures=%lu\r\n", failures != 0U ? 1U : 0U,
                 (unsigned int)(HCB_BACKEND_COUNT * (HCB_FAILURE_ONLY ? 1U : 3U)),
                 (unsigned long)failures);
}

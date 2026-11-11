/**
 * @file h743_bench.c
 * @brief STM32H743 scheduler placement and private cache locality measurements.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32h743xx.h"

#include "rtos.h"
#include "rtos_sched.h"

enum {
    H743_CACHE_LINE_BYTES = 32U,
    H743_CACHE_WORD_COUNT = 64U,
    H743_CACHE_SAMPLE_COUNT = 128U,
    H743_CACHE_PACKED_MIDDLE = 3U,
    H743_CACHE_PACKED_END = 7U,
    H743_CACHE_SPREAD_MIDDLE = 35U,
    H743_CACHE_SPREAD_END = 40U,
    H743_CACHE_CASE_COUNT = 7U
};

typedef struct locality_case {
    const char *name;
    uint32_t *words;
    bool is_packed;
    bool is_cold;
} locality_case_t;

_Alignas(H743_CACHE_LINE_BYTES) static uint32_t axi_words[H743_CACHE_WORD_COUNT]
    __attribute__((section(".h743_axi_bench")));
_Alignas(H743_CACHE_LINE_BYTES) static uint32_t dtcm_words[H743_CACHE_WORD_COUNT]
    __attribute__((section(".h743_dtcm_bench")));
static uint32_t samples[H743_CACHE_SAMPLE_COUNT];

void h743_bench_run(void *argument);

/** Both layouts execute the same instructions and return the same eight-word checksum. */
static __attribute__((noinline)) uint32_t load_fields(const volatile uint32_t *first,
                                                      const volatile uint32_t *middle,
                                                      const volatile uint32_t *last)
{
    uint32_t checksum = first[0];
    checksum ^= first[1];
    checksum ^= first[2];
    checksum ^= middle[0];
    checksum ^= middle[1];
    checksum ^= middle[2];
    checksum ^= middle[3];
    return checksum ^ last[0];
}

static uint32_t initialize_fields(uint32_t *words)
{
    uint32_t checksum = 0U;
    for (uint32_t index = 0U; index < 8U; ++index) {
        const uint32_t value = 0x730ABC9DU ^ ((index + 1U) * 0x7613ABU);
        words[index] = value;
        checksum ^= value;
    }
    for (uint32_t index = 0U; index < 4U; ++index) {
        words[H743_CACHE_SPREAD_MIDDLE + index] = words[H743_CACHE_PACKED_MIDDLE + index];
    }
    words[H743_CACHE_SPREAD_END] = words[H743_CACHE_PACKED_END];
    return checksum;
}

static bool check_buffer(const uint32_t *words, uintptr_t start, uintptr_t end)
{
    const uintptr_t address = (uintptr_t)words;
    return (address >= start) && (address <= end - sizeof(axi_words)) &&
        ((address % H743_CACHE_LINE_BYTES) == 0U);
}

static uint32_t get_cache_descriptor(bool is_instruction)
{
    const uint32_t old_selector = SCB->CSSELR;
    SCB->CSSELR = is_instruction ? SCB_CSSELR_IND_Msk : 0U;
    __DSB();
    __ISB();
    const uint32_t descriptor = SCB->CCSIDR;
    SCB->CSSELR = old_selector;
    __DSB();
    __ISB();
    return descriptor;
}

static uint32_t get_line_bytes(uint32_t descriptor)
{
    const uint32_t encoding = (descriptor & SCB_CCSIDR_LINESIZE_Msk) >> SCB_CCSIDR_LINESIZE_Pos;
    return 1U << (encoding + 4U);
}

static uint32_t get_cache_bytes(uint32_t descriptor)
{
    const uint32_t sets = ((descriptor & SCB_CCSIDR_NUMSETS_Msk) >> SCB_CCSIDR_NUMSETS_Pos) + 1U;
    const uint32_t ways = ((descriptor & SCB_CCSIDR_ASSOCIATIVITY_Msk) >>
                           SCB_CCSIDR_ASSOCIATIVITY_Pos) + 1U;
    return get_line_bytes(descriptor) * sets * ways;
}

static void report_kernel_layout(void)
{
    static const size_t offsets[] = {
        offsetof(rtos_kernel_t, sched_state), offsetof(rtos_kernel_t, tick_count),
        offsetof(rtos_kernel_t, ready_bitmap), offsetof(rtos_kernel_t, delay_head),
        offsetof(rtos_kernel_t, current_tcb), offsetof(rtos_kernel_t, next_tcb),
        offsetof(rtos_kernel_t, switch_count), offsetof(rtos_kernel_t, idle_tcb)
    };
    const size_t field_count = sizeof(offsets) / sizeof(offsets[0]);
    const uintptr_t base = (uintptr_t)&rtos_kernel;
    uintptr_t lines[sizeof(offsets) / sizeof(offsets[0])];
    size_t line_count = 0U;

    for (size_t field = 0U; field < field_count; ++field) {
        const uintptr_t line = (base + offsets[field]) / H743_CACHE_LINE_BYTES;
        size_t index = 0U;
        while ((index < line_count) && (lines[index] != line)) {
            ++index;
        }
        if (index == line_count) {
            lines[line_count] = line;
            ++line_count;
        }
    }
    (void)printf("CACHE|LAYOUT|kernel=%08lx|kernel_bytes=%lu|common_lines=%lu|current_offset=%lu|"
                 "ready_offset=%lu|axi=%08lx|dtcm=%08lx\r\n",
                 (unsigned long)base, (unsigned long)sizeof(rtos_kernel),
                 (unsigned long)line_count, (unsigned long)offsetof(rtos_kernel_t, current_tcb),
                 (unsigned long)offsetof(rtos_kernel_t, ready_heads),
                 (unsigned long)(uintptr_t)axi_words, (unsigned long)(uintptr_t)dtcm_words);
}

static void report_samples(const char *name, bool is_valid, bool invalidated)
{
    if (!is_valid) {
        (void)printf("CACHE|CASE|%s|FAIL\r\n", name);
        return;
    }

    uint64_t total = 0U;
    for (size_t index = 0U; index < H743_CACHE_SAMPLE_COUNT; ++index) {
        const uint32_t elapsed = samples[index];
        size_t position = index;
        total += elapsed;
        while ((position > 0U) && (samples[position - 1U] > elapsed)) {
            samples[position] = samples[position - 1U];
            --position;
        }
        samples[position] = elapsed;
    }
    const size_t p95 = (((H743_CACHE_SAMPLE_COUNT * 95U) + 99U) / 100U) - 1U;
    (void)printf("CACHE|CASE|%s|PASS|n=%u|invalidated=%u|avg=%lu|min=%lu|p50=%lu|p95=%lu|"
                 "max=%lu\r\n", name, (unsigned int)H743_CACHE_SAMPLE_COUNT,
                 (unsigned int)invalidated, (unsigned long)(total / H743_CACHE_SAMPLE_COUNT),
                 (unsigned long)samples[0],
                 (unsigned long)samples[(H743_CACHE_SAMPLE_COUNT / 2U) - 1U],
                 (unsigned long)samples[p95],
                 (unsigned long)samples[H743_CACHE_SAMPLE_COUNT - 1U]);
}

static bool measure_locality(const locality_case_t *test, uint32_t expected, bool is_dcache_on)
{
    const bool invalidate = test->is_cold && is_dcache_on;
    const volatile uint32_t *const first = test->words;
    const volatile uint32_t *const middle = first +
        (test->is_packed ? H743_CACHE_PACKED_MIDDLE : H743_CACHE_SPREAD_MIDDLE);
    const volatile uint32_t *const last = first +
        (test->is_packed ? H743_CACHE_PACKED_END : H743_CACHE_SPREAD_END);
    bool is_valid = true;

    for (size_t index = 0U; index < H743_CACHE_SAMPLE_COUNT; ++index) {
        const uint32_t mask = __get_PRIMASK();
        __disable_irq();
        if (invalidate) {
            /* Only the private AXI range is invalidated. It was cleaned after initialization. */
            SCB_InvalidateDCache_by_Addr(test->words, (int32_t)sizeof(axi_words));
        } else if (!test->is_cold) {
            (void)load_fields(first, middle, last);
        }
        __DSB();
        __ISB();
        const uint32_t start = DWT->CYCCNT;
        const uint32_t checksum = load_fields(first, middle, last);
        __DSB();
        const uint32_t elapsed = DWT->CYCCNT - start;
        __set_PRIMASK(mask);
        samples[index] = elapsed;
        is_valid = is_valid && (elapsed != 0U) && (checksum == expected);
    }
    report_samples(test->name, is_valid, invalidate);
    return is_valid;
}

static bool measure_scheduler(void)
{
    rtos_tcb_t *const task = rtos_task_get_current();
    bool is_valid = (task != NULL);

    for (size_t index = 0U; index < H743_CACHE_SAMPLE_COUNT; ++index) {
        RTOS_PORT_ENTER_CRITICAL();
        const uint32_t mask = __get_PRIMASK();
        __disable_irq();
        /* Prime the exact same scheduler path before measuring it. */
        rtos_sched_schedule();
        __DSB();
        __ISB();
        const uint32_t start = DWT->CYCCNT;
        rtos_sched_schedule();
        __DSB();
        const uint32_t elapsed = DWT->CYCCNT - start;
        is_valid = is_valid && (elapsed != 0U) && (rtos_kernel.current_tcb == task) &&
            (rtos_kernel.next_tcb == NULL) && (task->state == RTOS_TASK_RUNNING);
        __set_PRIMASK(mask);
        RTOS_PORT_EXIT_CRITICAL();
        samples[index] = elapsed;
    }
    report_samples("kernel_no_switch_warm", is_valid, false);
    return is_valid;
}

/** @brief Run isolated AXI/DTCM loads and the actual scheduler's no-switch path. */
void h743_bench_run(void *argument)
{
    (void)argument;
    static const locality_case_t cases[] = {
        {"axi_packed_warm", axi_words, true, false},
        {"axi_spread_warm", axi_words, false, false},
        {"axi_packed_cold", axi_words, true, true},
        {"axi_spread_cold", axi_words, false, true},
        {"dtcm_packed", dtcm_words, true, false},
        {"dtcm_spread", dtcm_words, false, false}
    };
    const uint32_t icache = get_cache_descriptor(true);
    const uint32_t dcache = get_cache_descriptor(false);
    const uint32_t ccr = SCB->CCR;
    const bool is_dcache_on = (ccr & SCB_CCR_DC_Msk) != 0U;
    const bool is_valid = (get_line_bytes(icache) == H743_CACHE_LINE_BYTES) &&
        (get_line_bytes(dcache) == H743_CACHE_LINE_BYTES) &&
        ((DWT->CTRL & (DWT_CTRL_NOCYCCNT_Msk | DWT_CTRL_CYCCNTENA_Msk)) ==
         DWT_CTRL_CYCCNTENA_Msk) &&
        check_buffer(axi_words, 0x24000000UL, 0x24080000UL) &&
        check_buffer(dtcm_words, 0x20000000UL, 0x20020000UL);
    (void)printf("CACHE|GEOM|icache=%u|dcache=%u|line=%lu|i_bytes=%lu|d_bytes=%lu|"
                 "ccr=%08lx|dwt=%08lx\r\n", (unsigned int)((ccr & SCB_CCR_IC_Msk) != 0U),
                 (unsigned int)is_dcache_on, (unsigned long)get_line_bytes(dcache),
                 (unsigned long)get_cache_bytes(icache), (unsigned long)get_cache_bytes(dcache),
                 (unsigned long)ccr, (unsigned long)DWT->CTRL);
    report_kernel_layout();

    uint32_t passed = 0U;
    if (is_valid) {
        const uint32_t axi_checksum = initialize_fields(axi_words);
        const uint32_t dtcm_checksum = initialize_fields(dtcm_words);
        if (is_dcache_on) {
            SCB_CleanDCache_by_Addr(axi_words, (int32_t)sizeof(axi_words));
        }
        for (size_t index = 0U; index < (sizeof(cases) / sizeof(cases[0])); ++index) {
            const locality_case_t *const test = &cases[index];
            const uint32_t expected = (test->words == axi_words) ? axi_checksum : dtcm_checksum;
            passed += (uint32_t)measure_locality(test, expected, is_dcache_on);
        }
        passed += (uint32_t)measure_scheduler();
    } else {
        (void)printf("CACHE|SETUP|FAIL\r\n");
    }
    (void)printf("CACHE|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)passed,
                 (unsigned long)(H743_CACHE_CASE_COUNT - passed));
    for (;;) {
        rtos_task_delay(1000U);
    }
}

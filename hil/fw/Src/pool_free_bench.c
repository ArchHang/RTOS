/**
 * @file pool_free_bench.c
 * @brief Measure and validate fixed block releases at known free list depths.
 */
#include "audit_regressions.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "main.h"
#include "rtos.h"

enum {
    POOL_BLOCK_COUNT = 32U,
    POOL_SMALL_BLOCK_SIZE = 36U,
    POOL_LARGE_BLOCK_SIZE = 64U,
    POOL_WARMUP_COUNT = 16U,
    POOL_SAMPLE_COUNT = 2048U,
};

typedef struct pool_release_stats {
    uint32_t sum;
    uint32_t minimum;
    uint32_t maximum;
} pool_release_stats_t;

static rtos_mempool_t bench_pool;
static _Alignas(void *) uint8_t bench_storage[POOL_BLOCK_COUNT * POOL_LARGE_BLOCK_SIZE
                                             + 2U * sizeof(void *)];

static uint8_t *get_pool_base(void)
{
    return &bench_storage[sizeof(void *)];
}

static bool initialize_pool(uint32_t block_size)
{
    return (rtos_mempool_init(&bench_pool, get_pool_base(), block_size,
                             POOL_BLOCK_COUNT) == RTOS_OK)
           && (bench_pool.block_size == block_size)
           && (rtos_mempool_get_free(&bench_pool) == POOL_BLOCK_COUNT);
}

static bool check_full_recovery(uint32_t block_size)
{
    void *blocks[POOL_BLOCK_COUNT];
    const uintptr_t base = (uintptr_t)get_pool_base();
    uint32_t seen = 0U;

    for (uint32_t index = 0U; index < POOL_BLOCK_COUNT; index++) {
        blocks[index] = rtos_mempool_alloc(&bench_pool);
        const uintptr_t address = (uintptr_t)blocks[index];
        if ((address < base) || ((address - base) >= (block_size * POOL_BLOCK_COUNT))
            || (((address - base) % block_size) != 0U)) {
            return false;
        }
        const uint32_t slot = (uint32_t)((address - base) / block_size);
        const uint32_t bit = UINT32_C(1) << slot;
        if ((seen & bit) != 0U) {
            return false;
        }
        seen |= bit;
    }
    if ((seen != UINT32_MAX) || (rtos_mempool_get_free(&bench_pool) != 0U)
        || (rtos_mempool_alloc(&bench_pool) != NULL)) {
        return false;
    }
    for (uint32_t index = 0U; index < POOL_BLOCK_COUNT; index++) {
        if (rtos_mempool_free(&bench_pool, blocks[index]) != RTOS_OK) {
            return false;
        }
    }
    return rtos_mempool_get_free(&bench_pool) == POOL_BLOCK_COUNT;
}

static bool check_rejections(uint32_t block_size)
{
    if (!initialize_pool(block_size)) {
        return false;
    }
    uint8_t *first = (uint8_t *)rtos_mempool_alloc(&bench_pool);
    void *second = rtos_mempool_alloc(&bench_pool);
    if ((first == NULL) || (second == NULL)) {
        return false;
    }
    uint8_t *base = get_pool_base();
    rtos_mempool_t invalid = bench_pool;
    invalid.block_size = UINT32_C(0x80000000);
    invalid.block_count = 2U;
    if ((rtos_mempool_free(NULL, first) != RTOS_ERR_NULL)
        || (rtos_mempool_free(&bench_pool, NULL) != RTOS_ERR_NULL)
        || (rtos_mempool_free(&bench_pool, &base[-1]) != RTOS_ERR_PARAM)
        || (rtos_mempool_free(&bench_pool, &base[block_size * POOL_BLOCK_COUNT])
            != RTOS_ERR_PARAM)
        || (rtos_mempool_free(&bench_pool, &first[1]) != RTOS_ERR_PARAM)
        || (rtos_mempool_free(&bench_pool, &first[sizeof(void *)]) != RTOS_ERR_PARAM)
        || (rtos_mempool_free(&invalid, first) != RTOS_ERR_PARAM)
        || (rtos_mempool_get_free(&bench_pool) != (POOL_BLOCK_COUNT - 2U))) {
        return false;
    }
    invalid.block_count = 0U;
    if (rtos_mempool_free(&invalid, first) != RTOS_ERR_PARAM) {
        return false;
    }
    invalid.block_count = POOL_BLOCK_COUNT;
    invalid.block_size = 0U;
    return (rtos_mempool_free(&invalid, first) == RTOS_ERR_PARAM)
           && (rtos_mempool_free(&bench_pool, first) == RTOS_OK)
           && (rtos_mempool_free(&bench_pool, first) == RTOS_ERR_ALREADY)
           && (rtos_mempool_get_free(&bench_pool) == (POOL_BLOCK_COUNT - 1U))
           && (rtos_mempool_free(&bench_pool, second) == RTOS_OK)
           && check_full_recovery(block_size);
}

static bool sample_release(void *target, uint32_t depth, pool_release_stats_t *stats)
{
    for (uint32_t index = 0U; index < (POOL_WARMUP_COUNT + POOL_SAMPLE_COUNT); index++) {
        const uint32_t started = DWT->CYCCNT;
        const rtos_status_t status = rtos_mempool_free(&bench_pool, target);
        const uint32_t elapsed = DWT->CYCCNT - started;
        if ((status != RTOS_OK) || (rtos_mempool_get_free(&bench_pool) != (depth + 1U))
            || (rtos_mempool_alloc(&bench_pool) != target)
            || (rtos_mempool_get_free(&bench_pool) != depth)) {
            return false;
        }
        if (index < POOL_WARMUP_COUNT) {
            continue;
        }
        if ((elapsed == 0U) || (elapsed > UINT32_MAX - stats->sum)) {
            return false;
        }
        stats->sum += elapsed;
        if (elapsed < stats->minimum) {
            stats->minimum = elapsed;
        }
        if (elapsed > stats->maximum) {
            stats->maximum = elapsed;
        }
    }
    return true;
}

static bool measure_pool(uint32_t block_size, uint32_t depth)
{
    if (!initialize_pool(block_size)) {
        return false;
    }
    void *held[POOL_BLOCK_COUNT];
    const uint32_t held_count = POOL_BLOCK_COUNT - depth;
    for (uint32_t index = 0U; index < held_count; index++) {
        held[index] = rtos_mempool_alloc(&bench_pool);
        if (held[index] == NULL) {
            return false;
        }
    }
    pool_release_stats_t stats = {.minimum = UINT32_MAX};
    if ((rtos_mempool_get_free(&bench_pool) != depth) || !sample_release(held[0], depth, &stats)) {
        return false;
    }
    for (uint32_t index = 0U; index < held_count; index++) {
        if (rtos_mempool_free(&bench_pool, held[index]) != RTOS_OK) {
            return false;
        }
    }
    if (!check_full_recovery(block_size)) {
        return false;
    }
    /* Serial capture independently verifies that all expected records arrived. */
    (void)printf("PFB|SAMPLE|size=%lu|depth=%lu|n=%u|sum=%lu|min=%lu|max=%lu\r\n",
                 (unsigned long)block_size, (unsigned long)depth, (unsigned int)POOL_SAMPLE_COUNT,
                 (unsigned long)stats.sum, (unsigned long)stats.minimum,
                 (unsigned long)stats.maximum);
    return true;
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    const uint32_t sizes[] = {POOL_SMALL_BLOCK_SIZE, POOL_LARGE_BLOCK_SIZE};
    const uint32_t depths[] = {0U, POOL_BLOCK_COUNT - 1U};
    uint32_t passed = 0U;
    uint32_t failed = 0U;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    (void)printf("PFB|META|clock_hz=%lu\r\n", (unsigned long)SystemCoreClock);
    for (uint32_t size_index = 0U; size_index < (sizeof(sizes) / sizeof(sizes[0])); size_index++) {
        const uint32_t block_size = sizes[size_index];
        if (check_rejections(block_size)) {
            passed++;
            (void)printf("PFB|REJECT|PASS|size=%lu\r\n", (unsigned long)block_size);
        } else {
            failed++;
            (void)printf("PFB|REJECT|FAIL|size=%lu\r\n", (unsigned long)block_size);
        }
        for (uint32_t index = 0U; index < (sizeof(depths) / sizeof(depths[0])); index++) {
            const uint32_t depth = depths[index];
            if (measure_pool(block_size, depth)) {
                passed++;
            } else {
                failed++;
                (void)printf("PFB|SAMPLE|FAIL|size=%lu|depth=%lu\r\n",
                             (unsigned long)block_size, (unsigned long)depth);
            }
        }
    }
    (void)printf("PFB|DONE|pass=%lu|fail=%lu\r\n", (unsigned long)passed, (unsigned long)failed);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

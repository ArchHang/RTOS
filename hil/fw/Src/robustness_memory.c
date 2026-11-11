/**
 * @file robustness_memory.c
 * @brief Exercise heap recovery and fixed-block pool alignment.
 *
 * Run before other tasks use the heap so live allocations cannot change the test layout.
 * The same fixture runs on target and in a single-threaded host test.
 */
#include "robustness_memory.h"
#include <stdint.h>
#include <string.h>
#include "robustness.h"
#include "rtos_heap.h"
#include "rtos_mem.h"

enum {
    HEAP_PROBE_BYTES = 64U,
    HEAP_JOINED_REQUEST = 120U,
    HEAP_EXCESS_REQUEST = 160U,
    POOL_BLOCK_BYTES = 9U,
    POOL_BLOCK_COUNT = 3U,
};

static rtos_mempool_t memory_probe_pool;
static _Alignas(void *) uint8_t memory_probe_pool_storage[64];

static void check_heap_free_pointer(void)
{
    uint8_t *payload = rtos_heap_alloc(HEAP_PROBE_BYTES);
    robustness_check("memory.heap_free_setup", payload != NULL);
    if (payload == NULL) {
        return;
    }

    memset(payload, 0x5A, HEAP_PROBE_BYTES);
    uint32_t free_before = rtos_heap_get_free();
    uint32_t releases_before = rtos_heap_get_free_count();
    uint32_t outside = 0U;
    rtos_heap_free(&outside);
    rtos_heap_free(payload + 1U);
    robustness_check("memory.heap_invalid_free_unchanged",
                     rtos_heap_get_free() == free_before &&
                     rtos_heap_get_free_count() == releases_before && payload[0] == 0x5AU);

    rtos_heap_free(payload);
    uint32_t free_after = rtos_heap_get_free();
    rtos_heap_free(payload);
    robustness_check("memory.heap_double_free_unchanged",
                     rtos_heap_get_free() == free_after &&
                     rtos_heap_get_free_count() == releases_before + 1U);
}

static void check_heap_adjacent_free_runs(void)
{
    uint32_t initial_max = rtos_heap_get_max_alloc();
    void *first = rtos_heap_alloc(HEAP_PROBE_BYTES);
    void *second = rtos_heap_alloc(HEAP_PROBE_BYTES);
    uint8_t *separator = rtos_heap_alloc(HEAP_PROBE_BYTES);
    uint32_t tail_size = rtos_heap_get_max_alloc();
    uint8_t *tail = rtos_heap_alloc(tail_size);
    bool is_ready = first != NULL && second != NULL && separator != NULL &&
                    tail != NULL && tail_size > HEAP_EXCESS_REQUEST &&
                    rtos_heap_get_max_alloc() == 0U;
    robustness_check("memory.heap_adjacent_setup", is_ready);
    if (!is_ready) {
        rtos_heap_free(first);
        rtos_heap_free(second);
        rtos_heap_free(separator);
        rtos_heap_free(tail);
        return;
    }

    separator[0] = 0xA5U;
    separator[HEAP_PROBE_BYTES - 1U] = 0x6BU;
    tail[0] = 0x31U;
    tail[tail_size - 1U] = 0x92U;
    rtos_heap_free(first);
    rtos_heap_free(second);

    robustness_check("memory.heap_adjacent_capacity",
                     rtos_heap_get_max_alloc() >= HEAP_JOINED_REQUEST);
    void *excess = rtos_heap_alloc(HEAP_EXCESS_REQUEST);
    robustness_check("memory.heap_live_separator_blocks_merge", excess == NULL);
    rtos_heap_free(excess);
    void *joined = rtos_heap_alloc(HEAP_JOINED_REQUEST);
    robustness_check("memory.heap_adjacent_allocation", joined == first);
    robustness_check("memory.heap_adjacent_preserves_live_payload",
                     separator[0] == 0xA5U && separator[HEAP_PROBE_BYTES - 1U] == 0x6BU &&
                     tail[0] == 0x31U && tail[tail_size - 1U] == 0x92U);

    rtos_heap_free(joined);
    rtos_heap_free(separator);
    rtos_heap_free(tail);
    robustness_check("memory.heap_full_capacity_recovered",
                     rtos_heap_get_max_alloc() == initial_max);
    void *full = rtos_heap_alloc(initial_max);
    robustness_check("memory.heap_full_size_reallocation", full != NULL);
    rtos_heap_free(full);
}

static void check_pool_alignment(void)
{
    const uint32_t block_align = (uint32_t)_Alignof(void *);
    rtos_status_t status = rtos_mempool_init(&memory_probe_pool, memory_probe_pool_storage,
                                            POOL_BLOCK_BYTES, POOL_BLOCK_COUNT);
    robustness_check("memory.pool_odd_size_init", status == RTOS_OK);
    if (status != RTOS_OK) {
        return;
    }

    void *blocks[POOL_BLOCK_COUNT];
    bool are_aligned = true;
    for (uint32_t index = 0U; index < POOL_BLOCK_COUNT; index++) {
        blocks[index] = rtos_mempool_alloc(&memory_probe_pool);
        if ((blocks[index] == NULL) || (((uintptr_t)blocks[index] % block_align) != 0U)) {
            are_aligned = false;
        }
    }
    robustness_check("memory.pool_every_block_aligned", are_aligned);
    robustness_check("memory.pool_odd_size_exhausted",
                     rtos_mempool_alloc(&memory_probe_pool) == NULL);

    bool are_restored = true;
    for (uint32_t index = 0U; index < POOL_BLOCK_COUNT; index++) {
        if (rtos_mempool_free(&memory_probe_pool, blocks[index]) != RTOS_OK) {
            are_restored = false;
        }
    }
    robustness_check("memory.pool_odd_size_restored",
                     are_restored && rtos_mempool_get_free(&memory_probe_pool) == POOL_BLOCK_COUNT);
}

void robustness_memory_run(void)
{
    check_heap_free_pointer();
    check_heap_adjacent_free_runs();
    check_pool_alignment();
}

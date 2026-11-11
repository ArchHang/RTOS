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
    HEAP_MIN_TAIL = 24U,
    HEAP_SHORT_TAIL = 16U,
    POOL_BOUNDARY_BLOCKS = 2U,
    POOL_BLOCK_BYTES = 9U,
    POOL_BLOCK_COUNT = 3U,
};

static rtos_mempool_t memory_probe_pool;
static _Alignas(void *) uint8_t memory_probe_pool_storage[64];

static struct pool_boundary_storage {
    void *guard;
    rtos_mempool_t control;
    _Alignas(void *) uint8_t bytes[POOL_BOUNDARY_BLOCKS * sizeof(void *)];
} s_pool_boundary;

static void check_heap_capacity(void)
{
    const uint32_t capacity = rtos_heap_get_max_alloc();
    const uint32_t free_before = rtos_heap_get_free();
    const uint32_t alloc_before = rtos_heap_get_alloc_count();
    const uint32_t release_before = rtos_heap_get_free_count();
    void *const zero = rtos_heap_alloc(0U);
    void *const overflow = rtos_heap_alloc(UINT32_MAX);
    void *const above_capacity = rtos_heap_alloc(capacity + 1U);
    rtos_heap_free(NULL);
    robustness_check("memory.heap_rejections_preserve_statistics",
                     zero == NULL && overflow == NULL && above_capacity == NULL &&
                     rtos_heap_get_free() == free_before && rtos_heap_get_max_alloc() == capacity &&
                     rtos_heap_get_alloc_count() == alloc_before &&
                     rtos_heap_get_free_count() == release_before);
    rtos_heap_free(zero);
    rtos_heap_free(overflow);
    rtos_heap_free(above_capacity);
    if (capacity <= HEAP_MIN_TAIL) {
        robustness_check("memory.heap_tail_fixture_capacity", false);
        return;
    }

    void *const whole = rtos_heap_alloc(capacity - HEAP_SHORT_TAIL);
    robustness_check("memory.heap_16_byte_tail_is_absorbed",
                     whole != NULL && rtos_heap_get_max_alloc() == 0U);
    rtos_heap_free(whole);

    void *const front = rtos_heap_alloc(capacity - HEAP_MIN_TAIL);
    const uint32_t tail_capacity = rtos_heap_get_max_alloc();
    void *const tail = rtos_heap_alloc(tail_capacity);
    robustness_check("memory.heap_24_byte_tail_can_be_reused",
                     front != NULL && tail != NULL && tail_capacity == HEAP_SHORT_TAIL &&
                     rtos_heap_get_max_alloc() == 0U);
    rtos_heap_free(tail);
    rtos_heap_free(front);
    robustness_check("memory.heap_exact_tail_accounting",
                     rtos_heap_get_max_alloc() == capacity &&
                     rtos_heap_get_alloc_count() - alloc_before == 3U &&
                     rtos_heap_get_free_count() - release_before == 3U);
}

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

static void check_pool_rejections(void)
{
    rtos_mempool_t *const pool = &s_pool_boundary.control;
    uint8_t *const storage = s_pool_boundary.bytes;
    uint8_t *const raw = (uint8_t *)(void *)&s_pool_boundary;
    uint8_t before[sizeof(s_pool_boundary)];
    const uint32_t node_size = (uint32_t)sizeof(void *);
    const uintptr_t last_aligned = UINTPTR_MAX & ~((uintptr_t)_Alignof(void *) - 1U);

    memset(&s_pool_boundary, 0x5A, sizeof(s_pool_boundary));
    memcpy(before, &s_pool_boundary, sizeof(before));
    const rtos_status_t results[] = {
        rtos_mempool_init(NULL, storage, 1U, 1U),
        rtos_mempool_init(pool, NULL, 1U, 1U),
        rtos_mempool_init(pool, storage, 0U, 1U),
        rtos_mempool_init(pool, storage, 1U, 0U),
        rtos_mempool_init(pool, storage + 1U, 1U, 1U),
        rtos_mempool_init(pool, storage, UINT32_MAX, 1U),
        rtos_mempool_init(pool, storage, node_size, UINT32_MAX),
        rtos_mempool_init(pool, (void *)last_aligned, node_size, 1U),
        rtos_mempool_init(pool, raw, node_size, 2U),
        rtos_mempool_init(pool, pool, node_size, 1U),
        rtos_mempool_init(pool, (uint8_t *)(void *)pool + node_size, node_size, 1U),
    };
    bool all_rejected = true;
    for (size_t index = 0U; index < sizeof(results) / sizeof(results[0]); ++index) {
        all_rejected = (results[index] == RTOS_ERR_PARAM) && all_rejected;
    }
    robustness_check("memory.pool_init_rejects_invalid_sizes_addresses_and_overlap",
                     all_rejected && memcmp(before, &s_pool_boundary, sizeof(before)) == 0);

    const rtos_status_t initialized = rtos_mempool_init(pool, storage, 1U, POOL_BOUNDARY_BLOCKS);
    robustness_check("memory.pool_adjacent_storage_and_minimum_stride",
                     initialized == RTOS_OK && pool->block_size == node_size &&
                     rtos_mempool_get_free(pool) == POOL_BOUNDARY_BLOCKS);
    if (initialized != RTOS_OK) {
        return;
    }
    uint8_t *const last = rtos_mempool_alloc(pool);
    void *const first = rtos_mempool_alloc(pool);
    const bool exhausted = rtos_mempool_alloc(pool) == NULL;
    robustness_check("memory.pool_exact_capacity",
                     first == storage && last == storage + node_size && exhausted &&
                     rtos_mempool_get_free(pool) == 0U);
    if ((first == NULL) || (last == NULL)) {
        return;
    }
    last[0] = 0x3CU;
    const rtos_status_t null_pool = rtos_mempool_free(NULL, last);
    const rtos_status_t null_block = rtos_mempool_free(pool, NULL);
    const rtos_status_t interior = rtos_mempool_free(pool, last + 1U);
    const rtos_status_t outside = rtos_mempool_free(pool, storage + sizeof(s_pool_boundary.bytes));
    robustness_check("memory.pool_free_rejections_preserve_live_block",
                     null_pool == RTOS_ERR_NULL && null_block == RTOS_ERR_NULL &&
                     interior == RTOS_ERR_PARAM && outside == RTOS_ERR_PARAM &&
                     rtos_mempool_get_free(pool) == 0U && last[0] == 0x3CU);
    const rtos_status_t last_freed = rtos_mempool_free(pool, last);
    const rtos_status_t duplicate = rtos_mempool_free(pool, last);
    const rtos_status_t first_freed = rtos_mempool_free(pool, first);
    robustness_check("memory.pool_double_free_and_null_observers",
                     last_freed == RTOS_OK && duplicate == RTOS_ERR_ALREADY &&
                     first_freed == RTOS_OK && rtos_mempool_get_free(pool) == POOL_BOUNDARY_BLOCKS &&
                     rtos_mempool_get_free(NULL) == 0U && rtos_mempool_alloc(NULL) == NULL);
}

void robustness_memory_run(void)
{
    check_heap_capacity();
    check_heap_free_pointer();
    check_heap_adjacent_free_runs();
    check_pool_alignment();
    check_pool_rejections();
}

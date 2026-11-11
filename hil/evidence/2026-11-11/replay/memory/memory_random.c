#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtos_heap.h"
#include "rtos_perf.h"
#include "rtos_task.h"

#define SLOT_COUNT 96U
#define EPOCHS 8U
#define OPERATIONS_PER_EPOCH 25000U
#define AUDIT_INTERVAL 257U
#define HEAP_ALIGNMENT 8U
#define POOL_MAX_BLOCK_BYTES 160U
#define POOL_CANARY 0xB7U

typedef struct live_block {
    uint8_t *data;
    uint32_t size;
    uint32_t tag;
} live_block_t;

typedef struct test_counts {
    uint32_t alloc;
    uint32_t free;
    uint32_t exhausted;
    uint32_t complete_reuse;
    uint32_t rejection;
    uint32_t peak_live;
    uint64_t checked_bytes;
} test_counts_t;

typedef struct pool_case {
    rtos_mempool_t pool;
    uint8_t *base;
    uint32_t capacity;
    uint32_t stride;
    uint32_t live;
    live_block_t blocks[SLOT_COUNT];
} pool_case_t;

static const char *family;
static uint32_t seed;
static uint32_t operation;
static uint32_t random_state;
static test_counts_t counts;
static _Alignas(void *) uint8_t pool_storage[SLOT_COUNT * POOL_MAX_BLOCK_BYTES + 16U];

void rtos_perf_register_pool(rtos_mempool_t *pool)
{
    (void)pool;
}

void rtos_assert_fail(const char *condition, const char *file, int line)
{
    fprintf(stderr, "RTOS_ASSERT %s:%d %s\n", file, line, condition);
    abort();
}

static void check(bool is_valid, const char *reason)
{
    if (!is_valid) {
        fprintf(stderr, "FAIL family=%s seed=0x%08lx operation=%lu reason=%s\n",
                family, (unsigned long)seed, (unsigned long)operation, reason);
        exit(1);
    }
}

static uint32_t next_random(void)
{
    uint32_t value = random_state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    random_state = value;
    return value;
}

static uint32_t align_size(uint32_t size, uint32_t alignment)
{
    return (size + alignment - 1U) & ~(alignment - 1U);
}

static uint8_t byte_pattern(uint32_t tag, uint32_t offset)
{
    const uint32_t value = tag ^ (offset * UINT32_C(0x9E3779B9));
    return (uint8_t)(value ^ (value >> 7U) ^ (value >> 15U) ^ (value >> 24U));
}

static void fill_block(live_block_t *block, uint8_t *data, uint32_t size)
{
    block->data = data;
    block->size = size;
    block->tag = next_random();
    for (uint32_t i = 0U; i < size; ++i) {
        data[i] = byte_pattern(block->tag, i);
    }
}

static void check_block(const live_block_t *block)
{
    if (block->data == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < block->size; ++i) {
        check(block->data[i] == byte_pattern(block->tag, i), "payload sentinel changed");
    }
    counts.checked_bytes += block->size;
}

static void audit_blocks(const live_block_t *blocks, uint32_t size)
{
    for (uint32_t i = 0U; i < size; ++i) {
        check_block(&blocks[i]);
    }
}

static void record_alloc(uint32_t live)
{
    ++counts.alloc;
    if (live > counts.peak_live) {
        counts.peak_live = live;
    }
}

static void begin_case(const char *name, uint32_t case_seed)
{
    family = name;
    seed = case_seed;
    operation = 0U;
    random_state = case_seed;
    memset(&counts, 0, sizeof(counts));
}

static void end_case(void)
{
    check(counts.alloc == counts.free, "unbalanced successful allocations");
    check(counts.exhausted != 0U, "allocator was not exercised under exhaustion");
    check(counts.complete_reuse == EPOCHS, "an epoch missed its full-capacity reuse check");
    printf("PASS family=%s seed=0x%08lx operations=%lu alloc=%lu free=%lu exhausted=%lu "
           "complete_reuse=%lu rejection=%lu peak_live=%lu checked_bytes=%llu\n",
           family, (unsigned long)seed, (unsigned long)operation,
           (unsigned long)counts.alloc, (unsigned long)counts.free,
           (unsigned long)counts.exhausted, (unsigned long)counts.complete_reuse,
           (unsigned long)counts.rejection, (unsigned long)counts.peak_live,
           (unsigned long long)counts.checked_bytes);
    fflush(stdout);
}

static uint32_t next_heap_request(void)
{
    static const uint32_t edges[] = {1U, 7U, 8U, 9U, 15U, 16U, 17U, 1023U, 1024U, 1025U};
    const uint32_t word = next_random();
    if ((word & 7U) == 0U) {
        return edges[(word >> 3U) % (sizeof(edges) / sizeof(edges[0]))];
    }
    if ((word & 7U) < 5U) {
        return 1U + ((word >> 3U) % 192U);
    }
    return 1U + ((word >> 3U) % 4097U);
}

static void check_heap_range(const live_block_t *blocks, uintptr_t base, uint32_t capacity,
                             uint8_t *data, uint32_t request)
{
    const uintptr_t addr = (uintptr_t)data;
    const uint32_t size = align_size(request, HEAP_ALIGNMENT);
    check((addr % HEAP_ALIGNMENT) == 0U, "unaligned heap pointer");
    check((addr >= base) && (size <= capacity) && ((addr - base) <= (capacity - size)),
          "heap result outside initial arena");
    for (uint32_t i = 0U; i < SLOT_COUNT; ++i) {
        if (blocks[i].data == NULL) {
            continue;
        }
        const uintptr_t other = (uintptr_t)blocks[i].data;
        const uint32_t other_size = align_size(blocks[i].size, HEAP_ALIGNMENT);
        const bool is_disjoint = (addr < other) ? (size <= other - addr)
                                               : (other_size <= addr - other);
        check(is_disjoint, "simultaneous heap allocations overlap");
    }
}

static void release_heap_block(live_block_t *block)
{
    check_block(block);
    rtos_heap_free(block->data);
    block->data = NULL;
    ++counts.free;
}

static void verify_heap_reuse(uint8_t *base, uint32_t capacity)
{
    check(rtos_heap_get_max_alloc() == capacity, "full contiguous heap capacity not restored");
    uint8_t *whole = rtos_heap_alloc(capacity);
    check(whole == base, "entire heap could not be reused at its original address");
    live_block_t probe;
    fill_block(&probe, whole, capacity);
    check_block(&probe);
    check(rtos_heap_alloc(1U) == NULL, "full heap unexpectedly allocated another block");
    ++counts.exhausted;
    rtos_heap_free(whole);
    check(rtos_heap_get_free() == capacity, "heap byte count not restored after coalescing");
    ++counts.complete_reuse;
}

static void run_heap(uint32_t case_seed)
{
    begin_case("heap", case_seed);
    const uint32_t capacity = rtos_heap_get_max_alloc();
    check((capacity >= 4097U) && (rtos_heap_get_free() == capacity), "bad initial heap capacity");
    uint8_t *base = rtos_heap_alloc(capacity);
    check(base != NULL, "initial full allocation failed");
    rtos_heap_free(base);
    const uint32_t alloc_before = rtos_heap_get_alloc_count();
    const uint32_t free_before = rtos_heap_get_free_count();
    live_block_t blocks[SLOT_COUNT] = {0};

    for (uint32_t epoch = 0U; epoch < EPOCHS; ++epoch) {
        uint32_t live = 0U;
        for (uint32_t step = 0U; step < OPERATIONS_PER_EPOCH; ++step) {
            ++operation;
            live_block_t *block = &blocks[next_random() % SLOT_COUNT];
            if (block->data != NULL) {
                release_heap_block(block);
                --live;
            } else {
                const uint32_t request = next_heap_request();
                uint8_t *data = rtos_heap_alloc(request);
                if (data == NULL) {
                    check(rtos_heap_get_max_alloc() < request, "alloc failed despite free extent");
                    ++counts.exhausted;
                } else {
                    check_heap_range(blocks, (uintptr_t)base, capacity, data, request);
                    fill_block(block, data, request);
                    record_alloc(++live);
                }
            }
            if ((step % AUDIT_INTERVAL) == 0U) {
                audit_blocks(blocks, SLOT_COUNT);
            }
        }
        for (uint32_t i = SLOT_COUNT; i > 0U; --i) {
            const uint32_t pick = next_random() % i;
            if (blocks[pick].data != NULL) {
                release_heap_block(&blocks[pick]);
            }
            blocks[pick] = blocks[i - 1U];
            blocks[i - 1U].data = NULL;
        }
        verify_heap_reuse(base, capacity);
    }
    check(rtos_heap_get_alloc_count() - alloc_before == counts.alloc + EPOCHS,
          "heap allocation count disagrees with host ledger");
    check(rtos_heap_get_free_count() - free_before == counts.free + EPOCHS,
          "heap free count disagrees with host ledger");
    end_case();
}

static void check_pool_storage(const pool_case_t *test)
{
    const size_t end = (size_t)(test->base - pool_storage) + test->capacity * test->stride;
    for (size_t i = 0U; i < (size_t)(test->base - pool_storage); ++i) {
        check(pool_storage[i] == POOL_CANARY, "pool wrote before provided storage");
    }
    for (size_t i = end; i < sizeof(pool_storage); ++i) {
        check(pool_storage[i] == POOL_CANARY, "pool wrote beyond provided storage");
    }
}

static void allocate_pool_block(pool_case_t *test, live_block_t *block)
{
    uint8_t *data = rtos_mempool_alloc(&test->pool);
    check(data != NULL, "pool unexpectedly exhausted");
    const uintptr_t addr = (uintptr_t)data;
    const uintptr_t base = (uintptr_t)test->base;
    check((addr >= base) && ((addr - base) < test->capacity * test->stride)
          && (((addr - base) % test->stride) == 0U), "invalid pool block address");
    for (uint32_t i = 0U; i < test->capacity; ++i) {
        check(test->blocks[i].data != data, "pool allocated a duplicate live block");
    }
    fill_block(block, data, test->stride);
    record_alloc(++test->live);
}

static void release_pool_block(pool_case_t *test, live_block_t *block, bool test_double_free)
{
    check_block(block);
    check(rtos_mempool_free(&test->pool, block->data) == RTOS_OK, "valid pool free failed");
    if (test_double_free) {
        check(rtos_mempool_free(&test->pool, block->data) == RTOS_ERR_ALREADY,
              "pool did not reject double free");
        ++counts.rejection;
    }
    block->data = NULL;
    --test->live;
    ++counts.free;
}

static void verify_pool_reuse(pool_case_t *test)
{
    for (uint32_t i = 0U; i < test->capacity; ++i) {
        allocate_pool_block(test, &test->blocks[i]);
    }
    check((rtos_mempool_get_free(&test->pool) == 0U) && (rtos_mempool_alloc(&test->pool) == NULL),
          "fully allocated pool did not report exhaustion");
    ++counts.exhausted;
    for (uint32_t i = test->capacity; i > 0U; --i) {
        const uint32_t pick = next_random() % i;
        release_pool_block(test, &test->blocks[pick], true);
        test->blocks[pick] = test->blocks[i - 1U];
        test->blocks[i - 1U].data = NULL;
    }
    check(rtos_mempool_get_free(&test->pool) == test->capacity, "pool capacity did not recover");
    check_pool_storage(test);
    ++counts.complete_reuse;
}

static void run_pool(uint32_t case_seed, uint32_t request, uint32_t capacity)
{
    begin_case("pool", case_seed);
    pool_case_t test = {0};
    test.base = pool_storage + 8U;
    test.capacity = capacity;
    const uint32_t alignment = (uint32_t)_Alignof(void *);
    const uint32_t min_size = request < sizeof(void *) ? (uint32_t)sizeof(void *) : request;
    test.stride = align_size(min_size, alignment);
    memset(pool_storage, POOL_CANARY, sizeof(pool_storage));
    check(rtos_mempool_init(&test.pool, test.base, request, capacity) == RTOS_OK,
          "pool init failed");
    check((test.pool.block_size == test.stride) && (test.pool.block_count == capacity),
          "pool initialized with unexpected geometry");
    printf("pool seed=0x%08lx request=%lu stride=%lu blocks=%lu\n", (unsigned long)seed,
           (unsigned long)request, (unsigned long)test.stride, (unsigned long)capacity);

    for (uint32_t epoch = 0U; epoch < EPOCHS; ++epoch) {
        for (uint32_t step = 0U; step < OPERATIONS_PER_EPOCH; ++step) {
            ++operation;
            live_block_t *block = &test.blocks[next_random() % capacity];
            if (block->data == NULL) {
                allocate_pool_block(&test, block);
            } else {
                if ((step % AUDIT_INTERVAL) == 0U) {
                    check(rtos_mempool_free(&test.pool, block->data + 1U) == RTOS_ERR_PARAM,
                          "pool did not reject interior address");
                    ++counts.rejection;
                }
                release_pool_block(&test, block, (step % 997U) == 0U);
            }
            check(rtos_mempool_get_free(&test.pool) == capacity - test.live,
                  "pool free count disagrees with host ledger");
            if ((step % AUDIT_INTERVAL) == 0U) {
                audit_blocks(test.blocks, capacity);
                check_pool_storage(&test);
            }
        }
        for (uint32_t i = 0U; i < capacity; ++i) {
            if (test.blocks[i].data != NULL) {
                release_pool_block(&test, &test.blocks[i], false);
            }
        }
        verify_pool_reuse(&test);
    }
    end_case();
}

int main(void)
{
    static const uint32_t seeds[] = {
        UINT32_C(0x00000001), UINT32_C(0x12345678),
        UINT32_C(0xA5A5A5A5), UINT32_C(0xDEADBEEF)
    };
    static const uint32_t requests[] = {1U, 9U, 33U, 129U};
    static const uint32_t capacities[] = {1U, 7U, 32U, SLOT_COUNT};
    if (rtos_heap_init() != RTOS_OK) {
        return 2;
    }
    printf("host_pointer_bytes=%zu host_pointer_alignment=%zu\n",
           sizeof(void *), _Alignof(void *));
    for (size_t i = 0U; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        run_heap(seeds[i]);
        run_pool(seeds[i], requests[i], capacities[i]);
    }
    puts("PASS random_memory 8 seed/backend cases");
    return 0;
}

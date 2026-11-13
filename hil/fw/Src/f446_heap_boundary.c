/**
 * @file f446_heap_boundary.c
 * @brief Public heap boundaries and controlled metadata fault injection on F446.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"

#include "rtos_config.h"
#include "rtos_heap.h"
#include "rtos_port.h"

#define HBN_REQUIRE(condition)                                              \
    do {                                                                   \
        if (!(condition)) {                                                \
            (void)printf("HBN|ERROR|line=%u\r\n", (unsigned int)__LINE__);   \
            return false;                                                  \
        }                                                                  \
    } while (0)

enum {
    HBN_HEADER_SIZE = 8U,
    HBN_MIN_BLOCK = 16U,
    HBN_MIN_TAIL = 24U,
    HBN_SLOT_LIMIT = RTOS_CONFIG_HEAP_SIZE / HBN_MIN_BLOCK + 1U,
    HBN_CASE_COUNT = 7U
};

typedef struct hbn_block {
    uint8_t *address;
    uint32_t physical_size;
    bool is_free;
} hbn_block_t;

typedef struct hbn_mask {
    uint32_t basepri;
    uint32_t primask;
    uint32_t nesting;
} hbn_mask_t;

static hbn_block_t s_blocks[HBN_SLOT_LIMIT];
static uint32_t s_capacity;
static uint32_t s_initial_live;
static uint32_t s_exhausted_count;
static uint32_t s_request_count;

void f446_heap_boundary_run(void *argument);

static hbn_mask_t read_mask(void)
{
    return (hbn_mask_t){__get_BASEPRI(), __get_PRIMASK(), rtos_port_get_critical_nesting()};
}

static bool matches_mask(hbn_mask_t expected)
{
    const hbn_mask_t actual = read_mask();
    return actual.basepri == expected.basepri && actual.primask == expected.primask &&
        actual.nesting == expected.nesting;
}

static bool has_live_balance(void)
{
    return rtos_heap_get_alloc_count() - rtos_heap_get_free_count() == s_initial_live;
}

static bool recover_capacity(void)
{
    HBN_REQUIRE(rtos_heap_get_max_alloc() == s_capacity && has_live_balance());
    uint8_t *const whole = rtos_heap_alloc(s_capacity);
    HBN_REQUIRE(whole != NULL && rtos_heap_get_free() == 0U &&
                rtos_heap_get_max_alloc() == 0U);
    whole[0] = 0x96U;
    whole[s_capacity - 1U] = 0x69U;
    HBN_REQUIRE(whole[0] == 0x96U && whole[s_capacity - 1U] == 0x69U);
    rtos_heap_free(whole);
    HBN_REQUIRE(rtos_heap_get_free() == s_capacity &&
                rtos_heap_get_max_alloc() == s_capacity && has_live_balance());
    return true;
}

static bool check_request_bounds(void)
{
    const uint32_t invalid[] = {0U, UINT32_MAX, UINT32_MAX - 1U, UINT32_C(0x80000000),
                                UINT32_C(0x7FFFFFFF), s_capacity + 1U, s_capacity + 8U};
    const uint32_t allocations = rtos_heap_get_alloc_count();
    const uint32_t releases = rtos_heap_get_free_count();
    for (uint32_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        HBN_REQUIRE(rtos_heap_alloc(invalid[index]) == NULL);
        HBN_REQUIRE(rtos_heap_get_free() == s_capacity &&
                    rtos_heap_get_max_alloc() == s_capacity);
    }
    for (uint32_t distance = 0U; distance <= 16U; ++distance) {
        HBN_REQUIRE(rtos_heap_alloc(UINT32_C(0x7FFFFFF0) + distance) == NULL);
    }
    for (uint32_t distance = 0U; distance < 16U; ++distance) {
        HBN_REQUIRE(rtos_heap_alloc(UINT32_MAX - distance) == NULL);
    }
    HBN_REQUIRE(rtos_heap_get_alloc_count() == allocations &&
                rtos_heap_get_free_count() == releases);
    return recover_capacity();
}

static bool check_one_request(uint32_t request)
{
    const uint32_t aligned = (request + 7U) & ~UINT32_C(7);
    uint8_t *const data = rtos_heap_alloc(request);
    HBN_REQUIRE(data != NULL && ((uintptr_t)data & 7U) == 0U);
    HBN_REQUIRE(rtos_heap_get_free() == s_capacity - aligned - HBN_HEADER_SIZE &&
                rtos_heap_get_max_alloc() == s_capacity - aligned - HBN_HEADER_SIZE);
    (void)memset(data, (int)(request & 0xFFU), request);
    for (uint32_t index = 0U; index < request; ++index) {
        HBN_REQUIRE(data[index] == (uint8_t)request);
    }
    rtos_heap_free(data);
    ++s_request_count;
    HBN_REQUIRE(rtos_heap_get_free() == s_capacity && has_live_balance());
    return true;
}

static bool check_rounding(void)
{
    for (uint32_t size = 1U; size <= 80U; ++size) {
        HBN_REQUIRE(check_one_request(size));
    }
    const uint32_t edges[] = {127U, 128U, 129U, 255U, 256U, 257U,
                              511U, 512U, 513U, 1023U, 1024U, 1025U};
    for (uint32_t index = 0U; index < sizeof(edges) / sizeof(edges[0]); ++index) {
        HBN_REQUIRE(check_one_request(edges[index]));
    }
    return recover_capacity();
}

static bool check_split_threshold(void)
{
    const uint32_t tails[] = {0U, 8U, 16U, 24U, 32U, 40U};
    for (uint32_t index = 0U; index < sizeof(tails) / sizeof(tails[0]); ++index) {
        const uint32_t tail = tails[index];
        const uint32_t request = s_capacity - tail;
        uint8_t *const data = rtos_heap_alloc(request);
        HBN_REQUIRE(data != NULL);
        data[0] = 0xC3U;
        data[request - 1U] = 0x3CU;
        const uint32_t remaining = tail >= HBN_MIN_TAIL ? tail - HBN_HEADER_SIZE : 0U;
        HBN_REQUIRE(rtos_heap_get_free() == remaining &&
                    rtos_heap_get_max_alloc() == remaining);
        uint8_t *const suffix = rtos_heap_alloc(remaining != 0U ? remaining : 1U);
        HBN_REQUIRE((suffix != NULL) == (remaining != 0U));
        HBN_REQUIRE(rtos_heap_get_free() == 0U && rtos_heap_get_max_alloc() == 0U);
        HBN_REQUIRE(data[0] == 0xC3U && data[request - 1U] == 0x3CU);
        rtos_heap_free(suffix);
        rtos_heap_free(data);
        HBN_REQUIRE(recover_capacity());
    }
    for (uint32_t distance = 0U; distance < 59U; ++distance) {
        const uint32_t request = s_capacity - 41U + distance;
        uint8_t *const data = rtos_heap_alloc(request);
        if (request > s_capacity) {
            HBN_REQUIRE(data == NULL && rtos_heap_get_free() == s_capacity &&
                        rtos_heap_get_max_alloc() == s_capacity);
            continue;
        }
        const uint32_t tail = s_capacity - ((request + 7U) & ~UINT32_C(7));
        const uint32_t remaining = tail >= HBN_MIN_TAIL ? tail - HBN_HEADER_SIZE : 0U;
        HBN_REQUIRE(data != NULL && rtos_heap_get_free() == remaining &&
                    rtos_heap_get_max_alloc() == remaining);
        data[0] = 0x8DU;
        data[request - 1U] = 0xD8U;
        HBN_REQUIRE(data[0] == 0x8DU && data[request - 1U] == 0xD8U);
        rtos_heap_free(data);
        HBN_REQUIRE(recover_capacity());
    }
    return true;
}

static bool check_block_model(uint32_t count)
{
    uint32_t free_bytes = 0U;
    uint32_t run = 0U;
    uint32_t largest = 0U;
    uint32_t live = s_initial_live;
    for (uint32_t index = 0U; index < count; ++index) {
        const hbn_block_t *const block = &s_blocks[index];
        if (block->physical_size == 0U) {
            continue;
        }
        if (block->is_free) {
            free_bytes += block->physical_size - HBN_HEADER_SIZE;
            run += block->physical_size;
            if (run > largest) {
                largest = run;
            }
        } else {
            HBN_REQUIRE(block->address[0] == (uint8_t)(index ^ 0x5AU));
            run = 0U;
            ++live;
        }
    }
    const uint32_t maximum = largest != 0U ? largest - HBN_HEADER_SIZE : 0U;
    HBN_REQUIRE(rtos_heap_get_free() == free_bytes && rtos_heap_get_max_alloc() == maximum);
    HBN_REQUIRE(rtos_heap_get_alloc_count() - rtos_heap_get_free_count() == live);
    return true;
}

static bool release_model_block(uint32_t index, uint32_t count)
{
    hbn_block_t *const block = &s_blocks[index];
    HBN_REQUIRE(!block->is_free && block->physical_size != 0U &&
                block->address[0] == (uint8_t)(index ^ 0x5AU));
    rtos_heap_free(block->address);
    block->is_free = true;
    uint32_t next = index + 1U;
    while (next < count && s_blocks[next].physical_size == 0U) {
        ++next;
    }
    if (next < count && s_blocks[next].is_free) {
        block->physical_size += s_blocks[next].physical_size;
        s_blocks[next].physical_size = 0U;
    }
    return true;
}

static bool check_exhaustion(void)
{
    uint32_t remaining = s_capacity + HBN_HEADER_SIZE;
    uint32_t count = 0U;
    while (remaining >= HBN_MIN_BLOCK) {
        HBN_REQUIRE(count < HBN_SLOT_LIMIT);
        const uint32_t used = remaining - HBN_MIN_BLOCK >= HBN_MIN_TAIL ?
                                HBN_MIN_BLOCK : remaining;
        uint8_t *const data = rtos_heap_alloc(1U);
        HBN_REQUIRE(data != NULL && ((uintptr_t)data & 7U) == 0U);
        if (count != 0U) {
            HBN_REQUIRE(data == s_blocks[count - 1U].address +
                        s_blocks[count - 1U].physical_size);
        }
        data[0] = (uint8_t)(count ^ 0x5AU);
        s_blocks[count] = (hbn_block_t){data, used, false};
        remaining -= used;
        ++count;
    }
    HBN_REQUIRE(remaining == 0U && count > 100U && rtos_heap_alloc(1U) == NULL);
    HBN_REQUIRE(check_block_model(count));
    for (uint32_t index = 0U; index < count; index += 2U) {
        HBN_REQUIRE(release_model_block(index, count));
    }
    HBN_REQUIRE(check_block_model(count));
    const uint32_t allocations = rtos_heap_get_alloc_count();
    const uint32_t free_bytes = rtos_heap_get_free();
    for (uint32_t attempt = 0U; attempt < 32U; ++attempt) {
        HBN_REQUIRE(rtos_heap_alloc(25U + attempt * 8U) == NULL);
    }
    HBN_REQUIRE(rtos_heap_get_alloc_count() == allocations &&
                rtos_heap_get_free() == free_bytes && check_block_model(count));
    for (uint32_t reverse = count; reverse != 0U;) {
        --reverse;
        if ((reverse & 1U) != 0U) {
            HBN_REQUIRE(release_model_block(reverse, count));
            if ((reverse % 17U) == 0U) {
                HBN_REQUIRE(check_block_model(count));
            }
        }
    }
    HBN_REQUIRE(check_block_model(count));
    s_exhausted_count = count;
    return recover_capacity();
}

static bool check_corruption_repair(void)
{
    const hbn_mask_t expected = read_mask();
    for (uint32_t kind = 0U; kind < 4U; ++kind) {
        uint8_t *const first = rtos_heap_alloc(32U);
        uint8_t *const second = rtos_heap_alloc(32U);
        uint8_t *const guard = rtos_heap_alloc(32U);
        uint8_t *const tail = rtos_heap_alloc(rtos_heap_get_max_alloc());
        HBN_REQUIRE(first != NULL && second == first + 40U && guard == second + 40U &&
                    tail != NULL && rtos_heap_get_free() == 0U);
        guard[0] = 0xB7U;
        guard[31] = 0x7BU;
        rtos_heap_free(second);
        volatile uint32_t *const damaged = (volatile uint32_t *)(second - HBN_HEADER_SIZE);
        const uint32_t magic = damaged[0];
        const uint32_t tag = damaged[1];
        if (kind == 0U) {
            damaged[0] = 0U;
        } else if (kind == 1U) {
            damaged[1] = UINT32_C(0x80000008);
        } else if (kind == 2U) {
            damaged[1] = UINT32_C(0x80000029);
        } else {
            damaged[1] = UINT32_MAX;
        }
        rtos_heap_free(first);
        const uint32_t allocations = rtos_heap_get_alloc_count();
        const uint32_t releases = rtos_heap_get_free_count();
        const bool is_rejected = rtos_heap_alloc(64U) == NULL &&
            rtos_heap_get_free() == 64U && rtos_heap_get_alloc_count() == allocations &&
            rtos_heap_get_free_count() == releases && matches_mask(expected);
        const bool is_guard_valid = guard[0] == 0xB7U && guard[31] == 0x7BU;
        damaged[0] = magic;
        damaged[1] = tag;
        HBN_REQUIRE(is_rejected && is_guard_valid);
        uint8_t *const joined = rtos_heap_alloc(64U);
        HBN_REQUIRE(joined == first && rtos_heap_get_free() == 0U);
        rtos_heap_free(tail);
        rtos_heap_free(guard);
        rtos_heap_free(joined);
        HBN_REQUIRE(recover_capacity());
    }
    return true;
}

static bool check_invalid_release(void)
{
    uint8_t *const whole = rtos_heap_alloc(s_capacity);
    HBN_REQUIRE(whole != NULL);
    (void)memset(whole, 0, 32U);
    const uint32_t allocations = rtos_heap_get_alloc_count();
    const uint32_t releases = rtos_heap_get_free_count();
    rtos_heap_free(NULL);
    rtos_heap_free((void *)(uintptr_t)1U);
    rtos_heap_free(whole + 1U);
    rtos_heap_free(whole + HBN_HEADER_SIZE);
    rtos_heap_free(whole + s_capacity);
    HBN_REQUIRE(rtos_heap_get_free() == 0U && rtos_heap_get_alloc_count() == allocations &&
                rtos_heap_get_free_count() == releases);
    rtos_heap_free(whole);
    rtos_heap_free(whole);
    HBN_REQUIRE(rtos_heap_get_free_count() == releases + 1U);
    return recover_capacity();
}

static bool probe_masked_failures(void)
{
    const hbn_mask_t expected = read_mask();
    uint8_t *const whole = rtos_heap_alloc(s_capacity);
    HBN_REQUIRE(whole != NULL && matches_mask(expected));
    bool passed = rtos_heap_alloc(1U) == NULL && matches_mask(expected);
    passed = rtos_heap_alloc(1U) == NULL && matches_mask(expected) && passed;
    passed = rtos_heap_get_max_alloc() == 0U && matches_mask(expected) && passed;
    passed = rtos_heap_alloc(UINT32_MAX) == NULL && matches_mask(expected) && passed;
    volatile uint32_t *const own_header = (volatile uint32_t *)(whole - HBN_HEADER_SIZE);
    const uint32_t magic = own_header[0];
    const uint32_t releases = rtos_heap_get_free_count();
    own_header[0] = 0U;
    rtos_heap_free(whole);
    passed = rtos_heap_get_free_count() == releases && rtos_heap_get_free() == 0U &&
        matches_mask(expected) && passed;
    own_header[0] = magic;
    rtos_heap_free(whole + 1U);
    passed = matches_mask(expected) && passed;
    rtos_heap_free(whole);
    passed = matches_mask(expected) && passed;
    rtos_heap_free(whole);
    passed = matches_mask(expected) && passed;
    HBN_REQUIRE(passed && rtos_heap_get_max_alloc() == s_capacity && has_live_balance());
    return true;
}

static bool probe_fragmented_failure(void)
{
    const hbn_mask_t expected = read_mask();
    uint8_t *const first = rtos_heap_alloc(32U);
    uint8_t *const guard = rtos_heap_alloc(32U);
    uint8_t *const third = rtos_heap_alloc(32U);
    uint8_t *const tail = rtos_heap_alloc(rtos_heap_get_max_alloc());
    HBN_REQUIRE(first != NULL && guard != NULL && third != NULL && tail != NULL);
    guard[0] = 0xE7U;
    guard[31] = 0x7EU;
    rtos_heap_free(first);
    rtos_heap_free(third);
    const uint32_t allocations = rtos_heap_get_alloc_count();
    HBN_REQUIRE(rtos_heap_alloc(64U) == NULL && matches_mask(expected));
    HBN_REQUIRE(rtos_heap_get_max_alloc() == 32U && matches_mask(expected));
    HBN_REQUIRE(rtos_heap_alloc(64U) == NULL && matches_mask(expected));
    HBN_REQUIRE(rtos_heap_get_free() == 64U &&
                rtos_heap_get_alloc_count() == allocations &&
                guard[0] == 0xE7U && guard[31] == 0x7EU);
    rtos_heap_free(tail);
    rtos_heap_free(guard);
    return recover_capacity() && matches_mask(expected);
}

static bool check_masked_failures(void)
{
    const hbn_mask_t initial = read_mask();
    RTOS_PORT_ENTER_CRITICAL();
    RTOS_PORT_ENTER_CRITICAL();
    const bool is_nested_valid = probe_masked_failures() && probe_fragmented_failure() &&
        check_corruption_repair();
    RTOS_PORT_EXIT_CRITICAL();
    RTOS_PORT_EXIT_CRITICAL();
    HBN_REQUIRE(is_nested_valid && matches_mask(initial));
    for (uint32_t combination = 1U; combination < 4U; ++combination) {
        const uint32_t strict = 3U << (8U - __NVIC_PRIO_BITS);
        __set_BASEPRI((combination & 1U) != 0U ? strict : 0U);
        __set_PRIMASK((combination & 2U) != 0U ? 1U : 0U);
        __DSB();
        __ISB();
        const bool passed = probe_masked_failures() && probe_fragmented_failure() &&
            check_corruption_repair();
        __set_BASEPRI(initial.basepri);
        __set_PRIMASK(initial.primask);
        __DSB();
        __ISB();
        HBN_REQUIRE(passed && matches_mask(initial));
    }
    return recover_capacity();
}

static uint32_t record_case(const char *name, bool passed)
{
    (void)printf("HBN|CASE|case=%s|%s\r\n", name, passed ? "PASS" : "FAIL");
    return passed ? 0U : 1U;
}

void f446_heap_boundary_run(void *argument)
{
    (void)argument;
    s_capacity = rtos_heap_get_max_alloc();
    s_initial_live = rtos_heap_get_alloc_count() - rtos_heap_get_free_count();
    (void)printf("HBN|START|capacity=%lu|heap=%lu|basepri=%u|live=%lu\r\n",
                 (unsigned long)s_capacity, (unsigned long)RTOS_CONFIG_HEAP_SIZE,
                 (unsigned int)RTOS_CONFIG_USE_BASEPRI, (unsigned long)s_initial_live);
    const hbn_mask_t clean = {0U, 0U, 0U};
    uint32_t failures = 0U;
    const bool is_ready = s_capacity > 4096U && matches_mask(clean) && recover_capacity();
    failures += record_case("request_bounds", is_ready && check_request_bounds());
    failures += record_case("rounding", failures == 0U && check_rounding());
    failures += record_case("split_threshold", failures == 0U && check_split_threshold());
    failures += record_case("exhaustion", failures == 0U && check_exhaustion());
    failures += record_case("corruption_repair", failures == 0U && check_corruption_repair());
    failures += record_case("invalid_release", failures == 0U && check_invalid_release());
    failures += record_case("masked_failures", failures == 0U && check_masked_failures());
    (void)printf("HBN|TOTAL|requests=%lu|min_blocks=%lu|free=%lu|max=%lu|live=%lu\r\n",
                 (unsigned long)s_request_count, (unsigned long)s_exhausted_count,
                 (unsigned long)rtos_heap_get_free(), (unsigned long)rtos_heap_get_max_alloc(),
                 (unsigned long)(rtos_heap_get_alloc_count() - rtos_heap_get_free_count()));
    (void)printf("HBN|DONE|status=%u|checks=%u|failures=%lu\r\n",
                 (unsigned int)(failures != 0U), (unsigned int)HBN_CASE_COUNT,
                 (unsigned long)failures);
}

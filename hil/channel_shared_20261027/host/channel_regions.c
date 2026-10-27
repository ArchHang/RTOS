/**
 * @file channel_regions.c
 * @brief Independent half-open region oracle for shared-memory channel indexing.
 *
 * The oracle uses an unsorted linear ledger, independent of production lookup.
 * The actual production C is included for later private index invariants; all
 * scenario actions use public APIs. This serial host fixture does not model IRQs.
 */
#include "host_port.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "../../../RTOS/IPC/rtos_channel.c"

#define REGION_CELL_BYTES (4096U)
#define REGION_PAD (512U)
#define REGION_OWNERS RTOS_CONFIG_CHANNEL_MAX_CHANNELS
#define REGION_CELLS (3U * (REGION_OWNERS + 1U))
#define REGION_ARENA_BYTES (REGION_CELL_BYTES * REGION_CELLS)
#define REGION_REORDER_STEPS (1000U)

typedef struct region_owner {
    rtos_channel_t *channel;
    rtos_channel_config_t config;
    uint32_t active;
} region_owner_t;

typedef struct region_expected {
    uintptr_t begin;
    uintptr_t end;
    uintptr_t owner;
} region_expected_t;

static uint8_t *arena;
static region_owner_t owners[REGION_OWNERS + 1U];
static region_expected_t expected_regions[3U * REGION_OWNERS];
static uint32_t expected_count;
static uint64_t checks;
static uint32_t random_state = 0xC712A395U;
static const char *current_case = "startup";

static void check(uint32_t condition, const char *expression, uint32_t line)
{
    checks++;
    if (condition == 0U) {
        (void)fprintf(stderr, "REGION|FAIL|case=%s line=%" PRIu32 " expr=%s\n",
            current_case, line, expression);
        exit(1);
    }
}

#define CHECK(expression) check((expression) != 0, #expression, __LINE__)
#define EXPECT(call, status) CHECK((call) == (status))

static uint32_t next_random(void)
{
    random_state ^= random_state << 13U;
    random_state ^= random_state >> 17U;
    random_state ^= random_state << 5U;
    return random_state;
}

static void *cell_address(uint32_t cell)
{
    return arena + cell * REGION_CELL_BYTES + REGION_PAD;
}

static void reset_layout(void)
{
    CHECK(expected_count == 0U);
    if (arena != NULL) {
        free(arena);
    }
    arena = malloc(REGION_ARENA_BYTES);
    CHECK(arena != NULL);
    CHECK(((uintptr_t)arena % RTOS_CHANNEL_ALIGNMENT) == 0U);
    CHECK(sizeof(rtos_channel_t) + REGION_PAD < REGION_CELL_BYTES);
    CHECK(sizeof(rtos_channel_stats_t) < REGION_PAD);
    memset(arena, 0xA7, REGION_ARENA_BYTES);
    memset(owners, 0, sizeof(owners));
    for (uint32_t index = 0U; index <= REGION_OWNERS; index++) {
        region_owner_t *owner = &owners[index];
        uint32_t short_span = index & 1U;
        owner->channel = cell_address(index);
        owner->config.slots = cell_address(REGION_OWNERS + 1U + index);
        owner->config.storage = cell_address(2U * (REGION_OWNERS + 1U) + index);
        owner->config.block_count = short_span != 0U ? 1U : 3U;
        owner->config.block_size = short_span != 0U ? 1U : 33U;
        owner->config.slots_size = short_span != 0U ? sizeof(rtos_channel_slot_t) : 256U;
        owner->config.storage_size = short_span != 0U ? 8U : 256U;
    }
}

static uint32_t oracle_overlaps(uintptr_t begin, size_t length)
{
    if (length == 0U || length > UINTPTR_MAX - begin) {
        return 1U;
    }
    uintptr_t end = begin + length;
    for (uint32_t index = 0U; index < expected_count; index++) {
        if (begin < expected_regions[index].end && end > expected_regions[index].begin) {
            return 1U;
        }
    }
    return 0U;
}

static void oracle_add(region_owner_t *owner)
{
    const uintptr_t begin[3] = {(uintptr_t)owner->channel, (uintptr_t)owner->config.slots,
        (uintptr_t)owner->config.storage};
    const size_t size[3] = {sizeof(*owner->channel), owner->config.slots_size,
        owner->config.storage_size};
    CHECK(owner->active == 0U && expected_count + 3U <= 3U * REGION_OWNERS);
    for (uint32_t index = 0U; index < 3U; index++) {
        CHECK(oracle_overlaps(begin[index], size[index]) == 0U);
        expected_regions[expected_count].begin = begin[index];
        expected_regions[expected_count].end = begin[index] + size[index];
        expected_regions[expected_count].owner = (uintptr_t)owner->channel;
        expected_count++;
    }
    owner->active = 1U;
}

static void oracle_remove(region_owner_t *owner)
{
    uint32_t kept = 0U;
    uint32_t removed = 0U;
    for (uint32_t index = 0U; index < expected_count; index++) {
        if (expected_regions[index].owner == (uintptr_t)owner->channel) {
            removed++;
        } else {
            expected_regions[kept++] = expected_regions[index];
        }
    }
    CHECK(owner->active != 0U && removed == 3U);
    expected_count = kept;
    owner->active = 0U;
}

static void check_index(void)
{
    CHECK(channel_region_count == expected_count);
    CHECK(channel_region_count <= 3U * REGION_OWNERS && channel_region_count % 3U == 0U);
    for (uint32_t index = 0U; index < channel_region_count; index++) {
        const channel_owned_region_t *actual = &channel_regions[index];
        CHECK(actual->begin < actual->end && actual->owner != NULL);
        if (index != 0U) {
            CHECK(channel_regions[index - 1U].end <= actual->begin);
        }
        uint32_t matches = 0U;
        for (uint32_t expected = 0U; expected < expected_count; expected++) {
            matches += actual->begin == expected_regions[expected].begin
                && actual->end == expected_regions[expected].end
                && (uintptr_t)actual->owner == expected_regions[expected].owner;
        }
        CHECK(matches == 1U);
    }
    for (uint32_t index = 0U; index <= REGION_OWNERS; index++) {
        if (owners[index].active != 0U) {
            uint32_t segments = 0U;
            uint32_t controls = 0U;
            for (uint32_t region = 0U; region < channel_region_count; region++) {
                const channel_owned_region_t *actual = &channel_regions[region];
                if (actual->owner == owners[index].channel) {
                    segments++;
                    controls += actual->begin == (uintptr_t)owners[index].channel
                        && actual->end == (uintptr_t)owners[index].channel
                            + sizeof(rtos_channel_t);
                }
            }
            CHECK(segments == 3U && controls == 1U);
        }
    }
}

static void add_owner(uint32_t index)
{
    region_owner_t *owner = &owners[index];
    EXPECT(rtos_channel_init(owner->channel, &owner->config), RTOS_OK);
    oracle_add(owner);
    check_index();
}

static void remove_owner(uint32_t index)
{
    region_owner_t *owner = &owners[index];
    EXPECT(rtos_channel_close(owner->channel), RTOS_OK);
    EXPECT(rtos_channel_deinit(owner->channel), RTOS_OK);
    oracle_remove(owner);
    check_index();
}

static void verify_owner(uint32_t index)
{
    region_owner_t *owner = &owners[index];
    rtos_channel_stats_t stats;
    EXPECT(rtos_channel_get_stats(owner->channel, &stats), RTOS_OK);
    CHECK(stats.capacity == owner->config.block_count && stats.free == stats.capacity);
    CHECK(stats.writing == 0U && stats.queued == 0U && stats.reading == 0U && stats.closed == 0U);
}

static void query_stats(uint32_t owner_index, uintptr_t candidate)
{
    size_t length = sizeof(rtos_channel_stats_t);
    uintptr_t low = (uintptr_t)arena;
    uintptr_t high = low + REGION_ARENA_BYTES;
    if (candidate < low || candidate > high || length > high - candidate) {
        return;
    }
    uint8_t before[sizeof(rtos_channel_stats_t)];
    memcpy(before, (void *)candidate, sizeof(before));
    uint32_t invalid = (candidate % _Alignof(rtos_channel_stats_t)) != 0U
        || oracle_overlaps(candidate, length) != 0U;
    rtos_status_t result = rtos_channel_get_stats(owners[owner_index].channel,
        (rtos_channel_stats_t *)candidate);
    if (invalid != 0U) {
        CHECK(result == RTOS_ERR_PARAM);
        CHECK(memcmp(before, (void *)candidate, sizeof(before)) == 0);
    } else {
        CHECK(result == RTOS_OK);
        rtos_channel_stats_t actual;
        memcpy(&actual, (void *)candidate, sizeof(actual));
        CHECK(actual.free == owners[owner_index].config.block_count);
        CHECK(actual.writing == 0U && actual.queued == 0U && actual.reading == 0U);
    }
}

static void query_boundaries(uint32_t owner_index)
{
    for (uint32_t index = 0U; index < expected_count; index++) {
        uintptr_t begin = expected_regions[index].begin;
        uintptr_t end = expected_regions[index].end;
        const uintptr_t candidate[] = {begin - sizeof(rtos_channel_stats_t),
            begin - sizeof(rtos_channel_stats_t) + 8U, begin - 8U, begin - 1U,
            begin, begin + 1U, begin + 8U, end - 8U, end - 1U, end, end + 1U, end + 8U};
        for (uint32_t query = 0U; query < sizeof(candidate) / sizeof(candidate[0]); query++) {
            query_stats(owner_index, candidate[query]);
        }
    }
    check_index();
}

static void reject_init_unchanged(region_owner_t *candidate, rtos_status_t status)
{
    uint8_t *before = malloc(REGION_ARENA_BYTES);
    CHECK(before != NULL);
    memcpy(before, arena, REGION_ARENA_BYTES);
    EXPECT(rtos_channel_init(candidate->channel, &candidate->config), status);
    CHECK(memcmp(before, arena, REGION_ARENA_BYTES) == 0);
    free(before);
    check_index();
}

static void exercise_ownership(uint32_t index, uint32_t other_index)
{
    region_owner_t *owner = &owners[index];
    region_owner_t *other = &owners[other_index];
    rtos_channel_writer_t writer;
    rtos_channel_writer_t old_writer;
    rtos_channel_reader_t reader;
    rtos_channel_reader_t old_reader;
    EXPECT(rtos_channel_acquire(owner->channel, &writer, 0U), RTOS_OK);
    old_writer = writer;
    ((uint8_t *)writer.data)[0] = (uint8_t)index;
    EXPECT(rtos_channel_publish(other->channel, &writer, 1U, index), RTOS_ERR_STALE);
    EXPECT(rtos_channel_cancel(other->channel, &writer), RTOS_ERR_STALE);
    CHECK(memcmp(&writer, &old_writer, sizeof(writer)) == 0);
    EXPECT(rtos_channel_publish(owner->channel, &writer, 1U, index), RTOS_OK);
    EXPECT(rtos_channel_receive(owner->channel, &reader, 0U), RTOS_OK);
    old_reader = reader;
    CHECK(reader.data == old_writer.data && reader.length == 1U && reader.tag == index);
    CHECK(((const uint8_t *)reader.data)[0] == (uint8_t)index);
    EXPECT(rtos_channel_cancel(owner->channel, &old_writer), RTOS_ERR_STALE);
    EXPECT(rtos_channel_release(other->channel, &reader), RTOS_ERR_STALE);
    CHECK(memcmp(&reader, &old_reader, sizeof(reader)) == 0);
    EXPECT(rtos_channel_release(owner->channel, &reader), RTOS_OK);
    EXPECT(rtos_channel_release(owner->channel, &old_reader), RTOS_ERR_STALE);
    verify_owner(index);
    verify_owner(other_index);
    check_index();
}

static void test_full_and_reorder(void)
{
    reset_layout();
    for (uint32_t count = 0U; count < REGION_OWNERS; count++) {
        add_owner(REGION_OWNERS - count - 1U);
    }
    query_boundaries(0U);
    reject_init_unchanged(&owners[REGION_OWNERS], RTOS_ERR_NO_MEM);
    for (uint32_t index = 0U; index < REGION_OWNERS; index++) {
        exercise_ownership(index, (index + 1U) % REGION_OWNERS);
    }
    for (uint32_t step = 0U; step < REGION_REORDER_STEPS; step++) {
        uint32_t selected = next_random() % REGION_OWNERS;
        rtos_channel_writer_t writer;
        EXPECT(rtos_channel_acquire(owners[selected].channel, &writer, 0U), RTOS_OK);
        rtos_channel_writer_t old = writer;
        EXPECT(rtos_channel_cancel(owners[selected].channel, &writer), RTOS_OK);
        remove_owner(selected);
        rtos_channel_stats_t stats;
        EXPECT(rtos_channel_get_stats(owners[selected].channel, &stats), RTOS_ERR_DELETED);
        uint32_t survivor = (selected + 1U) % REGION_OWNERS;
        query_boundaries(survivor);
        /* The previously full-table failure must not leave a ghost registration. */
        add_owner(REGION_OWNERS);
        exercise_ownership(REGION_OWNERS, survivor);
        remove_owner(REGION_OWNERS);
        add_owner(selected);
        rtos_channel_writer_t stale_before = old;
        EXPECT(rtos_channel_cancel(owners[selected].channel, &old), RTOS_ERR_STALE);
        CHECK(memcmp(&old, &stale_before, sizeof(old)) == 0);
        verify_owner(selected);
        query_boundaries(selected);
    }
    for (uint32_t index = 0U; index < REGION_OWNERS; index++) {
        remove_owner(index);
    }
    CHECK(expected_count == 0U);
    (void)printf("REGION|REORDER|steps=%u status=PASS\n", REGION_REORDER_STEPS);
}

static void test_adjacency_and_overlap(void)
{
    for (uint32_t part = 0U; part < 3U; part++) {
        reset_layout();
        owners[0].config.block_size = 1U;
        owners[0].config.block_count = 1U;
        owners[0].config.storage_size = 8U;
        owners[0].config.slots_size = sizeof(rtos_channel_slot_t);
        if (part == 0U) {
            owners[1].channel = (rtos_channel_t *)((uint8_t *)owners[0].channel
                + sizeof(rtos_channel_t));
        } else if (part == 1U) {
            owners[1].config.slots = owners[0].config.slots + 1U;
        } else {
            owners[1].config.storage = (uint8_t *)owners[0].config.storage + 8U;
        }
        add_owner(0U);
        add_owner(1U);
        query_boundaries(0U);
        exercise_ownership(0U, 1U);
        remove_owner(0U);
        query_boundaries(1U);
        remove_owner(1U);
    }

    reset_layout();
    owners[0].config.block_count = 1U;
    owners[0].config.block_size = 1U;
    owners[0].config.storage_size = 9U;
    owners[0].config.slots_size = sizeof(rtos_channel_slot_t) + 1U;
    add_owner(0U);
    rtos_channel_config_t saved = owners[1].config;
    owners[1].config.storage = (uint8_t *)owners[0].config.storage + 8U;
    reject_init_unchanged(&owners[1], RTOS_ERR_PARAM);
    owners[1].config = saved;
    owners[1].config.slots = owners[0].config.slots + 1U;
    reject_init_unchanged(&owners[1], RTOS_ERR_PARAM);
    owners[1].config = saved;
    add_owner(1U);
    query_boundaries(0U);
    remove_owner(1U);
    remove_owner(0U);
    (void)printf("REGION|EDGES|adjacent=PASS one_byte_overlap=PASS crossing_query=PASS\n");
}

static void test_pointer_boundaries(void)
{
    reset_layout();
    add_owner(0U);
    rtos_channel_stats_t *wrapped = (rtos_channel_stats_t *)(UINTPTR_MAX - 7U);
    EXPECT(rtos_channel_get_stats(owners[0].channel, wrapped), RTOS_ERR_PARAM);
    rtos_channel_config_t saved = owners[1].config;
    owners[1].config.storage = (void *)(UINTPTR_MAX - 7U);
    owners[1].config.storage_size = 16U;
    reject_init_unchanged(&owners[1], RTOS_ERR_PARAM);
    owners[1].config = saved;
    owners[1].config.slots = (rtos_channel_slot_t *)(UINTPTR_MAX - 7U);
    owners[1].config.slots_size = 64U;
    reject_init_unchanged(&owners[1], RTOS_ERR_PARAM);
    owners[1].config = saved;
    add_owner(1U);
    remove_owner(0U);
    remove_owner(1U);
}

static uint32_t waiting_owner;
static uint32_t waiting_acquire;
static uint32_t protect_old_control;
static rtos_channel_writer_t waiting_writer;

static void reindex_during_wait(void)
{
    if (waiting_acquire != 0U) {
        EXPECT(rtos_channel_cancel(owners[waiting_owner].channel, &waiting_writer), RTOS_OK);
    }
    remove_owner(waiting_owner);
    remove_owner(2U);
    add_owner(2U);
    if (protect_old_control == 0U) {
        add_owner(waiting_owner);
        if (waiting_acquire == 0U) {
            EXPECT(rtos_channel_acquire(owners[waiting_owner].channel, &waiting_writer, 0U),
                RTOS_OK);
            EXPECT(rtos_channel_publish(owners[waiting_owner].channel, &waiting_writer, 0U,
                0xA9U), RTOS_OK);
        }
#if defined(_WIN32)
    } else {
        DWORD previous;
        CHECK(VirtualProtect(owners[waiting_owner].channel, 4096U, PAGE_NOACCESS,
            &previous) != 0);
#endif
    }
    check_index();
}

static void test_pending_call_reindex(void)
{
    for (uint32_t acquire = 0U; acquire < 2U; acquire++) {
        reset_layout();
        for (uint32_t index = 0U; index < 4U; index++) {
            add_owner(index);
        }
        waiting_owner = 1U;
        waiting_acquire = acquire;
        protect_old_control = 0U;
        rtos_channel_writer_t writer;
        rtos_channel_reader_t reader;
        uint8_t before_writer[sizeof(writer)];
        uint8_t before_reader[sizeof(reader)];
        memset(&writer, 0xC3, sizeof(writer));
        memset(&reader, 0xD4, sizeof(reader));
        memcpy(before_writer, &writer, sizeof(writer));
        memcpy(before_reader, &reader, sizeof(reader));
        if (acquire != 0U) {
            EXPECT(rtos_channel_acquire(owners[waiting_owner].channel, &waiting_writer, 0U),
                RTOS_OK);
        }
        host_schedule_action = reindex_during_wait;
        if (acquire != 0U) {
            EXPECT(rtos_channel_acquire(owners[waiting_owner].channel, &writer, 10U),
                RTOS_ERR_DELETED);
            CHECK(memcmp(before_writer, &writer, sizeof(writer)) == 0);
        } else {
            EXPECT(rtos_channel_receive(owners[waiting_owner].channel, &reader, 10U),
                RTOS_ERR_DELETED);
            CHECK(memcmp(before_reader, &reader, sizeof(reader)) == 0);
        }
        host_schedule_action = NULL;
        CHECK(host_wait_calls == 1U);
        if (acquire == 0U) {
            EXPECT(rtos_channel_receive(owners[waiting_owner].channel, &reader, 0U), RTOS_OK);
            CHECK(reader.length == 0U && reader.tag == 0xA9U);
            EXPECT(rtos_channel_release(owners[waiting_owner].channel, &reader), RTOS_OK);
        }
        for (uint32_t index = 0U; index < 4U; index++) {
            verify_owner(index);
            remove_owner(index);
        }
        host_reset();
    }
    (void)printf("REGION|PENDING|acquire_reindex=PASS receive_reindex=PASS\n");
}

static void test_protected_removed_owner(void)
{
#if defined(_WIN32)
    reset_layout();
    rtos_channel_t *page = VirtualAlloc(NULL, 4096U, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(page != NULL);
    owners[0].channel = page;
    for (uint32_t index = 0U; index < 4U; index++) {
        add_owner(index);
    }
    rtos_channel_writer_t old;
    rtos_channel_writer_t writer;
    EXPECT(rtos_channel_acquire(page, &writer, 0U), RTOS_OK);
    old = writer;
    EXPECT(rtos_channel_cancel(page, &writer), RTOS_OK);
    waiting_owner = 0U;
    waiting_acquire = 0U;
    protect_old_control = 1U;
    rtos_channel_reader_t reader;
    rtos_channel_reader_t before;
    memset(&reader, 0xCE, sizeof(reader));
    memcpy(&before, &reader, sizeof(before));
    host_schedule_action = reindex_during_wait;
    EXPECT(rtos_channel_receive(page, &reader, 10U), RTOS_ERR_DELETED);
    host_schedule_action = NULL;
    CHECK(memcmp(&reader, &before, sizeof(reader)) == 0);
    rtos_channel_stats_t stats;
    EXPECT(rtos_channel_get_stats(page, &stats), RTOS_ERR_DELETED);
    exercise_ownership(1U, 2U);
    query_boundaries(1U);
    DWORD previous;
    CHECK(VirtualProtect(page, 4096U, PAGE_READWRITE, &previous) != 0);
    add_owner(0U);
    rtos_channel_writer_t stale_before = old;
    EXPECT(rtos_channel_cancel(page, &old), RTOS_ERR_STALE);
    CHECK(memcmp(&old, &stale_before, sizeof(old)) == 0);
    for (uint32_t index = 0U; index < 4U; index++) {
        remove_owner(index);
    }
    CHECK(VirtualFree(page, 0U, MEM_RELEASE) != 0);
    host_reset();
    protect_old_control = 0U;
    (void)printf("REGION|PROTECTED|removed_owner_noaccess=PASS\n");
#else
    (void)printf("REGION|PROTECTED|removed_owner_noaccess=NOT_RUN platform_not_windows\n");
#endif
}

int main(void)
{
    host_reset();
    current_case = "adjacent_and_crossing_intervals";
    test_adjacency_and_overlap();
    current_case = "max_address_overflow";
    test_pointer_boundaries();
    current_case = "full_table_and_random_reorder";
    test_full_and_reorder();
    current_case = "pending_call_after_index_compaction";
    test_pending_call_reindex();
    current_case = "protected_removed_owner";
    test_protected_removed_owner();
    CHECK(expected_count == 0U && rtos_port_crit_nest == 0U);
    free(arena);
    arena = NULL;
    (void)printf("REGION|SUMMARY|checks=%" PRIu64 " failures=0\n", checks);
    (void)printf("REGION|DONE|status=0\n");
    return 0;
}

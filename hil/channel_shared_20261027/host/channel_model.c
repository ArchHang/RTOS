/**
 * @file channel_model.c
 * @brief Independent channel ledger and deterministic native API tests.
 *
 * The production channel implementation is linked separately. Scheduling is
 * scripted by host_port.c, so this does not claim ARM interrupt/concurrency HIL.
 */
#include "host_port.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "rtos_channel.h"
#include "rtos_internal.h"
#include "rtos_task.h"

#define HOST_BLOCKS (16U)
#define HOST_BLOCK_SIZE (33U)
#define HOST_STRIDE (40U)
#define HOST_MODEL_STEPS (100000U)
#define HOST_GUARD UINT64_C(0xA31F9B5C27D640E8)

typedef struct host_fixture {
    uint64_t control_guard;
    rtos_channel_t channel;
    uint64_t control_tail;
    uint64_t slots_guard;
    rtos_channel_slot_t slots[HOST_BLOCKS];
    uint64_t slots_tail;
    _Alignas(8) uint8_t storage[16U + HOST_STRIDE * HOST_BLOCKS];
    rtos_channel_config_t config;
} host_fixture_t;

typedef enum host_state {
    HOST_STATE_FREE = 0,
    HOST_STATE_WRITING,
    HOST_STATE_QUEUED,
    HOST_STATE_READING
} host_state_t;

typedef struct host_model {
    host_state_t state[HOST_BLOCKS];
    rtos_channel_writer_t writers[HOST_BLOCKS];
    rtos_channel_reader_t readers[HOST_BLOCKS];
    uint8_t payload[HOST_BLOCKS][HOST_BLOCK_SIZE];
    uint32_t lengths[HOST_BLOCKS];
    uint32_t tags[HOST_BLOCKS];
    uint32_t ready[HOST_BLOCKS];
    uint32_t ready_head;
    uint32_t ready_count;
    uint32_t capacity;
    uint32_t closed;
    uint64_t acquired;
    uint64_t published;
    uint64_t received;
    uint64_t released;
    uint64_t cancelled;
    uint64_t acquire_misses;
    uint64_t receive_misses;
    uint64_t closed_rejections;
    rtos_channel_writer_t stale_writer;
    rtos_channel_reader_t stale_reader;
    uint32_t has_stale_writer;
    uint32_t has_stale_reader;
} host_model_t;

static uint64_t checks;
static uint64_t api_calls;
static uint64_t model_operations;
static uint32_t random_state = 0x619AC3E7U;
static const char *current_case = "startup";

static void host_check(uint32_t condition, const char *expression, uint32_t line)
{
    checks++;
    if (condition == 0U) {
        (void)fprintf(stderr, "HOST|FAIL|case=%s line=%" PRIu32 " expr=%s\n",
            current_case, line, expression);
        exit(1);
    }
}

static void host_status(rtos_status_t actual, rtos_status_t expected,
                        const char *expression, uint32_t line)
{
    api_calls++;
    if (actual != expected) {
        (void)fprintf(stderr,
            "HOST|STATUS|case=%s line=%" PRIu32 " actual=%d expected=%d expr=%s\n",
            current_case, line, (int)actual, (int)expected, expression);
    }
    host_check(actual == expected, expression, line);
}

#define CHECK(expression) host_check((expression) != 0, #expression, __LINE__)
#define EXPECT(call, result) host_status((call), (result), #call, __LINE__)

static uint32_t next_random(void)
{
    random_state ^= random_state << 13U;
    random_state ^= random_state >> 17U;
    random_state ^= random_state << 5U;
    return random_state;
}

static uint32_t bytes_are_zero(const void *memory, size_t size)
{
    const uint8_t *bytes = memory;
    for (size_t index = 0U; index < size; index++) {
        if (bytes[index] != 0U) {
            return 0U;
        }
    }
    return 1U;
}

static void prepare_fixture(host_fixture_t *fixture, uint32_t capacity)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->control_guard = HOST_GUARD;
    fixture->control_tail = HOST_GUARD;
    fixture->slots_guard = HOST_GUARD;
    fixture->slots_tail = HOST_GUARD;
    memset(fixture->storage, 0xA5, sizeof(fixture->storage));
    fixture->config.storage = fixture->storage + 8U;
    fixture->config.storage_size = sizeof(fixture->storage) - 16U;
    fixture->config.slots = fixture->slots;
    fixture->config.slots_size = sizeof(fixture->slots);
    fixture->config.block_size = HOST_BLOCK_SIZE;
    fixture->config.block_count = capacity;
}

static void check_guards(const host_fixture_t *fixture)
{
    CHECK(fixture->control_guard == HOST_GUARD);
    CHECK(fixture->control_tail == HOST_GUARD);
    CHECK(fixture->slots_guard == HOST_GUARD);
    CHECK(fixture->slots_tail == HOST_GUARD);
    for (uint32_t index = 0U; index < 8U; index++) {
        CHECK(fixture->storage[index] == 0xA5U);
        CHECK(fixture->storage[sizeof(fixture->storage) - 1U - index] == 0xA5U);
    }
    for (uint32_t block = 0U; block < HOST_BLOCKS; block++) {
        for (uint32_t offset = HOST_BLOCK_SIZE; offset < HOST_STRIDE; offset++) {
            CHECK(fixture->storage[8U + HOST_STRIDE * block + offset] == 0xA5U);
        }
    }
}

static void start_fixture(host_fixture_t *fixture, uint32_t capacity)
{
    prepare_fixture(fixture, capacity);
    EXPECT(rtos_channel_init(&fixture->channel, &fixture->config), RTOS_OK);
    check_guards(fixture);
}

static void stop_fixture(host_fixture_t *fixture)
{
    EXPECT(rtos_channel_close(&fixture->channel), RTOS_OK);
    EXPECT(rtos_channel_deinit(&fixture->channel), RTOS_OK);
    check_guards(fixture);
}

static void check_model(host_fixture_t *fixture, const host_model_t *model)
{
    rtos_channel_stats_t stats;
    uint32_t counts[4] = {0U, 0U, 0U, 0U};
    EXPECT(rtos_channel_get_stats(&fixture->channel, &stats), RTOS_OK);
    for (uint32_t index = 0U; index < model->capacity; index++) {
        CHECK((uint32_t)model->state[index] < 4U);
        counts[model->state[index]]++;
        CHECK((uint32_t)fixture->slots[index].state == (uint32_t)model->state[index]);
    }
    CHECK(stats.capacity == model->capacity);
    CHECK(stats.free == counts[HOST_STATE_FREE]);
    CHECK(stats.writing == counts[HOST_STATE_WRITING]);
    CHECK(stats.queued == counts[HOST_STATE_QUEUED]);
    CHECK(stats.reading == counts[HOST_STATE_READING]);
    CHECK(stats.free + stats.writing + stats.queued + stats.reading == stats.capacity);
    CHECK(stats.queued == model->ready_count);
    CHECK(stats.closed == model->closed);
    CHECK(stats.acquired == model->acquired);
    CHECK(stats.published == model->published);
    CHECK(stats.received == model->received);
    CHECK(stats.released == model->released);
    CHECK(stats.cancelled == model->cancelled);
    CHECK(stats.acquire_misses == model->acquire_misses);
    CHECK(stats.receive_misses == model->receive_misses);
    CHECK(stats.closed_rejections == model->closed_rejections);
    CHECK(stats.acquire_waits == 0U && stats.receive_waits == 0U);
    CHECK(stats.acquire_timeouts == 0U && stats.receive_timeouts == 0U);
    CHECK(stats.waiting_writers == 0U && stats.waiting_readers == 0U);
    CHECK(stats.queued_peak <= model->capacity && stats.in_use_peak <= model->capacity);
    CHECK(rtos_port_crit_nest == 0U);
    check_guards(fixture);
}

static uint32_t find_state(const host_model_t *model, host_state_t state, uint32_t start)
{
    for (uint32_t count = 0U; count < model->capacity; count++) {
        uint32_t index = (start + count) % model->capacity;
        if (model->state[index] == state) {
            return index;
        }
    }
    return HOST_BLOCKS;
}

static void model_acquire(host_fixture_t *fixture, host_model_t *model)
{
    rtos_channel_writer_t writer;
    rtos_channel_writer_t before;
    memset(&writer, 0xCC, sizeof(writer));
    memcpy(&before, &writer, sizeof(before));
    uint32_t available = find_state(model, HOST_STATE_FREE, 0U) != HOST_BLOCKS;
    rtos_status_t expected = model->closed != 0U ? RTOS_ERR_CLOSED
        : (available != 0U ? RTOS_OK : RTOS_ERR_TIMEOUT);
    EXPECT(rtos_channel_acquire(&fixture->channel, &writer, RTOS_NO_WAIT), expected);
    if (expected == RTOS_OK) {
        uint32_t slot = writer.ticket.slot;
        CHECK(slot < model->capacity && model->state[slot] == HOST_STATE_FREE);
        CHECK(writer.capacity == HOST_BLOCK_SIZE);
        CHECK(writer.data == fixture->storage + 8U + HOST_STRIDE * slot);
        CHECK(writer.ticket.channel_address == (uintptr_t)&fixture->channel);
        CHECK((writer.ticket.generation & 1U) != 0U);
        model->state[slot] = HOST_STATE_WRITING;
        model->writers[slot] = writer;
        model->acquired++;
        uint32_t seed = next_random();
        for (uint32_t offset = 0U; offset < HOST_BLOCK_SIZE; offset++) {
            model->payload[slot][offset] = (uint8_t)(seed + slot * 17U + offset * 29U);
        }
        memcpy(writer.data, model->payload[slot], HOST_BLOCK_SIZE);
    } else {
        CHECK(memcmp(&writer, &before, sizeof(writer)) == 0);
        if (expected == RTOS_ERR_CLOSED) {
            model->closed_rejections++;
        } else {
            model->acquire_misses++;
        }
    }
}

static void model_publish(host_fixture_t *fixture, host_model_t *model, uint32_t slot,
                          uint32_t invalid_length)
{
    rtos_channel_writer_t before;
    memcpy(&before, &model->writers[slot], sizeof(before));
    uint32_t length = invalid_length != 0U ? HOST_BLOCK_SIZE + 1U
        : next_random() % (HOST_BLOCK_SIZE + 1U);
    uint32_t tag = next_random();
    rtos_status_t expected = invalid_length != 0U ? RTOS_ERR_PARAM
        : (model->closed != 0U ? RTOS_ERR_CLOSED : RTOS_OK);
    EXPECT(rtos_channel_publish(&fixture->channel, &model->writers[slot], length, tag), expected);
    if (expected == RTOS_OK) {
        model->stale_writer = before;
        model->has_stale_writer = 1U;
        CHECK(bytes_are_zero(&model->writers[slot], sizeof(before)));
        model->state[slot] = HOST_STATE_QUEUED;
        model->lengths[slot] = length;
        model->tags[slot] = tag;
        model->ready[(model->ready_head + model->ready_count) % model->capacity] = slot;
        model->ready_count++;
        model->published++;
    } else {
        CHECK(memcmp(&model->writers[slot], &before, sizeof(before)) == 0);
        if (expected == RTOS_ERR_CLOSED) {
            model->closed_rejections++;
        }
    }
}

static void model_cancel(host_fixture_t *fixture, host_model_t *model, uint32_t slot)
{
    model->stale_writer = model->writers[slot];
    model->has_stale_writer = 1U;
    EXPECT(rtos_channel_cancel(&fixture->channel, &model->writers[slot]), RTOS_OK);
    CHECK(bytes_are_zero(&model->writers[slot], sizeof(model->writers[slot])));
    model->state[slot] = HOST_STATE_FREE;
    model->cancelled++;
}

static void model_receive(host_fixture_t *fixture, host_model_t *model)
{
    rtos_channel_reader_t reader;
    rtos_channel_reader_t before;
    memset(&reader, 0xDD, sizeof(reader));
    memcpy(&before, &reader, sizeof(before));
    rtos_status_t expected = model->ready_count != 0U ? RTOS_OK
        : (model->closed != 0U ? RTOS_ERR_CLOSED : RTOS_ERR_TIMEOUT);
    EXPECT(rtos_channel_receive(&fixture->channel, &reader, RTOS_NO_WAIT), expected);
    if (expected == RTOS_OK) {
        uint32_t slot = model->ready[model->ready_head];
        CHECK(reader.ticket.slot == slot);
        CHECK(model->state[slot] == HOST_STATE_QUEUED);
        CHECK(reader.data == fixture->storage + 8U + HOST_STRIDE * slot);
        CHECK(reader.length == model->lengths[slot] && reader.tag == model->tags[slot]);
        CHECK((reader.ticket.generation & 1U) == 0U);
        CHECK(memcmp(reader.data, model->payload[slot], reader.length) == 0);
        model->readers[slot] = reader;
        model->state[slot] = HOST_STATE_READING;
        model->ready_head = (model->ready_head + 1U) % model->capacity;
        model->ready_count--;
        model->received++;
    } else {
        CHECK(memcmp(&reader, &before, sizeof(reader)) == 0);
        if (expected == RTOS_ERR_CLOSED) {
            model->closed_rejections++;
        } else {
            model->receive_misses++;
        }
    }
}

static void model_release(host_fixture_t *fixture, host_model_t *model, uint32_t slot)
{
    CHECK(memcmp(model->readers[slot].data, model->payload[slot],
        model->readers[slot].length) == 0);
    model->stale_reader = model->readers[slot];
    model->has_stale_reader = 1U;
    EXPECT(rtos_channel_release(&fixture->channel, &model->readers[slot]), RTOS_OK);
    CHECK(bytes_are_zero(&model->readers[slot], sizeof(model->readers[slot])));
    model->state[slot] = HOST_STATE_FREE;
    model->released++;
}

static void model_stale(host_fixture_t *fixture, host_model_t *model)
{
    if (model->has_stale_writer != 0U) {
        rtos_channel_writer_t copy;
        memcpy(&copy, &model->stale_writer, sizeof(copy));
        EXPECT(rtos_channel_cancel(&fixture->channel, &copy), RTOS_ERR_STALE);
        CHECK(memcmp(&copy, &model->stale_writer, sizeof(copy)) == 0);
    } else if (model->has_stale_reader != 0U) {
        rtos_channel_reader_t copy;
        memcpy(&copy, &model->stale_reader, sizeof(copy));
        EXPECT(rtos_channel_release(&fixture->channel, &copy), RTOS_ERR_STALE);
        CHECK(memcmp(&copy, &model->stale_reader, sizeof(copy)) == 0);
    } else {
        model_acquire(fixture, model);
    }
}

static void close_model(host_fixture_t *fixture, host_model_t *model)
{
    EXPECT(rtos_channel_close(&fixture->channel), RTOS_OK);
    model->closed = 1U;
    EXPECT(rtos_channel_close(&fixture->channel), RTOS_ERR_ALREADY);
    for (uint32_t index = 0U; index < model->capacity; index++) {
        if (model->state[index] == HOST_STATE_WRITING) {
            model_publish(fixture, model, index, 0U);
            model_cancel(fixture, model, index);
        }
    }
    while (model->ready_count != 0U) {
        model_receive(fixture, model);
    }
    for (uint32_t index = 0U; index < model->capacity; index++) {
        if (model->state[index] == HOST_STATE_READING) {
            model_release(fixture, model, index);
        }
    }
    model_receive(fixture, model);
    check_model(fixture, model);
    EXPECT(rtos_channel_deinit(&fixture->channel), RTOS_OK);
}

static void random_model(uint32_t capacity)
{
    host_fixture_t fixture;
    host_model_t model;
    memset(&model, 0, sizeof(model));
    model.capacity = capacity;
    start_fixture(&fixture, capacity);
    for (uint32_t step = 0U; step < HOST_MODEL_STEPS; step++) {
        uint32_t random = next_random();
        uint32_t writer = find_state(&model, HOST_STATE_WRITING, random % capacity);
        uint32_t reader = find_state(&model, HOST_STATE_READING, random % capacity);
        switch (random % 8U) {
            case 0U:
                model_acquire(&fixture, &model);
                break;
            case 1U:
            case 6U:
                if (writer != HOST_BLOCKS) {
                    model_publish(&fixture, &model, writer, random % 8U == 6U);
                } else {
                    model_acquire(&fixture, &model);
                }
                break;
            case 2U:
                if (writer != HOST_BLOCKS) {
                    model_cancel(&fixture, &model, writer);
                } else {
                    model_receive(&fixture, &model);
                }
                break;
            case 3U:
                model_receive(&fixture, &model);
                break;
            case 4U:
                if (reader != HOST_BLOCKS) {
                    model_release(&fixture, &model, reader);
                } else {
                    model_acquire(&fixture, &model);
                }
                break;
            case 5U:
                model_stale(&fixture, &model);
                break;
            case 7U:
                model_receive(&fixture, &model);
                break;
            default:
                CHECK(0U);
                break;
        }
        model_operations++;
        check_model(&fixture, &model);
        if (step != 0U && step % 10000U == 0U) {
            uint64_t incarnation = fixture.channel.incarnation;
            close_model(&fixture, &model);
            memset(&model, 0, sizeof(model));
            model.capacity = capacity;
            EXPECT(rtos_channel_init(&fixture.channel, &fixture.config), RTOS_OK);
            CHECK(fixture.channel.incarnation != incarnation);
        }
    }
    close_model(&fixture, &model);
    (void)printf("HOST|MODEL|capacity=%" PRIu32 " operations=%u status=PASS\n",
        capacity, HOST_MODEL_STEPS);
}

static void directed_ownership(void)
{
    host_fixture_t first;
    host_fixture_t second;
    rtos_channel_writer_t writer;
    rtos_channel_writer_t old;
    rtos_channel_reader_t reader;
    rtos_channel_reader_t forged;
    rtos_channel_reader_t old_reader;
    start_fixture(&first, 1U);
    start_fixture(&second, 1U);
    EXPECT(rtos_channel_deinit(&first.channel), RTOS_ERR_BUSY);
    EXPECT(rtos_channel_acquire(&first.channel, &writer, 0U), RTOS_OK);
    old = writer;
    memset(writer.data, 0x3B, writer.capacity);
    EXPECT(rtos_channel_publish(&second.channel, &writer, 1U, 7U), RTOS_ERR_STALE);
    EXPECT(rtos_channel_cancel(&second.channel, &writer), RTOS_ERR_STALE);
    CHECK(memcmp(&writer, &old, sizeof(writer)) == 0);
    EXPECT(rtos_channel_publish(&first.channel, &writer, HOST_BLOCK_SIZE + 1U, 7U),
        RTOS_ERR_PARAM);
    CHECK(memcmp(&writer, &old, sizeof(writer)) == 0);
    EXPECT(rtos_channel_publish(&first.channel, &writer, HOST_BLOCK_SIZE, 7U), RTOS_OK);
    CHECK(bytes_are_zero(&writer, sizeof(writer)));
    EXPECT(rtos_channel_publish(&first.channel, &old, 1U, 7U), RTOS_ERR_STALE);
    EXPECT(rtos_channel_receive(&first.channel, &reader, 0U), RTOS_OK);
    CHECK(reader.data == old.data && reader.ticket.generation == old.ticket.generation + 1U);
    memset(&forged, 0, sizeof(forged));
    forged.ticket = old.ticket;
    forged.data = old.data;
    forged.length = reader.length;
    forged.sequence = reader.sequence;
    forged.tag = reader.tag;
    EXPECT(rtos_channel_release(&first.channel, &forged), RTOS_ERR_STALE);
    EXPECT(rtos_channel_release(&second.channel, &reader), RTOS_ERR_STALE);
    old_reader = reader;
    EXPECT(rtos_channel_close(&first.channel), RTOS_OK);
    EXPECT(rtos_channel_deinit(&first.channel), RTOS_ERR_BUSY);
    EXPECT(rtos_channel_release(&first.channel, &reader), RTOS_OK);
    CHECK(bytes_are_zero(&reader, sizeof(reader)));
    EXPECT(rtos_channel_release(&first.channel, &old_reader), RTOS_ERR_STALE);
    EXPECT(rtos_channel_deinit(&first.channel), RTOS_OK);
    EXPECT(rtos_channel_init(&first.channel, &first.config), RTOS_OK);
    EXPECT(rtos_channel_cancel(&first.channel, &old), RTOS_ERR_STALE);
    EXPECT(rtos_channel_release(&first.channel, &old_reader), RTOS_ERR_STALE);
    EXPECT(rtos_channel_acquire(&first.channel, &writer, 0U), RTOS_OK);
    EXPECT(rtos_channel_publish(&first.channel, &writer, 0U, UINT32_MAX), RTOS_OK);
    EXPECT(rtos_channel_receive(&first.channel, &reader, 0U), RTOS_OK);
    CHECK(reader.length == 0U && reader.tag == UINT32_MAX);
    EXPECT(rtos_channel_release(&first.channel, &reader), RTOS_OK);
    stop_fixture(&first);
    stop_fixture(&second);
}

static void reject_config(host_fixture_t *fixture, const rtos_channel_config_t *config)
{
    rtos_channel_t before;
    memcpy(&before, &fixture->channel, sizeof(before));
    EXPECT(rtos_channel_init(&fixture->channel, config), RTOS_ERR_PARAM);
    CHECK(memcmp(&fixture->channel, &before, sizeof(before)) == 0);
    check_guards(fixture);
}

static void directed_geometry(void)
{
    host_fixture_t fixture;
    host_fixture_t other;
    rtos_channel_config_t config;
    prepare_fixture(&fixture, 3U);
    EXPECT(rtos_channel_init(NULL, &fixture.config), RTOS_ERR_PARAM);
    reject_config(&fixture, NULL);
    config = fixture.config;
    config.block_count = 0U;
    reject_config(&fixture, &config);
    config.block_count = RTOS_CONFIG_CHANNEL_MAX_BLOCKS + 1U;
    reject_config(&fixture, &config);
    config = fixture.config;
    config.block_size = 0U;
    reject_config(&fixture, &config);
    config.block_size = UINT32_MAX;
    reject_config(&fixture, &config);
    config = fixture.config;
    config.storage = (uint8_t *)config.storage + 1U;
    reject_config(&fixture, &config);
    config = fixture.config;
    config.storage_size = HOST_STRIDE * 3U - 1U;
    reject_config(&fixture, &config);
    config = fixture.config;
    config.slots_size = sizeof(rtos_channel_slot_t) * 3U - 1U;
    reject_config(&fixture, &config);
    config = fixture.config;
    config.storage = (void *)(UINTPTR_MAX - 7U);
    config.storage_size = 16U;
    reject_config(&fixture, &config);
    config = fixture.config;
    config.slots = (rtos_channel_slot_t *)(UINTPTR_MAX - 7U);
    config.slots_size = sizeof(fixture.slots);
    reject_config(&fixture, &config);
    config = fixture.config;
    config.storage = &fixture.channel;
    config.storage_size = sizeof(fixture.channel);
    reject_config(&fixture, &config);
    config = fixture.config;
    config.slots = (rtos_channel_slot_t *)config.storage;
    reject_config(&fixture, &config);
    EXPECT(rtos_channel_init(&fixture.channel, &fixture.config), RTOS_OK);
    EXPECT(rtos_channel_init(&fixture.channel, &fixture.config), RTOS_ERR_ALREADY);
    prepare_fixture(&other, 1U);
    config = other.config;
    config.storage = (uint8_t *)fixture.config.storage + HOST_STRIDE * 8U;
    config.storage_size = HOST_STRIDE;
    EXPECT(rtos_channel_init(&other.channel, &config), RTOS_ERR_PARAM);
    config = other.config;
    config.slots = &fixture.slots[8];
    config.slots_size = sizeof(rtos_channel_slot_t);
    EXPECT(rtos_channel_init(&other.channel, &config), RTOS_ERR_PARAM);
    EXPECT(rtos_channel_acquire(&fixture.channel,
        (rtos_channel_writer_t *)fixture.config.storage, 0U), RTOS_ERR_PARAM);
    EXPECT(rtos_channel_receive(&fixture.channel,
        (rtos_channel_reader_t *)&fixture.slots[4], 0U), RTOS_ERR_PARAM);
    EXPECT(rtos_channel_get_stats(&fixture.channel,
        (rtos_channel_stats_t *)&fixture.channel), RTOS_ERR_PARAM);
    stop_fixture(&fixture);
}

static void directed_registry(void)
{
    host_fixture_t fixtures[RTOS_CONFIG_CHANNEL_MAX_CHANNELS + 1U];
    rtos_channel_writer_t writer;
    rtos_channel_reader_t reader;
    rtos_channel_stats_t stats;
    for (uint32_t index = 0U; index < RTOS_CONFIG_CHANNEL_MAX_CHANNELS; index++) {
        start_fixture(&fixtures[index], 1U);
    }
    host_fixture_t *last = &fixtures[RTOS_CONFIG_CHANNEL_MAX_CHANNELS - 1U];
    EXPECT(rtos_channel_acquire(&last->channel, &writer, 0U), RTOS_OK);
    EXPECT(rtos_channel_cancel(&last->channel, &writer), RTOS_OK);
    EXPECT(rtos_channel_acquire(&last->channel, &writer, 0U), RTOS_OK);
    void *original = writer.data;
    memset(writer.data, 0x79, HOST_BLOCK_SIZE);
    EXPECT(rtos_channel_publish(&last->channel, &writer, HOST_BLOCK_SIZE, 93U), RTOS_OK);
    /* An output in another channel's declared but unused tail remains forbidden. */
    EXPECT(rtos_channel_receive(&last->channel,
        (rtos_channel_reader_t *)&fixtures[0].slots[8], 0U), RTOS_ERR_PARAM);
    EXPECT(rtos_channel_receive(&last->channel, &reader, 0U), RTOS_OK);
    CHECK(reader.data == original && reader.length == HOST_BLOCK_SIZE && reader.tag == 93U);
    for (uint32_t index = 0U; index < reader.length; index++) {
        CHECK(((const uint8_t *)reader.data)[index] == 0x79U);
    }
    EXPECT(rtos_channel_release(&last->channel, &reader), RTOS_OK);
    EXPECT(rtos_channel_get_stats(&last->channel, &stats), RTOS_OK);
    CHECK(stats.free == 1U && stats.writing == 0U && stats.queued == 0U && stats.reading == 0U);
    CHECK(stats.acquired == 2U && stats.cancelled == 1U && stats.published == 1U);
    CHECK(stats.received == 1U && stats.released == 1U);
    prepare_fixture(&fixtures[RTOS_CONFIG_CHANNEL_MAX_CHANNELS], 1U);
    host_fixture_t *overflow = &fixtures[RTOS_CONFIG_CHANNEL_MAX_CHANNELS];
    rtos_channel_t before;
    memcpy(&before, &overflow->channel, sizeof(before));
    EXPECT(rtos_channel_init(&overflow->channel, &overflow->config), RTOS_ERR_NO_MEM);
    CHECK(memcmp(&before, &overflow->channel, sizeof(before)) == 0);
    stop_fixture(&fixtures[2]);
    EXPECT(rtos_channel_init(&overflow->channel, &overflow->config), RTOS_OK);
    stop_fixture(overflow);
    for (uint32_t index = 0U; index < RTOS_CONFIG_CHANNEL_MAX_CHANNELS; index++) {
        if (index != 2U) {
            stop_fixture(&fixtures[index]);
        }
    }
    (void)printf("HOST|REGISTRY|last_active_hot_api=PASS external_tail_alias=PASS\n");
}

static void directed_descriptor_overwrite(void)
{
    host_fixture_t fixture;
    rtos_channel_writer_t writer;
    rtos_channel_writer_t saved_writer;
    rtos_channel_reader_t reader;
    rtos_channel_reader_t saved_reader;
    rtos_channel_stats_t stats;
    start_fixture(&fixture, 2U);
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_OK);
    saved_writer = writer;
    /* Intentionally violate the output-lifetime contract, retaining cleanup authority. */
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_OK);
    CHECK(writer.data != saved_writer.data);
    EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
    CHECK(stats.free == 0U && stats.writing == 2U && stats.queued == 0U && stats.reading == 0U);
    EXPECT(rtos_channel_close(&fixture.channel), RTOS_OK);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_ERR_BUSY);
    EXPECT(rtos_channel_cancel(&fixture.channel, &saved_writer), RTOS_OK);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_ERR_BUSY);
    EXPECT(rtos_channel_cancel(&fixture.channel, &writer), RTOS_OK);
    EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
    CHECK(stats.free == 2U && stats.writing == 0U);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_OK);
    check_guards(&fixture);

    start_fixture(&fixture, 2U);
    for (uint32_t index = 0U; index < 2U; index++) {
        EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_OK);
        ((uint8_t *)writer.data)[0] = (uint8_t)(0x51U + index);
        EXPECT(rtos_channel_publish(&fixture.channel, &writer, 1U, index), RTOS_OK);
    }
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 0U), RTOS_OK);
    saved_reader = reader;
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 0U), RTOS_OK);
    CHECK(reader.data != saved_reader.data && reader.tag == 1U && saved_reader.tag == 0U);
    EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
    CHECK(stats.free == 0U && stats.writing == 0U && stats.queued == 0U && stats.reading == 2U);
    EXPECT(rtos_channel_close(&fixture.channel), RTOS_OK);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_ERR_BUSY);
    EXPECT(rtos_channel_release(&fixture.channel, &saved_reader), RTOS_OK);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_ERR_BUSY);
    EXPECT(rtos_channel_release(&fixture.channel, &reader), RTOS_OK);
    EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
    CHECK(stats.free == 2U && stats.reading == 0U);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_OK);
    check_guards(&fixture);
    (void)printf("HOST|CONTRACT|active_descriptor_overwrite=DEMONSTRATED backups_reclaimed=PASS\n");
}

static void directed_maximum_capacity(void)
{
    struct {
        uint64_t control_guard;
        rtos_channel_t channel;
        uint64_t control_tail;
        uint64_t slots_guard;
        rtos_channel_slot_t slots[RTOS_CONFIG_CHANNEL_MAX_BLOCKS];
        uint64_t slots_tail;
        _Alignas(8) uint8_t storage[16U + HOST_STRIDE * RTOS_CONFIG_CHANNEL_MAX_BLOCKS];
    } fixture;
    rtos_channel_writer_t writers[RTOS_CONFIG_CHANNEL_MAX_BLOCKS];
    rtos_channel_reader_t readers[RTOS_CONFIG_CHANNEL_MAX_BLOCKS];
    rtos_channel_writer_t unavailable;
    rtos_channel_writer_t before;
    rtos_channel_config_t config;
    rtos_channel_stats_t stats;
    const uint32_t capacity = RTOS_CONFIG_CHANNEL_MAX_BLOCKS;

    memset(&fixture, 0, sizeof(fixture));
    fixture.control_guard = HOST_GUARD;
    fixture.control_tail = HOST_GUARD;
    fixture.slots_guard = HOST_GUARD;
    fixture.slots_tail = HOST_GUARD;
    memset(fixture.storage, 0xA5, sizeof(fixture.storage));
    config.storage = fixture.storage + 8U;
    config.storage_size = sizeof(fixture.storage) - 16U;
    config.slots = fixture.slots;
    config.slots_size = sizeof(fixture.slots);
    config.block_size = HOST_BLOCK_SIZE;
    config.block_count = capacity;
    EXPECT(rtos_channel_init(&fixture.channel, &config), RTOS_OK);
    for (uint32_t index = 0U; index < capacity; index++) {
        EXPECT(rtos_channel_acquire(&fixture.channel, &writers[index], 0U), RTOS_OK);
        CHECK(writers[index].data == fixture.storage + 8U + index * HOST_STRIDE);
        CHECK(writers[index].ticket.slot == index && writers[index].capacity == HOST_BLOCK_SIZE);
        for (uint32_t byte = 0U; byte < HOST_BLOCK_SIZE; byte++) {
            ((uint8_t *)writers[index].data)[byte] = (uint8_t)(index + byte);
        }
        EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
        CHECK(stats.free == capacity - index - 1U && stats.writing == index + 1U);
        CHECK(stats.queued == 0U && stats.reading == 0U);
    }
    memset(&unavailable, 0xBE, sizeof(unavailable));
    memcpy(&before, &unavailable, sizeof(before));
    EXPECT(rtos_channel_acquire(&fixture.channel, &unavailable, 0U), RTOS_ERR_TIMEOUT);
    CHECK(memcmp(&unavailable, &before, sizeof(before)) == 0);
    /* Reverse publication order proves FIFO is publication order, not slot order. */
    for (uint32_t order = 0U; order < capacity; order++) {
        uint32_t index = capacity - order - 1U;
        EXPECT(rtos_channel_publish(&fixture.channel, &writers[index], HOST_BLOCK_SIZE, index),
            RTOS_OK);
        CHECK(bytes_are_zero(&writers[index], sizeof(writers[index])));
        EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
        CHECK(stats.free == 0U && stats.writing == capacity - order - 1U);
        CHECK(stats.queued == order + 1U && stats.reading == 0U);
    }
    EXPECT(rtos_channel_close(&fixture.channel), RTOS_OK);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_ERR_BUSY);
    for (uint32_t order = 0U; order < capacity; order++) {
        uint32_t index = capacity - order - 1U;
        EXPECT(rtos_channel_receive(&fixture.channel, &readers[order], 0U), RTOS_OK);
        CHECK(readers[order].sequence == (uint64_t)order + 1U);
        CHECK(readers[order].tag == index && readers[order].length == HOST_BLOCK_SIZE);
        CHECK(readers[order].data == fixture.storage + 8U + index * HOST_STRIDE);
        for (uint32_t byte = 0U; byte < HOST_BLOCK_SIZE; byte++) {
            CHECK(((const uint8_t *)readers[order].data)[byte] == (uint8_t)(index + byte));
        }
        EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
        CHECK(stats.queued == capacity - order - 1U && stats.reading == order + 1U);
        CHECK(stats.free == 0U && stats.writing == 0U);
    }
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_ERR_BUSY);
    for (uint32_t order = 0U; order < capacity; order++) {
        uint32_t index = capacity - order - 1U;
        EXPECT(rtos_channel_release(&fixture.channel, &readers[index]), RTOS_OK);
        CHECK(bytes_are_zero(&readers[index], sizeof(readers[index])));
        EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
        CHECK(stats.free == order + 1U && stats.reading == capacity - order - 1U);
        CHECK(stats.writing == 0U && stats.queued == 0U);
    }
    CHECK(stats.acquired == capacity && stats.published == capacity);
    CHECK(stats.received == capacity && stats.released == capacity && stats.cancelled == 0U);
    CHECK(stats.in_use_peak == capacity && stats.queued_peak == capacity);
    CHECK(stats.acquire_misses == 1U && stats.acquire_timeouts == 0U && stats.closed == 1U);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_OK);
    CHECK(fixture.control_guard == HOST_GUARD && fixture.control_tail == HOST_GUARD);
    CHECK(fixture.slots_guard == HOST_GUARD && fixture.slots_tail == HOST_GUARD);
    for (uint32_t index = 0U; index < 8U; index++) {
        CHECK(fixture.storage[index] == 0xA5U);
        CHECK(fixture.storage[sizeof(fixture.storage) - index - 1U] == 0xA5U);
    }
    for (uint32_t block = 0U; block < capacity; block++) {
        for (uint32_t byte = HOST_BLOCK_SIZE; byte < HOST_STRIDE; byte++) {
            CHECK(fixture.storage[8U + block * HOST_STRIDE + byte] == 0xA5U);
        }
    }
    (void)printf("HOST|MAXIMUM|capacity=%" PRIu32 " lifecycle=PASS\n", capacity);
}

static host_fixture_t *script_fixture;
static rtos_channel_writer_t script_writer;
static uint32_t script_action;
#if defined(_WIN32)
static rtos_channel_t *protected_channel;
static rtos_channel_slot_t *protected_slots;
static void *protected_payload;
#endif

static void scripted_schedule(void)
{
    if (script_action == 1U) {
        rtos_internal_wake_all(&script_fixture->channel.readers, RTOS_OK);
    } else if (script_action == 2U) {
        EXPECT(rtos_channel_cancel(&script_fixture->channel, &script_writer), RTOS_OK);
    } else if (script_action == 3U) {
        EXPECT(rtos_channel_close(&script_fixture->channel), RTOS_OK);
        EXPECT(rtos_channel_deinit(&script_fixture->channel), RTOS_OK);
        EXPECT(rtos_channel_init(&script_fixture->channel, &script_fixture->config), RTOS_OK);
    } else if (script_action == 4U) {
        EXPECT(rtos_channel_close(&script_fixture->channel), RTOS_OK);
    } else if (script_action == 6U || script_action == 8U) {
        EXPECT(rtos_channel_cancel(&script_fixture->channel, &script_writer), RTOS_OK);
        if (script_action == 6U) {
            /* Wake while eligible, then defer the original caller's execution. */
            rtos_kernel.tick_count += 5U;
        }
    } else if (script_action == 7U || script_action == 9U) {
        EXPECT(rtos_channel_publish(&script_fixture->channel, &script_writer, 1U, 17U),
            RTOS_OK);
        if (script_action == 7U) {
            rtos_kernel.tick_count += 5U;
        }
#if defined(_WIN32)
    } else if (script_action == 5U) {
        DWORD previous;
        EXPECT(rtos_channel_close(protected_channel), RTOS_OK);
        EXPECT(rtos_channel_deinit(protected_channel), RTOS_OK);
        free(protected_slots);
        free(protected_payload);
        protected_slots = NULL;
        protected_payload = NULL;
        CHECK(VirtualProtect(protected_channel, 4096U, PAGE_NOACCESS, &previous) != 0);
#endif
    } else {
        CHECK(0U);
    }
}

static void directed_waits(void)
{
    host_fixture_t fixture;
    rtos_channel_writer_t writer;
    rtos_channel_reader_t reader;
    rtos_channel_reader_t before;
    start_fixture(&fixture, 1U);
    script_fixture = &fixture;
    script_action = 1U;
    host_wait_advance = 2U;
    host_schedule_action = scripted_schedule;
    rtos_kernel.tick_count = UINT32_MAX - 2U;
    memset(&reader, 0xBC, sizeof(reader));
    memcpy(&before, &reader, sizeof(before));
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 5U), RTOS_ERR_TIMEOUT);
    CHECK(memcmp(&reader, &before, sizeof(reader)) == 0);
    CHECK(host_wait_calls == 3U);
    CHECK(host_wait_budgets[0] == 5U && host_wait_budgets[1] == 3U);
    CHECK(host_wait_budgets[2] == 1U);
    CHECK(rtos_kernel.tick_count == 3U);
    rtos_channel_stats_t wait_stats;
    EXPECT(rtos_channel_get_stats(&fixture.channel, &wait_stats), RTOS_OK);
    CHECK(wait_stats.receive_waits == 3U && wait_stats.receive_timeouts == 1U);
    CHECK(wait_stats.receive_misses == 0U && wait_stats.waiting_readers == 0U);
    host_wait_advance = 0U;
    host_schedule_action = NULL;
    host_basepri = 0x20U;
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 1U), RTOS_ERR_SCHED);
    host_basepri = 0U;
    host_primask = 1U;
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 1U), RTOS_ERR_SCHED);
    host_primask = 0U;
    rtos_port_enter_critical();
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 1U), RTOS_ERR_SCHED);
    rtos_port_exit_critical();
    CHECK(fixture.channel.readers == NULL);
    host_basepri = 0x20U;
    EXPECT(rtos_channel_acquire(&fixture.channel, &script_writer, 0U), RTOS_OK);
    EXPECT(rtos_channel_cancel(&fixture.channel, &script_writer), RTOS_OK);
    host_basepri = 0U;
    host_primask = 1U;
    EXPECT(rtos_channel_acquire(&fixture.channel, &script_writer, 0U), RTOS_OK);
    EXPECT(rtos_channel_cancel(&fixture.channel, &script_writer), RTOS_OK);
    host_primask = 0U;
    rtos_port_enter_critical();
    EXPECT(rtos_channel_acquire(&fixture.channel, &script_writer, 0U), RTOS_OK);
    EXPECT(rtos_channel_cancel(&fixture.channel, &script_writer), RTOS_OK);
    rtos_port_exit_critical();
    EXPECT(rtos_channel_acquire(&fixture.channel, &script_writer, 0U), RTOS_OK);
    script_action = 2U;
    host_schedule_action = scripted_schedule;
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 10U), RTOS_OK);
    host_schedule_action = NULL;
    EXPECT(rtos_channel_cancel(&fixture.channel, &writer), RTOS_OK);
    script_action = 3U;
    host_schedule_action = scripted_schedule;
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 10U), RTOS_ERR_DELETED);
    host_schedule_action = NULL;
    CHECK(memcmp(&reader, &before, sizeof(reader)) == 0);
    script_action = 4U;
    host_schedule_action = scripted_schedule;
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, RTOS_WAIT_FOREVER), RTOS_ERR_CLOSED);
    host_schedule_action = NULL;
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_OK);
    check_guards(&fixture);
    host_reset();
}

static void directed_deadline_grants(void)
{
    host_fixture_t fixture;
    rtos_channel_writer_t writer;
    rtos_channel_writer_t before_writer;
    rtos_channel_reader_t reader;
    rtos_channel_reader_t before_reader;
    rtos_channel_stats_t stats;

    for (uint32_t receive = 0U; receive < 2U; receive++) {
        for (uint32_t mode = 0U; mode < 3U; mode++) {
            start_fixture(&fixture, 1U);
            script_fixture = &fixture;
            EXPECT(rtos_channel_acquire(&fixture.channel, &script_writer, 0U), RTOS_OK);
            ((uint8_t *)script_writer.data)[0] = 0x96U;
            memset(&writer, 0xD1, sizeof(writer));
            memset(&reader, 0xD2, sizeof(reader));
            memcpy(&before_writer, &writer, sizeof(writer));
            memcpy(&before_reader, &reader, sizeof(reader));
            rtos_kernel.tick_count = UINT32_MAX - 2U;
            /* mode 0: early wake, late execution; 1: late arrival; 2: exact edge. */
            host_wait_advance = mode == 0U ? 1U : (mode == 1U ? 6U : 5U);
            script_action = (mode == 0U ? 6U : 8U) + receive;
            host_schedule_action = scripted_schedule;
            if (receive == 0U) {
                EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 5U), RTOS_ERR_TIMEOUT);
                CHECK(memcmp(&writer, &before_writer, sizeof(writer)) == 0);
            } else {
                EXPECT(rtos_channel_receive(&fixture.channel, &reader, 5U), RTOS_ERR_TIMEOUT);
                CHECK(memcmp(&reader, &before_reader, sizeof(reader)) == 0);
            }
            host_schedule_action = NULL;
            CHECK(host_wait_calls == 1U && host_wait_budgets[0] == 5U);
            EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
            CHECK(stats.waiting_readers == 0U && stats.waiting_writers == 0U);
            if (receive == 0U) {
                CHECK(stats.free == 1U && stats.writing == 0U);
                CHECK(stats.acquire_waits == 1U && stats.acquire_timeouts == 1U);
                CHECK(stats.acquire_misses == 0U);
                /* Already available on first entry remains an immediate finite grant. */
                EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 1U), RTOS_OK);
                EXPECT(rtos_channel_cancel(&fixture.channel, &writer), RTOS_OK);
            } else {
                CHECK(stats.queued == 1U && stats.reading == 0U);
                CHECK(stats.receive_waits == 1U && stats.receive_timeouts == 1U);
                CHECK(stats.receive_misses == 0U);
                EXPECT(rtos_channel_receive(&fixture.channel, &reader, 1U), RTOS_OK);
                CHECK(reader.length == 1U && reader.tag == 17U);
                CHECK(((const uint8_t *)reader.data)[0] == 0x96U);
                EXPECT(rtos_channel_release(&fixture.channel, &reader), RTOS_OK);
            }
            stop_fixture(&fixture);
            host_reset();
        }
    }
    (void)printf("HOST|DEADLINE|early_wake_late_run=PASS late_arrival=PASS exact_edge=PASS\n");
}

static void directed_protected_lifetime(void)
{
#if defined(_WIN32)
    rtos_channel_reader_t reader;
    rtos_channel_reader_t before;
    rtos_channel_config_t config;
    DWORD previous;
    protected_channel = VirtualAlloc(NULL, 4096U, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    protected_slots = calloc(1U, sizeof(*protected_slots));
    protected_payload = malloc(64U);
    CHECK(protected_channel != NULL && protected_slots != NULL && protected_payload != NULL);
    config.storage = protected_payload;
    config.storage_size = 64U;
    config.slots = protected_slots;
    config.slots_size = sizeof(*protected_slots);
    config.block_size = 33U;
    config.block_count = 1U;
    EXPECT(rtos_channel_init(protected_channel, &config), RTOS_OK);
    memset(&reader, 0xD7, sizeof(reader));
    memcpy(&before, &reader, sizeof(before));
    script_action = 5U;
    host_schedule_action = scripted_schedule;
    EXPECT(rtos_channel_receive(protected_channel, &reader, 10U), RTOS_ERR_DELETED);
    host_schedule_action = NULL;
    CHECK(memcmp(&reader, &before, sizeof(reader)) == 0);
    CHECK(VirtualProtect(protected_channel, 4096U, PAGE_READWRITE, &previous) != 0);
    CHECK(VirtualFree(protected_channel, 0U, MEM_RELEASE) != 0);
    protected_channel = NULL;
    host_reset();
    (void)printf("HOST|PROTECTED|old_control_noaccess=PASS\n");
#else
    (void)printf("HOST|PROTECTED|old_control_noaccess=NOT_RUN platform_not_windows\n");
#endif
}

static void directed_isr(void)
{
    host_fixture_t fixture;
    host_fixture_t other;
    rtos_channel_writer_t writer;
    rtos_channel_writer_t saved_writer;
    rtos_channel_reader_t reader;
    rtos_channel_reader_t saved_reader;
    rtos_channel_t saved_control;
    rtos_channel_stats_t before;
    rtos_channel_stats_t after;
    start_fixture(&fixture, 1U);
    prepare_fixture(&other, 1U);
    memcpy(&saved_control, &other.channel, sizeof(saved_control));
    EXPECT(rtos_channel_get_stats(&fixture.channel, &before), RTOS_OK);
    host_isr = 1U;
    for (uint32_t priority = 0U; priority < 5U; priority += 4U) {
        host_priority = priority << 4U;
        EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_ERR_ISR);
        EXPECT(rtos_channel_receive(&fixture.channel, &reader, 0U), RTOS_ERR_ISR);
    }
    host_priority = 0x50U;
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 1U), RTOS_ERR_ISR);
    EXPECT(rtos_channel_init(&other.channel, &other.config), RTOS_ERR_ISR);
    CHECK(memcmp(&saved_control, &other.channel, sizeof(saved_control)) == 0);
    EXPECT(rtos_channel_close(&fixture.channel), RTOS_ERR_ISR);
    EXPECT(rtos_channel_deinit(&fixture.channel), RTOS_ERR_ISR);
    EXPECT(rtos_channel_get_stats(&fixture.channel, &after), RTOS_ERR_ISR);
    host_isr = 0U;
    EXPECT(rtos_channel_get_stats(&fixture.channel, &after), RTOS_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    host_isr = 1U;
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_OK);
    saved_writer = writer;
    host_isr = 0U;
    EXPECT(rtos_channel_get_stats(&fixture.channel, &before), RTOS_OK);
    host_isr = 1U;
    for (uint32_t priority = 0U; priority < 5U; priority += 4U) {
        host_priority = priority << 4U;
        EXPECT(rtos_channel_publish(&fixture.channel, &writer, 1U, 6U), RTOS_ERR_ISR);
        EXPECT(rtos_channel_cancel(&fixture.channel, &writer), RTOS_ERR_ISR);
        CHECK(memcmp(&writer, &saved_writer, sizeof(writer)) == 0);
    }
    host_isr = 0U;
    EXPECT(rtos_channel_get_stats(&fixture.channel, &after), RTOS_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    host_isr = 1U;
    host_priority = 0x50U;
    memset(writer.data, 0x31, writer.capacity);
    EXPECT(rtos_channel_publish(&fixture.channel, &writer, 1U, 6U), RTOS_OK);
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 0U), RTOS_OK);
    saved_reader = reader;
    host_isr = 0U;
    EXPECT(rtos_channel_get_stats(&fixture.channel, &before), RTOS_OK);
    host_isr = 1U;
    for (uint32_t priority = 0U; priority < 5U; priority += 4U) {
        host_priority = priority << 4U;
        EXPECT(rtos_channel_release(&fixture.channel, &reader), RTOS_ERR_ISR);
        CHECK(memcmp(&reader, &saved_reader, sizeof(reader)) == 0);
    }
    host_isr = 0U;
    EXPECT(rtos_channel_get_stats(&fixture.channel, &after), RTOS_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    host_isr = 1U;
    host_priority = 0x50U;
    EXPECT(rtos_channel_release(&fixture.channel, &reader), RTOS_OK);
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_OK);
    EXPECT(rtos_channel_cancel(&fixture.channel, &writer), RTOS_OK);
    host_isr = 0U;
    stop_fixture(&fixture);
}

static void directed_saturation(void)
{
    host_fixture_t fixture;
    rtos_channel_writer_t writer;
    rtos_channel_writer_t unavailable;
    rtos_channel_writer_t old;
    rtos_channel_reader_t reader;
    rtos_channel_stats_t stats;
    start_fixture(&fixture, 1U);
    fixture.channel.stats.acquired = UINT64_MAX;
    fixture.channel.stats.published = UINT64_MAX;
    fixture.channel.stats.received = UINT64_MAX;
    fixture.channel.stats.released = UINT64_MAX;
    fixture.channel.stats.cancelled = UINT64_MAX;
    fixture.channel.stats.stale_rejections = UINT64_MAX;
    fixture.channel.stats.acquire_timeouts = UINT64_MAX;
    fixture.channel.stats.receive_timeouts = UINT64_MAX;
    fixture.channel.stats.acquire_misses = UINT64_MAX;
    fixture.channel.stats.receive_misses = UINT64_MAX;
    fixture.channel.stats.acquire_waits = UINT64_MAX;
    fixture.channel.stats.receive_waits = UINT64_MAX;
    fixture.channel.stats.closed_rejections = UINT64_MAX;
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_OK);
    old = writer;
    EXPECT(rtos_channel_acquire(&fixture.channel, &unavailable, 0U), RTOS_ERR_TIMEOUT);
    EXPECT(rtos_channel_publish(&fixture.channel, &writer, 0U, 0U), RTOS_OK);
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 0U), RTOS_OK);
    EXPECT(rtos_channel_release(&fixture.channel, &reader), RTOS_OK);
    EXPECT(rtos_channel_cancel(&fixture.channel, &old), RTOS_ERR_STALE);
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_OK);
    EXPECT(rtos_channel_cancel(&fixture.channel, &writer), RTOS_OK);
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 0U), RTOS_ERR_TIMEOUT);
    EXPECT(rtos_channel_get_stats(&fixture.channel, &stats), RTOS_OK);
    CHECK(stats.acquired == UINT64_MAX && stats.published == UINT64_MAX);
    CHECK(stats.received == UINT64_MAX && stats.released == UINT64_MAX);
    CHECK(stats.cancelled == UINT64_MAX && stats.stale_rejections == UINT64_MAX);
    CHECK(stats.acquire_timeouts == UINT64_MAX && stats.receive_timeouts == UINT64_MAX);
    CHECK(stats.acquire_misses == UINT64_MAX && stats.receive_misses == UINT64_MAX);
    CHECK(stats.acquire_waits == UINT64_MAX && stats.receive_waits == UINT64_MAX);
    fixture.channel.next_ticket = UINT64_MAX - 1U;
    memset(&writer, 0xAB, sizeof(writer));
    memcpy(&old, &writer, sizeof(old));
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_ERR_OVERFLOW);
    CHECK(memcmp(&writer, &old, sizeof(writer)) == 0);
    stop_fixture(&fixture);
}

static void negative_payload_detector(void)
{
    host_fixture_t fixture;
    rtos_channel_writer_t writer;
    rtos_channel_reader_t reader;
    uint8_t expected[HOST_BLOCK_SIZE];
    memset(expected, 0x76, sizeof(expected));
    start_fixture(&fixture, 1U);
    EXPECT(rtos_channel_acquire(&fixture.channel, &writer, 0U), RTOS_OK);
    memcpy(writer.data, expected, sizeof(expected));
    uint8_t *fault_address = writer.data;
    EXPECT(rtos_channel_publish(&fixture.channel, &writer, sizeof(expected), 0U), RTOS_OK);
    /* Deliberate external corruption tests the oracle, not automatic API CRC. */
    fault_address[11] ^= 1U;
    EXPECT(rtos_channel_receive(&fixture.channel, &reader, 0U), RTOS_OK);
    CHECK(memcmp(reader.data, expected, sizeof(expected)) != 0);
    EXPECT(rtos_channel_release(&fixture.channel, &reader), RTOS_OK);
    stop_fixture(&fixture);
    (void)printf("HOST|NEGATIVE|payload_detector=PASS injected=1 detected=1\n");
}

int main(void)
{
    host_reset();
    current_case = "geometry_and_overlap";
    directed_geometry();
    current_case = "ownership_transfer";
    directed_ownership();
    current_case = "registry_limit";
    directed_registry();
    current_case = "active_descriptor_overwrite_contract";
    directed_descriptor_overwrite();
    current_case = "maximum_legal_capacity";
    directed_maximum_capacity();
    current_case = "scripted_waits";
    directed_waits();
    current_case = "finite_grant_deadline";
    directed_deadline_grants();
    current_case = "protected_old_control_lifetime";
    directed_protected_lifetime();
    current_case = "simulated_isr_api_contract";
    directed_isr();
    current_case = "whitebox_saturation";
    directed_saturation();
    current_case = "negative_fixture_detector";
    negative_payload_detector();
    current_case = "model_capacity1";
    random_model(1U);
    current_case = "model_capacity3";
    random_model(3U);
    current_case = "model_capacity16";
    random_model(16U);
    CHECK(model_operations >= 200000U);
    CHECK(rtos_port_crit_nest == 0U);
    (void)printf("HOST|SUMMARY|checks=%" PRIu64 " api_calls=%" PRIu64
        " model_operations=%" PRIu64 " failures=0 sanitizer=none\n",
        checks, api_calls, model_operations);
    (void)printf("HOST|DONE|status=0\n");
    return 0;
}

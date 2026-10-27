#include "rtos_channel.h"

#include <stdint.h>
#include <string.h>

#define SAMPLE_BLOCK_BYTES (256U)
#define SAMPLE_BLOCK_COUNT (4U)
#define SAMPLE_WAIT_TICKS (10U)

typedef struct sample_channel {
    rtos_channel_t channel;
    rtos_channel_slot_t slots[SAMPLE_BLOCK_COUNT];
    _Alignas(RTOS_CHANNEL_ALIGNMENT)
        uint8_t storage[SAMPLE_BLOCK_COUNT * SAMPLE_BLOCK_BYTES];
    rtos_channel_writer_t writer;
    rtos_channel_reader_t reader;
} sample_channel_t;

static sample_channel_t s_sample;

rtos_status_t sample_channel_init(void)
{
    const rtos_channel_config_t config = {
        .storage = s_sample.storage,
        .storage_size = sizeof(s_sample.storage),
        .slots = s_sample.slots,
        .slots_size = sizeof(s_sample.slots),
        .block_size = SAMPLE_BLOCK_BYTES,
        .block_count = SAMPLE_BLOCK_COUNT,
    };

    return rtos_channel_init(&s_sample.channel, &config);
}

rtos_status_t sample_channel_produce(uint8_t value, uint32_t tag)
{
    if (s_sample.writer.data != NULL) {
        return RTOS_ERR_BUSY; /* 先处理上次未归还的能力。 */
    }

    rtos_status_t status = rtos_channel_acquire(&s_sample.channel,
        &s_sample.writer, SAMPLE_WAIT_TICKS);
    if (status != RTOS_OK) {
        return status;
    }

    memset(s_sample.writer.data, value, SAMPLE_BLOCK_BYTES);
    status = rtos_channel_publish(&s_sample.channel, &s_sample.writer,
        SAMPLE_BLOCK_BYTES, tag);
    if (status != RTOS_OK) {
        rtos_status_t cleanup = rtos_channel_cancel(&s_sample.channel, &s_sample.writer);
        if (cleanup != RTOS_OK) {
            return cleanup; /* 保留静态 writer，交由故障处理定位。 */
        }
    }
    return status;
}

rtos_status_t sample_channel_consume(uint32_t *sum)
{
    if (sum == NULL) {
        return RTOS_ERR_PARAM;
    }
    if (s_sample.reader.data != NULL) {
        return RTOS_ERR_BUSY;
    }

    rtos_status_t status = rtos_channel_receive(&s_sample.channel,
        &s_sample.reader, SAMPLE_WAIT_TICKS);
    if (status != RTOS_OK) {
        return status;
    }

    const uint8_t *bytes = s_sample.reader.data;
    uint32_t result = 0U;
    for (uint32_t index = 0U; index < s_sample.reader.length; index++) {
        result += bytes[index];
    }
    *sum = result;
    return rtos_channel_release(&s_sample.channel, &s_sample.reader);
}

rtos_status_t sample_channel_stop(void)
{
    rtos_status_t status = rtos_channel_close(&s_sample.channel);
    if ((status != RTOS_OK) && (status != RTOS_ERR_ALREADY)) {
        return status;
    }
    if (s_sample.writer.data != NULL) {
        status = rtos_channel_cancel(&s_sample.channel, &s_sample.writer);
        if (status != RTOS_OK) {
            return status;
        }
    }
    if (s_sample.reader.data != NULL) {
        status = rtos_channel_release(&s_sample.channel, &s_sample.reader);
        if (status != RTOS_OK) {
            return status;
        }
    }
    for (;;) {
        status = rtos_channel_receive(&s_sample.channel, &s_sample.reader, RTOS_NO_WAIT);
        if (status == RTOS_ERR_CLOSED) {
            break;
        }
        if (status != RTOS_OK) {
            return status;
        }
        /* 此处执行本示例明确选择的丢弃策略。 */
        status = rtos_channel_release(&s_sample.channel, &s_sample.reader);
        if (status != RTOS_OK) {
            return status;
        }
    }

    rtos_channel_stats_t stats;
    status = rtos_channel_get_stats(&s_sample.channel, &stats);
    if (status != RTOS_OK) {
        return status;
    }
    if (stats.free != stats.capacity) {
        return RTOS_ERR_BUSY;
    }
    return rtos_channel_deinit(&s_sample.channel);
}

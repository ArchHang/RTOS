#include "rtos_channel.h"
int channel_public_probe(void)
{
    rtos_channel_t channel;
    rtos_channel_slot_t slots[1];
    rtos_channel_writer_t writer;
    rtos_channel_reader_t reader;
    rtos_channel_stats_t stats;
    unsigned char storage[8];
    rtos_channel_config_t config;
    config.storage = storage;
    config.storage_size = sizeof(storage);
    config.slots = slots;
    config.slots_size = sizeof(slots);
    config.block_size = 8U;
    config.block_count = 1U;
    (void)rtos_channel_init(&channel, &config);
    (void)rtos_channel_acquire(&channel, &writer, RTOS_NO_WAIT);
    (void)rtos_channel_publish(&channel, &writer, 0U, 0U);
    (void)rtos_channel_cancel(&channel, &writer);
    (void)rtos_channel_receive(&channel, &reader, RTOS_NO_WAIT);
    (void)rtos_channel_release(&channel, &reader);
    (void)rtos_channel_get_stats(&channel, &stats);
    (void)rtos_channel_close(&channel);
    return (int)rtos_channel_deinit(&channel);
}

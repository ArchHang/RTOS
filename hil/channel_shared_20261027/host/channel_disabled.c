/** @file channel_disabled.c
 *  @brief Disabled APIs fail closed and preserve all supplied objects/views. */
#include "host_port.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "rtos_channel.h"

int main(void)
{
    rtos_channel_t channel;
    rtos_channel_t channel_before;
    rtos_channel_writer_t writer;
    rtos_channel_writer_t writer_before;
    rtos_channel_reader_t reader;
    rtos_channel_reader_t reader_before;
    rtos_channel_stats_t stats;
    rtos_channel_stats_t stats_before;
    rtos_channel_config_t config;
    uint32_t passed = 0U;
    memset(&channel, 0xA5, sizeof(channel));
    memset(&writer, 0xB6, sizeof(writer));
    memset(&reader, 0xC7, sizeof(reader));
    memset(&stats, 0xD8, sizeof(stats));
    memset(&config, 0xE9, sizeof(config));
    memcpy(&channel_before, &channel, sizeof(channel));
    memcpy(&writer_before, &writer, sizeof(writer));
    memcpy(&reader_before, &reader, sizeof(reader));
    memcpy(&stats_before, &stats, sizeof(stats));
    passed += rtos_channel_init(&channel, &config) == RTOS_ERR_DISABLED;
    passed += rtos_channel_acquire(&channel, &writer, 1U) == RTOS_ERR_DISABLED;
    passed += rtos_channel_publish(&channel, &writer, 1U, 2U) == RTOS_ERR_DISABLED;
    passed += rtos_channel_receive(&channel, &reader, 1U) == RTOS_ERR_DISABLED;
    passed += rtos_channel_release(&channel, &reader) == RTOS_ERR_DISABLED;
    passed += rtos_channel_cancel(&channel, &writer) == RTOS_ERR_DISABLED;
    passed += rtos_channel_close(&channel) == RTOS_ERR_DISABLED;
    passed += rtos_channel_deinit(&channel) == RTOS_ERR_DISABLED;
    passed += rtos_channel_get_stats(&channel, &stats) == RTOS_ERR_DISABLED;
    passed += memcmp(&channel, &channel_before, sizeof(channel)) == 0;
    passed += memcmp(&writer, &writer_before, sizeof(writer)) == 0;
    passed += memcmp(&reader, &reader_before, sizeof(reader)) == 0;
    passed += memcmp(&stats, &stats_before, sizeof(stats)) == 0;
    passed += rtos_channel_init(NULL, NULL) == RTOS_ERR_DISABLED;
    passed += rtos_channel_acquire(NULL, NULL, 0U) == RTOS_ERR_DISABLED;
    passed += rtos_channel_receive(NULL, NULL, 0U) == RTOS_ERR_DISABLED;
    uint32_t failed = passed != 16U;
    (void)printf("DISABLED|SUMMARY|pass=%" PRIu32 " fail=%" PRIu32 "\n", passed, failed);
    (void)printf("DISABLED|DONE|status=%" PRIu32 "\n", failed);
    return failed != 0U ? 1 : 0;
}

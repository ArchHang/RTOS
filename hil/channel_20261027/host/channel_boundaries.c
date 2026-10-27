/**
 * @file channel_boundaries.c
 * @brief White-box exhaustion checks including the unchanged production file.
 *
 * A separate executable deliberately reaches private counter boundaries. The
 * ordinary model links the real production translation unit independently.
 */
#include "host_port.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../RTOS/IPC/rtos_channel.c"

static uint32_t checks;

static void check(uint32_t condition, const char *expression, uint32_t line)
{
    checks++;
    if (condition == 0U) {
        (void)fprintf(stderr, "BOUNDARY|FAIL|line=%" PRIu32 " expr=%s\n", line, expression);
        exit(1);
    }
}

#define CHECK(expression) check((expression) != 0, #expression, __LINE__)

int main(void)
{
    _Alignas(8) uint8_t data_first[64];
    _Alignas(8) uint8_t data_second[64];
    rtos_channel_slot_t slots_first[1];
    rtos_channel_slot_t slots_second[1];
    rtos_channel_t first;
    rtos_channel_t second;
    rtos_channel_t before;
    rtos_channel_config_t config_first = {
        data_first, sizeof(data_first), slots_first, sizeof(slots_first), 33U, 1U
    };
    rtos_channel_config_t config_second = {
        data_second, sizeof(data_second), slots_second, sizeof(slots_second), 33U, 1U
    };
    rtos_channel_writer_t writer;
    rtos_channel_reader_t reader;
    host_reset();
    memset(&first, 0xA6, sizeof(first));
    memset(&second, 0xB8, sizeof(second));
    memcpy(&before, &second, sizeof(before));
    channel_last_incarnation = UINT64_MAX - 1U;
    CHECK(rtos_channel_init(&first, &config_first) == RTOS_OK);
    CHECK(first.incarnation == UINT64_MAX);
    CHECK(rtos_channel_init(&second, &config_second) == RTOS_ERR_OVERFLOW);
    CHECK(memcmp(&second, &before, sizeof(before)) == 0);
    CHECK(channel_last_incarnation == UINT64_MAX);
    CHECK(rtos_channel_acquire(&first, &writer, 0U) == RTOS_OK);
    first.next_ticket = UINT64_MAX - 3U;
    CHECK(rtos_channel_cancel(&first, &writer) == RTOS_OK);
    CHECK(rtos_channel_acquire(&first, &writer, 0U) == RTOS_OK);
    CHECK(writer.ticket.generation == UINT64_MAX - 2U);
    CHECK(first.next_ticket == UINT64_MAX - 1U);
    CHECK(rtos_channel_publish(&first, &writer, 0U, 0U) == RTOS_OK);
    CHECK(rtos_channel_receive(&first, &reader, 0U) == RTOS_OK);
    CHECK(reader.ticket.generation == UINT64_MAX - 1U);
    CHECK(rtos_channel_release(&first, &reader) == RTOS_OK);
    memset(&writer, 0xC9, sizeof(writer));
    rtos_channel_writer_t writer_before;
    memcpy(&writer_before, &writer, sizeof(writer_before));
    CHECK(rtos_channel_acquire(&first, &writer, 0U) == RTOS_ERR_OVERFLOW);
    CHECK(memcmp(&writer_before, &writer, sizeof(writer)) == 0);
    CHECK(first.stats.free == 1U && first.stats.writing == 0U);
    CHECK(rtos_channel_close(&first) == RTOS_OK);
    CHECK(rtos_channel_deinit(&first) == RTOS_OK);
    CHECK(rtos_channel_init(&first, &config_first) == RTOS_ERR_OVERFLOW);
    CHECK(channel_last_incarnation == UINT64_MAX);
    CHECK(rtos_port_crit_nest == 0U);
    (void)printf("BOUNDARY|SUMMARY|checks=%" PRIu32 " failures=0\n", checks);
    (void)printf("BOUNDARY|DONE|status=0\n");
    return 0;
}

/**
 * @file channel_fixture.c
 * @brief Shared result accounting and deterministic in-place payload helpers.
 */
#include "channel_fixture.h"

static uint32_t ch_pass_count;
static uint32_t ch_fail_count;
static uint32_t ch_checksum_sink;

void ch_expect(const char *name, uint32_t condition)
{
    if (condition != 0U) {
        ch_pass_count++;
    } else {
        ch_fail_count++;
    }
    (void)printf("CH|CHECK|%s|id=%lu name=%s\r\n",
        condition != 0U ? "PASS" : "FAIL",
        (unsigned long)(ch_pass_count + ch_fail_count), name);
}

void ch_finish(const char *mode)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t checksum = ch_checksum_sink;
    RTOS_PORT_EXIT_CRITICAL();
    (void)printf("CH|CHECKSUM|value=%08lx\r\n", (unsigned long)checksum);
    (void)printf("CH|SUMMARY|mode=%s pass=%lu fail=%lu\r\n", mode,
        (unsigned long)ch_pass_count, (unsigned long)ch_fail_count);
    (void)printf("CH|DONE|status=%lu\r\n", ch_fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

uint8_t ch_pattern(uint32_t seed, uint32_t offset)
{
    return (uint8_t)((seed * 33U) ^ (seed >> ((offset & 3U) * 8U))
        ^ offset ^ (offset >> 3U));
}

void ch_fill(void *buffer, uint32_t length, uint32_t seed)
{
    uint8_t *bytes = buffer;

    for (uint32_t index = 0U; index < length; index++) {
        bytes[index] = ch_pattern(seed, index);
    }
}

uint32_t ch_verify(const void *buffer, uint32_t length, uint32_t seed)
{
    const uint8_t *bytes = buffer;
    uint32_t checksum = 2166136261U;
    uint32_t valid = 1U;

    for (uint32_t index = 0U; index < length; index++) {
        valid &= bytes[index] == ch_pattern(seed, index);
        checksum = (checksum ^ bytes[index]) * 16777619U;
    }
    RTOS_PORT_ENTER_CRITICAL();
    ch_checksum_sink ^= checksum;
    RTOS_PORT_EXIT_CRITICAL();
    return valid;
}

uint32_t ch_conserved(const rtos_channel_stats_t *stats)
{
    return stats->capacity == stats->free + stats->writing + stats->queued + stats->reading;
}

uint32_t ch_wait_blocked(rtos_tcb_t *task, const void *object)
{
    for (uint32_t elapsed = 0U; elapsed < 100U; elapsed++) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t blocked = (task->blocked_on == object)
            && ((task->state == RTOS_TASK_BLOCKED) || (task->state == RTOS_TASK_DELAYED));
        RTOS_PORT_EXIT_CRITICAL();
        if (blocked != 0U) {
            return 1U;
        }
        rtos_task_delay(1U);
    }
    return 0U;
}


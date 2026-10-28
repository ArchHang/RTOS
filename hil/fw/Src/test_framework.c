/**
 * @file    test_framework.c
 * @brief   场景测试的统计与上报工具实现。
 *
 * @details 见 test_framework.h。本文件只依赖标准库与 kern_port.h, 两侧内核
 *          共用同一份实现, 保证统计口径完全一致。
 */
#include "test_framework.h"
#include "kern_port.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/** @brief 单行上报的最大字节数(含结束符)。 */
#define TEST_LINE_MAX 128U

/** @brief 统计器的空值哨兵(比任何实测周期都大)。 */
#define TEST_STATS_MIN_INIT 0xFFFFFFFFU

void test_stats_reset(test_stats_t *stats, uint32_t threshold)
{
    if (stats == NULL) {
        return;
    }
    stats->min = TEST_STATS_MIN_INIT;
    stats->max = 0U;
    stats->sum = 0U;
    stats->count = 0U;
    stats->bad = 0U;
    stats->done = 0U;
    stats->bad_threshold = threshold;
}

void test_stats_add(test_stats_t *stats, uint32_t value)
{
    if (stats == NULL) {
        return;
    }
    if (value < stats->min) {
        stats->min = value;
    }
    if (value > stats->max) {
        stats->max = value;
    }
    stats->sum += (uint64_t)value;
    stats->count = stats->count + 1U;
    if ((stats->bad_threshold != 0U) && (value > stats->bad_threshold)) {
        stats->bad = stats->bad + 1U;
    }
}

uint32_t test_stats_count(const test_stats_t *stats)
{
    return (stats != NULL) ? stats->count : 0U;
}

uint32_t test_stats_avg(const test_stats_t *stats)
{
    if ((stats == NULL) || (stats->count == 0U)) {
        return 0U;
    }
    return (uint32_t)(stats->sum / (uint64_t)stats->count);
}

void test_report_stats(const char *id, const char *name, const test_stats_t *stats)
{
    if ((id == NULL) || (name == NULL) || (stats == NULL)) {
        return;
    }
    uint32_t n = stats->count;
    uint32_t avg = test_stats_avg(stats);
    uint32_t min = (stats->count == 0U) ? 0U : stats->min;

    /* 整行在临界区内一次成形并发出: 传输期间不切任务, 避免 HAL 串口状态被
     * 另一个任务重入。行长度 < TEST_LINE_MAX。 */
    char line[TEST_LINE_MAX];

    (void)snprintf(line, sizeof(line),
                   "SC|%s|%s|N=%lu|avg=%lu|min=%lu|max=%lu|bad=%lu|us_avg=%lu\r\n", id, name,
                   (unsigned long)n, (unsigned long)avg, (unsigned long)min,
                   (unsigned long)stats->max, (unsigned long)stats->bad,
                   (unsigned long)kern_cycles_to_us(avg));

    kern_enter_critical();
    (void)fputs(line, stdout);
    kern_exit_critical();
}

void test_report_value(const char *id, const char *name, uint32_t value, const char *unit)
{
    if ((id == NULL) || (name == NULL) || (unit == NULL)) {
        return;
    }
    printf("SC|%s|%s|N=1|value=%lu|unit=%s\r\n", id, name, (unsigned long)value, unit);
}

void test_log(const char *format, ...)
{
    char line[TEST_LINE_MAX];
    va_list args;
    int written;

    (void)memcpy(line, "SC|LOG|", 7U);
    va_start(args, format);
    written = vsnprintf(line + 7, sizeof(line) - 7U, format, args);
    va_end(args);
    if (written < 0) {
        return;
    }

    /* 一次成形、临界区内发出: 避免与其他任务的 HAL 串口传输重入 */
    kern_enter_critical();
    (void)fputs(line, stdout);
    (void)fputs("\r\n", stdout);
    kern_exit_critical();
}

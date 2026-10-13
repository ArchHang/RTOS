/**
 * @file    test_framework.h
 * @brief   场景测试的统计与上报工具(两侧内核共用的纯 C 代码)。
 *
 * @details 只依赖 <stdint.h> 与 kern_port.h, 不含任何内核类型, 因此同一份
 *          代码可编译进两套固件。流式统计 (不缓存样本数组) —— 嵌入式 RAM
 *          受限, 单写者任务在线更新, 汇总任务在 done 之后读取。
 *
 *          输出协议(每场景若干行, 便于上位机解析):
 *            SC|<id>|<name>|N=<n>|avg=<v>|min=<v>|max=<v>|bad=<n>|<unit>
 *          单位固定为 CPU 周期(cycle); 需要微秒时由上位机按 5.952ns 换算,
 *          或读同一行的 us_avg 字段。
 */
#ifndef TEST_FRAMEWORK_H_
#define TEST_FRAMEWORK_H_

#include <stdint.h>

/** @brief 流式统计器: 供被测量任务写入、汇总任务读取。 */
typedef struct {
    volatile uint32_t min; /**< 最小值(初值 0xFFFFFFFF)。 */
    volatile uint32_t max; /**< 最大值。 */
    volatile uint64_t sum; /**< 累加和(64 位防溢出)。 */
    volatile uint32_t count; /**< 样本数。 */
    volatile uint32_t bad; /**< 超出调用方给定阈值的样本数。 */
    volatile uint32_t done; /**< 写者完成标志。 */
    uint32_t bad_threshold; /**< 判"坏"的阈值(0 = 不判)。 */
} test_stats_t;

/** @brief 重置统计器。
 *  @param stats     统计器。
 *  @param threshold 判坏阈值(0 表示不启用坏样本计数)。
 */
void test_stats_reset(test_stats_t *stats, uint32_t threshold);

/** @brief 记录一个样本。 */
void test_stats_add(test_stats_t *stats, uint32_t value);

/** @brief 样本数。 */
uint32_t test_stats_count(const test_stats_t *stats);

/** @brief 平均值(无样本时返回 0)。 */
uint32_t test_stats_avg(const test_stats_t *stats);

/**
 * @brief 上报一个场景的结果行。
 * @param id    场景编号(字符串, 如 "S1")。
 * @param name  场景名(英文短名, 便于两侧对照)。
 * @param stats 统计器。
 */
void test_report_stats(const char *id, const char *name, const test_stats_t *stats);

/**
 * @brief 上报一个单值结果(如吞吐量、内存字节数)。
 * @param id    场景编号。
 * @param name  场景名。
 * @param value 数值。
 * @param unit  单位字符串(如 "cyc"、"KB/s"、"bytes")。
 */
void test_report_value(const char *id, const char *name, uint32_t value, const char *unit);

/** @brief 打印一行自由文本(带统一前缀, 便于抓取)。 */
void test_log(const char *format, ...);

#endif /* TEST_FRAMEWORK_H_ */

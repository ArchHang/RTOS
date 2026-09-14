/**
 * @file    rtos_event.h
 * @brief   事件标志组接口
 *
 * @details 事件标志组提供 24 位(可配置)标志，任务可同时等待多个事件的
 *          任意一位(OR)或全部位(AND)。典型用途:
 *            - 多个传感器就绪后启动融合任务(AND)。
 *            - 任一故障发生即触发报警(OR)。
 *          标志位在触发后默认保持(需手动清除)或消费式清除。
 *
 *          STM32 系列本身有 EXTI 外部中断，事件标志组是其软件层补充。
 */
#ifndef RTOS_EVENT_H_
#define RTOS_EVENT_H_

#include <stdint.h>
#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 事件位类型(24 位可用，最高 8 位保留作内部标志)。 */
typedef uint32_t rtos_event_bits_t;

/** @brief 等待模式: 任一位置位(OR)或全部置位(AND)。 */
#define RTOS_EVENT_WAIT_ANY (0U) /**< 任一所等待位被置位即唤醒。 */
#define RTOS_EVENT_WAIT_ALL (1U) /**< 全部所等待位都被置位才唤醒。 */

/** @brief 退出时是否清除所等待的位。 */
#define RTOS_EVENT_CLEAR_ON_EXIT (1U << 24)

/**
 * @struct rtos_event
 * @brief  事件标志组对象。
 */
struct rtos_event {
    uint8_t is_initialized;
    uint8_t in_use;
    volatile rtos_event_bits_t bits; /**< 当前标志位。 */
    rtos_wait_node_t *wait_head; /**< 等待链表。 */
};

/**
 * @brief 初始化事件标志组。
 */
rtos_status_t rtos_event_init(rtos_event_t *event);

/**
 * @brief 设置标志位(可在中断中调用)。
 * @param event 事件组。
 * @param bits  要置位的位掩码。
 * @return 操作后的全部标志位。
 */
rtos_event_bits_t rtos_event_set(rtos_event_t *event, rtos_event_bits_t bits);

/**
 * @brief 清除标志位。
 */
rtos_event_bits_t rtos_event_clear(rtos_event_t *event, rtos_event_bits_t bits);

/**
 * @brief 等待事件。
 * @param event   事件组。
 * @param bits    等待的位掩码。
 * @param mode    RTOS_EVENT_WAIT_ANY / RTOS_EVENT_WAIT_ALL，可 | RTOS_EVENT_CLEAR_ON_EXIT。
 * @param timeout 超时。
 * @return 满足条件时的当前标志位；超时返回当前位(未满足)。
 */
rtos_event_bits_t rtos_event_wait(rtos_event_t *event, rtos_event_bits_t bits, uint32_t mode,
                                  rtos_tick_t timeout);

/** @brief 获取当前标志位。 */
rtos_event_bits_t rtos_event_get(const rtos_event_t *event);

/** @brief 销毁事件组。 */
rtos_status_t rtos_event_deinit(rtos_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_EVENT_H_ */

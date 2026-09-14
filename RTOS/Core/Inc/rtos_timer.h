/**
 * @file    rtos_timer.h
 * @brief   软件定时器接口
 *
 * @details 软件定时器允许任务注册在指定时刻触发的回调。本实现:
 *            - 单次定时器: 触发一次后停止。
 *            - 周期定时器: 每次触发后自动按周期重新计时。
 *          定时器回调运行在专用的定时器服务任务上下文中(非中断)，因此
 *          回调内可调用阻塞 API(但需注意阻塞会延迟后续定时器处理)。
 *
 *          命令通过命令队列从任意上下文(任务/ISR)发送到定时器任务，
 *          定时器任务消费命令并更新定时器链表。这种设计避免了在 ISR 中
 *          操作定时器链表的临界区问题。
 */
#ifndef RTOS_TIMER_H_
#define RTOS_TIMER_H_

#include <stdint.h>
#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 定时器模式。 */
typedef enum {
    RTOS_TIMER_ONE_SHOT = 0, /**< 单次定时器。 */
    RTOS_TIMER_PERIODIC = 1, /**< 周期定时器。 */
} rtos_timer_mode_t;

/**
 * @struct rtos_timer
 * @brief  软件定时器对象。
 */
struct rtos_timer {
    uint8_t is_initialized;
    uint8_t in_use;
    uint8_t is_active; /**< 是否已启动。 */
    rtos_timer_mode_t mode; /**< 单次/周期。 */
    rtos_tick_t period; /**< 周期(Tick)。 */
    rtos_tick_t expire_tick; /**< 到期时刻。 */
    rtos_timer_cb_t callback; /**< 回调函数。 */
    void *arg; /**< 回调参数。 */
    const char *name; /**< 定时器名。 */
    struct rtos_timer *next; /**< 活跃链表后继。 */
    struct rtos_timer *prev; /**< 活跃链表前驱。 */
};

/**
 * @brief 创建定时器(静态)。
 * @param timer    定时器对象。
 * @param name     名字。
 * @param callback 回调(运行于定时器任务上下文)。
 * @param arg      回调参数。
 * @param period   周期(Tick)。
 * @param mode     单次/周期。
 */
rtos_status_t rtos_timer_create(rtos_timer_t *timer, const char *name, rtos_timer_cb_t callback,
                                void *arg, rtos_tick_t period, rtos_timer_mode_t mode);

/** @brief 启动定时器。 */
rtos_status_t rtos_timer_start(rtos_timer_t *timer);

/** @brief 停止定时器。 */
rtos_status_t rtos_timer_stop(rtos_timer_t *timer);

/** @brief 修改周期(停止后重启)。 */
rtos_status_t rtos_timer_change_period(rtos_timer_t *timer, rtos_tick_t period);

/** @brief 删除定时器。 */
rtos_status_t rtos_timer_delete(rtos_timer_t *timer);

/** @brief 定时器服务任务(内部使用，由 rtos_init 创建)。 */
void rtos_timer_service_task(void *arg);

/** @brief 初始化定时器子系统(内部使用)。 */
rtos_status_t rtos_timer_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_TIMER_H_ */

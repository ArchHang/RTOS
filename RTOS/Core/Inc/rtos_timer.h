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
 *          命令通过命令队列从其他任务/ISR发送到定时器任务，
 *          定时器任务消费命令并更新定时器链表。这种设计避免了在 ISR 中
 *          操作定时器链表的临界区问题。
 *          服务任务的回调自身调用 timer API 时就地执行，避免等自己的命令队列。
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
    uint8_t is_active; /**< 是否已启动。 */
    /**
     * @brief 回调执行窗口内"被 stop/delete"的一次性标记(内核内部使用)。
     * @details 服务任务的顺序是"摘链 → 跑回调 → 重插周期定时器"。回调中调用
     *          rtos_timer_stop/delete 时该定时器已被摘链, 其 `is_active` 为 0,
     *          普通的"在链则摘"逻辑成为空操作, 无法阻止随后的重插 —— 表现为
     *          "回调里停不掉自己, 周期定时器一直触发"。
     *          本字段就是该窗口内的唯一痕迹: 置位后 timer_reinsert_periodic
     *          跳过重插并复位。用户代码不应读写。
     */
    uint8_t deactivated;
    uint32_t generation; /**< 创建代次，防止旧队列命令操作重建后的对象。 */
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
 * @note 首次创建不要求预先清零 storage。仍在活跃链或正在执行回调的对象
 *       返回 RTOS_ERR_ALREADY；已停止/已到期对象可重建。
 */
rtos_status_t rtos_timer_create(rtos_timer_t *timer, const char *name, rtos_timer_cb_t callback,
                                void *arg, rtos_tick_t period, rtos_timer_mode_t mode);

/** @brief 启动定时器。 */
rtos_status_t rtos_timer_start(rtos_timer_t *timer);

/** @brief 停止定时器。 */
rtos_status_t rtos_timer_stop(rtos_timer_t *timer);

/** @brief 修改周期(停止后重启)。 */
rtos_status_t rtos_timer_change_period(rtos_timer_t *timer, rtos_tick_t period);

/** @brief 异步删除定时器。
 * @note 返回 OK 表示命令已接受。storage 必须保持有效直到命令被服务任务消费，
 *       且正在执行的回调已返回；回调中自删也不能立即释放该对象的 storage。
 *       同一有效 storage 被重新 create 后，旧代次命令自动丢弃。 */
rtos_status_t rtos_timer_delete(rtos_timer_t *timer);

/** @brief 定时器服务任务(内部使用，由 rtos_init 创建)。 */
void rtos_timer_service_task(void *arg);

/** @brief 初始化定时器子系统(内部使用)。
 * @note 仅在线程模式调用；成功后重复调用返回 RTOS_ERR_ALREADY，
 *       保留已有任务、命令及活跃定时器；初始化失败时允许重试。 */
rtos_status_t rtos_timer_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_TIMER_H_ */

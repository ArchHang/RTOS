/**
 * @file    rtos_mutex.h
 * @brief   互斥锁接口(含优先级继承)
 *
 * @details 互斥锁用于保护共享资源的互斥访问。与信号量的区别:
 *            - 互斥锁有"拥有者"概念，只有拥有者能释放。
 *            - 实现优先级继承: 当高优先级任务等待低优先级任务持有的锁时，
 *              临时提升持有者优先级，避免优先级反转。
 *            - 不允许在中断中获取/释放。
 *          支持嵌套(同一任务可多次获取，需等次数释放)。
 */
#ifndef RTOS_MUTEX_H_
#define RTOS_MUTEX_H_

#include <stdint.h>
#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct rtos_mutex
 * @brief  互斥锁对象。
 */
struct rtos_mutex {
    uint8_t is_initialized;
    uint8_t in_use;
    rtos_tcb_t *owner; /**< 当前持有者(NULL=未持有)。 */
    uint32_t recursion_count; /**< 递归获取计数。 */
    rtos_prio_t owner_orig_priority; /**< 持有者原始优先级(继承后恢复)。 */
    rtos_wait_node_t *wait_head; /**< 等待链表头(按优先级排序)。 */
};

/**
 * @brief 初始化互斥锁。
 */
rtos_status_t rtos_mutex_init(rtos_mutex_t *mutex);

/**
 * @brief 获取互斥锁(可递归)。
 * @param mutex    互斥锁。
 * @param timeout  超时。RTOS_WAIT_FOREVER 为永久等待。
 * @return RTOS_OK / RTOS_ERR_TIMEOUT。
 * @note   不可在中断中调用。
 */
rtos_status_t rtos_mutex_take(rtos_mutex_t *mutex, rtos_tick_t timeout);

/**
 * @brief 释放互斥锁(递归次数归零后真正释放，触发优先级恢复与唤醒)。
 * @note   不可在中断中调用。
 */
rtos_status_t rtos_mutex_give(rtos_mutex_t *mutex);

/** @brief 销毁互斥锁。 */
rtos_status_t rtos_mutex_deinit(rtos_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_MUTEX_H_ */

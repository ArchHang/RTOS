/**
 * @file    rtos_sem.h
 * @brief   信号量接口
 *
 * @details 信号量用于任务间同步与资源计数。本实现支持:
 *            - 计数信号量: 用于管理多个同类资源(如 DMA 通道)。
 *            - 二值信号量: 计数上限为 1，用于简单同步。
 *          阻塞等待链为"尾插 + 优先级位图"结构: 入链 O(1), 唤醒时由位图
 *          (__builtin_ctz)选出最高优先级等待者, 不依赖链内顺序。
 *          信号量内部不维护拥有者，因此不解决优先级反转(用互斥锁)。
 */
#ifndef RTOS_SEM_H_
#define RTOS_SEM_H_

#include <stdint.h>
#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct rtos_sem
 * @brief  计数信号量对象。
 */
struct rtos_sem {
    uint8_t is_initialized; /**< 初始化标志(对象池复用判断)。 */
    volatile uint32_t count; /**< 当前可用资源数。 */
    uint32_t max_count; /**< 最大计数上限。 */
    rtos_wait_node_t *wait_head; /**< 等待链头(仅维护同优先级先后, 唤醒序由位图决定)。 */
    /** @brief 等待链中"存在等待者"的优先级位图(bit p = 优先级 p 有人等)。
     *  @details 与 wait_head 配合把"入链"从 O(n) 有序插入降为 O(1) 尾插:
     *           优先级顺序由位图 + 唤醒时 __builtin_ctz 取最高位来保证。
     *           与 rtos_kernel.ready_bitmap 同一套"数值小=优先级高"约定。 */
    volatile uint32_t wait_bitmap;
};

/**
 * @brief 初始化信号量。
 * @param sem       信号量指针。
 * @param init_cnt  初始计数。
 * @param max_cnt   最大计数(1 表示二值信号量)。
 * @return 有任务正在等待本对象时返回 RTOS_ERR_ALREADY 并保留原状态;
 *         无等待者时允许重新初始化。首次调用可传入未初始化的存储。
 */
rtos_status_t rtos_sem_init(rtos_sem_t *sem, uint32_t init_cnt, uint32_t max_cnt);

/**
 * @brief 获取(等待)信号量。
 * @param sem     信号量。
 * @param timeout 等待超时(Tick)。RTOS_NO_WAIT / RTOS_WAIT_FOREVER。
 * @return RTOS_OK 成功 / RTOS_ERR_TIMEOUT 超时。
 * @note   在中断中调用必须 timeout=RTOS_NO_WAIT。
 */
rtos_status_t rtos_sem_take(rtos_sem_t *sem, rtos_tick_t timeout);

/**
 * @brief 释放信号量(可在中断中调用)。
 */
rtos_status_t rtos_sem_give(rtos_sem_t *sem);

/**
 * @brief 获取当前计数。
 */
uint32_t rtos_sem_get_count(const rtos_sem_t *sem);

/** @brief 销毁信号量(唤醒所有等待者)。 */
rtos_status_t rtos_sem_deinit(rtos_sem_t *sem);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_SEM_H_ */

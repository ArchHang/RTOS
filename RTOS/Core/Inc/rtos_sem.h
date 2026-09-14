/**
 * @file    rtos_sem.h
 * @brief   信号量接口
 *
 * @details 信号量用于任务间同步与资源计数。本实现支持:
 *            - 计数信号量: 用于管理多个同类资源(如 DMA 通道)。
 *            - 二值信号量: 计数上限为 1，用于简单同步。
 *          阻塞等待采用等待链表(FIFO 或按优先级)，唤醒时取出链首任务。
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
    uint8_t in_use; /**< 使用中标志(对象池管理)。 */
    volatile uint32_t count; /**< 当前可用资源数。 */
    uint32_t max_count; /**< 最大计数上限。 */
    rtos_wait_node_t *wait_head; /**< 等待链表头(按优先级排序)。 */
};

/**
 * @brief 初始化信号量。
 * @param sem       信号量指针。
 * @param init_cnt  初始计数。
 * @param max_cnt   最大计数(1 表示二值信号量)。
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

/**
 * @file    rtos_internal.h
 * @brief   内核内部接口(非公共 API)
 *
 * @details 本文件声明内核模块间共享但不暴露给用户应用的内部函数。
 *          IPC 模块(信号量/互斥锁/队列/事件)通过这些函数实现任务阻塞与唤醒，
 *          复用统一的等待链表与延时表机制。
 */
#ifndef RTOS_INTERNAL_H_
#define RTOS_INTERNAL_H_

#include "rtos_types.h"
#include "rtos_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 阻塞当前任务并挂入对象等待链表。
 * @param obj     所等待的对象指针(用于校验与互斥锁优先级继承)。
 * @param head    对象等待链表头(按优先级排序的指针的地址)。
 * @param timeout 超时(0=不阻塞立即返回, RTOS_WAIT_FOREVER=永久)。
 * @return RTOS_OK 进入阻塞(返回时表示被唤醒)；RTOS_ERR_TIMEOUT 直接返回。
 *
 * @details 调用流程(IPC 模块典型用法):
 *          1. 检查对象状态(如信号量计数)。
 *          2. 若需等待，调用本函数挂入等待链表。
 *          3. 函数返回后检查 wait_result 判断是对象唤醒还是超时。
 */
rtos_status_t rtos_internal_block_current_on(void *obj, rtos_wait_node_t **head,
                                             rtos_tick_t timeout);

/**
 * @brief 唤醒等待链表中最高优先级的任务。
 * @param head 等待链表头指针的地址。
 * @return 被唤醒的 TCB，NULL 表示链表为空。
 *
 * @details 从链表移除该节点，清除阻塞标记，放入就绪表，并在必要时触发抢占。
 */
rtos_tcb_t *rtos_internal_wake_highest(rtos_wait_node_t **head);

/**
 * @brief 唤醒等待链表中所有任务(用于对象销毁)。
 */
void rtos_internal_wake_all(rtos_wait_node_t **head, rtos_status_t reason);

/**
 * @brief 按优先级将等待节点插入链表(数值小在前)。
 */
void rtos_internal_wait_insert_by_prio(rtos_wait_node_t **head, rtos_wait_node_t *node);

/**
 * @brief 从等待链表移除节点。
 */
void rtos_internal_wait_remove(rtos_wait_node_t **head, rtos_wait_node_t *node);

/**
 * @brief 释放任务持有的所有互斥锁(任务删除时调用)。
 * @details 将每个被持有互斥锁的所有权移交给最高优先级等待者(或释放锁),
 *          防止 mutex->owner 指向已释放的 TCB(use-after-free)与等待者永久死锁。
 * @note  由 rtos_mutex.c 实现; 任务状态需已置 DELETED(避免就绪表误操作)。
 */
void rtos_internal_mutex_release_all(rtos_tcb_t *owner);

/**
 * @brief 互斥锁等待者离开等待链表后, 重新评估持有者的继承优先级。
 * @details 等待者因超时/删除/挂起离开时调用, 撤销不再有依据的优先级提升。
 *          非互斥锁对象(信号量/队列/事件等)自动忽略。
 * @note  由 rtos_mutex.c 实现; 调用前需已完成等待链表移除。
 */
void rtos_internal_mutex_waiter_left(rtos_tcb_t *waiter);

/**
 * @brief 计算任务的有效优先级(考虑优先级继承)。
 * @details 遍历任务持有的所有互斥锁, 取等待者中最高优先级(数值最小)与
 *          base_priority 的较小值, 作为有效优先级。
 *          供 rtos_task_set_priority 调用, 避免修改 base_priority 时
 *          丢弃来自互斥锁等待者的优先级提升。
 * @note  由 rtos_mutex.c 实现; 调用方需在临界区内。
 * @return 有效优先级(0=最高)。
 */
rtos_prio_t rtos_internal_mutex_compute_effective_priority(rtos_tcb_t *owner);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_INTERNAL_H_ */

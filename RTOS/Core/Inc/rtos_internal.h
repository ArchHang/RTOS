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

/* ============================== 阻塞许可守卫 ============================== */

/* Dedicated HIL builds may interrupt the pre-exclusion liveness window. */
#if defined(RTOS_HIL_OBJECT_LIFETIME)
void rtos_hil_object_access_hook(const void *object, uint32_t site);
#define RTOS_INTERNAL_OBJECT_ACCESS_HOOK(object, site) \
    rtos_hil_object_access_hook((object), (site))
#else
#define RTOS_INTERNAL_OBJECT_ACCESS_HOOK(object, site) ((void)0)
#endif

/**
 * @brief 判断当前是否允许把任务置入阻塞态。
 *
 * @return RTOS_OK 可阻塞; RTOS_ERR_SCHED 不允许阻塞。
 *
 * @details [缺陷修复 SD-1] 为什么必须有这道守卫:
 *          `rtos_sched_suspend()` 只把 `sched_state` 置为 SUSPENDED, 而
 *          `rtos_sched_tick()` 在该状态下**直接 return** —— tick_count 停止推进,
 *          `process_delayed_tasks()` 不再运行。此时若把任务阻塞掉(延时/等信号量/
 *          等队列...), 它既不会被 tick 唤醒、也不会有超时到期, 即**永久挂死**,
 *          且没有任何错误码或断言提示。
 *          这不是理论推演: 真机实测 `rtos_sched_suspend()` 之后调用
 *          `rtos_task_delay(60)` 永不返回(见 docs/rtos_testing_report.md §10.1)。
 *
 *          契约(修复后): **挂起调度器期间, 凡会阻塞的 API 一律立即返回
 *          `RTOS_ERR_SCHED`, 不做任何阻塞**。非阻塞路径(如 `RTOS_NO_WAIT`、
 *          计数充足时的 `sem_take`)不受影响, 仍照常工作。
 *
 * @note  任意上下文可调用(只读 `sched_state`, 不进出临界区)。
 */
static inline rtos_status_t rtos_internal_block_permitted(void)
{
    return ((rtos_kernel.sched_state == RTOS_SCHED_RUNNING) &&
            (rtos_kernel.current_tcb != NULL)) ? RTOS_OK : RTOS_ERR_SCHED;
}

/* Caller holds the critical section. Does not inspect uninitialized objects. */
rtos_bool_t rtos_internal_object_has_waiters(const void *obj);

/**
 * @brief 提交候选上下文切换，并返回实际运行任务。
 *
 * @details 调用方已保存任务上下文并屏蔽内核中断。无候选任务或调度器
 *          暂停时仍返回当前任务，供移植层恢复原上下文。公共 void 入口保留。
 * @return 提交后的当前 TCB；内核尚未安装当前任务时为 NULL。
 */
rtos_tcb_t *rtos_sched_commit_context_switch(void);

/**
 * @brief 阻塞当前任务并挂入对象等待链表。
 * @param obj     所等待的对象指针(用于归属校验与互斥锁优先级继承)。
 * @param head    对象等待链表头的地址。
 * @param timeout 超时(Tick)。RTOS_WAIT_FOREVER=永久阻塞。
 * @return RTOS_OK 表示已进入阻塞(返回时已被唤醒); RTOS_ERR_SCHED 表示调度器不在
 *         运行态, 调用方应放弃阻塞并把该错误码透传给上层。
 *
 * @details 调用流程(IPC 模块典型用法):
 *          1. 检查对象状态(如信号量计数);
 *          2. 若需等待, 调用本函数挂入等待链表;
 *          3. 函数返回 RTOS_OK 后检查 wait_result 判断是对象唤醒还是超时。
 *
 * @note  本函数内部已包含"调度器运行态"守卫(见 rtos_internal_block_permitted),
 *        因此 **调用方不需要重复判断 `sched_state`**; 但**必须**检查返回值,
 *        返回 RTOS_ERR_SCHED 时不得把节点留在等待链上(本函数在返回前已不做任何
 *        链表操作, 调用方只需直接返回该错误码)。
 *
 * @note  timeout == 0 **不是**"立即返回": 任务仍会入链并阻塞到下一个 Tick。
 *        非阻塞语义必须由调用方在进入本函数前自行判断(timeout == RTOS_NO_WAIT
 *        时直接返回 RTOS_ERR_TIMEOUT), 四个 IPC 模块均如此处理。
 *
 * @note  调用前必须已进入临界区。
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
 * @brief 将等待节点尾插到等待链表(不定序, 配合优先级位图使用)。
 * @param head 等待链表头指针的地址。
 * @param node 待入链节点。
 * @note  与 rtos_internal_wait_insert_by_prio 不可在同一对象上混用。
 */
void rtos_internal_wait_append(rtos_wait_node_t **head, rtos_wait_node_t *node);

/**
 * @brief 唤醒"位图索引等待链"中最高优先级的任务。
 * @param head   等待链表头指针的地址(同优先级内 FIFO)。
 * @param bitmap 存在等待者的优先级位图(调用方维护, 本函数负责清位)。
 * @return 被唤醒的 TCB, NULL 表示无等待者。
 */
rtos_tcb_t *rtos_internal_wake_highest_bm(rtos_wait_node_t **head, volatile uint32_t *bitmap);

/**
 * @brief 从等待链表移除节点。
 */
void rtos_internal_wait_remove(rtos_wait_node_t **head, rtos_wait_node_t *node);

/**
 * @brief 从等待链表移除节点, 并同步维护位图索引链的优先级位图。
 *
 * @details 用于超时/删除等**非唤醒**路径的摘链: 位图位可能与同优先级的其它
 *          等待者共享, 故摘链后需扫描剩余链, 确认该优先级已无人等待才清位。
 *          节点 queue_bitmap == NULL(有序链: 事件组/互斥锁)时为纯摘链。
 *
 * @param head [in,out] 等待链表头指针的地址。
 * @param node [in]     待移除节点。
 * @note  调用方需在临界区内。
 */
void rtos_internal_wait_remove_sync_bitmap(rtos_wait_node_t **head, rtos_wait_node_t *node);

/**
 * @brief 释放任务持有的所有互斥锁(任务删除时调用)。
 * @details 将每个被持有互斥锁的所有权移交给最高优先级等待者(或释放锁),
 *          防止 mutex->owner 指向已释放的 TCB(use-after-free)与等待者永久死锁。
 * @note  由 rtos_mutex.c 实现; 任务状态需已置 DELETED(避免就绪表误操作)。
 */
void rtos_internal_mutex_release_all(rtos_tcb_t *owner);

/**
 * @brief 等待者优先级变化后, 重新评估其所在互斥锁持有者的继承优先级。
 * @details 见 rtos_mutex.c 的实现说明: 阻塞期间被改优先级的等待者, 必须让
 *          持有者的提升值同步更新, 否则优先级继承失效。
 * @note  由 rtos_task_set_priority 调用; 调用方需在临界区内。
 */
void rtos_internal_mutex_waiter_requeued(rtos_tcb_t *waiter);

/**
 * @brief 互斥锁等待者离开等待链表后, 重新评估持有者的继承优先级。
 * @details 等待者因超时/删除/挂起离开时调用, 撤销不再有依据的优先级提升。
 *          非互斥锁对象(信号量/队列/事件等)自动忽略。
 * @note  由 rtos_mutex.c 实现; 调用前需已完成等待链表移除。
 */
void rtos_internal_mutex_waiter_left(rtos_tcb_t *waiter);

/**
 * @brief 优先级变化后重挂任务所在的对象等待链(按新优先级)。
 * @details 等待链按优先级排序插入; set_priority 或互斥锁优先级继承的
 *          提升/恢复改变任务优先级后, 若该任务正阻塞在某对象等待链上,
 *          必须重排, 否则 wake_highest 按旧优先级取链首 → 唤醒顺序倒置。
 * @note  由 rtos_task.c 实现; 调用方需在临界区内。
 */
void rtos_internal_requeue_wait_lists(rtos_tcb_t *tcb);

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

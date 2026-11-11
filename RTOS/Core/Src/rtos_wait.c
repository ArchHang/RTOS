/**
 * @file    rtos_wait.c
 * @brief   内核共用的对象等待链、阻塞登记与唤醒机制。
 *
 * @details IPC、超时和优先级继承通过本模块维护等待节点。
 *          调用方持有内核临界区；本模块不改变临界区嵌套层级。
 *          阻塞登记只请求调度，实际任务切换由退出临界区后的 PendSV 完成。
 */
#include "rtos_internal.h"
#include "rtos_task.h"

/**
 * @brief 按优先级将等待节点插入链表(数值小在前)。
 */
void rtos_internal_wait_insert_by_prio(rtos_wait_node_t **head, rtos_wait_node_t *node)
{
    rtos_wait_node_t *cur = *head;
    rtos_wait_node_t *prev = NULL;
    const rtos_prio_t priority = node->queued_priority;

    while ((cur != NULL) && (cur->queued_priority <= priority)) {
        prev = cur;
        cur = cur->next;
    }

    node->next = cur;
    node->prev = prev;
    if (prev != NULL) {
        prev->next = node;
    } else {
        *head = node;
    }
    if (cur != NULL) {
        cur->prev = node;
    }
}

/**
 * @brief 从等待链表移除节点。
 */
void rtos_internal_wait_remove(rtos_wait_node_t **head, rtos_wait_node_t *node)
{
    rtos_wait_node_t *next = node->next;
    rtos_wait_node_t *prev = node->prev;

    /* 合法非循环链的邻居不是 node，先快照可避免邻居写入后的重复 SRAM 读取。 */
    if (prev != NULL) {
        prev->next = next;
    } else {
        *head = next;
    }
    if (next != NULL) {
        next->prev = prev;
    }
    node->next = NULL;
    node->prev = NULL;
}

/** @brief 摘除有序等待链上的节点并维护可选的优先级位图。 */
void rtos_internal_wait_remove_sync_bitmap(rtos_wait_node_t **head, rtos_wait_node_t *node)
{
    volatile uint32_t *const bitmap = node->queue_bitmap;
    if (bitmap != NULL) {
        const rtos_wait_node_t *const previous = node->prev;
        const rtos_wait_node_t *const next = node->next;
        const rtos_prio_t priority = node->queued_priority;
        /* Equal-priority nodes are contiguous. During a requeue, priority
         * still describes the old position even after the task value changes. */
        if (((previous == NULL) || (previous->queued_priority != priority)) &&
            ((next == NULL) || (next->queued_priority != priority))) {
            *bitmap &= ~(1UL << priority);
        }
    }

    rtos_internal_wait_remove(head, node);
}

/**
 * @brief 将当前任务挂入对象等待链表并阻塞。
 * @param obj     所等待对象指针。
 * @param head    对象等待链表头指针的地址。
 * @param timeout 超时 Tick 数；非阻塞语义由调用方在进入本函数前处理。
 * @return RTOS_OK 表示已登记阻塞；RTOS_ERR_SCHED 表示未修改等待状态。
 *
 * @details 调用前必须已进入临界区。函数会:
 *          1. 初始化当前任务的等待节点。
 *          2. 按优先级插入对象等待链表(高优先级在前，便于优先唤醒)。
 *          3. 调用 sched_block 设置阻塞/延时态。
 *          4. 选定候选任务并请求 PendSV。
 *          本函数仍在调用方临界区内返回。调用方退出最外层临界区后，
 *          PendSV 才能切出当前任务；任务恢复运行后再读取 wait_result。
 */
rtos_status_t rtos_internal_block_current_on(void *obj, rtos_wait_node_t **head,
                                             rtos_tick_t timeout)
{
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    RTOS_ASSERT(cur != NULL);

    /* [缺陷修复 SD-1] 调度器不在运行态(如被 rtos_sched_suspend 挂起)时禁止阻塞:
     * 此时 tick 已停走, 阻塞下去既不会被唤醒也不会有超时 ⇒ 永久挂死。
     * 必须在**任何状态修改之前**返回, 保证不留下半初始化的等待节点。 */
    if (rtos_internal_block_permitted() != RTOS_OK) {
        return RTOS_ERR_SCHED;
    }

    cur->blocked_on = obj;
    cur->wait_node.tcb = cur;
    cur->wait_node.on_timeout = NULL;
    cur->wait_node.wait_obj = obj;
    cur->wait_node.timed_out = RTOS_FALSE;
    cur->wait_node.next = NULL;
    cur->wait_node.prev = NULL;
    /* 注意: transfer_buf 不在此清除 —— 队列零拷贝需在调用本函数前设置并保留。 */
    cur->wait_node.list_head = head; /* 记录所在链表，供超时清理 */
    cur->wait_node.queue_bitmap = NULL; /* 通用阻塞入口不维护观测位图。 */
    cur->wait_node.queued_priority = cur->priority;
    cur->wait_result = RTOS_OK;

    /* 按优先级插入对象等待链表 */
    rtos_internal_wait_insert_by_prio(head, &cur->wait_node);

    /* 进入阻塞态(同时处理延时表) */
    rtos_sched_block(cur, timeout);

    /* 登记候选切换；PendSV 在调用方退出最外层临界区后执行。 */
    rtos_sched_schedule();
    return RTOS_OK;
}

/**
 * @brief 唤醒等待链表中的最高优先级任务(并从链表移除)。
 * @param head 等待链表头指针的地址。
 * @return 被唤醒的 TCB，NULL 表示链表空。
 *
 * @details 链表已按优先级排序，链首即最高优先级。本函数:
 *          1. 取链首节点并从等待链表移除。
 *          2. 清除阻塞标记与对象指针。
 *          3. 调用 sched_unblock 从延时表移除(若因超时入表)并放入就绪表。
 *          4. sched_unblock 内会自动判断是否需要触发抢占。
 */
rtos_tcb_t *rtos_internal_wake_highest(rtos_wait_node_t **head)
{
    rtos_wait_node_t *best = *head;
    if (best == NULL) {
        return NULL;
    }

    /* 从等待链表移除(链首即最高优先级) */
    rtos_internal_wait_remove(head, best);

    best->tcb->blocked_on = NULL;
    best->tcb->wait_result = RTOS_OK;
    best->list_head = NULL; /* 标记已离开等待链表(防御性: 与 queue 零拷贝路径一致) */

    rtos_sched_unblock(best->tcb);
    return best->tcb;
}

/** @brief 唤醒有序等待链的首节点并同步更新信号量的观测位图。 */
void rtos_internal_wake_head_with_bitmap(rtos_wait_node_t **head, rtos_wait_node_t *node,
                                         volatile uint32_t *bitmap)
{
    rtos_wait_node_t *const next = node->next;
    const rtos_prio_t priority = node->queued_priority;
    if ((next == NULL) || (next->queued_priority != priority)) {
        *bitmap &= ~(1UL << priority);
    }
    *head = next;
    if (next != NULL) {
        next->prev = NULL;
    }
    node->next = NULL;

    rtos_tcb_t *const task = node->tcb;
    task->blocked_on = NULL;
    task->wait_result = RTOS_OK;
    node->list_head = NULL;

    rtos_sched_unblock(task);
}

/**
 * @brief 唤醒等待链表中所有任务(用于对象销毁)。
 */
void rtos_internal_wake_all(rtos_wait_node_t **head, rtos_status_t reason)
{
    while (*head != NULL) {
        rtos_wait_node_t *node = *head;
        rtos_internal_wait_remove(head, node);
        node->tcb->blocked_on = NULL;
        node->tcb->wait_result = reason;
        node->list_head = NULL;
        node->is_mutex_wait = RTOS_FALSE;
        rtos_sched_unblock(node->tcb);
    }
}

/* ============================== 等待链重挂(优先级变化后) ============================== */

void rtos_internal_requeue_wait_lists(rtos_tcb_t *tcb)
{
    if ((tcb->blocked_on == NULL) || (tcb->wait_node.list_head == NULL)) {
        return;
    }

    rtos_wait_node_t *const node = &tcb->wait_node;
    rtos_wait_node_t **const head = node->list_head;
    const rtos_prio_t new_prio = tcb->priority;
    if (new_prio == node->queued_priority) {
        /* Effective no-op must preserve FIFO. */
        return;
    }

    rtos_internal_wait_remove_sync_bitmap(head, node);
    node->queued_priority = new_prio;
    rtos_internal_wait_insert_by_prio(head, node);
    if (node->queue_bitmap != NULL) {
        *node->queue_bitmap |= (1UL << new_prio);
    }
}

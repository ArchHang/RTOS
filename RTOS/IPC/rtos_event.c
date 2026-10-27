/**
 * @file    rtos_event.c
 * @brief   事件标志组实现
 *
 * @details 事件标志组提供 24 位标志(位 24~31 保留作内部标志)，任务可同时
 *          等待多个位:
 *            - RTOS_EVENT_WAIT_ANY: 任一所等待位被置位即唤醒。
 *            - RTOS_EVENT_WAIT_ALL: 全部所等待位都被置位才唤醒。
 *          可选 RTOS_EVENT_CLEAR_ON_EXIT: 唤醒时清除所等待的位。
 *
 *          实现要点:
 *            - set 时遍历等待链表，检查每个等待者是否满足条件，满足则唤醒。
 *            - 等待参数存于 wait_node 自有字段(避免与 tcb->notify_value 冲突):
 *                transfer_size = 等待位掩码, wake_tick = 等待模式,
 *                transfer_buf   = 唤醒时回传的当前标志位。
 *              旧实现复用 notify_value, 会与 rtos_task_notify 跨模块冲突
 *              (阻塞期间 notify 覆盖 notify_value), 已修复。
 *
 *          set 可在中断中调用。
 */
#include "rtos_event.h"
#include "rtos_internal.h"
#include "rtos_sched.h"
#include "rtos_task.h" /* rtos_tcb_t 完整定义(访问 node->tcb->blocked_on/wait_result) */
#include <string.h>

/**
 * @brief 检查等待条件是否满足。
 * @param current_bits 当前标志位。
 * @param wait_bits    所等待的位掩码。
 * @param mode         等待模式。
 * @return TRUE 满足，FALSE 不满足。
 */
static rtos_bool_t event_check(rtos_event_bits_t current_bits, rtos_event_bits_t wait_bits,
                               uint32_t mode)
{
    if ((mode & 0x1U) == RTOS_EVENT_WAIT_ALL) {
        /* ALL: 所等待位全部置位 */
        return ((current_bits & wait_bits) == wait_bits) ? RTOS_TRUE : RTOS_FALSE;
    } else {
        /* ANY: 任一所等待位置位 */
        return ((current_bits & wait_bits) != 0U) ? RTOS_TRUE : RTOS_FALSE;
    }
}

/* Called by the timeout path while the node is still on the event's wait
 * list. Deinit cannot release this object until that critical section ends. */
static void event_capture_timeout(rtos_wait_node_t *node)
{
    rtos_event_t *event = (rtos_event_t *)node->wait_obj;
    node->transfer_buf = (void *)(uintptr_t)event->bits;
}

rtos_status_t rtos_event_init(rtos_event_t *event)
{
    if (event == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_ASSERT_ISR_OK();
    RTOS_PORT_ENTER_CRITICAL();
    if (rtos_internal_object_has_waiters((void *)event)) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_ALREADY;
    }
    memset(event, 0, sizeof(*event));
    event->bits = 0U;
    event->wait_head = NULL;
    event->is_initialized = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_event_bits_t rtos_event_set(rtos_event_t *event, rtos_event_bits_t bits)
{
    if ((event == NULL) || (event->is_initialized == 0U)) {
        return 0U;
    }
    RTOS_ASSERT_ISR_OK(); /* [guard G-3] 违约优先级中断调用时立即捕获 */

    RTOS_INTERNAL_OBJECT_ACCESS_HOOK(event, 5U);
    RTOS_PORT_ENTER_CRITICAL();
    /* An allowed ISR may deinitialize the still-live storage after the fast
     * precheck. Revalidate with exclusion before any state access or wait. */
    if (event->is_initialized == 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return 0U;
    }
    event->bits |= bits;

    /* 遍历等待链表，唤醒满足条件的等待者 */
    rtos_wait_node_t *node = event->wait_head;
    while (node != NULL) {
        rtos_wait_node_t *next = node->next;
        /* 等待参数存于 wait_node 自有字段, 不复用 tcb->notify_value:
         *   - transfer_size: 所等待的位掩码(由 event_wait 设置)
         *   - wake_tick:     等待模式(由 event_wait 设置)
         *   - transfer_buf:  唤醒时回传的当前标志位(由本函数设置)
         * 旧实现复用 tcb->notify_value, 会与 rtos_task_notify 跨模块冲突
         * (任务阻塞在 event_wait 时若被 notify, notify_value 被覆盖)。 */
        rtos_event_bits_t wait_bits = (rtos_event_bits_t)node->transfer_size;
        uint32_t mode = node->wake_tick;

        if (event_check(event->bits, wait_bits, mode) == RTOS_TRUE) {
            /* 清除所等待位(若要求) —— 注意: 先保存 result 再清除 */
            rtos_event_bits_t result_bits = event->bits;
            if ((mode & RTOS_EVENT_CLEAR_ON_EXIT) != 0U) {
                event->bits &= ~wait_bits;
            }
            /* 唤醒该任务 */
            rtos_internal_wait_remove(&event->wait_head, node);
            node->tcb->blocked_on = NULL;
            node->tcb->wait_result = RTOS_OK;
            node->list_head = NULL; /* 标记已离开等待链表 */
            /* 把满足时的标志位存入 transfer_buf 供任务读取(清除前的值) */
            node->transfer_buf = (void *)(uintptr_t)result_bits;
            rtos_sched_unblock(node->tcb);
        }
        node = next;
    }

    rtos_event_bits_t result = event->bits;
    RTOS_PORT_EXIT_CRITICAL();
    return result;
}

rtos_event_bits_t rtos_event_clear(rtos_event_t *event, rtos_event_bits_t bits)
{
    if ((event == NULL) || (event->is_initialized == 0U)) {
        return 0U;
    }
    RTOS_ASSERT_ISR_OK();
    RTOS_INTERNAL_OBJECT_ACCESS_HOOK(event, 6U);
    RTOS_PORT_ENTER_CRITICAL();
    /* An allowed ISR may deinitialize the still-live storage after the fast
     * precheck. Revalidate with exclusion before any state access or wait. */
    if (event->is_initialized == 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return 0U;
    }
    event->bits &= ~bits;
    rtos_event_bits_t result = event->bits;
    RTOS_PORT_EXIT_CRITICAL();
    return result;
}

rtos_event_bits_t rtos_event_wait(rtos_event_t *event, rtos_event_bits_t wait_bits, uint32_t mode,
                                  rtos_tick_t timeout)
{
    if ((event == NULL) || (event->is_initialized == 0U) || (wait_bits == 0U)) {
        return 0U;
    }
    RTOS_ASSERT_ISR_OK();
    if ((mode & ~0x1U & ~RTOS_EVENT_CLEAR_ON_EXIT) != 0U) {
        return 0U; /* 非法模式位 */
    }

    rtos_bool_t in_isr = RTOS_PORT_IN_ISR();
    if (in_isr && (timeout != RTOS_NO_WAIT)) {
        return 0U;
    }

    RTOS_INTERNAL_OBJECT_ACCESS_HOOK(event, 7U);
    RTOS_PORT_ENTER_CRITICAL();
    /* An allowed ISR may deinitialize the still-live storage after the fast
     * precheck. Revalidate with exclusion before any state access or wait. */
    if (event->is_initialized == 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return 0U;
    }

    /* 先检查是否已满足 */
    if (event_check(event->bits, wait_bits, mode) == RTOS_TRUE) {
        /* 保存清除前的值作为返回值。旧实现在此处错误地用
         * "event->bits | wait_bits" 重算 result, ANY 模式下会把未置位的
         * 等待位也置成 1 返回, 与"返回清除前的真实标志位"语义不符。 */
        rtos_event_bits_t result = event->bits;
        if ((mode & RTOS_EVENT_CLEAR_ON_EXIT) != 0U) {
            event->bits &= ~wait_bits;
        }
        RTOS_PORT_EXIT_CRITICAL();
        return result;
    }

    if (timeout == RTOS_NO_WAIT) {
        rtos_event_bits_t result = event->bits;
        RTOS_PORT_EXIT_CRITICAL();
        return result;
    }

    /* [缺陷修复 SD-1] 挂起期间禁止阻塞: 此时 tick 停走, 阻塞下去既不会被
     * rtos_event_set 之外的任何路径唤醒, 也没有超时来源 ⇒ 永久挂死。
     * 本接口返回的是**位掩码**而非状态码, 无法上报 RTOS_ERR_SCHED ——
     * 按本模块既有约定(见文件头与 deinit 路径)返回 0 表示"未满足/不可用",
     * 与"当前位恰好为 0"不可区分(限制 R8, 已在头文件注明)。 */
    if (rtos_internal_block_permitted() != RTOS_OK) {
        RTOS_PORT_EXIT_CRITICAL();
        return 0U;
    }

    /* 阻塞等待: 等待位存于 wait_node.transfer_size, 模式存于 wake_tick。
     * 不复用 tcb->notify_value —— 否则与 rtos_task_notify 跨模块冲突
     * (阻塞期间 notify 会覆盖 notify_value, 唤醒后读回的是通知值而非标志位)。 */
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    cur->blocked_on = (void *)event;
    cur->wait_node.tcb = cur;
    cur->wait_node.wait_obj = (void *)event;
    cur->wait_node.timed_out = RTOS_FALSE;
    cur->wait_node.next = NULL;
    cur->wait_node.prev = NULL;
    cur->wait_node.transfer_size = wait_bits; /* 等待位掩码 */
    cur->wait_node.transfer_buf = NULL; /* 唤醒时由 event_set 写入结果 */
    cur->wait_node.wake_tick = mode; /* 复用字段存模式 */
    cur->wait_node.list_head = &event->wait_head; /* 供超时清理 */
    /* [缺陷修复 EH-1] 必须显式清除队列侧字段: 事件组用的是"按优先级有序链",
     * 不是位图索引链。若沿用上一次阻塞(如信号量超时)遗留的 queue_bitmap,
     * 本节点会被 rtos_internal_requeue_wait_lists 误判为位图链成员:
     *   1. 去 read-modify-write **上一次那个信号量**的 wait_bitmap
     *      (该对象可能已 deinit/存储已释放 → 对任意内存的读改写);
     *   2. 并被错误地"尾插回原位序", 而不是按优先级插入事件等待链。
     * 真机实测(D4 场景): queue_bitmap 非 NULL、旧信号量位图被写入 bit11。
     * 对照实现: rtos_internal_block_current_on 与 rtos_task_notify_wait
     * 都已显式置 NULL, 此处属于遗漏。 */
    cur->wait_node.queue_bitmap = NULL;
    cur->wait_node.queued_priority = cur->priority;
    cur->wait_node.on_timeout = event_capture_timeout;
    cur->wait_result = RTOS_OK;

    rtos_internal_wait_insert_by_prio(&event->wait_head, &cur->wait_node);
    rtos_sched_block(cur, timeout);
    RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_EVENT, cur,
        event, timeout, wait_bits, RTOS_OK, mode);
    rtos_sched_schedule();
    RTOS_PORT_EXIT_CRITICAL();

    /* 被唤醒: 从 wait_node.transfer_buf 读取 event_set 回传的标志位 */
    cur = rtos_kernel.current_tcb;
    rtos_event_bits_t result = (rtos_event_bits_t)(uintptr_t)cur->wait_node.transfer_buf;
    /* Success and timeout both carry a snapshot in the TCB. After timeout the
     * task has left the event list, so the object may have been deinitialized
     * and reused before this task resumes. Never dereference event here.
     * Deinit/suspend keep transfer_buf at NULL and therefore return zero. */
    return RTOS_INTERNAL_DIAG_RETURN_VALUE(RTOS_DIAG_OP_EVENT, cur,
        result, cur->wait_result);
}

rtos_event_bits_t rtos_event_get(const rtos_event_t *event)
{
    if (event == NULL) {
        return 0U;
    }
    return event->bits;
}

rtos_status_t rtos_event_deinit(rtos_event_t *event)
{
    if (event == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_ASSERT_ISR_OK();
    RTOS_PORT_ENTER_CRITICAL();
    rtos_internal_wake_all(&event->wait_head, RTOS_ERR_DELETED);
    event->is_initialized = 0U;
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

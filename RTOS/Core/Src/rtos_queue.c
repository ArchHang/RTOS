/**
 * @file    rtos_queue.c
 * @brief   消息队列实现
 *
 * @details 基于环形缓冲区的定长消息队列。
 *            发送: 写入 tail 槽位，tail = (tail+1) % capacity，count++。
 *            接收: 读取 head 槽位，head = (head+1) % capacity，count--。
 *          队列满时发送者阻塞(挂入 send_wait_head)；队列空时接收者阻塞
 *          (挂入 recv_wait_head)。
 *
 *          零拷贝直传优化(优于环形缓冲两次拷贝):
 *            - 接收者阻塞时，其接收缓冲指针存入 wait_node.transfer_buf。
 *            - 发送者到达发现接收等待者: 直接从发送缓冲 memcpy 到接收缓冲，
 *              唤醒接收者，消息不经过环形缓冲，仅 1 次拷贝(FreeRTOS 同款优化)。
 *            - 对称: 发送者阻塞时存发送缓冲指针，接收者到达直接拷贝并唤醒发送者。
 *
 *          支持插队发送(to_front=TRUE)，用于紧急消息(写入 head 前一格)。
 *          注意: 零拷贝直传仅在 to_front=FALSE 时启用(插队消息须入队保序)。
 *
 *          ISR 中调用必须 timeout=RTOS_NO_WAIT。
 */
#include "rtos_queue.h"
#include "rtos_internal.h"
#include "rtos_sched.h"
#include "rtos_task.h" /* rtos_tcb_t 完整定义(访问 node->tcb->blocked_on/wait_result) */
#include <string.h>

rtos_status_t rtos_queue_init(rtos_queue_t *queue, void *storage, uint32_t item_size,
                              uint32_t capacity)
{
    if ((queue == NULL) || (storage == NULL) || (item_size == 0U) || (capacity == 0U)) {
        return RTOS_ERR_PARAM;
    }
    memset(queue, 0, sizeof(*queue));
    queue->storage = (uint8_t *)storage;
    queue->item_size = item_size;
    queue->capacity = capacity;
    queue->count = 0U;
    queue->head = 0U;
    queue->tail = 0U;
    queue->send_wait_head = NULL;
    queue->recv_wait_head = NULL;
    queue->is_initialized = 1U;
    return RTOS_OK;
}

/** @brief 计算环形缓冲区下一索引。 */
static inline uint32_t queue_next_index(uint32_t idx, uint32_t capacity)
{
    return (idx + 1U) >= capacity ? 0U : (idx + 1U);
}

rtos_status_t rtos_queue_send(rtos_queue_t *queue, const void *item, rtos_tick_t timeout,
                              rtos_bool_t to_front)
{
    if ((queue == NULL) || (item == NULL)) {
        return RTOS_ERR_NULL;
    }
    if (queue->is_initialized == 0U) {
        return RTOS_ERR_PARAM;
    }

    rtos_bool_t in_isr = RTOS_PORT_IN_ISR();
    if (in_isr && (timeout != RTOS_NO_WAIT)) {
        return RTOS_ERR_ISR;
    }

    RTOS_PORT_ENTER_CRITICAL();

    /* 情况1: 队列有接收等待者 -> 零拷贝直传(跳过入队，仅 1 次拷贝)
     * 接收者仅在队列空时阻塞，故 recv_wait_head!=NULL 必有 count==0，
     * to_front 在空队列无意义，统一走直传(修复 to_front+等待者死锁)。 */
    if (queue->recv_wait_head != NULL) {
        rtos_wait_node_t *node = queue->recv_wait_head;
        rtos_internal_wait_remove(&queue->recv_wait_head, node);
        void *recv_buf = node->transfer_buf; /* 接收者阻塞前存入的接收缓冲 */
        node->transfer_buf = NULL; /* 标记已被直传取走 */
        node->list_head = NULL;
        /* 先拷贝再唤醒: 保证接收者运行时数据已就绪(临界区保证接收者未运行) */
        memcpy(recv_buf, item, queue->item_size);
        node->tcb->blocked_on = NULL;
        node->tcb->wait_result = RTOS_OK;
        rtos_sched_unblock(node->tcb);
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 情况2: 队列未满 -> 直接入队 */
    if (queue->count < queue->capacity) {
        if (to_front == RTOS_FALSE) {
            uint8_t *dst = queue->storage + queue->tail * queue->item_size;
            memcpy(dst, item, queue->item_size);
            queue->tail = queue_next_index(queue->tail, queue->capacity);
        } else {
            /* 插队首: 写入 head 前一格 */
            uint32_t new_head = (queue->head == 0U) ? (queue->capacity - 1U) : (queue->head - 1U);
            uint8_t *dst = queue->storage + new_head * queue->item_size;
            memcpy(dst, item, queue->item_size);
            queue->head = new_head;
        }
        queue->count++;
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 情况3: 队列满且不等待 */
    if (timeout == RTOS_NO_WAIT) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_TIMEOUT;
    }

    /* 情况4: 队列满 -> 阻塞发送者(记录发送缓冲以供零拷贝直传) */
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    cur->wait_node.transfer_buf = (void *)item; /* 存发送缓冲指针 */
    cur->wait_node.transfer_size = queue->item_size;
    rtos_internal_block_current_on((void *)queue, &queue->send_wait_head, timeout);
    RTOS_PORT_EXIT_CRITICAL();

    /* 被唤醒: 检查结果
     * - transfer_buf==NULL: 消息已被接收者直传拷贝走，直接返回成功。
     * - wait_result!=OK: 超时或对象删除，返回错误。
     * - 否则: 队列腾出空间，重试入队。 */
    cur = rtos_kernel.current_tcb;
    if (cur->wait_result != RTOS_OK) {
        return cur->wait_result;
    }
    if (cur->wait_node.transfer_buf == NULL) {
        return RTOS_OK; /* 消息已被接收者直传取走 */
    }

    /* 重试入队: 若槽位被其他任务/ISR 偷走则重新阻塞, 而非返回错误。
     * 注: 重新阻塞使用原始 timeout, 多次被偷时总等待时间可能超预期。
     *     这优于错误返回 RTOS_ERR_TIMEOUT(任务并未超时, 只是槽位被偷)。 */
    for (;;) {
        RTOS_PORT_ENTER_CRITICAL();
        if (queue->count < queue->capacity) {
            if (to_front == RTOS_FALSE) {
                uint8_t *dst = queue->storage + queue->tail * queue->item_size;
                memcpy(dst, item, queue->item_size);
                queue->tail = queue_next_index(queue->tail, queue->capacity);
            } else {
                uint32_t new_head =
                    (queue->head == 0U) ? (queue->capacity - 1U) : (queue->head - 1U);
                uint8_t *dst = queue->storage + new_head * queue->item_size;
                memcpy(dst, item, queue->item_size);
                queue->head = new_head;
            }
            queue->count++;
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_OK;
        }
        /* 槽位被偷: 重新阻塞等待 */
        cur = rtos_kernel.current_tcb;
        cur->wait_node.transfer_buf = (void *)item;
        cur->wait_node.transfer_size = queue->item_size;
        rtos_internal_block_current_on((void *)queue, &queue->send_wait_head, timeout);
        RTOS_PORT_EXIT_CRITICAL();

        cur = rtos_kernel.current_tcb;
        if (cur->wait_result != RTOS_OK) {
            return cur->wait_result;
        }
        if (cur->wait_node.transfer_buf == NULL) {
            return RTOS_OK; /* 消息已被接收者直传取走 */
        }
    }
}

rtos_status_t rtos_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout)
{
    if ((queue == NULL) || (item == NULL)) {
        return RTOS_ERR_NULL;
    }
    if (queue->is_initialized == 0U) {
        return RTOS_ERR_PARAM;
    }

    rtos_bool_t in_isr = RTOS_PORT_IN_ISR();
    if (in_isr && (timeout != RTOS_NO_WAIT)) {
        return RTOS_ERR_ISR;
    }

    RTOS_PORT_ENTER_CRITICAL();

    /* 情况1: 队列有消息 -> 直接出队 */
    if (queue->count > 0U) {
        uint8_t *src = queue->storage + queue->head * queue->item_size;
        memcpy(item, src, queue->item_size);
        queue->head = queue_next_index(queue->head, queue->capacity);
        queue->count--;

        /* 出队后腾出空位: 若有发送等待者，直接把其消息拷入环形缓冲(代入队)，
         * 唤醒发送者(其 transfer_buf 置 NULL 表示已代办，无需重试)。 */
        if (queue->send_wait_head != NULL) {
            rtos_wait_node_t *node = queue->send_wait_head;
            rtos_internal_wait_remove(&queue->send_wait_head, node);
            uint8_t *dst = queue->storage + queue->tail * queue->item_size;
            memcpy(dst, node->transfer_buf, queue->item_size);
            queue->tail = queue_next_index(queue->tail, queue->capacity);
            queue->count++; /* 空位被重新填入 */
            node->transfer_buf = NULL; /* 标记已代办入队 */
            node->list_head = NULL;
            node->tcb->blocked_on = NULL;
            node->tcb->wait_result = RTOS_OK;
            rtos_sched_unblock(node->tcb);
        }

        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 情况2: 队列空且不等待 */
    if (timeout == RTOS_NO_WAIT) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_TIMEOUT;
    }

    /* 情况3: 队列空 -> 阻塞接收者(记录接收缓冲以供零拷贝直传) */
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    cur->wait_node.transfer_buf = item; /* 存接收缓冲指针 */
    cur->wait_node.transfer_size = queue->item_size;
    rtos_internal_block_current_on((void *)queue, &queue->recv_wait_head, timeout);
    RTOS_PORT_EXIT_CRITICAL();

    /* 被唤醒:
     * - transfer_buf==NULL: 发送者已直传拷贝到 item，直接返回成功。
     * - wait_result!=OK: 超时或对象删除，返回错误。
     * - 否则: 消息可能被其他任务偷走, 重试出队, 若仍空则重新阻塞。 */
    cur = rtos_kernel.current_tcb;
    if (cur->wait_result != RTOS_OK) {
        return cur->wait_result;
    }
    if (cur->wait_node.transfer_buf == NULL) {
        return RTOS_OK; /* 消息已由发送者直传到 item */
    }

    /* 重试出队: 若消息被偷则重新阻塞, 而非返回错误 */
    for (;;) {
        RTOS_PORT_ENTER_CRITICAL();
        if (queue->count > 0U) {
            uint8_t *src = queue->storage + queue->head * queue->item_size;
            memcpy(item, src, queue->item_size);
            queue->head = queue_next_index(queue->head, queue->capacity);
            queue->count--;

            /* 唤醒发送等待者(代办入队) */
            if (queue->send_wait_head != NULL) {
                rtos_wait_node_t *node = queue->send_wait_head;
                rtos_internal_wait_remove(&queue->send_wait_head, node);
                uint8_t *dst = queue->storage + queue->tail * queue->item_size;
                memcpy(dst, node->transfer_buf, queue->item_size);
                queue->tail = queue_next_index(queue->tail, queue->capacity);
                queue->count++;
                node->transfer_buf = NULL;
                node->list_head = NULL;
                node->tcb->blocked_on = NULL;
                node->tcb->wait_result = RTOS_OK;
                rtos_sched_unblock(node->tcb);
            }

            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_OK;
        }
        /* 消息被偷: 重新阻塞等待 */
        cur = rtos_kernel.current_tcb;
        cur->wait_node.transfer_buf = item;
        cur->wait_node.transfer_size = queue->item_size;
        rtos_internal_block_current_on((void *)queue, &queue->recv_wait_head, timeout);
        RTOS_PORT_EXIT_CRITICAL();

        cur = rtos_kernel.current_tcb;
        if (cur->wait_result != RTOS_OK) {
            return cur->wait_result;
        }
        if (cur->wait_node.transfer_buf == NULL) {
            return RTOS_OK; /* 消息已由发送者直传到 item */
        }
    }
}

uint32_t rtos_queue_get_count(const rtos_queue_t *queue)
{
    return (queue != NULL) ? queue->count : 0U;
}

uint32_t rtos_queue_get_free(const rtos_queue_t *queue)
{
    if (queue == NULL) {
        return 0U;
    }
    return (queue->capacity > queue->count) ? (queue->capacity - queue->count) : 0U;
}

rtos_status_t rtos_queue_deinit(rtos_queue_t *queue)
{
    if (queue == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_PORT_ENTER_CRITICAL();
    rtos_internal_wake_all(&queue->send_wait_head, RTOS_ERR_DELETED);
    rtos_internal_wake_all(&queue->recv_wait_head, RTOS_ERR_DELETED);
    queue->is_initialized = 0U;
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

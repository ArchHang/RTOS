/**
 * @file    rtos_queue.h
 * @brief   消息队列接口
 *
 * @details 消息队列用于在任务/中断间传递定长数据(按值拷贝)。
 *          本实现采用环形缓冲区:
 *            - 发送: 写入 tail 指向的槽位，tail 回绕，count++。
 *            - 接收: 读取 head 槽位，head 回绕，count--。
 *          队列满时发送者阻塞(可选超时)；队列空时接收者阻塞。
 *          ISR 中发送可选触发优先级升顶(发送解阻塞的高优先级任务立即运行)。
 *
 *          等待链表按任务优先级排序，保证高优先级任务先得到消息。
 */
#ifndef RTOS_QUEUE_H_
#define RTOS_QUEUE_H_

#include <stdint.h>
#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct rtos_queue
 * @brief  消息队列对象。
 */
struct rtos_queue {
    uint8_t is_initialized;
    uint8_t in_use;
    uint8_t *storage; /**< 数据存储区指针。 */
    uint32_t item_size; /**< 单条消息字节数。 */
    uint32_t capacity; /**< 队列容量(消息条数)。 */
    volatile uint32_t count; /**< 当前消息数。 */
    uint32_t head; /**< 读指针(下一条待读)。 */
    uint32_t tail; /**< 写指针(下一条待写)。 */
    rtos_wait_node_t *send_wait_head; /**< 发送等待链表(队列满时)。 */
    rtos_wait_node_t *recv_wait_head; /**< 接收等待链表(队列空时)。 */
};

/**
 * @brief 初始化队列(静态存储)。
 * @param queue     队列对象。
 * @param storage   存储区(由用户提供，大小 = item_size * capacity)。
 * @param item_size 单条消息字节数。
 * @param capacity  容量。
 */
rtos_status_t rtos_queue_init(rtos_queue_t *queue, void *storage, uint32_t item_size,
                              uint32_t capacity);

/**
 * @brief 发送消息(队尾)。
 * @param queue    队列。
 * @param item     待发送数据(按值拷贝，不可 NULL)。
 * @param timeout  超时。ISR 中必须为 RTOS_NO_WAIT。
 * @param to_front 是否插队首(用于紧急消息)。0=正常发送。
 */
rtos_status_t rtos_queue_send(rtos_queue_t *queue, const void *item, rtos_tick_t timeout,
                              rtos_bool_t to_front);

/**
 * @brief 接收消息(队首)。
 * @param queue    队列。
 * @param item     接收缓冲(不可 NULL)。
 * @param timeout  超时。ISR 中必须为 RTOS_NO_WAIT。
 */
rtos_status_t rtos_queue_recv(rtos_queue_t *queue, void *item, rtos_tick_t timeout);

/** @brief 查询当前消息数。 */
uint32_t rtos_queue_get_count(const rtos_queue_t *queue);

/** @brief 查询剩余空间。 */
uint32_t rtos_queue_get_free(const rtos_queue_t *queue);

/** @brief 销毁队列(唤醒所有等待者)。 */
rtos_status_t rtos_queue_deinit(rtos_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_QUEUE_H_ */

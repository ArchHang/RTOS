/**
 * @file    rtos_types.h
 * @brief   RTOS 公共类型定义
 *
 * @details 集中定义内核各模块共用的基础类型、状态码与任务控制块(TCB)结构。
 *          这些类型是任务管理、调度器、通信机制的共同语言，单独抽出以避免
 *          头文件间的循环依赖。
 */
#ifndef RTOS_TYPES_H_
#define RTOS_TYPES_H_

#include <stdint.h>
#include <stddef.h>
#include "rtos_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== 基础类型别名 ============================== */

/** @brief 布尔类型。 */
typedef uint8_t rtos_bool_t;

/** @brief 任务优先级类型。0 为最高优先级。 */
typedef uint32_t rtos_prio_t;

/** @brief Tick 计数类型，32 位可支持约 49 天(1kHz)。 */
typedef uint32_t rtos_tick_t;

/** @brief 任务入口函数指针类型。 */
typedef void (*rtos_task_func_t)(void *arg);

/** @brief 软件定时器回调函数类型。 */
typedef void (*rtos_timer_cb_t)(void *arg);

/** @brief 通用回调钩子类型。 */
typedef void (*rtos_hook_t)(void);

#ifndef RTOS_TRUE
#define RTOS_TRUE ((rtos_bool_t)1)
#endif
#ifndef RTOS_FALSE
#define RTOS_FALSE ((rtos_bool_t)0)
#endif

/* ============================== 错误状态码 ============================== */

/**
 * @brief 内核 API 返回状态码。
 * @note  遵循 0 表示成功、负值表示错误的惯例。
 */
typedef enum {
    RTOS_OK = 0, /**< 操作成功。 */
    RTOS_ERR_PARAM = -1, /**< 参数非法。 */
    RTOS_ERR_NO_MEM = -2, /**< 内存不足(任务池/队列池已满)。 */
    RTOS_ERR_TIMEOUT = -3, /**< 等待超时。 */
    RTOS_ERR_ISR = -4, /**< 在中断中调用了不允许的 API。 */
    RTOS_ERR_ALREADY = -5, /**< 已处于目标状态(如调度器已启动)。 */
    RTOS_ERR_NULL = -6, /**< 空指针。 */
    RTOS_ERR_OVERFLOW = -7, /**< 溢出(队列满/信号量计数超限)。 */
    RTOS_ERR_DELETED = -8, /**< 对象已被删除。 */
    RTOS_ERR_SCHED = -9, /**< 调度器状态错误。 */
} rtos_status_t;

/** @brief 等待方式: 是否阻塞及超时长度。 */
#define RTOS_NO_WAIT ((rtos_tick_t)0U) /**< 不阻塞，立即返回。 */
#define RTOS_WAIT_FOREVER ((rtos_tick_t)0xFFFFFFFFU) /**< 永久等待。 */

/* ============================== 任务状态 ============================== */

/**
 * @brief 任务运行状态。
 * @details 状态机:
 *            创建 -> READY -> RUNNING -> (BLOCKED/TIMEOUT) -> READY
 *            任何状态 -> DELETED
 */
typedef enum {
    RTOS_TASK_READY = 0, /**< 就绪: 等待调度器选中运行。 */
    RTOS_TASK_RUNNING = 1, /**< 运行: 当前占用 CPU(仅一个任务)。 */
    RTOS_TASK_BLOCKED = 2, /**< 阻塞: 等待信号量/队列/事件。 */
    RTOS_TASK_SUSPENDED = 3, /**< 挂起: 被显式挂起，不参与调度。 */
    RTOS_TASK_DELAYED = 4, /**< 延时: 等待指定 Tick 数。 */
    RTOS_TASK_DELETED = 5, /**< 已删除: TCB 可回收。 */
} rtos_task_state_t;

/** @brief 任务通知类型，用于轻量级任务间同步。 */
typedef enum {
    RTOS_NOTIFY_NONE = 0,
    RTOS_NOTIFY_VALUE = 1, /**< 通知一个 32 位值。 */
    RTOS_NOTIFY_BIT = 2, /**< 通知一组位(事件)。 */
    RTOS_NOTIFY_INCREMENT = 3, /**< 计数加一。 */
} rtos_notify_type_t;

/* ============================== 前向声明 ============================== */

/** @brief 任务控制块(TCB)。完整定义见 rtos_task.h。 */
typedef struct rtos_tcb rtos_tcb_t;

/** @brief 信号量。完整定义见 rtos_sem.h。 */
typedef struct rtos_sem rtos_sem_t;

/** @brief 互斥锁。完整定义见 rtos_mutex.h。 */
typedef struct rtos_mutex rtos_mutex_t;

/** @brief 消息队列。完整定义见 rtos_queue.h。 */
typedef struct rtos_queue rtos_queue_t;

/** @brief 事件标志组。完整定义见 rtos_event.h。 */
typedef struct rtos_event rtos_event_t;

/** @brief 软件定时器。完整定义见 rtos_timer.h。 */
typedef struct rtos_timer rtos_timer_t;

/* ============================== 阻塞节点 ============================== */

/**
 * @brief 阻塞等待节点。
 * @details 当任务在信号量/队列等对象上阻塞时，该节点被挂入对象的等待链表。
 *          节点内部携带 TCB 指针与唤醒原因，便于内核统一处理超时与解除阻塞。
 *          采用侵入式双向链表，避免动态分配。
 *
 *          零拷贝传递: 队列发送/接收阻塞时，transfer_buf 指向用户缓冲，
 *          唤醒对端时直接在两个缓冲间拷贝一次，跳过环形缓冲(对比传统实现
 *          节省一次 memcpy)。transfer_size 记录消息字节数(= item_size)。
 *
 *          超时清理: list_head 记录节点所在等待链表头的地址(由 block_current_on
 *          或 event_wait 设置; notify_wait 等无链表场景置 NULL)。process_delayed_tasks
 *          据此在超时时将节点从对象等待链表移除，避免悬空节点被后续 wake_highest/
 *          零拷贝直传误用(防止 use-after-free 与重复唤醒)。
 */
typedef struct rtos_wait_node {
    rtos_tcb_t *tcb; /**< 等待的任务。 */
    rtos_tick_t wake_tick; /**< 超时唤醒时刻(0=永不超时)。 */
    rtos_bool_t timed_out; /**< 唤醒原因: 是否因超时唤醒。 */
    void *wait_obj; /**< 所等待的对象指针(用于校验)。 */
    struct rtos_wait_node *next; /**< 链表后继。 */
    struct rtos_wait_node *prev; /**< 链表前驱。 */
    void *transfer_buf; /**< 零拷贝缓冲: 发送方=待发消息; 接收方=接收缓冲。 */
    uint32_t transfer_size; /**< 零拷贝字节数(队列场景= item_size)。 */
    struct rtos_wait_node **list_head; /**< 所在等待链表头地址(超时清理用; NULL=不在链表)。 */
} rtos_wait_node_t;

#ifdef __cplusplus
}
#endif

#endif /* RTOS_TYPES_H_ */

/**
 * @file rtos_channel.h
 * @brief 固定容量、单所有者的借用消息通道。
 *
 * @details FREE -> WRITING -> QUEUED -> READING -> FREE，取消 WRITING 可归还 FREE。
 * 通道只移动槽位索引及描述符，不复制 payload。每次 acquire 预留两个 64 位票据，
 * writer 使用第一个，receive 使用第二个；旧生产者票据不能归还消费者的缓冲。
 * 通道实例号在登记时单调分配，重建相同地址不会重新接纳旧票据，计数耗尽拒绝新增。
 *
 * close 禁止 acquire/publish，允许 drain、cancel/release；不强制回收借出的块。
 * deinit 仅在已关闭、全部块归还且等待链为空时注销。阻塞调用被唤醒后，必须按地址
 * 和保存的实例号重新查登记表，再访问对象，避免已唤醒但尚未返回的调用误用新实例。
 *
 * 票据可在经过同步的任务或合法 ISR 之间转交，不绑定某个任务。持有者被删除或丢失票据
 * 不会自动回收缓冲，管理者必须在停止所有访问后用保存的票据归还；否则 deinit BUSY。
 * 共享 payload 仅在当前租约期间有效；通道不搬运数据、不依赖 DMA 或芯片外设。
 * 不能阻止应用直接越界或使用旧裸指针。元数据/视图不得与通道存储重叠。
 */
#ifndef RTOS_CHANNEL_H_
#define RTOS_CHANNEL_H_

#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTOS_CHANNEL_ALIGNMENT (8U)
#define RTOS_CHANNEL_INDEX_NONE UINT32_MAX

typedef enum rtos_channel_slot_state {
    RTOS_CHANNEL_SLOT_FREE = 0,
    RTOS_CHANNEL_SLOT_WRITING = 1,
    RTOS_CHANNEL_SLOT_QUEUED = 2,
    RTOS_CHANNEL_SLOT_READING = 3
} rtos_channel_slot_state_t;

/** @brief 调用方提供的槽位元数据。登记期间不得直接修改。 */
typedef struct rtos_channel_slot {
    uint64_t generation;
    uint64_t sequence;
    uint32_t length;
    uint32_t tag;
    uint32_t next;
    rtos_channel_slot_state_t state;
} rtos_channel_slot_t;

/** @brief 不可由应用改写的能力票据；channel_address 用整数保存以避免旧裸指针解引用。 */
typedef struct rtos_channel_ticket {
    uint64_t incarnation;
    uint64_t generation;
    uintptr_t channel_address;
    uint32_t slot;
} rtos_channel_ticket_t;

/** @brief 生产者可写视图。成功 publish/cancel 会清零该视图。 */
typedef struct rtos_channel_writer {
    rtos_channel_ticket_t ticket;
    void *data;
    uint32_t capacity;
} rtos_channel_writer_t;

/** @brief 消费者只读视图。成功 release 会清零该视图。 */
typedef struct rtos_channel_reader {
    uint64_t sequence;
    rtos_channel_ticket_t ticket;
    const void *data;
    uint32_t length;
    uint32_t tag;
} rtos_channel_reader_t;

/** @brief 静态存储配置，长度是实际提供的字节数，初始化不会申请堆。 */
typedef struct rtos_channel_config {
    void *storage;
    size_t storage_size;
    rtos_channel_slot_t *slots;
    size_t slots_size;
    uint32_t block_size;
    uint32_t block_count;
} rtos_channel_config_t;

/** @brief 资源账本；capacity=free+writing+queued+reading，统计计数饱和而不回绕。 */
typedef struct rtos_channel_stats {
    uint64_t acquired;
    uint64_t published;
    uint64_t received;
    uint64_t released;
    uint64_t cancelled;
    uint64_t stale_rejections;
    uint64_t acquire_misses;
    uint64_t receive_misses;
    uint64_t acquire_waits;
    uint64_t receive_waits;
    uint64_t acquire_timeouts;
    uint64_t receive_timeouts;
    uint64_t closed_rejections;
    uint32_t capacity;
    uint32_t free;
    uint32_t writing;
    uint32_t queued;
    uint32_t reading;
    uint32_t queued_peak;
    uint32_t in_use_peak;
    uint32_t waiting_writers;
    uint32_t waiting_readers;
    uint32_t closed;
} rtos_channel_stats_t;

/** @brief 供调用方静态分配的控制块。所有字段由实现独占，不是可修改的公共状态。 */
typedef struct rtos_channel {
    uint64_t incarnation;
    uint64_t next_ticket;
    rtos_channel_stats_t stats;
    rtos_channel_slot_t *slots;
    uint8_t *storage;
    rtos_wait_node_t *writers;
    rtos_wait_node_t *readers;
    uint32_t block_size;
    uint32_t stride;
    uint32_t free_head;
    uint32_t ready_head;
    uint32_t ready_tail;
} rtos_channel_t;

#if RTOS_CONFIG_USE_CHANNEL

/**
 * @brief 登记并初始化静态通道，仅任务/启动上下文调用。
 * @param channel [out] 控制块，不得与其他活跃通道的控制/数据/元数据重叠。
 * @param config [in] 配置，storage 至少按 8 字节对齐，实际 stride=align8(block_size)。
 * @return OK；PARAM 为尺寸/对齐/重叠错误；ALREADY 为已登记；NO_MEM 为登记表满；
 *         OVERFLOW 为实例号耗尽；ISR 为不允许的上下文。失败不登记、不改写控制块。
 * @note storage 至少 stride*block_count 字节，slots 至少 block_count*sizeof(slot)。
 */
rtos_status_t rtos_channel_init(rtos_channel_t *channel, const rtos_channel_config_t *config);

/**
 * @brief 借出可写块，仅 acquire/receive 可能阻塞。
 * @param channel [in,out] 已登记通道。
 * @param writer [out] 成功时接收能力，失败保持不变；不得覆写尚未归还的能力。
 * @param timeout [in] NO_WAIT、1..0x7FFFFFFF 或 WAIT_FOREVER；重试共用一次总预算。
 * @return OK、TIMEOUT、CLOSED、DELETED、SCHED、ISR、PARAM 或票据耗尽 OVERFLOW。
 * @note 合法 syscall ISR 只允许 NO_WAIT；违规 ISR 无论是否开启断言均被拒绝。
 *       已经等待过的有限调用到期后不再授予资源；任务何时实际返回仍取决于调度。
 */
rtos_status_t rtos_channel_acquire(rtos_channel_t *channel, rtos_channel_writer_t *writer,
                                   rtos_tick_t timeout);

/**
 * @brief 提交写入块，成功即失去写权限，队列容量等于总块数，合法 writer 必有名额。
 * @param channel [in,out] 所属通道。
 * @param writer [in,out] 有效写入能力。
 * @param length [in] 有效字节数，0..capacity。
 * @param tag [in] 应用自定义标记，通道不解释。
 * @return OK；STALE 为错误能力；CLOSED 为已关闭；PARAM 为长度等输入错误。
 * @note 失败保留 writer 和所有权，允许修正重试或 cancel；不阻塞，合法 ISR 可调用。
 */
rtos_status_t rtos_channel_publish(rtos_channel_t *channel, rtos_channel_writer_t *writer,
                                   uint32_t length, uint32_t tag);

/**
 * @brief 取消 WRITING，归还块并使原能力失效，不阻塞，关闭后仍允许调用。
 * @param channel [in,out] 所属通道。
 * @param writer [in,out] 待取消能力；失败保持不变。
 * @return OK、STALE、DELETED、PARAM 或 ISR。
 */
rtos_status_t rtos_channel_cancel(rtos_channel_t *channel, rtos_channel_writer_t *writer);

/**
 * @brief 按成功发布 FIFO 借出只读消息；关闭后仍可排空已发布数据。
 * @param channel [in,out] 已登记通道。
 * @param reader [out] 成功时接收只读能力；失败保持不变，不得覆写未归还能力。
 * @param timeout [in] 与 acquire 相同的总等待预算。
 * @return OK；空且关闭为 CLOSED；其余与 acquire 对应。
 * @note 只有在任务实际运行并成功取得资源时才授予租约，唤醒不预留块。
 */
rtos_status_t rtos_channel_receive(rtos_channel_t *channel, rtos_channel_reader_t *reader,
                                   rtos_tick_t timeout);

/**
 * @brief 归还 READING 消息，不阻塞，关闭后仍可归还；成功清零 reader。
 * @param channel [in,out] 所属通道。
 * @param reader [in,out] 消费者能力；失败保持不变。
 * @return OK、STALE、DELETED、PARAM 或 ISR。
 */
rtos_status_t rtos_channel_release(rtos_channel_t *channel, rtos_channel_reader_t *reader);

/**
 * @brief 关闭新流量并唤醒等待者，仅任务上下文调用，不强制回收借用块。
 * @param channel [in,out] 待关闭通道。
 * @return OK、ALREADY、DELETED、PARAM 或 ISR。
 * @note 尚未持有租约的 waiter 返回 CLOSED；READY 消息可以继续 receive/drain。
 */
rtos_status_t rtos_channel_close(rtos_channel_t *channel);

/**
 * @brief 注销已关闭且已完全归还的通道，仅任务上下文调用。
 * @param channel [in,out] 控制块。成功后允许释放控制、数据和元数据存储。
 * @return OK；未关闭或仍有在途/排队块、等待者为 BUSY；其余为 DELETED/PARAM/ISR。
 * @note 已被唤醒但尚未返回的调用只比较保存的地址和实例号，不能解引用已注销存储。
 */
rtos_status_t rtos_channel_deinit(rtos_channel_t *channel);

/**
 * @brief 取得状态守恒账本，仅任务上下文；等待者计数需要 O(W) 扫描。
 * @param channel [in] 已登记通道。
 * @param stats [out] 成功时复制快照；失败保持不变。
 * @return OK、DELETED、PARAM 或 ISR。
 */
rtos_status_t rtos_channel_get_stats(const rtos_channel_t *channel, rtos_channel_stats_t *stats);

#else

static inline rtos_status_t rtos_channel_init(rtos_channel_t *channel,
                                              const rtos_channel_config_t *config)
{
    (void)channel;
    (void)config;
    return RTOS_ERR_DISABLED;
}

static inline rtos_status_t rtos_channel_acquire(rtos_channel_t *channel,
    rtos_channel_writer_t *writer, rtos_tick_t timeout)
{
    (void)channel;
    (void)writer;
    (void)timeout;
    return RTOS_ERR_DISABLED;
}

static inline rtos_status_t rtos_channel_publish(rtos_channel_t *channel,
    rtos_channel_writer_t *writer, uint32_t length, uint32_t tag)
{
    (void)channel;
    (void)writer;
    (void)length;
    (void)tag;
    return RTOS_ERR_DISABLED;
}

static inline rtos_status_t rtos_channel_cancel(rtos_channel_t *channel,
                                               rtos_channel_writer_t *writer)
{
    (void)channel;
    (void)writer;
    return RTOS_ERR_DISABLED;
}

static inline rtos_status_t rtos_channel_receive(rtos_channel_t *channel,
    rtos_channel_reader_t *reader, rtos_tick_t timeout)
{
    (void)channel;
    (void)reader;
    (void)timeout;
    return RTOS_ERR_DISABLED;
}

static inline rtos_status_t rtos_channel_release(rtos_channel_t *channel,
                                                rtos_channel_reader_t *reader)
{
    (void)channel;
    (void)reader;
    return RTOS_ERR_DISABLED;
}

static inline rtos_status_t rtos_channel_close(rtos_channel_t *channel)
{
    (void)channel;
    return RTOS_ERR_DISABLED;
}

static inline rtos_status_t rtos_channel_deinit(rtos_channel_t *channel)
{
    (void)channel;
    return RTOS_ERR_DISABLED;
}

static inline rtos_status_t rtos_channel_get_stats(const rtos_channel_t *channel,
                                                  rtos_channel_stats_t *stats)
{
    (void)channel;
    (void)stats;
    return RTOS_ERR_DISABLED;
}

#endif

#ifdef __cplusplus
}
#endif
#endif /* RTOS_CHANNEL_H_ */

/**
 * @file    rtos_usb.c
 * @brief   USB 子系统实现(对象池 / 堆 / ISR / CDC ACM 封装)
 *
 * @details 本文件实现 rtos_usb.h 声明的 USB 子系统。核心职责:
 *
 *          1. 静态对象池管理:
 *             - sem/mutex/mq/thread/timer 各维护一个静态数组池。
 *             - create 时扫描空闲槽(in_use==0), 标记并初始化底层 RTOS 对象。
 *             - delete 时 deinit 底层对象并归还槽位。
 *             - 池操作在临界区内扫描+标记, 防止多任务/ISR 并发分配同一槽。
 *
 *          2. USB 堆(双档固定块池):
 *             - 小块池(<=64B) + 大块池(<=512B), 各占 HEAP_SIZE 一半。
 *             - 用 rtos_mempool_t 实现, 复用内核 O(1) 分配器, 自动接入 perf 统计。
 *             - malloc 按 size 选档, free 按地址范围归还。
 *
 *          3. ISR 注册表:
 *             - 静态数组, 建立 irqn -> handler 映射。
 *             - dispatch 在用户 IRQHandler 中调用, 查表执行。
 *
 *          4. CDC ACM 用户封装:
 *             - 内置 USB 描述符(设备/配置/字符串/质量)。
 *             - 事件处理器: CONFIGURED 时启动首次 OUT 读取。
 *             - OUT 端点回调: 将收到的数据写入 RX 环形缓冲, 信号通知读线程。
 *             - IN 端点回调: 处理 ZLP, 信号通知写线程传输完成。
 *             - rtos_usb_cdc_read/write: 阻塞式 API, 内部用信号量同步。
 *
 *          5. 与内核和谐共存:
 *             - 线程经 rtos_task_create 创建, 自动接入 rtos_perf 与栈高水位。
 *             - 复用 rtos_sem/mutex/queue/timer, 优先级继承等特性自动生效。
 *             - 不修改任何内核源码, 纯适配层。
 */
#include "rtos_usb.h"

#if RTOS_CONFIG_USE_USB

#include "rtos_mem.h"
#include "rtos_sched.h"
#include <string.h>

/* CherryUSB 头文件(CDC ACM 封装用) */
#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#if RTOS_CONFIG_USB_USE_MSC
#include "usbd_msc.h" /* MSC 大容量存储(与 CDC 组成复合设备) */
#endif
#if RTOS_CONFIG_USB_USE_HID
#include "usbd_hid.h" /* HID(与 CDC/MSC 组成复合设备) */
#endif
#include "usb_dwc2_param.h" /* dwc2_get_user_fifo_config 自定义 FIFO 划分 */

/* ============================== DWC2 FIFO 自定义划分 ============================== */
/* F446 OTG_FS: 6 端点(EP0~EP5), FIFO 总深 320 字。默认 ST 参数只给 EP0~EP3
 * 分配 TX FIFO, 无法支撑复合设备的 4 个非控制 IN 端点。重新划分:
 *   RX FIFO          = 176 字 (704B, 所有 OUT 端点共享, 最小要求 47 字)
 *   TX FIFO[0](EP0)  = 16 字 (控制端点)
 *   TX FIFO[1](EP1)  = 64 字 (256B, CDC bulk IN 0x81, 全速 MPS=64B 余量充足)
 *   TX FIFO[2](EP2)  = 16 字 (保留, EP2 仅用作 OUT)
 *   TX FIFO[3](EP3)  = 16 字 (64B, CDC 通知 IN 0x83)
 *   TX FIFO[4](EP4)  = 16 字 (64B, MSC bulk IN 0x84, 全速 MPS=64B 恰好)
 *   TX FIFO[5](EP5)  = 16 字 (64B, HID 中断 IN 0x85, 与 MSC OUT 0x05 同号不同向)
 * 合计 176+144=320 字, 与 GHWCFG3 报告的 DFIFO 深度一致。
 * 由 usb_config.h 中的 CONFIG_USB_DWC2_CUSTOM_FIFO 启用。 */
void dwc2_get_user_fifo_config(uint32_t reg_base, struct usb_dwc2_user_fifo_config *config)
{
    (void)reg_base;
    config->device_rx_fifo_size = 176U;
    memset(config->device_tx_fifo_size, 0, sizeof(config->device_tx_fifo_size));
    config->device_tx_fifo_size[0] = 16U; /* EP0 控制 */
    config->device_tx_fifo_size[1] = 64U; /* EP1 IN: CDC bulk */
    config->device_tx_fifo_size[2] = 16U; /* EP2 保留 */
    config->device_tx_fifo_size[3] = 16U; /* EP3 IN: CDC 通知 */
    config->device_tx_fifo_size[4] = 16U; /* EP4 IN: MSC bulk */
    config->device_tx_fifo_size[5] = 16U; /* EP5 IN: HID 中断 */
}

/* ============================== 静态对象池 ============================== */

static rtos_usb_sem_t s_sem_pool[RTOS_CONFIG_USB_OSAL_SEM_COUNT];
static rtos_usb_mutex_t s_mutex_pool[RTOS_CONFIG_USB_OSAL_MUTEX_COUNT];
static rtos_usb_mq_t s_mq_pool[RTOS_CONFIG_USB_OSAL_MQ_COUNT];
static rtos_usb_thread_t s_thread_pool[RTOS_CONFIG_USB_OSAL_THREAD_COUNT];
static rtos_usb_timer_t s_timer_pool[RTOS_CONFIG_USB_OSAL_TIMER_COUNT];

/* ============================== ISR 注册表 ============================== */

typedef struct {
    uint32_t irqn;
    rtos_usb_isr_handler_t handler;
    void *arg;
    uint8_t used;
} rtos_usb_isr_entry_t;

static rtos_usb_isr_entry_t s_isr_table[RTOS_CONFIG_USB_ISR_TABLE_SIZE];

/* ============================== USB 堆(双档固定块池) ============================== */

#define USB_SMALL_BLOCK_SIZE (64U)
#define USB_LARGE_BLOCK_SIZE (512U)
#define USB_SMALL_BLOCK_COUNT ((RTOS_CONFIG_USB_HEAP_SIZE / 2U) / USB_SMALL_BLOCK_SIZE)
#define USB_LARGE_BLOCK_COUNT ((RTOS_CONFIG_USB_HEAP_SIZE / 2U) / USB_LARGE_BLOCK_SIZE)

static uint8_t s_small_heap[USB_SMALL_BLOCK_SIZE * USB_SMALL_BLOCK_COUNT]
    __attribute__((aligned(4)));
static uint8_t s_large_heap[USB_LARGE_BLOCK_SIZE * USB_LARGE_BLOCK_COUNT]
    __attribute__((aligned(4)));

static rtos_mempool_t s_small_pool;
static rtos_mempool_t s_large_pool;

/* ============================== 毫秒转 Tick ============================== */

static rtos_tick_t usb_ms_to_ticks(uint32_t ms)
{
    if (ms == RTOS_WAIT_FOREVER) {
        /* 永久等待: 直接透传, 避免下方乘法溢出 */
        return RTOS_WAIT_FOREVER;
    }
    if (ms == 0U) {
        return 0U;
    }
    uint32_t ticks = (ms * RTOS_CONFIG_TICK_RATE_HZ) / 1000U;
    if (ticks == 0U) {
        ticks = 1U;
    }
    return ticks;
}

/* ============================== 内部辅助: 池分配/归还 ============================== */

static rtos_usb_sem_t *alloc_sem_slot(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_OSAL_SEM_COUNT; i++) {
        if (s_sem_pool[i].in_use == 0U) {
            s_sem_pool[i].in_use = 1U;
            RTOS_PORT_EXIT_CRITICAL();
            return &s_sem_pool[i];
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return NULL;
}

static rtos_usb_mutex_t *alloc_mutex_slot(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_OSAL_MUTEX_COUNT; i++) {
        if (s_mutex_pool[i].in_use == 0U) {
            s_mutex_pool[i].in_use = 1U;
            RTOS_PORT_EXIT_CRITICAL();
            return &s_mutex_pool[i];
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return NULL;
}

static rtos_usb_mq_t *alloc_mq_slot(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_OSAL_MQ_COUNT; i++) {
        if (s_mq_pool[i].in_use == 0U) {
            s_mq_pool[i].in_use = 1U;
            RTOS_PORT_EXIT_CRITICAL();
            return &s_mq_pool[i];
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return NULL;
}

static rtos_usb_thread_t *alloc_thread_slot(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_OSAL_THREAD_COUNT; i++) {
        if (s_thread_pool[i].in_use == 0U) {
            s_thread_pool[i].in_use = 1U;
            RTOS_PORT_EXIT_CRITICAL();
            return &s_thread_pool[i];
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return NULL;
}

static rtos_usb_timer_t *alloc_timer_slot(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_OSAL_TIMER_COUNT; i++) {
        if (s_timer_pool[i].in_use == 0U) {
            s_timer_pool[i].in_use = 1U;
            RTOS_PORT_EXIT_CRITICAL();
            return &s_timer_pool[i];
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return NULL;
}

/* ============================== 初始化 ============================== */

rtos_status_t rtos_usb_init(void)
{
    memset(s_sem_pool, 0, sizeof(s_sem_pool));
    memset(s_mutex_pool, 0, sizeof(s_mutex_pool));
    memset(s_mq_pool, 0, sizeof(s_mq_pool));
    memset(s_thread_pool, 0, sizeof(s_thread_pool));
    memset(s_timer_pool, 0, sizeof(s_timer_pool));
    memset(s_isr_table, 0, sizeof(s_isr_table));

    rtos_status_t st =
        rtos_mempool_init(&s_small_pool, s_small_heap, USB_SMALL_BLOCK_SIZE, USB_SMALL_BLOCK_COUNT);
    if (st != RTOS_OK) {
        return st;
    }
    st =
        rtos_mempool_init(&s_large_pool, s_large_heap, USB_LARGE_BLOCK_SIZE, USB_LARGE_BLOCK_COUNT);
    return st;
}

/* ============================== ISR 注册与分发 ============================== */

rtos_status_t rtos_usb_register_isr(uint32_t irqn, rtos_usb_isr_handler_t handler, void *arg)
{
    if (handler == NULL) {
        return RTOS_ERR_NULL;
    }

    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_ISR_TABLE_SIZE; i++) {
        if ((s_isr_table[i].used != 0U) && (s_isr_table[i].irqn == irqn)) {
            s_isr_table[i].handler = handler;
            s_isr_table[i].arg = arg;
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_OK;
        }
    }
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_ISR_TABLE_SIZE; i++) {
        if (s_isr_table[i].used == 0U) {
            s_isr_table[i].irqn = irqn;
            s_isr_table[i].handler = handler;
            s_isr_table[i].arg = arg;
            s_isr_table[i].used = 1U;
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_OK;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_ERR_NO_MEM;
}

void rtos_usb_irq_dispatch(uint32_t irqn)
{
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_ISR_TABLE_SIZE; i++) {
        if ((s_isr_table[i].used != 0U) && (s_isr_table[i].irqn == irqn)) {
            s_isr_table[i].handler(s_isr_table[i].arg);
            return;
        }
    }
}

/* ============================== 信号量 ============================== */

rtos_usb_sem_t *rtos_usb_sem_create(uint32_t init_count, uint32_t max_count)
{
    rtos_usb_sem_t *slot = alloc_sem_slot();
    if (slot == NULL) {
        return NULL;
    }
    uint32_t max_cnt = max_count;
    if (max_cnt < 1U) {
        max_cnt = 1U;
    }
    if (max_cnt < init_count) {
        max_cnt = init_count;
    }
    if (rtos_sem_init(&slot->sem, init_count, max_cnt) != RTOS_OK) {
        slot->in_use = 0U;
        return NULL;
    }
    return slot;
}

void rtos_usb_sem_delete(rtos_usb_sem_t *sem)
{
    if (sem == NULL) {
        return;
    }
    rtos_sem_deinit(&sem->sem);
    sem->in_use = 0U;
}

rtos_status_t rtos_usb_sem_take(rtos_usb_sem_t *sem, rtos_tick_t timeout)
{
    if (sem == NULL) {
        return RTOS_ERR_NULL;
    }
    return rtos_sem_take(&sem->sem, timeout);
}

rtos_status_t rtos_usb_sem_give(rtos_usb_sem_t *sem)
{
    if (sem == NULL) {
        return RTOS_ERR_NULL;
    }
    return rtos_sem_give(&sem->sem);
}

void rtos_usb_sem_reset(rtos_usb_sem_t *sem)
{
    if (sem == NULL) {
        return;
    }
    /* 将计数清零: 在临界区内操作, 唤醒所有等待者(它们会因 count=0 而继续阻塞)。
     * 注意: 本 RTOS 的 rtos_sem 没有 reset API, 这里手动清零 count。
     * 等待者不会丢失(它们仍在等待链表上), 只是不会因这次 reset 而被唤醒。 */
    RTOS_PORT_ENTER_CRITICAL();
    sem->sem.count = 0U;
    RTOS_PORT_EXIT_CRITICAL();
}

/* ============================== 互斥锁 ============================== */

rtos_usb_mutex_t *rtos_usb_mutex_create(void)
{
    rtos_usb_mutex_t *slot = alloc_mutex_slot();
    if (slot == NULL) {
        return NULL;
    }
    if (rtos_mutex_init(&slot->mutex) != RTOS_OK) {
        slot->in_use = 0U;
        return NULL;
    }
    return slot;
}

void rtos_usb_mutex_delete(rtos_usb_mutex_t *mutex)
{
    if (mutex == NULL) {
        return;
    }
    rtos_mutex_deinit(&mutex->mutex);
    mutex->in_use = 0U;
}

rtos_status_t rtos_usb_mutex_take(rtos_usb_mutex_t *mutex)
{
    if (mutex == NULL) {
        return RTOS_ERR_NULL;
    }
    return rtos_mutex_take(&mutex->mutex, RTOS_WAIT_FOREVER);
}

rtos_status_t rtos_usb_mutex_give(rtos_usb_mutex_t *mutex)
{
    if (mutex == NULL) {
        return RTOS_ERR_NULL;
    }
    return rtos_mutex_give(&mutex->mutex);
}

/* ============================== 消息队列 ============================== */

rtos_usb_mq_t *rtos_usb_mq_create(uint32_t max_msgs)
{
    if ((max_msgs == 0U) || (max_msgs > RTOS_CONFIG_USB_MQ_DEPTH)) {
        return NULL;
    }
    rtos_usb_mq_t *slot = alloc_mq_slot();
    if (slot == NULL) {
        return NULL;
    }
    if (rtos_queue_init(&slot->queue, slot->storage, sizeof(uintptr_t), max_msgs) != RTOS_OK) {
        slot->in_use = 0U;
        return NULL;
    }
    return slot;
}

void rtos_usb_mq_delete(rtos_usb_mq_t *mq)
{
    if (mq == NULL) {
        return;
    }
    rtos_queue_deinit(&mq->queue);
    mq->in_use = 0U;
}

rtos_status_t rtos_usb_mq_send(rtos_usb_mq_t *mq, uintptr_t addr, rtos_tick_t timeout)
{
    if (mq == NULL) {
        return RTOS_ERR_NULL;
    }
    return rtos_queue_send(&mq->queue, &addr, timeout, RTOS_FALSE);
}

rtos_status_t rtos_usb_mq_recv(rtos_usb_mq_t *mq, uintptr_t *addr, rtos_tick_t timeout)
{
    if ((mq == NULL) || (addr == NULL)) {
        return RTOS_ERR_NULL;
    }
    return rtos_queue_recv(&mq->queue, addr, timeout);
}

/* ============================== 线程 ============================== */

rtos_usb_thread_t *rtos_usb_thread_create(const char *name, uint32_t stack_size, rtos_prio_t prio,
                                          rtos_task_func_t entry, void *arg)
{
    if (entry == NULL) {
        return NULL;
    }
    /* 优先级越界保护: CherryUSB 可能传入 >= MAX_PRIORITIES 的值 */
    if (prio >= RTOS_CONFIG_MAX_PRIORITIES) {
        prio = RTOS_CONFIG_MAX_PRIORITIES - 1U;
    }
    rtos_usb_thread_t *slot = alloc_thread_slot();
    if (slot == NULL) {
        return NULL;
    }
    uint32_t actual_stack = stack_size;
    if ((actual_stack == 0U) || (actual_stack > RTOS_CONFIG_USB_THREAD_STACK_SIZE)) {
        actual_stack = RTOS_CONFIG_USB_THREAD_STACK_SIZE;
    }
    if (actual_stack < RTOS_CONFIG_MINIMAL_STACK_SIZE) {
        actual_stack = RTOS_CONFIG_MINIMAL_STACK_SIZE;
    }
    if (rtos_task_create_static(&slot->tcb, slot->stack, actual_stack, entry, arg, prio, name) !=
        RTOS_OK) {
        slot->in_use = 0U;
        return NULL;
    }
    return slot;
}

void rtos_usb_thread_delete(rtos_usb_thread_t *thread)
{
    if (thread == NULL) {
        rtos_task_delete(NULL);
        return;
    }
    rtos_task_delete(&thread->tcb);
    thread->in_use = 0U;
}

void rtos_usb_thread_schedule_other(void)
{
    /* CherryUSB usb_osal_thread_schedule_other: 暂时降低当前线程优先级到最低,
     * 让出 CPU, 然后恢复。用于让低优先级 USB 任务有机会执行。 */
    rtos_tcb_t *cur = rtos_task_get_current();
    if (cur == NULL) {
        return;
    }
    rtos_prio_t old_prio = rtos_task_get_priority(cur);
    rtos_task_set_priority(cur, RTOS_CONFIG_MAX_PRIORITIES - 1U);
    rtos_task_yield();
    rtos_task_set_priority(cur, old_prio);
}

/* ============================== 定时器 ============================== */

rtos_usb_timer_t *rtos_usb_timer_create(const char *name, uint32_t ms, rtos_timer_cb_t cb,
                                        void *arg, bool is_periodic)
{
    if (cb == NULL) {
        return NULL;
    }
    rtos_usb_timer_t *slot = alloc_timer_slot();
    if (slot == NULL) {
        return NULL;
    }
    rtos_timer_mode_t mode = is_periodic ? RTOS_TIMER_PERIODIC : RTOS_TIMER_ONE_SHOT;
    if (rtos_timer_create(&slot->timer, name, cb, arg, usb_ms_to_ticks(ms), mode) != RTOS_OK) {
        slot->in_use = 0U;
        return NULL;
    }
    return slot;
}

void rtos_usb_timer_delete(rtos_usb_timer_t *timer)
{
    if (timer == NULL) {
        return;
    }
    rtos_timer_delete(&timer->timer);
    timer->in_use = 0U;
}

void rtos_usb_timer_start(rtos_usb_timer_t *timer)
{
    if (timer == NULL) {
        return;
    }
    rtos_timer_start(&timer->timer);
}

void rtos_usb_timer_stop(rtos_usb_timer_t *timer)
{
    if (timer == NULL) {
        return;
    }
    rtos_timer_stop(&timer->timer);
}

/* ============================== USB 堆 ============================== */

void *rtos_usb_malloc(size_t size)
{
    if (size == 0U) {
        return NULL;
    }
    if (size <= USB_SMALL_BLOCK_SIZE) {
        return rtos_mempool_alloc(&s_small_pool);
    }
    if (size <= USB_LARGE_BLOCK_SIZE) {
        return rtos_mempool_alloc(&s_large_pool);
    }
    return NULL;
}

void rtos_usb_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    uint8_t *p = (uint8_t *)ptr;
    if ((p >= s_small_heap) && (p < s_small_heap + sizeof(s_small_heap))) {
        rtos_mempool_free(&s_small_pool, ptr);
    } else if ((p >= s_large_heap) && (p < s_large_heap + sizeof(s_large_heap))) {
        rtos_mempool_free(&s_large_pool, ptr);
    }
}

void rtos_usb_msleep(uint32_t ms)
{
    rtos_task_delay(usb_ms_to_ticks(ms));
}

/*============================================================================*/
/*====================  CDC ACM 用户封装(虚拟串口)  ==========================*/
/*============================================================================*/

/* 端点地址(端点号 <= 5, 受 F446 OTG_FS 6 端点限制; 同端点号 IN/OUT 互相独立) */
#define CDC_IN_EP 0x81 /* EP1 IN (CDC 批量 IN) */
#define CDC_OUT_EP 0x02 /* EP2 OUT (CDC 批量 OUT) */
#define CDC_INT_EP 0x83 /* EP3 IN (CDC 通知) */

#if RTOS_CONFIG_USB_USE_MSC
#define MSC_IN_EP 0x84 /* EP4 IN (MSC 批量 IN) */
#define MSC_OUT_EP 0x05 /* EP5 OUT (MSC 批量 OUT) */
#endif

#if RTOS_CONFIG_USB_USE_HID
#define HID_OUT_EP 0x01 /* EP1 OUT (HID 中断 OUT, 与 CDC_IN_EP 同端点号不同方向) */
#define HID_IN_EP 0x85 /* EP5 IN (HID 中断 IN, 与 MSC_OUT_EP 同端点号不同方向) */
#define HID_MPS 64U /* HID 报告大小(字节) */
#define HID_INTERVAL 1U /* 轮询间隔(ms) */
#endif

#define USBD_VID RTOS_CONFIG_USB_VID
#define USBD_PID RTOS_CONFIG_USB_PID
#define USBD_MAX_POWER 100 /* 电流(单位 2mA, 100=200mA) */
#define USBD_LANGID_STRING 1033 /* 英语 */

/* 复合设备接口数: CDC(IAD, 2 接口) + MSC(1) + HID(1) */
#define USBD_CONFIG_ITF_NUM \
    (0x02U + (RTOS_CONFIG_USB_USE_MSC ? 1U : 0U) + (RTOS_CONFIG_USB_USE_HID ? 1U : 0U))

#define USB_CONFIG_SIZE                                                                              \
    (9U + CDC_ACM_DESCRIPTOR_LEN + (RTOS_CONFIG_USB_USE_MSC ? MSC_DESCRIPTOR_LEN : 0U) +             \
     (RTOS_CONFIG_USB_USE_HID ? HID_CUSTOM_INOUT_DESCRIPTOR_LEN : 0U))

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

#if RTOS_CONFIG_USB_USE_MSC
#ifdef CONFIG_USB_HS
#define MSC_MAX_MPS 512
#else
#define MSC_MAX_MPS 64
#endif
#endif

/* 类被裁剪时提供长度回退值, 使上面的 USB_CONFIG_SIZE 宏始终可展开 */
#if !RTOS_CONFIG_USB_USE_MSC
#define MSC_DESCRIPTOR_LEN 0U
#endif
#if !RTOS_CONFIG_USB_USE_HID
#define HID_CUSTOM_INOUT_DESCRIPTOR_LEN 0U
#endif

/* ---- 环形缓冲优化(Linux/FreeRTOS StreamBuffer/Zephyr ring_buf 通用模式) ---- */
/* 采用"原始计数器"模式: head/tail 始终递增(uint32_t 自然回绕), 仅在数组访问时
 * 用掩码(2 的幂)或取模(非 2 的幂)转换为索引。
 *   优点: count/space 计算用简单减法, 无取模; 2 的幂时数组访问用位与(快 3~5 倍)。
 *   COUNT(h,t) = h - t         (uint32_t 自然回绕, 永远正确)
 *   SPACE(h,t) = SIZE - 1 - COUNT(h,t)  (保留 1 字节区分满/空) */
#define RTOS_USB_IS_POW2(x) (((x) != 0U) && (((x) & ((x) - 1U)) == 0U))

#if RTOS_USB_IS_POW2(RTOS_CONFIG_USB_CDC_RX_BUF_SIZE)
/* 2 的幂: 用位掩码替代取模 */
#define RX_BUF_POS(i) ((i) & (RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - 1U))
#else
/* 非 2 的幂: 用取模 */
#define RX_BUF_POS(i) ((i) % RTOS_CONFIG_USB_CDC_RX_BUF_SIZE)
#endif
/* COUNT/SPACE 对 2 的幂和非 2 的幂都适用(依赖 uint32_t 自然回绕) */
#define RX_BUF_SPACE(h, t) (RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - 1U - ((h) - (t)))
#define RX_BUF_COUNT(h, t) ((h) - (t))

#ifndef RTOS_USB_MIN
#define RTOS_USB_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* ---- 描述符 ---- */

#if RTOS_CONFIG_USB_USE_HID
/* 自定义厂商定义 HID 报告描述符: 64 字节 Input 报告 + 64 字节 Output 报告。
 * 主机侧可用 hidapi 或 CherryUSB 的 tools/test_srcipts/test_hid_inout.py 测试。 */
static const uint8_t s_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF, /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,       /* Usage (0x01) */
    0xA1, 0x01,       /* Collection (Application) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8 bits) */
    0x95, 0x40,       /*   Report Count (64) --- Input 报告 */
    0x09, 0x02,       /*   Usage (0x02) */
    0x81, 0x02,       /*   Input (Data, Var, Abs) */
    0x95, 0x40,       /*   Report Count (64) --- Output 报告 */
    0x09, 0x03,       /*   Usage (0x03) */
    0x91, 0x02,       /*   Output (Data, Var, Abs) */
    0xC0,             /* End Collection */
};
#endif

static const uint8_t s_device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)};

static const uint8_t s_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, USBD_CONFIG_ITF_NUM, 0x01, USB_CONFIG_BUS_POWERED,
                               USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
#if RTOS_CONFIG_USB_USE_MSC
        ,
    MSC_DESCRIPTOR_INIT(0x02, MSC_OUT_EP, MSC_IN_EP, MSC_MAX_MPS, 0x00)
#endif
#if RTOS_CONFIG_USB_USE_HID
        ,
    /* HID 接口号: CDC 占 0/1, MSC 启用时占 2, HID 紧随其后 */
    HID_CUSTOM_INOUT_DESCRIPTOR_INIT((0x02U + (RTOS_CONFIG_USB_USE_MSC ? 1U : 0U)), 0x00,
                                     sizeof(s_hid_report_descriptor), HID_OUT_EP, HID_IN_EP,
                                     HID_MPS, HID_INTERVAL)
#endif
};

static const uint8_t s_device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *s_string_descriptors[] = {
    (const char[]){0x09, 0x04}, /* Langid */
    "RTOS", /* Manufacturer */
#if RTOS_CONFIG_USB_USE_HID
    "RTOS CDC MSC HID", /* Product (复合设备: 虚拟串口 + U盘 + HID) */
#elif RTOS_CONFIG_USB_USE_MSC
    "RTOS CDC MSC", /* Product (复合设备: 虚拟串口 + U盘) */
#else
    "RTOS CDC ACM", /* Product */
#endif
    "000000000001", /* Serial Number */
};

static const uint8_t *s_device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_descriptor;
}

static const uint8_t *s_config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_config_descriptor;
}

static const uint8_t *s_device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_quality_descriptor;
}

static const char *s_string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index >= (sizeof(s_string_descriptors) / sizeof(char *))) {
        return NULL;
    }
    return s_string_descriptors[index];
}

static const struct usb_descriptor s_cdc_descriptor = {
    .device_descriptor_callback = s_device_descriptor_callback,
    .config_descriptor_callback = s_config_descriptor_callback,
    .device_quality_descriptor_callback = s_device_quality_descriptor_callback,
    .string_descriptor_callback = s_string_descriptor_callback,
};

/* ---- CDC 设备上下文 ---- */

typedef struct {
    volatile bool connected; /* 主机已配置 */
    volatile bool tx_busy; /* TX DMA 传输进行中(回调清除) */
    volatile uint32_t tx_done_seq; /* TX 完成序号(仅 IN 回调完成分支递增,
                                    * 写者用它区分"真完成"与"事件唤醒") */
    rtos_usb_sem_t *rx_sem; /* RX 数据可用信号量 */
    rtos_usb_sem_t *tx_sem; /* TX 完成信号量 */
    rtos_usb_mutex_t *tx_mutex; /* TX 串行化互斥锁 */
    /* RX 环形缓冲(ISR 写, 任务读) */
    uint8_t rx_buf[RTOS_CONFIG_USB_CDC_RX_BUF_SIZE];
    volatile uint32_t rx_head; /* 写入位置(ISR 更新) */
    volatile uint32_t rx_tail; /* 读取位置(任务更新) */
    volatile uint32_t rx_dropped; /* RX 缓冲满丢弃的字节数(调试观测) */
    /* OUT 端点接收缓冲(DMA 用, 非缓存+对齐) */
    USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep_out_buf[RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE];
    /* IN 端点发送缓冲(DMA 用, 非缓存+对齐)。
     * 用户数据先拷贝到此缓冲再 DMA, 保证:
     *   1. 超时返回后 DMA 不会读到已复用的用户缓冲
     *   2. Cortex-M7 D-cache 一致性(非缓存段) */
    USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep_in_buf[RTOS_CONFIG_USB_CDC_TX_BUF_SIZE];
    /* CherryUSB 接口/端点对象 */
    struct usbd_interface intf0; /* CDC 通信接口 */
    struct usbd_interface intf1; /* CDC 数据接口 */
    struct usbd_endpoint cdc_out_ep; /* OUT 端点 */
    struct usbd_endpoint cdc_in_ep; /* IN 端点 */
#if RTOS_CONFIG_USB_USE_MSC
    struct usbd_interface msc_intf; /* MSC 接口(端点由 usbd_msc 类内部管理) */
#endif
#if RTOS_CONFIG_USB_USE_HID
    struct usbd_interface hid_intf; /* HID 接口 */
    struct usbd_endpoint hid_out_ep; /* HID 中断 OUT 端点 */
    struct usbd_endpoint hid_in_ep; /* HID 中断 IN 端点 */
    rtos_usb_sem_t *hid_tx_sem; /* HID IN 发送完成信号量 */
    rtos_usb_sem_t *hid_rx_sem; /* HID OUT 报告到达信号量 */
    rtos_usb_mutex_t *hid_tx_mutex; /* HID 写入串行化互斥锁(多任务并发保护) */
    volatile bool hid_tx_busy; /* HID IN 发送进行中 */
    volatile uint32_t hid_rx_len; /* 待读 OUT 报告长度(0=无) */
    uint8_t hid_rx_buf[HID_MPS]; /* OUT 报告单槽缓冲(读走前新报告被丢弃) */
    /* IN/OUT 端点 DMA 缓冲(非缓存+对齐) */
    USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_ep_in_buf[HID_MPS];
    USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_ep_out_buf[HID_MPS];
#endif
    bool initialized; /* 已初始化标志 */
} rtos_usb_cdc_dev_t;

static rtos_usb_cdc_dev_t s_cdc_dev[CONFIG_USBDEV_MAX_BUS];

/* ---- 事件处理器 ---- */

static void rtos_usb_cdc_event_handler(uint8_t busid, uint8_t event)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    switch (event) {
        case USBD_EVENT_CONFIGURED:
            dev->connected = true;
            dev->tx_busy = false;
            /* 清空 RX 缓冲 + 重置丢弃计数 */
            dev->rx_head = 0U;
            dev->rx_tail = 0U;
            dev->rx_dropped = 0U;
            /* 唤醒可能在 RESET 后仍阻塞的写线程(让其检查 connected 后重试) */
            if (dev->tx_sem != NULL) {
                rtos_usb_sem_give(dev->tx_sem);
            }
            /* 启动首次 OUT 读取(异步, 收到数据后回调中继续读) */
            usbd_ep_start_read(busid, CDC_OUT_EP, dev->ep_out_buf,
                               RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE);
#if RTOS_CONFIG_USB_USE_HID
            dev->hid_tx_busy = false;
            dev->hid_rx_len = 0U;
            /* 启动首次 HID OUT 读取 */
            usbd_ep_start_read(busid, HID_OUT_EP, dev->hid_ep_out_buf, HID_MPS);
#endif
            break;

        case USBD_EVENT_DISCONNECTED:
            dev->connected = false;
            dev->tx_busy = false;
            /* 唤醒可能阻塞的读/写线程 */
            if (dev->rx_sem != NULL) {
                rtos_usb_sem_give(dev->rx_sem);
            }
            if (dev->tx_sem != NULL) {
                rtos_usb_sem_give(dev->tx_sem);
            }
#if RTOS_CONFIG_USB_USE_HID
            dev->hid_tx_busy = false;
            if (dev->hid_rx_sem != NULL) {
                rtos_usb_sem_give(dev->hid_rx_sem);
            }
            if (dev->hid_tx_sem != NULL) {
                rtos_usb_sem_give(dev->hid_tx_sem);
            }
#endif
            break;

        case USBD_EVENT_RESET:
            dev->connected = false;
            dev->tx_busy = false;
            /* 清空 RX 缓冲(避免 RESET 后读者读到陈旧数据) */
            dev->rx_head = 0U;
            dev->rx_tail = 0U;
            /* 唤醒可能阻塞的读/写线程(主机重新枚举时只有 RESET, 无 DISCONNECTED)。
             * 否则用 RTOS_WAIT_FOREVER 的线程会永久阻塞。 */
            if (dev->rx_sem != NULL) {
                rtos_usb_sem_give(dev->rx_sem);
            }
            if (dev->tx_sem != NULL) {
                rtos_usb_sem_give(dev->tx_sem);
            }
#if RTOS_CONFIG_USB_USE_HID
            dev->hid_tx_busy = false;
            if (dev->hid_rx_sem != NULL) {
                rtos_usb_sem_give(dev->hid_rx_sem);
            }
            if (dev->hid_tx_sem != NULL) {
                rtos_usb_sem_give(dev->hid_tx_sem);
            }
#endif
            break;

        default:
            break;
    }
}

/* ---- OUT 端点回调(数据到达) ---- */
/* 性能优化: 用 memcpy 两段拷贝替代逐字节循环, ISR 耗时减少 5~10 倍。
 * 环形缓冲可能跨越末尾, 需要分两段拷贝:
 *   第一段: head -> 缓冲末尾
 *   第二段: 缓冲起始 -> 剩余空间 */

static void rtos_usb_cdc_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (nbytes > 0U) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t head = dev->rx_head;
        uint32_t tail = dev->rx_tail;
        uint32_t space = RX_BUF_SPACE(head, tail);
        /* 防御: nbytes 不应超过 ep_out_buf 大小(硬件契约), 但防御性截断 */
        uint32_t bounded_nbytes = RTOS_USB_MIN(nbytes, RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE);
        uint32_t to_copy = RTOS_USB_MIN(bounded_nbytes, space);

        if (to_copy > 0U) {
            /* 第一段: 从 head 到缓冲末尾。
             * 注意: head/tail 是"原始计数器"(始终递增, uint32_t 自然回绕),
             *       仅在数组索引时用 RX_BUF_POS() 转换, 切勿回写掩码后的值。 */
            uint32_t first_chunk =
                RTOS_USB_MIN(to_copy, RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - RX_BUF_POS(head));
            memcpy(&dev->rx_buf[RX_BUF_POS(head)], &dev->ep_out_buf[0], first_chunk);
            head += first_chunk;

            /* 第二段: 缓冲起始到剩余 */
            uint32_t second_chunk = to_copy - first_chunk;
            if (second_chunk > 0U) {
                memcpy(&dev->rx_buf[0], &dev->ep_out_buf[first_chunk], second_chunk);
                head += second_chunk;
            }
            dev->rx_head = head;
        }
        /* 统计丢弃字节(调试观测, 不影响功能) */
        if (nbytes > to_copy) {
            dev->rx_dropped += (nbytes - to_copy);
        }
        RTOS_PORT_EXIT_CRITICAL();

        /* 通知读线程有数据可用 */
        if (dev->rx_sem != NULL) {
            rtos_usb_sem_give(dev->rx_sem);
        }
    }

    /* 启动下一次 OUT 读取 */
    usbd_ep_start_read(busid, CDC_OUT_EP, dev->ep_out_buf, RTOS_CONFIG_USB_CDC_EP_OUT_BUF_SIZE);
}

/* ---- IN 端点回调(发送完成) ---- */
/* 安全优化: 仅在 tx_busy=true 时给信号量, 防止陈旧回调(超时后迟到的完成)
 * 产生虚假信号污染下一次发送。 */

static void rtos_usb_cdc_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    /* ZLP 处理: 当发送长度是 MPS 整数倍且非零时, 需补发零长度包。
     * 防御: usbd_get_ep_mps 在端点未配置时返回 0, 需避免除零。 */
    uint16_t mps = usbd_get_ep_mps(busid, ep);
    if ((nbytes > 0U) && (mps > 0U) && ((nbytes % mps) == 0U)) {
        usbd_ep_start_write(busid, CDC_IN_EP, NULL, 0U);
    } else {
        /* 仅在有活跃发送时才通知, 避免陈旧信号 */
        if (dev->tx_busy) {
            dev->tx_busy = false;
            dev->tx_done_seq++; /* 真完成标记: 区别于 RESET/DISCONNECT 的事件唤醒 */
            if (dev->tx_sem != NULL) {
                rtos_usb_sem_give(dev->tx_sem);
            }
        }
    }
}

/* ---- HID IN 端点回调(报告发送完成) ---- */
/* 中断端点无 ZLP 语义(仅批量/控制端点需要), 直接通知即可。 */
#if RTOS_CONFIG_USB_USE_HID
static void rtos_usb_hid_in_done(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    (void)nbytes;
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (dev->hid_tx_busy) {
        dev->hid_tx_busy = false;
        if (dev->hid_tx_sem != NULL) {
            rtos_usb_sem_give(dev->hid_tx_sem);
        }
    }
}

/* ---- HID OUT 端点回调(主机发来 Output 报告) ---- */
/* 单槽缓冲: 上一个报告未被读走时, 新报告直接丢弃(避免撕裂)。 */
static void rtos_usb_hid_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (nbytes > HID_MPS) {
        nbytes = HID_MPS;
    }
    if (nbytes > 0U) {
        RTOS_PORT_ENTER_CRITICAL();
        if (dev->hid_rx_len == 0U) {
            memcpy(dev->hid_rx_buf, dev->hid_ep_out_buf, nbytes);
            dev->hid_rx_len = nbytes;
        }
        RTOS_PORT_EXIT_CRITICAL();

        if (dev->hid_rx_sem != NULL) {
            rtos_usb_sem_give(dev->hid_rx_sem);
        }
    }

    /* 启动下一次 HID OUT 读取 */
    usbd_ep_start_read(busid, HID_OUT_EP, dev->hid_ep_out_buf, HID_MPS);
}
#endif /* RTOS_CONFIG_USB_USE_HID */

/* ---- CDC ACM 公共 API ---- */

rtos_status_t rtos_usb_cdc_init(uint8_t busid, uintptr_t reg_base)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return RTOS_ERR_PARAM;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (dev->initialized) {
        return RTOS_ERR_ALREADY;
    }

    /* 初始化上下文 */
    memset(dev, 0, sizeof(*dev));

    /* 创建同步对象 */
    dev->rx_sem = rtos_usb_sem_create(0U, RTOS_CONFIG_USB_CDC_RX_BUF_SIZE);
    if (dev->rx_sem == NULL) {
        return RTOS_ERR_NO_MEM;
    }
    dev->tx_sem = rtos_usb_sem_create(0U, 1U);
    if (dev->tx_sem == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        dev->rx_sem = NULL;
        return RTOS_ERR_NO_MEM;
    }
    dev->tx_mutex = rtos_usb_mutex_create();
    if (dev->tx_mutex == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        return RTOS_ERR_NO_MEM;
    }
#if RTOS_CONFIG_USB_USE_HID
    dev->hid_tx_sem = rtos_usb_sem_create(0U, 1U);
    if (dev->hid_tx_sem == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        rtos_usb_mutex_delete(dev->tx_mutex);
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        dev->tx_mutex = NULL;
        return RTOS_ERR_NO_MEM;
    }
    dev->hid_rx_sem = rtos_usb_sem_create(0U, 1U);
    if (dev->hid_rx_sem == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        rtos_usb_mutex_delete(dev->tx_mutex);
        rtos_usb_sem_delete(dev->hid_tx_sem);
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        dev->tx_mutex = NULL;
        dev->hid_tx_sem = NULL;
        return RTOS_ERR_NO_MEM;
    }
    dev->hid_tx_mutex = rtos_usb_mutex_create();
    if (dev->hid_tx_mutex == NULL) {
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        rtos_usb_mutex_delete(dev->tx_mutex);
        rtos_usb_sem_delete(dev->hid_tx_sem);
        rtos_usb_sem_delete(dev->hid_rx_sem);
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        dev->tx_mutex = NULL;
        dev->hid_tx_sem = NULL;
        dev->hid_rx_sem = NULL;
        return RTOS_ERR_NO_MEM;
    }
#endif

    /* 注册端点回调 */
    dev->cdc_out_ep.ep_addr = CDC_OUT_EP;
    dev->cdc_out_ep.ep_cb = rtos_usb_cdc_bulk_out;
    dev->cdc_in_ep.ep_addr = CDC_IN_EP;
    dev->cdc_in_ep.ep_cb = rtos_usb_cdc_bulk_in;
#if RTOS_CONFIG_USB_USE_HID
    dev->hid_out_ep.ep_addr = HID_OUT_EP;
    dev->hid_out_ep.ep_cb = rtos_usb_hid_out;
    dev->hid_in_ep.ep_addr = HID_IN_EP;
    dev->hid_in_ep.ep_cb = rtos_usb_hid_in_done;
#endif

    /* CherryUSB 初始化序列 */
    usbd_desc_register(busid, &s_cdc_descriptor);
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &dev->intf0));
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &dev->intf1));
    usbd_add_endpoint(busid, &dev->cdc_out_ep);
    usbd_add_endpoint(busid, &dev->cdc_in_ep);
#if RTOS_CONFIG_USB_USE_MSC
    /* MSC 接口(批量 IN/OUT 端点由 usbd_msc 类内部注册与管理) */
    usbd_add_interface(busid, usbd_msc_init_intf(busid, &dev->msc_intf, MSC_OUT_EP, MSC_IN_EP));
    usbd_msc_set_readonly(busid, RTOS_CONFIG_USB_MSC_READONLY ? true : false);
#endif
#if RTOS_CONFIG_USB_USE_HID
    /* HID 接口(报告描述符经 usbd_hid_init_intf 注册, GET_DESCRIPTOR 由类内部处理) */
    usbd_add_interface(busid, usbd_hid_init_intf(busid, &dev->hid_intf, s_hid_report_descriptor,
                                                 sizeof(s_hid_report_descriptor)));
    usbd_add_endpoint(busid, &dev->hid_out_ep);
    usbd_add_endpoint(busid, &dev->hid_in_ep);
#endif

    int ret = usbd_initialize(busid, reg_base, rtos_usb_cdc_event_handler);
    if (ret != 0) {
        /* 反初始化: usbd_initialize 内部在 usb_dc_init 之前已触发
         * USBD_EVENT_INIT(usbd_msc 会创建 MSC 线程+消息队列)。不调
         * usbd_deinitialize 会导致这些对象泄漏(池槽位累积耗尽,
         * 重试若干次后线程池耗尽)。 */
        usbd_deinitialize(busid);
        rtos_usb_sem_delete(dev->rx_sem);
        rtos_usb_sem_delete(dev->tx_sem);
        rtos_usb_mutex_delete(dev->tx_mutex);
#if RTOS_CONFIG_USB_USE_HID
        rtos_usb_sem_delete(dev->hid_tx_sem);
        rtos_usb_sem_delete(dev->hid_rx_sem);
        rtos_usb_mutex_delete(dev->hid_tx_mutex);
        dev->hid_tx_sem = NULL;
        dev->hid_rx_sem = NULL;
        dev->hid_tx_mutex = NULL;
#endif
        /* 清除悬空指针, 保证下次调用 init 时状态干净 */
        dev->rx_sem = NULL;
        dev->tx_sem = NULL;
        dev->tx_mutex = NULL;
        return RTOS_ERR_PARAM;
    }

    dev->initialized = true;
    return RTOS_OK;
}

void rtos_usb_cdc_isr(uint8_t busid)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    USBD_IRQHandler(busid);
}

bool rtos_usb_cdc_is_connected(uint8_t busid)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return false;
    }
    return s_cdc_dev[busid].connected;
}

int rtos_usb_cdc_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    if ((busid >= CONFIG_USBDEV_MAX_BUS) || (data == NULL) || (len == 0U)) {
        return 0;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (!dev->connected) {
        return 0;
    }

    /* 截止时间(deadline)计算: 总超时分摊到所有分块, 避免超时倍增。
     * +1: 保证 timeout_ms=0(TX 空闲时应发送, 头文件契约)也给第一次
     * 迭代一次发送机会, 而非立即超时返回 0。
     * 到期判断用有符号差值比较, 免疫 tick 计数回绕(约 49.7 天):
     * now 在回绕点附近时无符号比较方向错误, 会立即假超时。 */
    bool has_deadline = (timeout_ms != RTOS_WAIT_FOREVER);
    rtos_tick_t deadline = 0U;
    if (has_deadline) {
        deadline = rtos_sched_get_tick_count() + usb_ms_to_ticks(timeout_ms) + 1U;
    }

    uint32_t total_sent = 0U;

    /* 串行化写入(多任务安全)。互斥锁永久等待, 与 FreeRTOS 行为一致。 */
    rtos_usb_mutex_take(dev->tx_mutex);

    while ((total_sent < len) && dev->connected) {
        /* 计算剩余超时 */
        rtos_tick_t remaining = RTOS_WAIT_FOREVER;
        if (has_deadline) {
            rtos_tick_t now = rtos_sched_get_tick_count();
            if ((int32_t)(now - deadline) >= 0) {
                break; /* 总超时(回绕安全) */
            }
            remaining = deadline - now;
        }

        /* 若上次发送超时(DMA 仍在途), 先等它完成再开新发送。
         * 这避免了 DMA 竞态: 不会在 DMA 读 ep_in_buf 时覆盖它。 */
        if (dev->tx_busy) {
            uint32_t seq_prev = dev->tx_done_seq;
            rtos_status_t st = rtos_usb_sem_take(dev->tx_sem, remaining);
            if (st != RTOS_OK) {
                break; /* 等待前次 DMA 也超时 */
            }
            dev->tx_busy = false;
            /* 完成序号未变: 唤醒来自事件(RESET/DISCONNECT)而非完成回调,
             * 前次 DMA 已被总线复位冲掉, 未到达主机。 */
            if (dev->tx_done_seq == seq_prev) {
                break;
            }
            /* 重新计算剩余时间 */
            if (has_deadline) {
                rtos_tick_t now = rtos_sched_get_tick_count();
                if ((int32_t)(now - deadline) >= 0) {
                    break;
                }
                remaining = deadline - now;
            }
        }

        /* 分块: 不超过内部 TX 缓冲大小 */
        uint32_t chunk = len - total_sent;
        if (chunk > RTOS_CONFIG_USB_CDC_TX_BUF_SIZE) {
            chunk = RTOS_CONFIG_USB_CDC_TX_BUF_SIZE;
        }

        /* 拷贝到 DMA-safe 内部缓冲(解决 cache 一致性 + 超时后缓冲复用问题) */
        memcpy(dev->ep_in_buf, &data[total_sent], chunk);

        /* 清除陈旧信号, 标记发送中, 启动 DMA */
        rtos_usb_sem_reset(dev->tx_sem);
        dev->tx_busy = true;
        uint32_t seq = dev->tx_done_seq;

        int ret = usbd_ep_start_write(busid, CDC_IN_EP, dev->ep_in_buf, chunk);
        if (ret != 0) {
            /* 端点忙或错误: 放弃本块(下次调用会重试) */
            dev->tx_busy = false;
            break;
        }

        /* 等待本块发送完成(IN 回调 give 信号量) */
        rtos_status_t st = rtos_usb_sem_take(dev->tx_sem, remaining);
        if (st != RTOS_OK) {
            /* 超时: tx_busy 保持 true(DMA 可能仍在途)。
             * 下次调用会先等它完成。break 返回已发送量。 */
            break;
        }
        dev->tx_busy = false;

        /* 完成序号未变: 唤醒来自事件(RESET/DISCONNECT)而非本块完成。
         * 在途块已被总线复位冲掉 —— 即使快速重新枚举使 connected 恢复
         * true, 该块也从未到达主机, 不得计入已发送量(防静默丢数据)。 */
        if (dev->tx_done_seq == seq) {
            break;
        }

        total_sent += chunk;
    }

    rtos_usb_mutex_give(dev->tx_mutex);
    return (int)total_sent;
}

int rtos_usb_cdc_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    if ((busid >= CONFIG_USBDEV_MAX_BUS) || (buf == NULL) || (len == 0U)) {
        return 0;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];
    rtos_tick_t timeout = usb_ms_to_ticks(timeout_ms);

    for (;;) {
        /* 块拷贝读取: 用 memcpy 两段拷贝替代逐字节循环 */
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t head = dev->rx_head;
        uint32_t tail = dev->rx_tail;
        uint32_t avail = RX_BUF_COUNT(head, tail);
        uint32_t to_read = RTOS_USB_MIN(len, avail);

        if (to_read > 0U) {
            /* 第一段: 从 tail 到缓冲末尾。
             * 注意: head/tail 是"原始计数器"(始终递增, uint32_t 自然回绕),
             *       仅在数组索引时用 RX_BUF_POS() 转换, 切勿回写掩码后的值。 */
            uint32_t first_chunk =
                RTOS_USB_MIN(to_read, RTOS_CONFIG_USB_CDC_RX_BUF_SIZE - RX_BUF_POS(tail));
            memcpy(&buf[0], &dev->rx_buf[RX_BUF_POS(tail)], first_chunk);
            tail += first_chunk;

            /* 第二段: 缓冲起始到剩余 */
            uint32_t second_chunk = to_read - first_chunk;
            if (second_chunk > 0U) {
                memcpy(&buf[first_chunk], &dev->rx_buf[0], second_chunk);
                tail += second_chunk;
            }
            dev->rx_tail = tail;
        }
        RTOS_PORT_EXIT_CRITICAL();

        if (to_read > 0U) {
            return (int)to_read;
        }

        /* 缓冲为空: 若未连接则返回 0 */
        if (!dev->connected) {
            return 0;
        }

        /* 等待数据到达(OUT 回调 give 信号量) */
        rtos_status_t st = rtos_usb_sem_take(dev->rx_sem, timeout);
        if (st != RTOS_OK) {
            return 0; /* 超时 */
        }
        /* 收到信号, 循环回去读取数据 */
    }
}

uint32_t rtos_usb_cdc_available(uint8_t busid)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return 0U;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    RTOS_PORT_ENTER_CRITICAL();
    uint32_t head = dev->rx_head;
    uint32_t tail = dev->rx_tail;
    RTOS_PORT_EXIT_CRITICAL();

    return RX_BUF_COUNT(head, tail);
}

void rtos_usb_cdc_flush_rx(uint8_t busid)
{
    if (busid >= CONFIG_USBDEV_MAX_BUS) {
        return;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    RTOS_PORT_ENTER_CRITICAL();
    dev->rx_tail = dev->rx_head;
    RTOS_PORT_EXIT_CRITICAL();
}

/*============================================================================*/
/*====================  HID 用户封装(自定义厂商报告)  ========================*/
/*============================================================================*/

#if RTOS_CONFIG_USB_USE_HID

int rtos_usb_hid_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    if ((busid >= CONFIG_USBDEV_MAX_BUS) || (data == NULL) || (len == 0U)) {
        return 0;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];

    if (!dev->connected) {
        return 0;
    }
    if (len > HID_MPS) {
        len = HID_MPS; /* 单报告最大 64B */
    }
    rtos_tick_t timeout = usb_ms_to_ticks(timeout_ms);

    /* 串行化写入(多任务并发保护, 与 CDC write 同模式)。无锁时并发调用会:
     * 双写同一 DMA 缓冲(前者数据被覆盖)、重复编程端点破坏在途传输、
     * 完成信号串号(FOREVER 等待者永久挂起)。 */
    rtos_usb_mutex_take(dev->hid_tx_mutex);

    /* 若上次发送仍在途, 先等其完成(避免 DMA 缓冲被覆盖) */
    if (dev->hid_tx_busy) {
        rtos_status_t st = rtos_usb_sem_take(dev->hid_tx_sem, timeout);
        if (st != RTOS_OK) {
            rtos_usb_mutex_give(dev->hid_tx_mutex);
            return 0;
        }
        dev->hid_tx_busy = false;
        if (!dev->connected) {
            rtos_usb_mutex_give(dev->hid_tx_mutex);
            return 0;
        }
    }

    /* 拷贝到 DMA-safe 缓冲, 清除陈旧信号后启动发送 */
    memcpy(dev->hid_ep_in_buf, data, len);
    rtos_usb_sem_reset(dev->hid_tx_sem);
    dev->hid_tx_busy = true;

    if (usbd_ep_start_write(busid, HID_IN_EP, dev->hid_ep_in_buf, len) != 0) {
        dev->hid_tx_busy = false;
        rtos_usb_mutex_give(dev->hid_tx_mutex);
        return 0;
    }

    /* 等待发送完成(IN 回调 give 信号量) */
    rtos_status_t st = rtos_usb_sem_take(dev->hid_tx_sem, timeout);
    if (st != RTOS_OK) {
        /* 超时: hid_tx_busy 保持 true, 下次调用先等完成 */
        rtos_usb_mutex_give(dev->hid_tx_mutex);
        return 0;
    }
    dev->hid_tx_busy = false;
    rtos_usb_mutex_give(dev->hid_tx_mutex);
    return (int)len;
}

int rtos_usb_hid_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    if ((busid >= CONFIG_USBDEV_MAX_BUS) || (buf == NULL) || (len == 0U)) {
        return 0;
    }
    rtos_usb_cdc_dev_t *dev = &s_cdc_dev[busid];
    rtos_tick_t timeout = usb_ms_to_ticks(timeout_ms);

    for (;;) {
        /* 单槽缓冲: 有待读报告则拷贝返回 */
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t n = dev->hid_rx_len;
        if (n > 0U) {
            if (n > len) {
                n = len;
            }
            memcpy(buf, dev->hid_rx_buf, n);
            dev->hid_rx_len = 0U;
        }
        RTOS_PORT_EXIT_CRITICAL();

        if (n > 0U) {
            return (int)n;
        }

        if (!dev->connected) {
            return 0;
        }

        rtos_status_t st = rtos_usb_sem_take(dev->hid_rx_sem, timeout);
        if (st != RTOS_OK) {
            return 0; /* 超时 */
        }
    }
}

#else /* !RTOS_CONFIG_USB_USE_HID */

int rtos_usb_hid_write(uint8_t busid, const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    (void)busid;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return 0;
}

int rtos_usb_hid_read(uint8_t busid, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    (void)busid;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return 0;
}

#endif /* RTOS_CONFIG_USB_USE_HID */

/*============================================================================*/
/*====================  MSC 大容量存储封装(RAM 盘)  ==========================*/
/*============================================================================*/
/* CherryUSB 的 usbd_msc.c 通过以下三个弱约定回调访问存储介质:
 *   usbd_msc_get_cap       → 上报容量(扇区数/扇区大小)
 *   usbd_msc_sector_read   → 读扇区(运行于 usbd_msc 线程)
 *   usbd_msc_sector_write  → 写扇区(运行于 usbd_msc 线程)
 * 此处用静态 RAM 数组模拟 U 盘。用户接入 SPI Flash/SD 卡时, 只需将这三个
 * 函数内部的 memcpy 替换为实际介质的读写驱动即可。 */

#if RTOS_CONFIG_USB_USE_MSC

#define MSC_BLOCK_SIZE (512U)
#define MSC_BLOCK_NUM (RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE / MSC_BLOCK_SIZE)

/* 编译期检查: RAM 盘大小必须是 512 的整数倍且非空 */
#if (MSC_BLOCK_NUM * MSC_BLOCK_SIZE) != RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE
#error "RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE must be a multiple of 512"
#endif

static uint8_t s_msc_ram_disk[RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE]
    __attribute__((aligned(4)));

/* 扇区读写统计(观测用, MSC 任务周期性打印) */
static volatile uint32_t s_msc_read_sectors;
static volatile uint32_t s_msc_write_sectors;

void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t *block_num, uint32_t *block_size)
{
    (void)busid;
    (void)lun;
    *block_num = MSC_BLOCK_NUM;
    *block_size = MSC_BLOCK_SIZE;
}

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer,
                         uint32_t length)
{
    (void)busid;
    (void)lun;
    if ((sector >= MSC_BLOCK_NUM) || (buffer == NULL) ||
        ((length % MSC_BLOCK_SIZE) != 0U) ||
        ((sector + length / MSC_BLOCK_SIZE) > MSC_BLOCK_NUM)) {
        return -1;
    }
    memcpy(buffer, &s_msc_ram_disk[sector * MSC_BLOCK_SIZE], length);
    s_msc_read_sectors += length / MSC_BLOCK_SIZE;
    return 0;
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t *buffer,
                          uint32_t length)
{
#if RTOS_CONFIG_USB_MSC_READONLY
    (void)busid;
    (void)lun;
    (void)sector;
    (void)buffer;
    (void)length;
    return -1;
#else
    (void)busid;
    (void)lun;
    if ((sector >= MSC_BLOCK_NUM) || (buffer == NULL) ||
        ((length % MSC_BLOCK_SIZE) != 0U) ||
        ((sector + length / MSC_BLOCK_SIZE) > MSC_BLOCK_NUM)) {
        return -1;
    }
    memcpy(&s_msc_ram_disk[sector * MSC_BLOCK_SIZE], buffer, length);
    s_msc_write_sectors += length / MSC_BLOCK_SIZE;
    return 0;
#endif
}

void rtos_usb_msc_get_stats(uint8_t busid, uint32_t *reads, uint32_t *writes)
{
    (void)busid;
    if (reads != NULL) {
        *reads = s_msc_read_sectors;
    }
    if (writes != NULL) {
        *writes = s_msc_write_sectors;
    }
}

#else /* !RTOS_CONFIG_USB_USE_MSC */

void rtos_usb_msc_get_stats(uint8_t busid, uint32_t *reads, uint32_t *writes)
{
    (void)busid;
    if (reads != NULL) {
        *reads = 0U;
    }
    if (writes != NULL) {
        *writes = 0U;
    }
}

#endif /* RTOS_CONFIG_USB_USE_MSC */

#endif /* RTOS_CONFIG_USE_USB */

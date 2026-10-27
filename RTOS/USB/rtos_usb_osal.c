/**
 * @file    rtos_usb_osal.c
 * @brief   USB 对象资源管理与 CherryUSB OSAL 适配
 *
 * @details 本文件集中管理 USB 对象池、固定块堆和 ISR 注册表, 并实现
 *          rtos_usb_* 句柄封装与 CherryUSB 的 usb_osal_* 兼容接口。
 *          设备协议与 MSC 存储后端分别位于 rtos_usb_device.c / rtos_usb_msc.c。
 *          集成 CherryUSB 时:
 *            - 不编译 CherryUSB 自带的 osal/usb_osal_*.c
 *            - usb_config.h 已包含 rtos_usb.h, 使 OSAL 类型衔接
 *            - 将本文件加入工程编译
 *
 *          适配要点(CherryUSB OSAL 与本 RTOS 的差异):
 *
 *            1. 类型衔接:
 *               - CherryUSB: usb_osal_sem_t / usb_osal_mutex_t 等定义为 void *
 *               - 本 RTOS:   内部用 rtos_usb_sem_t / rtos_usb_mutex_t 等结构体
 *               - 适配: usb_osal_* 函数在 void* 与内部结构体指针间做转换。
 *
 *            2. 超时单位:
 *               - CherryUSB: timeout 参数为毫秒(ms), USB_OSAL_WAITING_FOREVER=永久等待
 *               - 本 RTOS:   timeout 参数为 Tick, RTOS_WAIT_FOREVER=永久等待
 *               - 适配: 内部调用 rtos_usb_timeout_ticks() 转换。
 *
 *            3. 栈大小单位:
 *               - CherryUSB: stack_size 为字节数
 *               - 本 RTOS:   stack_size 为字数(sizeof(rtos_stack_t) = 4)
 *               - 适配: stack_words = stack_bytes / sizeof(rtos_stack_t)
 *
 *            4. 返回值:
 *               - CherryUSB: int, 0=成功, -USB_ERR_TIMEOUT=超时
 *               - 本 RTOS:   rtos_status_t, RTOS_OK=0, 其余负值
 *               - 适配: 手动映射 RTOS_OK→0, RTOS_ERR_TIMEOUT→-USB_ERR_TIMEOUT, 其他→-1。
 *
 *            5. 信号量:
 *               - usb_osal_sem_create(initial): 二值信号量(max=1, init=initial)
 *               - usb_osal_sem_create_counting(max): 计数信号量(max=max, init=0)
 *               - usb_osal_sem_give: ISR 与任务均安全
 *
 *            6. 互斥锁:
 *               - usb_osal_mutex_take: 无 timeout 参数, 永久等待
 *               - 不可在 ISR 调用(本 RTOS 互斥锁限制)
 *
 *            7. 定时器(特殊处理):
 *               - CherryUSB 的 usb_osal_timer_create 返回 struct usb_osal_timer *
 *                 (堆分配), 其中 ->timer 字段存储内部 RTOS 定时器句柄。
 *               - 本实现: 从 USB 堆分配 struct usb_osal_timer, 从对象池分配
 *                 rtos_usb_timer_t, 将池指针存入 ->timer。
 *               - start/stop/delete 通过 ->timer 访问内部句柄。
 *
 *            8. 临界区:
 *               - CherryUSB: enter_critical_section 返回 size_t flag,
 *                            leave_critical_section(flag) 恢复
 *               - 本 RTOS:   RTOS_PORT_ENTER/EXIT_CRITICAL 内部嵌套计数自动管理,
 *                            支持 ISR 与任务上下文
 *               - 适配: enter 返回 0(flag 不使用), exit 忽略 flag 直接调用 EXIT。
 *
 *            9. 消息队列:
 *               - send: ISR 中非阻塞, 任务中永久等待(与 FreeRTOS OSAL 一致)
 *               - recv: 支持 timeout, ISR 中必须非阻塞
 */
#include "rtos_usb.h"

#if RTOS_CONFIG_USE_USB

#include <stddef.h>
#include <string.h>

#include "usb_config.h" /* 日志配置必须在 usb_log.h 之前可见。 */
#include "usb_errno.h"
#include "usb_log.h"
#include "usb_osal.h"

#include "rtos_mem.h"
#include "rtos_port.h"
#include "rtos_sched.h"

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

/* ============================== 内部辅助: 池分配/归还 ============================== */

static rtos_usb_sem_t *alloc_sem_slot(void)
{
    RTOS_ASSERT_ISR_OK();
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
    RTOS_ASSERT_ISR_OK();
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
    RTOS_ASSERT_ISR_OK();
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
    RTOS_ASSERT_ISR_OK();
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = 0U; i < RTOS_CONFIG_USB_OSAL_THREAD_COUNT; i++) {
        /* A returning/self-deleting static task cannot free its own stack.
         * Reclaim it only after PendSV has stopped using that TCB. State 2
         * reserves a slot while task_create initializes its previous TCB. */
        if ((s_thread_pool[i].in_use == 1U) &&
            (s_thread_pool[i].tcb.state == RTOS_TASK_DELETED) &&
            (&s_thread_pool[i].tcb != rtos_task_get_current())) {
            s_thread_pool[i].in_use = 0U;
        }
        if (s_thread_pool[i].in_use == 0U) {
            s_thread_pool[i].in_use = 2U;
            RTOS_PORT_EXIT_CRITICAL();
            return &s_thread_pool[i];
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return NULL;
}

static rtos_usb_timer_t *alloc_timer_slot(uint32_t start_index)
{
    RTOS_ASSERT_ISR_OK();
    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = start_index; i < RTOS_CONFIG_USB_OSAL_TIMER_COUNT; i++) {
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

    RTOS_ASSERT_ISR_OK();
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
    RTOS_ASSERT_ISR_OK();
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
    RTOS_ASSERT_ISR_OK();
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
    slot->in_use = 1U;
    return slot;
}

void rtos_usb_thread_delete(rtos_usb_thread_t *thread)
{
    if (RTOS_PORT_IN_ISR()) return;
    if (thread == NULL) {
        rtos_task_delete(NULL);
        return;
    }
    if (&thread->tcb == rtos_task_get_current()) {
        /* The slot is reaped by alloc_thread_slot after switching away. */
        (void)rtos_task_delete(&thread->tcb);
        return;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (thread->in_use != 1U) {
        RTOS_PORT_EXIT_CRITICAL();
        return;
    }
    thread->in_use = 2U; /* Prevent a higher task from reaping before return. */
    RTOS_PORT_EXIT_CRITICAL();
    rtos_status_t status = rtos_task_delete(&thread->tcb);
    RTOS_PORT_ENTER_CRITICAL();
    thread->in_use = status == RTOS_OK ? 0U : 1U;
    RTOS_PORT_EXIT_CRITICAL();
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
    rtos_timer_mode_t mode = is_periodic ? RTOS_TIMER_PERIODIC : RTOS_TIMER_ONE_SHOT;
    uint32_t start_index = 0U;
    while (start_index < RTOS_CONFIG_USB_OSAL_TIMER_COUNT) {
        rtos_usb_timer_t *slot = alloc_timer_slot(start_index);
        if (slot == NULL) {
            return NULL;
        }
        rtos_status_t status =
            rtos_timer_create(&slot->timer, name, cb, arg, rtos_usb_timeout_ticks(ms), mode);
        if (status == RTOS_OK) {
            return slot;
        }
        slot->in_use = 0U;
        if (status != RTOS_ERR_ALREADY) {
            return NULL;
        }
        /* DELETE is asynchronous: this free slot can still belong to an
         * active timer until TIMER processes its accepted delete command.
         * Preserve the kernel guard and try another unused slot. */
        start_index = (uint32_t)(slot - s_timer_pool) + 1U;
    }
    return NULL;
}

rtos_status_t rtos_usb_timer_delete_checked(rtos_usb_timer_t *timer)
{
    if (timer == NULL) {
        return RTOS_ERR_NULL;
    }
    rtos_status_t status = rtos_timer_delete(&timer->timer);
    if (status == RTOS_OK) timer->in_use = 0U;
    return status;
}

void rtos_usb_timer_delete(rtos_usb_timer_t *timer)
{
    (void)rtos_usb_timer_delete_checked(timer);
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
    rtos_task_delay(rtos_usb_timeout_ticks(ms));
}

/* ============================== CherryUSB 接口适配 ============================== */

/* ============================== 内部辅助 ============================== */

/** @brief rtos_status_t → CherryUSB int 返回值。 */
static int rtos_status_to_osal(rtos_status_t st)
{
    if (st == RTOS_OK) {
        return 0;
    }
    if (st == RTOS_ERR_TIMEOUT) {
        return -USB_ERR_TIMEOUT;
    }
    return -1;
}

/* ============================== 线程 ============================== */

usb_osal_thread_t usb_osal_thread_create(const char *name, uint32_t stack_size, uint32_t prio,
                                         usb_thread_entry_t entry, void *args)
{
    /* CherryUSB stack_size 单位为字节, 本 RTOS 单位为字(4 字节) */
    uint32_t stack_words = stack_size / sizeof(rtos_stack_t);
    if ((stack_size % sizeof(rtos_stack_t)) != 0U) {
        stack_words++;
    }
    if (stack_words == 0U) {
        stack_words = RTOS_CONFIG_USB_THREAD_STACK_SIZE;
    }
    /* usb_thread_entry_t 与 rtos_task_func_t 签名一致(都是 void(*)(void*)) */
    if (prio >= RTOS_CONFIG_MAX_PRIORITIES) {
        prio = RTOS_CONFIG_MAX_PRIORITIES - 1U;
    }
    rtos_usb_thread_t *t =
        rtos_usb_thread_create(name, stack_words, (rtos_prio_t)prio, (rtos_task_func_t)entry, args);
    if (t == NULL) {
        USB_LOG_ERR("usb_osal_thread_create %s failed\r\n", name ? name : "?");
        return NULL;
    }
    return (usb_osal_thread_t)t;
}

void usb_osal_thread_delete(usb_osal_thread_t thread)
{
    rtos_usb_thread_delete((rtos_usb_thread_t *)thread);
}

void usb_osal_thread_schedule_other(void)
{
    rtos_usb_thread_schedule_other();
}

/* ============================== 信号量 ============================== */

usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count)
{
    /* CherryUSB: 二值信号量(max=1), 与 FreeRTOS xSemaphoreCreateCounting(1, init) 一致 */
    rtos_usb_sem_t *sem = rtos_usb_sem_create(initial_count, 1U);
    if (sem == NULL) {
        USB_LOG_ERR("usb_osal_sem_create failed\r\n");
    }
    return (usb_osal_sem_t)sem;
}

usb_osal_sem_t usb_osal_sem_create_counting(uint32_t max_count)
{
    /* CherryUSB: 计数信号量(max=max_count, init=0) */
    rtos_usb_sem_t *sem = rtos_usb_sem_create(0U, max_count);
    if (sem == NULL) {
        USB_LOG_ERR("usb_osal_sem_create_counting failed\r\n");
    }
    return (usb_osal_sem_t)sem;
}

void usb_osal_sem_delete(usb_osal_sem_t sem)
{
    rtos_usb_sem_delete((rtos_usb_sem_t *)sem);
}

int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout)
{
    if (sem == NULL) {
        return -1;
    }
    rtos_status_t st = rtos_usb_sem_take((rtos_usb_sem_t *)sem, rtos_usb_timeout_ticks(timeout));
    return rtos_status_to_osal(st);
}

int usb_osal_sem_give(usb_osal_sem_t sem)
{
    if (sem == NULL) {
        return -1;
    }
    rtos_status_t st = rtos_usb_sem_give((rtos_usb_sem_t *)sem);
    return rtos_status_to_osal(st);
}

void usb_osal_sem_reset(usb_osal_sem_t sem)
{
    rtos_usb_sem_reset((rtos_usb_sem_t *)sem);
}

/* ============================== 互斥锁 ============================== */

usb_osal_mutex_t usb_osal_mutex_create(void)
{
    rtos_usb_mutex_t *mutex = rtos_usb_mutex_create();
    if (mutex == NULL) {
        USB_LOG_ERR("usb_osal_mutex_create failed\r\n");
    }
    return (usb_osal_mutex_t)mutex;
}

void usb_osal_mutex_delete(usb_osal_mutex_t mutex)
{
    rtos_usb_mutex_delete((rtos_usb_mutex_t *)mutex);
}

int usb_osal_mutex_take(usb_osal_mutex_t mutex)
{
    if (mutex == NULL) {
        return -1;
    }
    /* CherryUSB: 无 timeout 参数, 永久等待 */
    rtos_status_t st = rtos_usb_mutex_take((rtos_usb_mutex_t *)mutex);
    return rtos_status_to_osal(st);
}

int usb_osal_mutex_give(usb_osal_mutex_t mutex)
{
    if (mutex == NULL) {
        return -1;
    }
    rtos_status_t st = rtos_usb_mutex_give((rtos_usb_mutex_t *)mutex);
    return rtos_status_to_osal(st);
}

/* ============================== 消息队列 ============================== */

usb_osal_mq_t usb_osal_mq_create(uint32_t max_msgs)
{
    rtos_usb_mq_t *mq = rtos_usb_mq_create(max_msgs);
    if (mq == NULL) {
        USB_LOG_ERR("usb_osal_mq_create failed\r\n");
    }
    return (usb_osal_mq_t)mq;
}

void usb_osal_mq_delete(usb_osal_mq_t mq)
{
    rtos_usb_mq_delete((rtos_usb_mq_t *)mq);
}

int usb_osal_mq_send(usb_osal_mq_t mq, uintptr_t addr)
{
    if (mq == NULL) {
        return -1;
    }
    /* ISR 中非阻塞, 任务中永久等待(与 FreeRTOS OSAL 一致) */
    rtos_tick_t timeout = RTOS_PORT_IN_ISR() ? RTOS_NO_WAIT : RTOS_WAIT_FOREVER;
    rtos_status_t st = rtos_usb_mq_send((rtos_usb_mq_t *)mq, addr, timeout);
    return rtos_status_to_osal(st);
}

int usb_osal_mq_recv(usb_osal_mq_t mq, uintptr_t *addr, uint32_t timeout)
{
    if ((mq == NULL) || (addr == NULL)) {
        return -1;
    }
    rtos_status_t st = rtos_usb_mq_recv((rtos_usb_mq_t *)mq, addr, rtos_usb_timeout_ticks(timeout));
    return rtos_status_to_osal(st);
}

/* ============================== 定时器 ============================== */

struct usb_osal_timer *usb_osal_timer_create(const char *name, uint32_t timeout_ms,
                                             usb_timer_handler_t handler, void *argument,
                                             bool is_period)
{
    /* 从 USB 堆分配 CherryUSB 的 struct usb_osal_timer(sizeof 约 20 字节, 走小块池) */
    struct usb_osal_timer *timer =
        (struct usb_osal_timer *)rtos_usb_malloc(sizeof(struct usb_osal_timer));
    if (timer == NULL) {
        USB_LOG_ERR("usb_osal_timer_create alloc failed\r\n");
        return NULL;
    }
    memset(timer, 0, sizeof(*timer));
    timer->handler = handler;
    timer->argument = argument;
    timer->is_period = is_period;
    timer->timeout_ms = timeout_ms;

    /* 从对象池分配内部 RTOS 定时器, 句柄存入 ->timer 字段 */
    timer->timer = (void *)rtos_usb_timer_create(name, timeout_ms, (rtos_timer_cb_t)handler,
                                                 argument, is_period);
    if (timer->timer == NULL) {
        USB_LOG_ERR("usb_osal_timer_create pool failed\r\n");
        rtos_usb_free(timer);
        return NULL;
    }
    return timer;
}

void usb_osal_timer_delete(struct usb_osal_timer *timer)
{
    if (timer == NULL) {
        return;
    }
    if (rtos_usb_timer_delete_checked((rtos_usb_timer_t *)timer->timer) == RTOS_OK) {
        rtos_usb_free(timer);
    }
}

void usb_osal_timer_start(struct usb_osal_timer *timer)
{
    if (timer == NULL) {
        return;
    }
    rtos_usb_timer_start((rtos_usb_timer_t *)timer->timer);
}

void usb_osal_timer_stop(struct usb_osal_timer *timer)
{
    if (timer == NULL) {
        return;
    }
    rtos_usb_timer_stop((rtos_usb_timer_t *)timer->timer);
}

/* ============================== 内存 ============================== */

void *usb_osal_malloc(size_t size)
{
    return rtos_usb_malloc(size);
}

void usb_osal_free(void *ptr)
{
    rtos_usb_free(ptr);
}

/* ============================== 时间 ============================== */

void usb_osal_msleep(uint32_t delay)
{
    rtos_usb_msleep(delay);
}

/* ============================== 临界区 ============================== */

size_t usb_osal_enter_critical_section(void)
{
    /* 本 RTOS 的 RTOS_PORT_ENTER_CRITICAL 内部用嵌套计数管理 BASEPRI,
     * 支持 ISR 与任务上下文, 因此 flag 值不使用(返回 0 即可)。 */
    RTOS_PORT_ENTER_CRITICAL();
    return 0;
}

void usb_osal_leave_critical_section(size_t flag)
{
    (void)flag;
    RTOS_PORT_EXIT_CRITICAL();
}

#endif /* RTOS_CONFIG_USE_USB */

/**
 * @file    rtos_usb_osal.c
 * @brief   CherryUSB OSAL 适配层(usb_osal_* 命名兼容)
 *
 * @details 本文件实现 CherryUSB 期望的 usb_osal_* 接口, 内部调用 rtos_usb.c
 *          提供的句柄式封装。集成 CherryUSB 时:
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
 *               - 适配: 内部调用 usb_osal_ms_to_ticks() 转换。
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

#include "rtos_port.h"
#include "usb_config.h" /* CONFIG_USB_PRINTF / CONFIG_USB_DBG_LEVEL(须在 usb_log.h 之前) */
#include "usb_osal.h" /* CherryUSB OSAL 类型与声明 */
#include "usb_errno.h" /* USB_ERR_TIMEOUT 等 */
#include "usb_log.h" /* USB_LOG_ERR */
#include <stddef.h>
#include <string.h>

/* ============================== 内部辅助 ============================== */

/** @brief 毫秒转 Tick。USB_OSAL_WAITING_FOREVER → RTOS_WAIT_FOREVER。 */
static rtos_tick_t usb_osal_ms_to_ticks(uint32_t ms)
{
    if (ms == USB_OSAL_WAITING_FOREVER) {
        return RTOS_WAIT_FOREVER;
    }
    if (ms == 0U) {
        return RTOS_NO_WAIT;
    }
    rtos_tick_t ticks = (ms * RTOS_CONFIG_TICK_RATE_HZ) / 1000U;
    if (ticks == 0U) {
        ticks = 1U;
    }
    return ticks;
}

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
    if (stack_words == 0U) {
        stack_words = RTOS_CONFIG_USB_THREAD_STACK_SIZE;
    }
    /* usb_thread_entry_t 与 rtos_task_func_t 签名一致(都是 void(*)(void*)) */
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
    rtos_status_t st = rtos_usb_sem_take((rtos_usb_sem_t *)sem, usb_osal_ms_to_ticks(timeout));
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
    rtos_status_t st = rtos_usb_mq_recv((rtos_usb_mq_t *)mq, addr, usb_osal_ms_to_ticks(timeout));
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
    rtos_usb_timer_delete((rtos_usb_timer_t *)timer->timer);
    rtos_usb_free(timer);
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

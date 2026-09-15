/**
 * @file    rtos_timer.c
 * @brief   软件定时器实现
 *
 * @details 软件定时器由专用服务任务管理。设计要点:
 *            - 命令队列: 任务/ISR 通过队列向服务任务发送 start/stop 等命令，
 *              避免在 ISR 中直接操作定时器链表(临界区问题)。
 *            - 活跃链表: 按 expire_tick 升序的单向链表，服务任务每次处理时
 *              检查链首是否到期。
 *            - 服务任务阻塞: 以"距下一最近到期时刻"为超时阻塞在命令队列上。
 *              命令到来或超时到期都会唤醒它。
 *            - 回调执行: 在服务任务上下文(非中断)，因此回调可调用阻塞 API
 *              (但阻塞会延迟后续定时器处理，应避免长耗时操作)。
 *
 *          定时器模式:
 *            - 单次(ONE_SHOT): 触发一次后从活跃链表移除。
 *            - 周期(PERIODIC): 触发后重新设置 expire_tick 加入链表。
 *
 *          定时器精度受 Tick 频率限制(默认 1ms)。提高 Tick 频率可获更高精度
 *          但会增加 CPU 开销。
 */
#include "rtos_timer.h"
#include "rtos_sched.h"
#include "rtos_task.h"
#include "rtos_queue.h" /* rtos_queue_recv (命令队列) */
#include <string.h>

/* ============================== 内部数据 ============================== */

/** @brief 定时器命令类型。 */
typedef enum {
    TIMER_CMD_START = 1,
    TIMER_CMD_STOP = 2,
    TIMER_CMD_CHANGE_PERIOD = 3,
    TIMER_CMD_DELETE = 4,
} timer_cmd_type_t;

/** @brief 定时器命令结构(通过队列发送)。 */
typedef struct {
    timer_cmd_type_t type;
    rtos_timer_t *timer;
    rtos_tick_t new_period; /* 仅 CHANGE_PERIOD 使用 */
} timer_cmd_t;

/** @brief 命令队列存储区。 */
#define TIMER_CMD_QUEUE_LEN 16U
static timer_cmd_t s_cmd_storage[TIMER_CMD_QUEUE_LEN];

/** @brief 命令队列对象。 */
static rtos_queue_t s_cmd_queue;

/** @brief 服务任务 TCB 与栈。 */
static rtos_tcb_t s_timer_tcb;
static rtos_stack_t s_timer_stack[RTOS_CONFIG_TIMER_TASK_STACK_SIZE];

/** @brief 活跃定时器链表头(按 expire_tick 升序)。 */
static rtos_timer_t *s_active_head = NULL;

/* ============================== 内部函数 ============================== */

/**
 * @brief 将定时器按 expire_tick 升序插入活跃链表。
 */
static void timer_active_insert(rtos_timer_t *timer)
{
    rtos_timer_t *cur = s_active_head;
    rtos_timer_t *prev = NULL;

    while ((cur != NULL) && (cur->expire_tick <= timer->expire_tick)) {
        prev = cur;
        cur = cur->next;
    }

    timer->next = cur;
    timer->prev = prev;
    if (prev != NULL) {
        prev->next = timer;
    } else {
        s_active_head = timer;
    }
    if (cur != NULL) {
        cur->prev = timer;
    }
    timer->is_active = 1U;
}

/**
 * @brief 从活跃链表移除定时器。
 */
static void timer_active_remove(rtos_timer_t *timer)
{
    if (timer->prev != NULL) {
        timer->prev->next = timer->next;
    } else {
        s_active_head = timer->next;
    }
    if (timer->next != NULL) {
        timer->next->prev = timer->prev;
    }
    timer->next = NULL;
    timer->prev = NULL;
    timer->is_active = 0U;
}

/**
 * @brief 收集到期定时器(临界区内调用): 从活跃链表移除所有到期定时器,
 *        复用 next 指针串成临时单向链表返回(prev 已被 remove 置 NULL)。
 * @return 到期定时器链表头, NULL 表示无到期定时器。
 */
static rtos_timer_t *timer_collect_expired(void)
{
    rtos_tick_t now = rtos_kernel.tick_count;
    rtos_timer_t *expired_head = NULL;

    while ((s_active_head != NULL) && (s_active_head->expire_tick <= now)) {
        rtos_timer_t *t = s_active_head;
        timer_active_remove(t);
        t->next = expired_head; /* 复用 next 串成临时链 */
        expired_head = t;
    }
    return expired_head;
}

/**
 * @brief 执行到期定时器回调(临界区外调用)。
 * @details 在临界区外执行用户回调, 回调可安全调用阻塞 API。
 *          回调中调用 rtos_timer_start/stop 等仅向命令队列投递命令,
 *          不会立即修改活跃链表, 与本函数无竞争。
 */
static void timer_run_callbacks(rtos_timer_t *expired_head)
{
    rtos_timer_t *t = expired_head;
    while (t != NULL) {
        if (t->callback != NULL) {
            t->callback(t->arg);
        }
        t = t->next;
    }
}

/**
 * @brief 将周期定时器重新插入活跃链表(临界区内调用)。
 * @details 单次定时器不重插(已在 collect 中置 is_active=0)。
 *          使用当前 tick_count 计算新到期时刻, 比原始到期时刻更准确。
 *
 *          指针清理规则(关键):
 *          - 周期定时器: timer_active_insert 已将其链入活跃链表并设置
 *            next/prev, 之后绝不可再清空 —— 否则链表在正向遍历时被截断,
 *            排在其后的定时器成为孤儿(回调永不触发, 且后续 remove 会经
 *            陈旧指针写内存)。此为旧版 bug: 两个不同周期的定时器共存时,
 *            周期短者每次重插都会把周期长者"挤丢"。
 *          - 单次定时器: 不重插, 清空指针仅作清理(无害)。
 */
static void timer_reinsert_periodic(rtos_timer_t *expired_head)
{
    rtos_tick_t now = rtos_kernel.tick_count;
    rtos_timer_t *t = expired_head;
    while (t != NULL) {
        rtos_timer_t *next = t->next;
        if (t->mode == RTOS_TIMER_PERIODIC) {
            t->expire_tick = now + t->period;
            timer_active_insert(t); /* insert 已设置 next/prev, 不可清 */
        } else {
            t->next = NULL; /* 仅单次定时器清理指针(安全) */
            t->prev = NULL;
        }
        t = next;
    }
}

/**
 * @brief 计算距下一到期的 Tick 数(临界区内调用)。
 * @return Tick 数, RTOS_WAIT_FOREVER 表示无活跃定时器。
 */
static rtos_tick_t timer_get_next_remain(void)
{
    if (s_active_head == NULL) {
        return RTOS_WAIT_FOREVER;
    }
    rtos_tick_t now = rtos_kernel.tick_count;
    rtos_tick_t remain = s_active_head->expire_tick - now;
    return (remain == 0U) ? 1U : remain;
}

/**
 * @brief 发送命令到服务任务队列。
 */
static rtos_status_t timer_send_cmd(timer_cmd_type_t type, rtos_timer_t *timer,
                                    rtos_tick_t new_period)
{
    timer_cmd_t cmd;
    cmd.type = type;
    cmd.timer = timer;
    cmd.new_period = new_period;

    /* 中断中不阻塞 */
    rtos_tick_t timeout = RTOS_PORT_IN_ISR() ? RTOS_NO_WAIT : 100U;
    return rtos_queue_send(&s_cmd_queue, &cmd, timeout, RTOS_FALSE);
}

/* ============================== 公共 API ============================== */

rtos_status_t rtos_timer_init(void)
{
    rtos_status_t st =
        rtos_queue_init(&s_cmd_queue, s_cmd_storage, sizeof(timer_cmd_t), TIMER_CMD_QUEUE_LEN);
    if (st != RTOS_OK) {
        return st;
    }
    s_active_head = NULL;

    /* 创建定时器服务任务(静态分配: 内核服务任务不依赖堆, 保证可用性) */
    st = rtos_task_create_static(&s_timer_tcb, s_timer_stack, RTOS_CONFIG_TIMER_TASK_STACK_SIZE,
                                 rtos_timer_service_task, NULL, RTOS_CONFIG_TIMER_TASK_PRIORITY,
                                 "TIMER");
    return st;
}

rtos_status_t rtos_timer_create(rtos_timer_t *timer, const char *name, rtos_timer_cb_t callback,
                                void *arg, rtos_tick_t period, rtos_timer_mode_t mode)
{
    if ((timer == NULL) || (callback == NULL) || (period == 0U)) {
        return RTOS_ERR_PARAM;
    }
    memset(timer, 0, sizeof(*timer));
    timer->mode = mode;
    timer->period = period;
    timer->callback = callback;
    timer->arg = arg;
    timer->name = name;
    timer->is_active = 0U;
    timer->is_initialized = 1U;
    return RTOS_OK;
}

rtos_status_t rtos_timer_start(rtos_timer_t *timer)
{
    if ((timer == NULL) || (timer->is_initialized == 0U)) {
        return RTOS_ERR_PARAM;
    }
    return timer_send_cmd(TIMER_CMD_START, timer, 0U);
}

rtos_status_t rtos_timer_stop(rtos_timer_t *timer)
{
    if ((timer == NULL) || (timer->is_initialized == 0U)) {
        return RTOS_ERR_PARAM;
    }
    return timer_send_cmd(TIMER_CMD_STOP, timer, 0U);
}

rtos_status_t rtos_timer_change_period(rtos_timer_t *timer, rtos_tick_t period)
{
    if ((timer == NULL) || (timer->is_initialized == 0U) || (period == 0U)) {
        return RTOS_ERR_PARAM;
    }
    return timer_send_cmd(TIMER_CMD_CHANGE_PERIOD, timer, period);
}

rtos_status_t rtos_timer_delete(rtos_timer_t *timer)
{
    if ((timer == NULL) || (timer->is_initialized == 0U)) {
        return RTOS_ERR_PARAM;
    }
    return timer_send_cmd(TIMER_CMD_DELETE, timer, 0U);
}

/* ============================== 服务任务 ============================== */

/**
 * @brief 处理一条命令。
 */
static void timer_handle_cmd(timer_cmd_t *cmd)
{
    rtos_timer_t *t = cmd->timer;
    if ((t == NULL) || (t->is_initialized == 0U)) {
        return;
    }

    switch (cmd->type) {
        case TIMER_CMD_START:
            /* 若已在活跃链表，先移除 */
            if (t->is_active) {
                timer_active_remove(t);
            }
            t->expire_tick = rtos_kernel.tick_count + t->period;
            timer_active_insert(t);
            break;

        case TIMER_CMD_STOP:
            if (t->is_active) {
                timer_active_remove(t);
            }
            break;

        case TIMER_CMD_CHANGE_PERIOD:
            if (t->is_active) {
                timer_active_remove(t);
                t->period = cmd->new_period;
                t->expire_tick = rtos_kernel.tick_count + t->period;
                timer_active_insert(t);
            } else {
                t->period = cmd->new_period;
            }
            break;

        case TIMER_CMD_DELETE:
            if (t->is_active) {
                timer_active_remove(t);
            }
            t->is_initialized = 0U;
            break;

        default:
            break;
    }
}

void rtos_timer_service_task(void *arg)
{
    (void)arg;
    timer_cmd_t cmd;

    for (;;) {
        /* 1. 临界区内: 收集到期定时器 */
        RTOS_PORT_ENTER_CRITICAL();
        rtos_timer_t *expired = timer_collect_expired();
        RTOS_PORT_EXIT_CRITICAL();

        /* 2. 临界区外: 执行回调(可安全调用阻塞 API) */
        timer_run_callbacks(expired);

        /* 3. 临界区内: 重插周期定时器, 之后再计算阻塞超时。
         *    [bug fix] 原实现在重插之前计算 timeout: 单一周期定时器触发时
         *    被摘出链表, 此刻链表为空 → timeout=WAIT_FOREVER → recv 永久
         *    等待命令, 周期定时器从此不再触发(实测: 100ms 周期只 fire 1 次)。
         *    将 timeout 计算移到重插之后即可拿到新 expire 时刻。 */
        RTOS_PORT_ENTER_CRITICAL();
        timer_reinsert_periodic(expired);
        rtos_tick_t timeout = timer_get_next_remain();
        RTOS_PORT_EXIT_CRITICAL();

        /* 4. 阻塞等待命令或超时(到期) */
        rtos_status_t st = rtos_queue_recv(&s_cmd_queue, &cmd, timeout);

        if (st == RTOS_OK) {
            /* 处理命令(可能多个，循环取空队列) */
            RTOS_PORT_ENTER_CRITICAL();
            timer_handle_cmd(&cmd);
            /* 继续非阻塞取剩余命令 */
            while (rtos_queue_recv(&s_cmd_queue, &cmd, RTOS_NO_WAIT) == RTOS_OK) {
                timer_handle_cmd(&cmd);
            }
            RTOS_PORT_EXIT_CRITICAL();
        }
        /* 超时(st == RTOS_ERR_TIMEOUT): 下一轮循环处理到期定时器 */
    }
}

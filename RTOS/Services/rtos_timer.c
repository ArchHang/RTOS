/**
 * @file    rtos_timer.c
 * @brief   软件定时器实现
 *
 * @details 软件定时器由专用服务任务管理。设计要点:
 *            - 命令队列: 任务/ISR 通过队列向服务任务发送 start/stop 等命令，
 *              避免在 ISR 中直接操作定时器链表(临界区问题)。
 *            - 活跃链表: 按 expire_tick 升序的双向链表，服务任务每次处理时
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

#include <stdbool.h>
#include <string.h>

#include "rtos_internal.h"
#include "rtos_queue.h"
#include "rtos_sched.h"
#include "rtos_task.h"

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
    uint32_t generation;
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
static rtos_timer_t *s_callback_timer = NULL;
static uint32_t s_generation = 0U;
static uint8_t s_inited = 0U;

/* ============================== 内部函数 ============================== */

/* [bug fix BUG-1] expire_tick 全部改带符号差值比较(回绕安全, 与 rtos_sched.c 同源):
 * 原无符号比较下, period=0xFFFFFFFF 的周期定时器 expire 回绕成 now-1 → 每个 tick
 * 都"到期" → 回调 1kHz 风暴; 回绕点附近 start 的正常定时器可被延迟最长 24 天。 */
static inline int32_t timer_tick_diff(rtos_tick_t a, rtos_tick_t b)
{
    return (int32_t)(a - b);
}

/**
 * @brief 将定时器按 expire_tick 升序插入活跃链表。
 */
static void timer_active_insert(rtos_timer_t *timer)
{
    rtos_tick_t now = rtos_kernel.tick_count;
    rtos_timer_t *cur = s_active_head;
    rtos_timer_t *prev = NULL;

    /* 以当前时刻为共同基准，过期节点先于未来节点。直接比较两到期时刻
     * 在一个已过期、另一个周期接近 2^31 时可能越过有符号半平面。 */
    while ((cur != NULL) &&
           (timer_tick_diff(cur->expire_tick, now) <= timer_tick_diff(timer->expire_tick, now))) {
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
 * @brief 取出一个到期定时器(临界区内调用)。
 * @details 回调中的 start 可以修改活跃链表，因此不可再复用 next 串接
 *          待执行链。逐个派发使回调改变自身或其他定时器都不会污染遍历。
 */
static rtos_timer_t *timer_take_expired(void)
{
    rtos_tick_t now = rtos_kernel.tick_count;
    if ((s_active_head != NULL) && (timer_tick_diff(s_active_head->expire_tick, now) <= 0)) {
        rtos_timer_t *t = s_active_head;
        timer_active_remove(t);
        return t;
    }
    return NULL;
}

/**
 * @brief 执行到期定时器回调(临界区外调用)。
 * @details 在临界区外执行用户回调, 回调可安全调用阻塞 API。
 *          回调中控制 API 先消费已接纳 FIFO 前缀，再同步执行自身命令；
 *          此时仅持有当前节点，不依赖它的链指针。
 */
static void timer_run_callbacks(rtos_timer_t *t)
{
    if ((t != NULL) && (t->callback != NULL)) {
        t->callback(t->arg);
    }
}

/**
 * @brief 将周期定时器重新插入活跃链表(临界区内调用)。
 * @details 原到期时刻递推保持绝对相位。若回调错过多个周期，跳过已错过
 *          的触发点，避免回调追赶风暴；显式 start 的重计时优先于自动重插。
 */
static void timer_reinsert_periodic(rtos_timer_t *t, rtos_tick_t previous_expire)
{
    rtos_tick_t now = rtos_kernel.tick_count;
    if ((t->is_initialized != 0U) && (t->mode == RTOS_TIMER_PERIODIC) &&
        (t->deactivated == 0U) && (t->is_active == 0U)) {
        rtos_tick_t next_expire = previous_expire + t->period;
        if (timer_tick_diff(next_expire, now) <= 0) {
            uint32_t missed = (now - next_expire) / t->period + 1U;
            next_expire += missed * t->period;
        }
        t->expire_tick = next_expire;
        timer_active_insert(t);
    }
    t->deactivated = 0U;
}

/* ============================== 服务任务 ============================== */

static rtos_status_t timer_handle_cmd(const timer_cmd_t *cmd);

static rtos_status_t timer_apply_cmd(const timer_cmd_t *cmd)
{
    RTOS_PORT_ENTER_CRITICAL();
    const rtos_status_t status = timer_handle_cmd(cmd);
    RTOS_PORT_EXIT_CRITICAL();
    return status;
}

static void timer_drain_prefix(uint32_t pending)
{
    /* Only the service consumes the queue. IRQs and senders can append, but
     * these first pending commands cannot be displaced when we unmask IRQs. */
    for (uint32_t index = 0U; index < pending; ++index) {
        timer_cmd_t queued;
        if (rtos_queue_recv(&s_cmd_queue, &queued, RTOS_NO_WAIT) != RTOS_OK) {
            break;
        }
        (void)timer_apply_cmd(&queued);
    }
}

static void timer_drain_accepted(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    const uint32_t pending = rtos_queue_get_count(&s_cmd_queue);
    RTOS_PORT_EXIT_CRITICAL();
    timer_drain_prefix(pending);
}

/**
 * @brief 发送命令到服务任务队列。
 */
static rtos_status_t timer_send_cmd(timer_cmd_type_t type, rtos_timer_t *timer,
                                    rtos_tick_t new_period)
{
    RTOS_ASSERT_ISR_OK();
    const bool in_isr = RTOS_PORT_IN_ISR() != RTOS_FALSE;
    const bool execute_here = !in_isr && rtos_kernel.current_tcb == &s_timer_tcb;
    timer_cmd_t cmd;
    cmd.type = type;
    cmd.timer = timer;
    cmd.new_period = new_period;
    RTOS_PORT_ENTER_CRITICAL();
    if (timer->is_initialized == 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    cmd.generation = timer->generation;
    const uint32_t pending = execute_here ? rtos_queue_get_count(&s_cmd_queue) : 0U;
    RTOS_PORT_EXIT_CRITICAL();

    if (execute_here) {
        /* A callback cannot wait on its own queue. Apply the already accepted
         * prefix first so an earlier IRQ command cannot undo this operation.
         * Recheck the target after that prefix, which may have deleted it. */
        timer_drain_prefix(pending);
        return timer_apply_cmd(&cmd);
    }

    /* 中断中不阻塞 */
    const rtos_tick_t timeout = in_isr ? RTOS_NO_WAIT : 100U;
    return rtos_queue_send(&s_cmd_queue, &cmd, timeout, RTOS_FALSE);
}

/* ============================== 公共 API ============================== */

rtos_status_t rtos_timer_init(void)
{
    /* Initialisation creates a task, so it is a thread-context operation.
     * Serialize the full nonblocking setup: a concurrent caller must not reset
     * the command queue or active timers before duplicate-task detection. */
    if (RTOS_PORT_IN_ISR()) {
        return RTOS_ERR_ISR;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (s_inited != 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_ALREADY;
    }
    rtos_status_t st =
        rtos_queue_init(&s_cmd_queue, s_cmd_storage, sizeof(timer_cmd_t), TIMER_CMD_QUEUE_LEN);
    if (st != RTOS_OK) {
        RTOS_PORT_EXIT_CRITICAL();
        return st;
    }
    s_active_head = NULL;
    s_callback_timer = NULL;

    /* 创建定时器服务任务(静态分配: 内核服务任务不依赖堆, 保证可用性) */
    st = rtos_task_create_static(&s_timer_tcb, s_timer_stack, RTOS_CONFIG_TIMER_TASK_STACK_SIZE,
                                 rtos_timer_service_task, NULL, RTOS_CONFIG_TIMER_TASK_PRIORITY,
                                 "TIMER");
    if (st == RTOS_OK) {
        s_inited = 1U;
    }
    /* Leave s_inited clear on failure, allowing a later valid retry. The new
     * service task cannot run until this outer critical section is released. */
    RTOS_PORT_EXIT_CRITICAL();
    return st;
}

rtos_status_t rtos_timer_create(rtos_timer_t *timer, const char *name, rtos_timer_cb_t callback,
                                void *arg, rtos_tick_t period, rtos_timer_mode_t mode)
{
    if ((timer == NULL) || (callback == NULL) || (period == 0U) ||
        ((mode != RTOS_TIMER_ONE_SHOT) && (mode != RTOS_TIMER_PERIODIC))) {
        return RTOS_ERR_PARAM;
    }
    /* [bug fix BUG-1] 周期必须落在带符号差值可表示域内(2^31), 否则 expire_tick
     * 回绕后到期判断/排序错乱(周期定时器以 1kHz 风暴执行)。 */
    if (period >= 0x80000000U) {
        return RTOS_ERR_PARAM;
    }
    /* 首次 storage 可以未清零：通过已知活跃对象的地址查重，不能读取
     * timer 内未初始化的标志。回调阻塞期间也不允许别人重建当前对象。 */
    RTOS_ASSERT_ISR_OK();
    RTOS_PORT_ENTER_CRITICAL();
    if (timer == s_callback_timer) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_ALREADY;
    }
    for (rtos_timer_t *cur = s_active_head; cur != NULL; cur = cur->next) {
        if (cur == timer) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_ERR_ALREADY;
        }
    }
    memset(timer, 0, sizeof(*timer));
    s_generation++;
    if (s_generation == 0U) {
        s_generation++;
    }
    timer->generation = s_generation;
    timer->mode = mode;
    timer->period = period;
    timer->callback = callback;
    timer->arg = arg;
    timer->name = name;
    timer->is_active = 0U;
    timer->is_initialized = 1U;
    RTOS_PORT_EXIT_CRITICAL();
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
    /* [缺陷修复 TM-1] 与 rtos_timer_create 同口径的上界校验, 原实现遗漏。
     * 缺此校验时 change_period(0xFFFFFFFF) 会让 expire_tick = now + period
     * 回绕到过去, 周期定时器每一轮服务循环都判定为"已到期"并被重插回过去
     * → 回调风暴(真机实测: 100ms 内触发 100 次, 即 1kHz)。
     * create 的注释已写明该风险, 本处属于同一不变式的漏检。 */
    if (period >= 0x80000000U) {
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

void rtos_timer_service_task(void *arg)
{
    (void)arg;
    timer_cmd_t cmd;

    for (;;) {
        /* 逐个取出和派发，避免回调 API 改写待执行链的 next 指针。 */
        RTOS_PORT_ENTER_CRITICAL();
        rtos_timer_t *expired = timer_take_expired();
        if (expired != NULL) {
            const rtos_tick_t previous_expire = expired->expire_tick;
            s_callback_timer = expired;
            RTOS_PORT_EXIT_CRITICAL();

            timer_run_callbacks(expired);

            /* 回调可能改变链表或消耗多个 Tick；重插后重新检查到期节点。 */
            RTOS_PORT_ENTER_CRITICAL();
            timer_reinsert_periodic(expired, previous_expire);
            s_callback_timer = NULL;
            RTOS_PORT_EXIT_CRITICAL();
            /* 处理当前已接纳的命令，再检查到期，避免两侧相互饿死。 */
            timer_drain_accepted();
            continue;
        }

        /* 服务任务独占活跃链的修改，可在本次检查内读取下一截止时刻。 */
        const bool has_deadline = (s_active_head != NULL);
        const rtos_tick_t deadline = has_deadline ? s_active_head->expire_tick : 0U;
        RTOS_PORT_EXIT_CRITICAL();

        /* 绝对到期时刻由接收路径在挂等待前换算，抢占不会重启超时预算。 */
        rtos_status_t st = has_deadline
            ? rtos_internal_queue_recv_until(&s_cmd_queue, &cmd, deadline)
            : rtos_queue_recv(&s_cmd_queue, &cmd, RTOS_WAIT_FOREVER);

        if (st == RTOS_OK) {
            (void)timer_apply_cmd(&cmd);
            timer_drain_accepted();
        }
        /* 超时(st == RTOS_ERR_TIMEOUT): 下一轮循环处理到期定时器 */
    }
}

/**
 * @brief 处理一条命令(临界区内执行)。
 */
static rtos_status_t timer_handle_cmd(const timer_cmd_t *cmd)
{
    rtos_timer_t *t = cmd->timer;
    if ((t == NULL) || (t->is_initialized == 0U) || (t->generation != cmd->generation)) {
        return RTOS_ERR_PARAM;
    }

    switch (cmd->type) {
        case TIMER_CMD_START:
            /* 若已在活跃链表，先移除 */
            if (t->is_active) {
                timer_active_remove(t);
            }
            t->deactivated = 0U; /* 重新启动: 清掉上一次回调留下的停用标记 */
            t->expire_tick = rtos_kernel.tick_count + t->period;
            timer_active_insert(t);
            break;

        case TIMER_CMD_STOP:
            if (t->is_active) {
                timer_active_remove(t);
            }
            /* [缺陷修复 TC-1] 若本命令来自定时器回调内部(此刻该定时器已被
             * collect 摘链, is_active==0, 上面的摘链是空操作), 必须留下标记,
             * 否则随后的 timer_reinsert_periodic 会把它重新插回活跃链表 ——
             * 回调里 stop 自己完全无效。 */
            if ((t->is_initialized != 0U) && (t->is_active == 0U)) {
                t->deactivated = 1U;
            }
            break;

        case TIMER_CMD_CHANGE_PERIOD:
            if (t->is_active) {
                timer_active_remove(t);
                t->period = cmd->new_period;
                t->deactivated = 0U; /* 改周期即重新计时: 停用标记作废 */
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
            t->deactivated = 1U; /* [TC-1] 回调内自删同样必须阻止重插 */
            break;

        default:
            return RTOS_ERR_PARAM;
    }
    return RTOS_OK;
}

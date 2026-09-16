/**
 * @file    rtos_mutex.c
 * @brief   互斥锁实现(含优先级继承)
 *
 * @details 互斥锁与信号量的关键区别:
 *            - 有"拥有者"概念，仅拥有者可释放。
 *            - 支持递归(同一任务可多次 take，等次数 give 后真正释放)。
 *            - 实现优先级继承: 防止优先级反转。
 *
 *          优先级继承原理:
 *            当高优先级任务 H 等待低优先级任务 L 持有的锁时，临时将 L 的
 *            优先级提升到 H 的级别，使 L 能尽快运行并释放锁，避免中间优先级
 *            任务 M 抢占 L 导致 H 长时间阻塞。L 释放锁后恢复原优先级。
 *
 *          不可在中断中使用(中断没有"拥有者"语义)。
 */
#include "rtos_mutex.h"
#include "rtos_internal.h"
#include "rtos_sched.h"
#include "rtos_task.h" /* rtos_tcb_t 完整定义(访问 owner->priority/state) */
#include <string.h>

/* ============================== 互斥锁注册表 ============================== */
/* 记录所有已初始化互斥锁, 供两类清理使用:
 *   1. 任务删除时释放其持有的所有锁(rtos_internal_mutex_release_all)
 *   2. 等待者离开等待链表时重新评估持有者的继承优先级
 *      (rtos_internal_mutex_waiter_left) */

/** @brief 已初始化互斥锁注册表。 */
static rtos_mutex_t *s_mutex_registry[RTOS_CONFIG_MAX_MUTEXES];

/** @brief 注册表当前条目数。 */
static uint32_t s_mutex_count = 0U;

/**
 * @brief 统一设置持有者优先级: 重挂就绪表 + 重挂其所在的对象等待链。
 * @details [bug fix PI-1/PI-2] 优先级变化的完整动作:
 *          1. 就绪表按新优先级重挂(原逻辑);
 *          2. 若 owner 正阻塞在另一把锁/其他对象的等待链上, 该链按优先级
 *             排序, 必须重排 —— 否则唤醒顺序按旧优先级倒置。
 * @note  调用方需在临界区内。
 */
static void mutex_set_owner_priority(rtos_tcb_t *owner, rtos_prio_t new_prio)
{
    if (owner->priority == new_prio) {
        return;
    }
    rtos_bool_t was_ready =
        (owner->state == RTOS_TASK_READY) || (owner->state == RTOS_TASK_RUNNING);
    if (was_ready) {
        rtos_sched_remove_ready(owner);
    }
    owner->priority = new_prio;
    if (was_ready) {
        rtos_sched_add_ready(owner);
    }
    rtos_internal_requeue_wait_lists(owner);
}

/**
 * @brief 尝试提升持有者优先级(优先级继承)。
 * @param mutex 互斥锁。
 * @param waiter_prio 等待任务的优先级。
 * @details 若持有者当前优先级 > 等待者(数值更大=更低)，则提升持有者到等待者级别。
 */
static void mutex_try_inherit(rtos_mutex_t *mutex, rtos_prio_t waiter_prio)
{
    rtos_tcb_t *owner = mutex->owner;
    if ((owner != NULL) && (owner->priority > waiter_prio)) {
        /* 持有者优先级更低(数值大)，提升到等待者级别 */
        mutex_set_owner_priority(owner, waiter_prio);
    }
}

/**
 * @brief 恢复持有者优先级(多锁安全的优先级继承逆操作)。
 * @param owner 曾被提升优先级的任务。
 *
 * @details [bug fix PI-1] 恢复到"全部持有锁的综合有效优先级", 而非无条件回
 *          base_priority。原实现 A(base5) 持 L1、L2, H1(1) 等 L1 → A 提到 1,
 *          give L1 后无条件恢复 base 5 —— 但 A 仍持 L2 且 H2(2) 在等, 应保持 2;
 *          且此后无任何事件再把 A 提回(提升只发生在新等待者进入 take 时)
 *          → 持续到 A 释放 L2 为止的确定性优先级反转。
 *          rtos_internal_mutex_compute_effective_priority 扫描全部注册锁取
 *          最高等待者优先级, 即正确语义(该函数原本已存在但从未被接入)。
 *
 * @note  调用方必须在计算前先清除 owner 对"正在释放的锁"的所有权
 *       (mutex->owner = NULL), 否则会把该锁尚在链上的等待者也计入, 造成
 *       提升值泄漏(释放后再无人把它降回)。
 */
static void mutex_restore_priority(rtos_tcb_t *owner)
{
    if (owner == NULL) {
        return;
    }
    mutex_set_owner_priority(owner, rtos_internal_mutex_compute_effective_priority(owner));
}

rtos_status_t rtos_mutex_init(rtos_mutex_t *mutex)
{
    if (mutex == NULL) {
        return RTOS_ERR_NULL;
    }
    memset(mutex, 0, sizeof(*mutex));
    mutex->owner = NULL;
    mutex->recursion_count = 0U;
    mutex->wait_head = NULL;
    mutex->is_initialized = 1U;

    /* 注册到互斥锁注册表(供任务删除时清理所有权)。
     * [bug fix G-1] 池满时显式报错而非静默放弃注册: 未注册锁的 owner 被删时
     * release_all 扫不到 → 所有权悬垂 + 等待者永久死锁, 失败必须是显式的。 */
    RTOS_PORT_ENTER_CRITICAL();
    if (s_mutex_count < RTOS_CONFIG_MAX_MUTEXES) {
        s_mutex_registry[s_mutex_count] = mutex;
        s_mutex_count++;
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }
    RTOS_PORT_EXIT_CRITICAL();
    mutex->is_initialized = 0U;
    return RTOS_ERR_NO_MEM;
}

rtos_status_t rtos_mutex_take(rtos_mutex_t *mutex, rtos_tick_t timeout)
{
    if (mutex == NULL) {
        return RTOS_ERR_NULL;
    }
    if (mutex->is_initialized == 0U) {
        return RTOS_ERR_PARAM;
    }
    if (RTOS_PORT_IN_ISR()) {
        return RTOS_ERR_ISR; /* 互斥锁不可在中断中使用 */
    }

    RTOS_PORT_ENTER_CRITICAL();
    rtos_tcb_t *cur = rtos_kernel.current_tcb;

    /* 情况1: 锁未被持有 -> 当前任务获取 */
    if (mutex->owner == NULL) {
        mutex->owner = cur;
        mutex->recursion_count = 1U;
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 情况2: 当前任务已持有 -> 递归计数加一 */
    if (mutex->owner == cur) {
        mutex->recursion_count++;
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 情况3: 被其他任务持有 -> 阻塞等待(可能触发优先级继承) */
    if (timeout == RTOS_NO_WAIT) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_TIMEOUT;
    }

    /* 优先级继承: 提升持有者优先级 */
    mutex_try_inherit(mutex, cur->priority);

    /* 阻塞等待 */
    rtos_internal_block_current_on((void *)mutex, &mutex->wait_head, timeout);
    RTOS_PORT_EXIT_CRITICAL();

    /* 被唤醒: 由 give 在唤醒前已完成所有权移交(owner = 本任务, recursion=1),
     *         无需在此设置, 避免与 give 之间产生竞争窗口。
     *         仅需检查唤醒结果: OK=获得锁, TIMEOUT/DELETED=未获得。 */
    cur = rtos_kernel.current_tcb;
    return cur->wait_result;
}

rtos_status_t rtos_mutex_give(rtos_mutex_t *mutex)
{
    if (mutex == NULL) {
        return RTOS_ERR_NULL;
    }
    if (mutex->is_initialized == 0U) {
        return RTOS_ERR_PARAM;
    }
    if (RTOS_PORT_IN_ISR()) {
        return RTOS_ERR_ISR;
    }

    RTOS_PORT_ENTER_CRITICAL();
    rtos_tcb_t *cur = rtos_kernel.current_tcb;

    /* 仅拥有者可释放 */
    if (mutex->owner != cur) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }

    /* 递归计数减一，未归零则仍持有 */
    mutex->recursion_count--;
    if (mutex->recursion_count > 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 真正释放: 恢复优先级 + 唤醒等待者并移交所有权
     *
     * 顺序关键(必须先恢复后唤醒):
     *   1. 先摘本锁所有权再恢复 old_owner 优先级(compute_effective 不计入本锁
     *      的剩余等待者), 保证随后 wake_highest 内 sched_unblock 触发
     *      sched_schedule 时, 调度决策基于 old_owner 的正确优先级, 被唤醒者
     *      若优先级更高可立即抢占。
     *   2. 唤醒最高优先级等待者, 并在其运行前即移交所有权(owner=woken,
     *      recursion=1), 消除"owner=NULL 窗口内被第三方 take 窃取"的竞争。
     *      本锁临界区内无并发, 先置 owner=NULL 是安全的。 */
    rtos_tcb_t *old_owner = cur;
    mutex->owner = NULL;
    mutex_restore_priority(old_owner);

    rtos_tcb_t *woken = rtos_internal_wake_highest(&mutex->wait_head);
    if (woken != NULL) {
        /* 移交所有权: 被唤醒任务运行时即已是 owner, 无需在其 take 中再设置 */
        mutex->owner = woken;
        mutex->recursion_count = 1U;
    }

    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_mutex_deinit(rtos_mutex_t *mutex)
{
    if (mutex == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_PORT_ENTER_CRITICAL();
    /* 先摘所有权再恢复(compute_effective 不计入本锁等待者, 见 restore 注释) */
    rtos_tcb_t *deinit_owner = mutex->owner;
    mutex->owner = NULL;
    if (deinit_owner != NULL) {
        mutex_restore_priority(deinit_owner);
    }
    rtos_internal_wake_all(&mutex->wait_head, RTOS_ERR_DELETED);
    mutex->is_initialized = 0U;

    /* 从注册表移除(swap-with-last) */
    for (uint32_t i = 0U; i < s_mutex_count; i++) {
        if (s_mutex_registry[i] == mutex) {
            s_mutex_count--;
            s_mutex_registry[i] = s_mutex_registry[s_mutex_count];
            s_mutex_registry[s_mutex_count] = NULL;
            break;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

/* ============================== 内部接口(任务删除/等待者离开时调用) ============================== */

/**
 * @brief 重新评估互斥锁持有者的继承优先级。
 * @details 当某个等待者因超时/删除/挂起离开等待链表后调用。
 *          [bug fix PI-1] 改用 compute_effective_priority 综合全部持有锁:
 *          原实现只扫本锁剩余等待者, 多锁场景下会把其他锁带来的提升一并撤销
 *          (与 restore 同源的确定性反转)。
 * @note  本锁的所有权仍在(owner 未摘除), 其剩余等待者正确计入。
 */
static void mutex_recompute_inheritance(rtos_mutex_t *mutex)
{
    rtos_tcb_t *owner = mutex->owner;
    if (owner == NULL) {
        return;
    }
    mutex_set_owner_priority(owner, rtos_internal_mutex_compute_effective_priority(owner));
}

void rtos_internal_mutex_waiter_left(rtos_tcb_t *waiter)
{
    if ((waiter == NULL) || (waiter->wait_node.wait_obj == NULL)) {
        return;
    }

    RTOS_PORT_ENTER_CRITICAL();
    void *obj = waiter->wait_node.wait_obj;
    for (uint32_t i = 0U; i < s_mutex_count; i++) {
        if (s_mutex_registry[i] == (rtos_mutex_t *)obj) {
            /* 该对象确为互斥锁: 等待者减少, 重新评估持有者继承优先级。
             * 非互斥锁对象(信号量/队列/事件等)不会被匹配, 自动忽略。 */
            mutex_recompute_inheritance((rtos_mutex_t *)obj);
            break;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

void rtos_internal_mutex_release_all(rtos_tcb_t *owner)
{
    if (owner == NULL) {
        return;
    }

    RTOS_PORT_ENTER_CRITICAL();
    for (uint32_t i = 0U; i < s_mutex_count; i++) {
        rtos_mutex_t *mutex = s_mutex_registry[i];
        if ((mutex != NULL) && (mutex->owner == owner)) {
            /* 先摘所有权再恢复(compute_effective 不计入本锁等待者, 见 restore 注释)。
             * 任务状态已置 DELETED, mutex_set_owner_priority 不会触碰就绪表
             * (was_ready 为 false), 等待链重挂因 blocked_on 已清空而空操作。 */
            mutex->owner = NULL;
            mutex_restore_priority(owner);

            /* 移交所有权给最高优先级等待者, 或释放锁。
             * 顺序与 give 一致: 先恢复再唤醒, 保证被唤醒者若优先级更高可立即抢占。 */
            rtos_tcb_t *woken = rtos_internal_wake_highest(&mutex->wait_head);
            if (woken != NULL) {
                mutex->owner = woken;
                mutex->recursion_count = 1U;
            } else {
                mutex->recursion_count = 0U;
            }
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

rtos_prio_t rtos_internal_mutex_compute_effective_priority(rtos_tcb_t *owner)
{
    if (owner == NULL) {
        return (rtos_prio_t)RTOS_CONFIG_MAX_PRIORITIES;
    }

    /* 从 base_priority 开始, 取所有持有锁的等待者中最高优先级(数值最小) */
    rtos_prio_t effective = owner->base_priority;
    for (uint32_t i = 0U; i < s_mutex_count; i++) {
        rtos_mutex_t *mutex = s_mutex_registry[i];
        if ((mutex != NULL) && (mutex->owner == owner)) {
            rtos_wait_node_t *node = mutex->wait_head;
            while (node != NULL) {
                if (node->tcb->priority < effective) {
                    effective = node->tcb->priority;
                }
                node = node->next;
            }
        }
    }
    return effective;
}

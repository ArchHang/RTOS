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
        rtos_bool_t was_ready =
            (owner->state == RTOS_TASK_READY) || (owner->state == RTOS_TASK_RUNNING);
        if (was_ready) {
            rtos_sched_remove_ready(owner);
        }
        owner->priority = waiter_prio;
        if (was_ready) {
            rtos_sched_add_ready(owner);
        }
    }
}

/**
 * @brief 恢复任务到基础优先级(优先级继承的逆操作)。
 * @param owner 曾被提升优先级的任务(已释放或即将释放互斥锁)。
 *
 * @details 全释放互斥锁后调用。任务不再持有该锁, 应恢复到 base_priority。
 *
 *          设计说明:
 *          - 本实现不跟踪任务持有的多个互斥锁, 因此多锁场景下可能丢失
 *            来自其他锁的优先级提升(已知限制; FreeRTOS 通过维护"持有锁
 *            列表"重新计算最高优先级来解决)。单锁场景完全正确。
 *          - 无条件恢复 base_priority: 释放后不再持有该锁, 不应基于该锁的
 *            等待者维持提升(旧实现检查 wait_head 是错误的——被唤醒的等待者
 *            尚在链表中会导致误判"保持提升")。
 *
 *          O(1): 仅就绪表移除/重新加入, 无链表遍历。
 */
static void mutex_restore_priority(rtos_tcb_t *owner)
{
    if (owner == NULL) {
        return;
    }

    /* 仅当优先级确实被改变才需要更新就绪表 */
    if (owner->priority == owner->base_priority) {
        return;
    }

    rtos_bool_t was_ready =
        (owner->state == RTOS_TASK_READY) || (owner->state == RTOS_TASK_RUNNING);
    if (was_ready) {
        rtos_sched_remove_ready(owner);
    }
    owner->priority = owner->base_priority;
    if (was_ready) {
        rtos_sched_add_ready(owner);
    }
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
     * 注册表容量 = MAX_MUTEXES, 正常使用不会溢出; 池满时放弃注册(防御性)。 */
    RTOS_PORT_ENTER_CRITICAL();
    if (s_mutex_count < RTOS_CONFIG_MAX_MUTEXES) {
        s_mutex_registry[s_mutex_count] = mutex;
        s_mutex_count++;
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
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
        mutex->owner_orig_priority = cur->priority;
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
     *   1. 先恢复 old_owner 到 base_priority, 保证随后 wake_highest 内
     *      sched_unblock 触发 sched_schedule 时, 调度决策基于 old_owner 的
     *      正确(base)优先级, 被唤醒者若优先级更高可立即抢占。
     *      (若先唤醒, old_owner 仍处于被提升的优先级, 可能与唤醒者优先级
     *       相等而不触发切换; 随后恢复时又不再触发调度 → 切换丢失。)
     *   2. 唤醒最高优先级等待者, 并在其运行前即移交所有权(owner=woken,
     *      recursion=1), 消除"owner=NULL 窗口内被第三方 take 窃取"的竞争
     *      (旧实现在此处无条件置 owner=NULL, 由被唤醒的 take 重新设置,
     *       中间窗口存在锁被抢夺的风险)。 */
    rtos_tcb_t *old_owner = cur;
    mutex_restore_priority(old_owner);

    rtos_tcb_t *woken = rtos_internal_wake_highest(&mutex->wait_head);
    if (woken != NULL) {
        /* 移交所有权: 被唤醒任务运行时即已是 owner, 无需在其 take 中再设置 */
        mutex->owner = woken;
        mutex->owner_orig_priority = woken->priority;
        mutex->recursion_count = 1U;
    } else {
        mutex->owner = NULL;
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
    /* 恢复持有者优先级(若被提升) */
    if (mutex->owner != NULL) {
        mutex_restore_priority(mutex->owner);
    }
    rtos_internal_wake_all(&mutex->wait_head, RTOS_ERR_DELETED);
    mutex->owner = NULL;
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
 * @details 当某个等待者因超时/删除/挂起离开等待链表后调用: 持有者只需维持到
 *          "剩余等待者中的最高优先级"即可; 若已无等待者则恢复到基础优先级。
 * @note 与现有实现一致的多锁限制: 持有者同时持有多把锁时, 其他锁带来的提升
 *       可能因此被撤销(已知限制, 单锁场景完全正确)。
 */
static void mutex_recompute_inheritance(rtos_mutex_t *mutex)
{
    rtos_tcb_t *owner = mutex->owner;
    if (owner == NULL) {
        return;
    }

    /* 计算剩余等待者中的最高优先级(数值最小) */
    rtos_prio_t boost_prio = owner->base_priority;
    rtos_bool_t need_boost = RTOS_FALSE;
    rtos_wait_node_t *node = mutex->wait_head;
    while (node != NULL) {
        if (node->tcb->priority < boost_prio) {
            boost_prio = node->tcb->priority;
            need_boost = RTOS_TRUE;
        }
        node = node->next;
    }

    rtos_prio_t new_prio = need_boost ? boost_prio : owner->base_priority;
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
            /* 恢复被提升的优先级(任务状态已置 DELETED, 不触就绪表) */
            mutex_restore_priority(owner);

            /* 移交所有权给最高优先级等待者, 或释放锁。
             * 顺序与 give 一致: 先恢复再唤醒, 保证被唤醒者若优先级更高可立即抢占。 */
            rtos_tcb_t *woken = rtos_internal_wake_highest(&mutex->wait_head);
            if (woken != NULL) {
                mutex->owner = woken;
                mutex->owner_orig_priority = woken->priority;
                mutex->recursion_count = 1U;
            } else {
                mutex->owner = NULL;
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

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

/* One owner per registered mutex, plus the task that just released its last
 * mutex. Scratch values start at base priorities so a dependency cycle cannot
 * retain an obsolete donation after the external high-priority waiter leaves. */
typedef struct mutex_priority_graph {
    rtos_tcb_t *owners[RTOS_CONFIG_MAX_MUTEXES + 1U];
    rtos_prio_t effective[RTOS_CONFIG_MAX_MUTEXES + 1U];
    uint32_t count;
} mutex_priority_graph_t;

static uint32_t mutex_graph_index(const mutex_priority_graph_t *graph,
                                  const rtos_tcb_t *owner)
{
    for (uint32_t index = 0U; index < graph->count; index++) {
        if (graph->owners[index] == owner) {
            return index;
        }
    }
    return graph->count;
}

static void mutex_graph_add_owner(mutex_priority_graph_t *graph, rtos_tcb_t *owner)
{
    if ((owner == NULL) || (mutex_graph_index(graph, owner) != graph->count)) {
        return;
    }
    RTOS_ASSERT(graph->count < RTOS_CONFIG_MAX_MUTEXES + 1U);
    graph->owners[graph->count] = owner;
    graph->effective[graph->count] = owner->base_priority;
    graph->count++;
}

static rtos_bool_t mutex_graph_relax(mutex_priority_graph_t *graph)
{
    rtos_bool_t changed = RTOS_FALSE;

    for (uint32_t index = 0U; index < s_mutex_count; index++) {
        rtos_mutex_t *mutex = s_mutex_registry[index];
        uint32_t owner = mutex_graph_index(graph, mutex->owner);
        if (owner == graph->count) {
            continue;
        }
        for (rtos_wait_node_t *node = mutex->wait_head; node != NULL; node = node->next) {
            RTOS_ASSERT((node->tcb != NULL) && (node->wait_obj == (void *)mutex)
                && (node->list_head == &mutex->wait_head));
            if ((node->tcb == NULL) || (node->wait_obj != (void *)mutex)
                || (node->list_head != &mutex->wait_head)) {
                break;
            }
            uint32_t donor = mutex_graph_index(graph, node->tcb);
            rtos_prio_t priority = donor < graph->count
                ? graph->effective[donor] : node->tcb->priority;
            if (priority < graph->effective[owner]) {
                graph->effective[owner] = priority;
                changed = RTOS_TRUE;
            }
        }
    }
    return changed;
}

/** @brief Recompute transitive donations without recursion or temporary drops.
 *  @param detached_owner Task that may just have lost its final owned mutex.
 *  @note Caller holds the kernel critical section. Each pass propagates at
 *        least one dependency edge; at most count passes cover every simple
 *        owner path, including paths into cycles. This does not resolve a
 *        deadlock: applications still need lock ordering or finite timeouts. */
static void mutex_refresh_priority_graph(rtos_tcb_t *detached_owner)
{
    mutex_priority_graph_t graph = {0};
    rtos_bool_t changed = RTOS_FALSE;

    for (uint32_t index = 0U; index < s_mutex_count; index++) {
        mutex_graph_add_owner(&graph, s_mutex_registry[index]->owner);
    }
    mutex_graph_add_owner(&graph, detached_owner);
    for (uint32_t pass = 0U; pass < graph.count; pass++) {
        if (mutex_graph_relax(&graph) == RTOS_FALSE) {
            break;
        }
    }
    for (uint32_t index = 0U; index < graph.count; index++) {
        if (graph.owners[index]->priority != graph.effective[index]) {
            mutex_set_owner_priority(graph.owners[index], graph.effective[index]);
            changed = RTOS_TRUE;
        }
    }
    if ((changed == RTOS_TRUE) && (rtos_kernel.sched_state == RTOS_SCHED_RUNNING)) {
        rtos_sched_schedule();
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
 *          从基础优先级重新求解全部所有者的传递捐赠，覆盖等待链与循环依赖；
 *          从旧的有效优先级起算会使已撤销的捐赠滞留在循环中。
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
    mutex_refresh_priority_graph(owner);
}

rtos_status_t rtos_mutex_init(rtos_mutex_t *mutex)
{
    if (mutex == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_ASSERT_ISR_OK();
    RTOS_PORT_ENTER_CRITICAL();
    /* Match the address before reading/resetting the object: init supports
     * previously uninitialized automatic storage. Duplicate registration would
     * leave a dangling entry after the first deinit and lose an active owner. */
    for (uint32_t i = 0U; i < s_mutex_count; i++) {
        if (s_mutex_registry[i] == mutex) {
            /* Preserve the existing idle re-init contract without appending a
             * second registry entry. An owned/waited-on mutex cannot reset. */
            rtos_status_t status = (mutex->owner == NULL && mutex->wait_head == NULL &&
                                    mutex->recursion_count == 0U) ? RTOS_OK : RTOS_ERR_ALREADY;
            RTOS_PORT_EXIT_CRITICAL();
            return status;
        }
    }
    if (s_mutex_count >= RTOS_CONFIG_MAX_MUTEXES) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_NO_MEM;
    }
    memset(mutex, 0, sizeof(*mutex));
    mutex->owner = NULL;
    mutex->recursion_count = 0U;
    mutex->wait_head = NULL;
    mutex->is_initialized = 1U;

    /* 注册到互斥锁注册表(供任务删除时清理所有权)。
     * [bug fix G-1] 池满时显式报错而非静默放弃注册: 未注册锁的 owner 被删时
     * release_all 扫不到 → 所有权悬垂 + 等待者永久死锁, 失败必须是显式的。 */
    s_mutex_registry[s_mutex_count] = mutex;
    s_mutex_count++;
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

    RTOS_INTERNAL_OBJECT_ACCESS_HOOK(mutex, 8U);
    RTOS_PORT_ENTER_CRITICAL();
    /* An allowed ISR may deinitialize the still-live storage after the fast
     * precheck. Revalidate with exclusion before any state access or wait. */
    if (mutex->is_initialized == 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
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
        if (mutex->recursion_count == UINT32_MAX) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_ERR_OVERFLOW;
        }
        mutex->recursion_count++;
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 情况3: 被其他任务持有 -> 阻塞等待(可能触发优先级继承) */
    if (timeout == RTOS_NO_WAIT) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_TIMEOUT;
    }

    /* [缺陷修复 SD-1] 挂起期间禁止阻塞: 必须在 mutex_try_inherit **之前**返回,
     * 否则会白白提升持有者优先级(且此后无人把它恢复, 因为恢复发生在释放路径上,
     * 而释放者可能就是被永久阻塞的那个任务)。 */
    if (rtos_internal_block_permitted() != RTOS_OK) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_SCHED;
    }

    /* Publish the new dependency before solving the donation graph. The
     * helper may pend a switch, but this outer critical section still holds. */
    if (rtos_internal_block_current_on((void *)mutex, &mutex->wait_head, timeout) != RTOS_OK) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_SCHED;
    }
    mutex_refresh_priority_graph(NULL);
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

    RTOS_INTERNAL_OBJECT_ACCESS_HOOK(mutex, 9U);
    RTOS_PORT_ENTER_CRITICAL();
    /* An allowed ISR may deinitialize the still-live storage after the fast
     * precheck. Revalidate with exclusion before any state access or wait. */
    if (mutex->is_initialized == 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    rtos_tcb_t *cur = rtos_kernel.current_tcb;

    /* 仅拥有者可释放 */
    if ((mutex->owner != cur) || (mutex->recursion_count == 0U)) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }

    /* 递归计数减一，未归零则仍持有 */
    mutex->recursion_count--;
    if (mutex->recursion_count > 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    /* 在同一临界区内完成移交，再求解包含新所有者与旧所有者的捐赠图。
     * wake 只挂起 PendSV，任务不能在所有权和有效优先级更新前运行。
     * 全图刷新会重新选择 next，避免使用 wake 时的临时调度结果。 */
    rtos_tcb_t *old_owner = cur;
    mutex->owner = NULL;
    rtos_tcb_t *woken = rtos_internal_wake_highest(&mutex->wait_head);
    if (woken != NULL) {
        /* 移交所有权: 被唤醒任务运行时即已是 owner, 无需在其 take 中再设置 */
        mutex->owner = woken;
        mutex->recursion_count = 1U;
    }
    mutex_restore_priority(old_owner);

    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_mutex_deinit(rtos_mutex_t *mutex)
{
    if (mutex == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_ASSERT_ISR_OK();
    RTOS_PORT_ENTER_CRITICAL();
    /* 先摘所有权再恢复(compute_effective 不计入本锁等待者, 见 restore 注释) */
    rtos_tcb_t *deinit_owner = mutex->owner;
    mutex->owner = NULL;
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
    mutex_restore_priority(deinit_owner);
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

/* ============================== 内部接口(任务删除/等待者离开时调用) ============================== */

/**
 * @brief 重新评估互斥锁持有者的继承优先级。
 * @details 当某个等待者因超时/删除/挂起离开等待链表后调用。
 *          从全部持有锁和等待依赖重新求解有效优先级，传播提升与撤销。
 * @note  本锁的所有权仍在(owner 未摘除), 其剩余等待者正确计入。
 */
static void mutex_recompute_inheritance(rtos_mutex_t *mutex)
{
    rtos_tcb_t *owner = mutex->owner;
    if (owner == NULL) {
        return;
    }
    mutex_refresh_priority_graph(owner);
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
            /* 移交所有权给最高优先级等待者，或释放锁；随后刷新捐赠图。
             * 外层临界区保证全部锁清理完成前任务不会运行。 */
            rtos_tcb_t *woken = rtos_internal_wake_highest(&mutex->wait_head);
            if (woken != NULL) {
                mutex->owner = woken;
                mutex->recursion_count = 1U;
            } else {
                mutex->recursion_count = 0U;
            }
            mutex_restore_priority(owner);
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

rtos_prio_t rtos_internal_mutex_compute_effective_priority(rtos_tcb_t *owner)
{
    if (owner == NULL) {
        return (rtos_prio_t)RTOS_CONFIG_MAX_PRIORITIES;
    }

    /* 每把锁的等待链始终按有效优先级排序: 入链和优先级变化都会
     * insert_by_prio/requeue，离开时只摘链。故每把锁只需检查链首，
     * 不必重复扫描同一把锁的全部等待者。调用方持有内核临界区。 */
    rtos_prio_t effective = owner->base_priority;
    for (uint32_t i = 0U; i < s_mutex_count; i++) {
        rtos_mutex_t *mutex = s_mutex_registry[i];
        if ((mutex != NULL) && (mutex->owner == owner)) {
            rtos_wait_node_t *node = mutex->wait_head;
            if (node != NULL) {
                if (node->tcb->priority < effective) {
                    effective = node->tcb->priority;
                }
            }
        }
    }
    return effective;
}

/* ============================== 内部接口: 注册表占用查询 ============================== */

/**
 * @brief 等待者优先级变化后, 重新评估其所在互斥锁持有者的继承优先级。
 *
 * @param waiter [in] 优先级刚被修改的任务(可能正阻塞在某把互斥锁上)。
 *
 * @details [缺陷修复 BM-3] 优先级继承只在"新等待者进入 take"与"等待者离开"
 *          两个时刻重新评估。若等待者在**阻塞期间**被 `rtos_task_set_priority`
 *          改了优先级, 原实现只重排了等待链, 持有者的提升值仍停留在旧优先级:
 *             L(base 10) 持锁 → H(base 8) 阻塞, L 被提升到 8;
 *             set_priority(H, 2) → 等待链已把 H 排到最前, 但 L 仍是 8;
 *             中优先级任务 M(5) 依旧能抢占 L → H(有效优先级 2) 仍被无限期饥饿,
 *             即优先级继承要消除的那个反转又回来了。
 *
 *          本函数按"等待者 → 它阻塞在哪把锁"反查注册表(不需要在 TCB 里保存
 *          互斥锁类型信息), 重算该锁持有者的有效优先级。
 *
 * @note 调用方需在临界区内。
 */
void rtos_internal_mutex_waiter_requeued(rtos_tcb_t *waiter)
{
    if (waiter == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < s_mutex_count; i++) {
        rtos_mutex_t *mutex = s_mutex_registry[i];
        if ((mutex == NULL) || (mutex->owner == NULL) || (mutex->owner == waiter)) {
            continue;
        }
        /* 遍历本锁等待链时校验节点的归属: wait_obj 必须回指本锁, 且节点自报的
         * list_head 必须就是本锁的链头地址。等待链的 prev/next 由多个模块共享
         * 同一个侵入式节点结构, 节点可能属于别的对象(或已被复用), 无校验的
         * 正向遍历一旦遇到陈旧指针就会解引用野地址(实测: 曾在此处触发
         * BusFault PRECISERR, BFAR = 0x056884B5)。校验失败即视为不在本链。 */
        for (rtos_wait_node_t *node = mutex->wait_head; node != NULL; node = node->next) {
            if (node->wait_obj != (void *)mutex) {
                break;
            }
            if (node->list_head != &mutex->wait_head) {
                break;
            }
            if (node->tcb == waiter) {
                mutex_restore_priority(mutex->owner);
                return;
            }
        }
    }
}

/**
 * @brief 当前已注册的互斥锁数量。
 * @return 注册表条目数(0 .. RTOS_CONFIG_MAX_MUTEXES)。
 *
 * @details 供回归用例做"注册表槽位无泄漏"守卫: 注册表容量有限, 若创建方忘记
 *          deinit, 槽位会被长期占用, 后续 mutex_init 返回 ERR_NO_MEM ——
 *          该锁随即失去优先级继承(释放路径 compute_effective_priority 需要
 *          通过注册表枚举持有者的锁), 表现为静默的优先级反转。
 *          实测过该级联故障, 故把占用数暴露为可观测的内部接口。
 */
uint32_t rtos_internal_mutex_registry_count(void)
{
    return s_mutex_count;
}

/**
 * @file    rtos_task.c
 * @brief   任务管理实现
 *
 * @details 实现任务创建/删除/延时/挂起/恢复/优先级调整/任务通知。
 *          所有修改内核数据结构的 API 都通过临界区保护。
 *
 *          任务生命周期:
 *            create -> READY -> RUNNING -> (delay/blocked) -> READY -> ...
 *                  -> delete -> TCB 回收(若动态分配)
 *
 *          栈溢出检测: 方法2 在栈底写入魔数 0xDEADBEEF，每次切换时校验。
 *          任务通知: 轻量级同步，无独立对象，直接写 TCB 字段。
 */
#include "rtos_task.h"
#include "rtos_sched.h"
#include "rtos_internal.h"
#include "rtos_perf.h"
#include "rtos_heap.h"
#include <string.h>

/* 内部接口: 在 rtos_sched.c 中实现 */
extern uint32_t rtos_internal_alloc_task_id(void);

/** @brief 通知状态枚举(内部)。 */
#define NOTIFY_IDLE 0U
#define NOTIFY_PENDING 1U
#define NOTIFY_RECEIVED 2U

/** @brief 栈溢出检测魔数。 */
#define RTOS_STACK_MAGIC (0xDEADBEEFU)

/* ============================== 延迟释放(自删除场景) ============================== */
/* 当任务自删除且为动态分配时, TCB 与栈不可立即释放: PendSV 切换上下文时仍需
 * 写入 current_tcb->top_of_stack。故将待释放 TCB 链入此链表, 由 idle 任务
 * 在后续调度中清理(此时 PendSV 早已完成, TCB 不再被引用)。
 *
 * 链表使用 TCB 的 next_ready 字段复用为链表指针(任务已删除, 不在就绪表)。
 * 操作均在临界区内, 无需额外同步。 */
static rtos_tcb_t *s_pending_free_list = NULL;

/**
 * @brief 将动态分配的 TCB 加入待释放链表(供 idle 任务清理)。
 * @note  调用前必须已移出所有调度链表, 且不在当前运行位置。
 */
static void defer_free_tcb(rtos_tcb_t *tcb)
{
    tcb->next_ready = s_pending_free_list;
    s_pending_free_list = tcb;
}

/**
 * @brief 清理待释放链表(在 idle 任务中调用)。
 * @details 释放每个待释放 TCB 的栈与 TCB 本身。此时 PendSV 早已完成切换,
 *          TCB 不再被任何上下文引用, 可安全释放。
 */
static void process_pending_free(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    rtos_tcb_t *list = s_pending_free_list;
    s_pending_free_list = NULL;
    RTOS_PORT_EXIT_CRITICAL();

    while (list != NULL) {
        rtos_tcb_t *tcb = list;
        list = tcb->next_ready;
        /* 释放栈与 TCB(顺序无关, 二者独立分配) */
        rtos_heap_free(tcb->stack_base);
        rtos_heap_free(tcb);
    }
}

/* ============================== 内部函数 ============================== */

/* [fix G-5] 原静态函数 check_stack_overflow 已内联进 idle 巡检
 * (钩子移出临界区后不再需要独立函数), 删除以避免死代码。 */

/**
 * @brief 任务退出函数(任务 entry 返回时调用)。
 * @details 任务函数不应返回。若返回，本函数将删除任务并切换。
 *          由 rtos_port_init_stack 作为初始 LR 装入, 必须为外部链接。
 */
void rtos_task_exit_handler(void)
{
    rtos_task_delete(NULL);
    while (1) {
    } /* 永不执行到此 */
}

/**
 * @brief 按优先级将等待节点插入链表(数值小在前)。
 */
void rtos_internal_wait_insert_by_prio(rtos_wait_node_t **head, rtos_wait_node_t *node)
{
    rtos_wait_node_t *cur = *head;
    rtos_wait_node_t *prev = NULL;

    while ((cur != NULL) && (cur->tcb->priority <= node->tcb->priority)) {
        prev = cur;
        cur = cur->next;
    }

    node->next = cur;
    node->prev = prev;
    if (prev != NULL) {
        prev->next = node;
    } else {
        *head = node;
    }
    if (cur != NULL) {
        cur->prev = node;
    }
}

/**
 * @brief 从等待链表移除节点。
 */
void rtos_internal_wait_remove(rtos_wait_node_t **head, rtos_wait_node_t *node)
{
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        *head = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
    node->next = NULL;
    node->prev = NULL;
}

/**
 * @brief 将当前任务挂入对象等待链表并阻塞。
 * @param obj     所等待对象指针。
 * @param head    对象等待链表头指针的地址。
 * @param timeout 超时(0=不阻塞)。
 * @return RTOS_OK 表示进入阻塞(返回时已被唤醒)，需调用方检查 wait_result。
 *
 * @details 调用前必须已进入临界区。函数会:
 *          1. 初始化当前任务的等待节点。
 *          2. 按优先级插入对象等待链表(高优先级在前，便于优先唤醒)。
 *          3. 调用 sched_block 设置阻塞/延时态。
 *          4. 触发调度切换到下一任务。
 *          当任务再次被调度运行时(被对象唤醒或超时)，函数返回。
 */
rtos_status_t rtos_internal_block_current_on(void *obj, rtos_wait_node_t **head,
                                             rtos_tick_t timeout)
{
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    RTOS_ASSERT(cur != NULL);

    cur->blocked_on = obj;
    cur->wait_node.tcb = cur;
    cur->wait_node.wait_obj = obj;
    cur->wait_node.timed_out = RTOS_FALSE;
    cur->wait_node.next = NULL;
    cur->wait_node.prev = NULL;
    /* 注意: transfer_buf 不在此清除 —— 队列零拷贝需在调用本函数前设置并保留。 */
    cur->wait_node.list_head = head; /* 记录所在链表，供超时清理 */
    cur->wait_result = RTOS_OK;

    /* 按优先级插入对象等待链表 */
    rtos_internal_wait_insert_by_prio(head, &cur->wait_node);

    /* 进入阻塞态(同时处理延时表) */
    rtos_sched_block(cur, timeout);

    /* 触发调度(切到下一任务)，返回时当前任务已被唤醒恢复运行 */
    rtos_sched_schedule();
    return RTOS_OK;
}

/**
 * @brief 唤醒等待链表中的最高优先级任务(并从链表移除)。
 * @param head 等待链表头指针的地址。
 * @return 被唤醒的 TCB，NULL 表示链表空。
 *
 * @details 链表已按优先级排序，链首即最高优先级。本函数:
 *          1. 取链首节点并从等待链表移除。
 *          2. 清除阻塞标记与对象指针。
 *          3. 调用 sched_unblock 从延时表移除(若因超时入表)并放入就绪表。
 *          4. sched_unblock 内会自动判断是否需要触发抢占。
 */
rtos_tcb_t *rtos_internal_wake_highest(rtos_wait_node_t **head)
{
    rtos_wait_node_t *best = *head;
    if (best == NULL) {
        return NULL;
    }

    /* 从等待链表移除(链首即最高优先级) */
    rtos_internal_wait_remove(head, best);

    best->tcb->blocked_on = NULL;
    best->tcb->wait_result = RTOS_OK;
    best->list_head = NULL; /* 标记已离开等待链表(防御性: 与 queue 零拷贝路径一致) */

    rtos_sched_unblock(best->tcb);
    return best->tcb;
}

/**
 * @brief 唤醒等待链表中所有任务(用于对象销毁)。
 */
void rtos_internal_wake_all(rtos_wait_node_t **head, rtos_status_t reason)
{
    while (*head != NULL) {
        rtos_wait_node_t *node = *head;
        rtos_internal_wait_remove(head, node);
        node->tcb->blocked_on = NULL;
        node->tcb->wait_result = reason;
        node->list_head = NULL;
        rtos_sched_unblock(node->tcb);
    }
}

/* ============================== 公共 API ============================== */

/**
 * @brief 创建任务的核心实现(共享于静态与动态两条路径)。
 * @details 已假定 tcb/stack_buf/stack_size 等参数合法。仅负责:
 *          - TCB 字段初始化
 *          - 栈初始化(伪硬件帧)
 *          - 加入就绪表与性能监视器
 *          - 必要时触发抢占
 */
static rtos_status_t task_create_core(rtos_tcb_t *tcb, rtos_stack_t *stack_buf, uint32_t stack_size,
                                      rtos_task_func_t entry, void *arg, rtos_prio_t priority,
                                      const char *name, rtos_bool_t is_static)
{
    /* 初始化 TCB */
    memset(tcb, 0, sizeof(*tcb));
    tcb->is_static = is_static;
    tcb->priority = priority;
    tcb->base_priority = priority;
    tcb->state = RTOS_TASK_READY;
    tcb->task_id = rtos_internal_alloc_task_id();
    tcb->notify_state = NOTIFY_IDLE;
    tcb->notify_value = 0U;

    if (name != NULL) {
        strncpy(tcb->name, name, RTOS_CONFIG_MAX_TASK_NAME_LEN - 1U);
        tcb->name[RTOS_CONFIG_MAX_TASK_NAME_LEN - 1U] = '\0';
    } else {
        tcb->name[0] = '\0';
    }

    /* 初始化栈: 调用移植层把伪现场压入栈 */
    tcb->stack_base = stack_buf;
    tcb->stack_size = stack_size;
    tcb->top_of_stack = rtos_port_init_stack(stack_buf + stack_size, stack_size, entry, arg);

#if RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW
    tcb->stack_magic = RTOS_STACK_MAGIC;
    *(volatile uint32_t *)stack_buf = RTOS_STACK_MAGIC;
#endif

    /* 性能监视器: 初始化任务性能字段, 填充栈水印, 加入注册表。
     * 必须在加入就绪表前调用(确保任务可运行前已被观测)。
     * 栈水印填充会覆盖栈底魔数 *stack_buf(该值仅在 TCB.stack_magic 校验,
     * 栈内副本从不读取, 覆盖无害)。 */
    rtos_perf_on_task_created(tcb);

    /* 加入就绪表(临界区) */
    RTOS_PORT_ENTER_CRITICAL();
    rtos_sched_add_ready(tcb);
    /* 若调度器已运行且新任务优先级更高，立即切换 */
    if ((rtos_kernel.sched_state == RTOS_SCHED_RUNNING) &&
        (priority < rtos_kernel.current_tcb->priority)) {
        rtos_sched_schedule();
    }
    RTOS_PORT_EXIT_CRITICAL();

    return RTOS_OK;
}

rtos_status_t rtos_task_create_static(rtos_tcb_t *tcb, rtos_stack_t *stack_buf, uint32_t stack_size,
                                      rtos_task_func_t entry, void *arg, rtos_prio_t priority,
                                      const char *name)
{
    if ((tcb == NULL) || (stack_buf == NULL) || (entry == NULL)) {
        return RTOS_ERR_NULL;
    }
    if (priority >= RTOS_CONFIG_MAX_PRIORITIES) {
        return RTOS_ERR_PARAM;
    }
    /* [bug fix BUG-4] 最小栈 = init 伪帧 18 字 + FPU 任务阻塞时 PendSV 压栈
     * (硬件扩展帧 26 + S16-S31 16 + R4-R11 8 + R3/LR 2 = 52 字) + 基本
     * 调用链与局部变量裕量 ≈ 85 字 → 取 96 字。
     * 原校验 20 字漏算 FPU 与调用链(实测: 20 字 FPU 任务首次切换即越界写穿
     * heap header, 全系统任务创建静默失败); 40 字在 FPU+深调用链下仍会下溢
     * (实测确认, 溢出钩子触发)。 */
    if ((stack_size < 96U) || (stack_size > (0x7FFFFFFFU / sizeof(rtos_stack_t)))) {
        return RTOS_ERR_PARAM;
    }

    return task_create_core(tcb, stack_buf, stack_size, entry, arg, priority, name,
                            RTOS_TRUE /* is_static */);
}

rtos_status_t rtos_task_create(rtos_tcb_t **tcb_out, uint32_t stack_size, rtos_task_func_t entry,
                               void *arg, rtos_prio_t priority, const char *name)
{
    if ((tcb_out == NULL) || (entry == NULL)) {
        return RTOS_ERR_NULL;
    }
    if (priority >= RTOS_CONFIG_MAX_PRIORITIES) {
        return RTOS_ERR_PARAM;
    }
    /* 与 rtos_task_create_static 相同的最小栈校验(FPU 扩展帧, 见上文注释)。
     * 动态栈从堆分配, 上限同时受堆大小约束。 */
    if ((stack_size < 96U) || (stack_size > (RTOS_CONFIG_HEAP_SIZE / sizeof(rtos_stack_t)))) {
        return RTOS_ERR_PARAM;
    }

    /* 从堆分配 TCB */
    rtos_tcb_t *tcb = (rtos_tcb_t *)rtos_heap_alloc(sizeof(rtos_tcb_t));
    if (tcb == NULL) {
        return RTOS_ERR_NO_MEM;
    }

    /* 从堆分配栈(8 字节对齐由 rtos_heap_alloc 保证) */
    rtos_stack_t *stack_buf = (rtos_stack_t *)rtos_heap_alloc(stack_size * sizeof(rtos_stack_t));
    if (stack_buf == NULL) {
        rtos_heap_free(tcb);
        return RTOS_ERR_NO_MEM;
    }

    rtos_status_t st = task_create_core(tcb, stack_buf, stack_size, entry, arg, priority, name,
                                        RTOS_FALSE /* is_static */);
    if (st != RTOS_OK) {
        /* 失败时回滚堆分配 */
        rtos_heap_free(stack_buf);
        rtos_heap_free(tcb);
        return st;
    }

    *tcb_out = tcb;
    return RTOS_OK;
}

rtos_status_t rtos_task_delete(rtos_tcb_t *tcb)
{
    rtos_bool_t self_delete = RTOS_FALSE;

    /* ISR 中禁止删除任务: 自删除会标记被中断任务为 DELETED 但其继续运行至
     * 下次调度; 删除其他任务虽可行但涉及调度触发, 本实现统一禁止 ISR 调用。
     * ISR 中如需终止任务, 应通过队列/通知委托给任务上下文执行。 */
    if (RTOS_PORT_IN_ISR()) {
        return RTOS_ERR_ISR;
    }

    if (tcb == NULL) {
        tcb = rtos_kernel.current_tcb;
        self_delete = RTOS_TRUE;
    } else if (tcb == rtos_kernel.current_tcb) {
        /* 显式传入自身句柄与 delete(NULL) 语义等价: 任务仍运行在该栈上,
         * 必须走 defer_free 延迟回收。否则立即 free 会释放正在使用的栈,
         * 函数返回/PendSV 写 top_of_stack 均为 use-after-free。 */
        self_delete = RTOS_TRUE;
    }
    if (tcb == NULL) {
        return RTOS_ERR_NULL;
    }

    RTOS_PORT_ENTER_CRITICAL();

    /* [bug fix RACE-3] 双重删除防护: 自删除的动态任务在 idle 回收前, TCB 挂在
     * s_pending_free_list 上且 state=DELETED; 若持旧句柄再次 delete, 非自删路径
     * 会立即 heap_free, 新任务复用该内存后 idle 再释放一次 → 释放正在使用的
     * TCB/栈。同款防护顺带拒绝删除 idle(删 idle 后系统进入不可恢复脏态)。 */
    if (tcb->state == RTOS_TASK_DELETED) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    if (tcb == rtos_kernel.idle_tcb) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }

    /* 从就绪表移除 */
    if (tcb->state == RTOS_TASK_READY || tcb->state == RTOS_TASK_RUNNING) {
        rtos_sched_remove_ready(tcb);
    }
    /* 从延时表移除(DELAYED 态: 带超时阻塞或纯延时)。
     * 必须直接从延时表移除, 不能复用 sched_unblock: unblock 会把任务加入
     * 就绪表并在其优先级更高时触发调度, 使 next_tcb 指向本任务; 随后本任务
     * 状态置 DELETED 且动态内存被释放, PendSV 将切换到已释放的 TCB
     * → use-after-free 崩溃。 */
    else if (tcb->state == RTOS_TASK_DELAYED) {
        rtos_sched_remove_delayed(tcb);
    }
    /* BLOCKED 态(永久阻塞): 不在任何调度链表, 无需处理 */

    /* 从对象等待链表移除(若阻塞在 sem/mutex/queue/event 上):
     * 不移除会留下悬空节点, 后续 wake_highest/wake_all 会解引用已释放的 TCB
     * → use-after-free。notify_wait 无链表(list_head=NULL), 跳过。
     * 若被删任务是互斥锁等待者, 同时通过 waiter_left 重新评估持有者的
     * 继承优先级(等待者减少后撤销多余的提升)。 */
    if (tcb->blocked_on != NULL) {
        if (tcb->wait_node.list_head != NULL) {
            rtos_internal_wait_remove(tcb->wait_node.list_head, &tcb->wait_node);
            tcb->wait_node.list_head = NULL;
        }
        /* 若该任务正阻塞在互斥锁上, 等待者减少后重新评估持有者的继承优先级 */
        rtos_internal_mutex_waiter_left(tcb);
        tcb->blocked_on = NULL;
    }

    tcb->state = RTOS_TASK_DELETED;

    /* [bug fix RACE-1] 若该任务已被选为 next_tcb(PendSV 已 pend 未执行),
     * 必须失效并重选, 否则 PendSV 切换到已 DELETED/已释放的 TCB
     * → use-after-free 或强置 RUNNING 的脏状态。 */
    if (rtos_kernel.next_tcb == tcb) {
        rtos_kernel.next_tcb = NULL;
    }

    /* 性能监视器: 从注册表移除(swap-with-last, O(1))。必须在 delete_hook 前,
     * 避免 hook 中用户查询到已销毁任务的悬空指针。 */
    rtos_perf_on_task_deleted(tcb);

    /* 若该任务持有互斥锁: 将所有权移交给最高优先级等待者(或释放锁)。
     * 必须在 TCB/栈释放前调用, 否则 mutex->owner 指向已释放的 TCB
     * → use-after-free, 且其余等待者永久死锁。 */
    rtos_internal_mutex_release_all(tcb);

#if RTOS_CONFIG_USE_DELETE_HOOK
    rtos_task_delete_hook(tcb);
#endif

    /* 动态分配的内存回收策略:
     *   - 非自删除: TCB 不在运行位置, 可立即释放
     *   - 自删除: TCB 仍是 current_tcb, PendSV 切换时需写入 top_of_stack,
     *     必须延迟到 idle 任务清理
     * 静态任务不释放(用户管理) */
    if (!tcb->is_static) {
        if (self_delete) {
            defer_free_tcb(tcb);
        } else {
            rtos_heap_free(tcb->stack_base);
            rtos_heap_free(tcb);
        }
    }

    if (self_delete) {
        /* 切换到下一任务 */
        rtos_sched_schedule();
    }

    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

void rtos_task_yield(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    /* 把当前任务移到同优先级就绪链表尾 */
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    /* [bug fix RACE-4] 阻塞路径"出就绪表→schedule→退出临界区→PendSV 入口"间
     * 存在微窗口, 此时若高优先级 ISR 调用 yield, cur 已是 DELAYED/BLOCKED 但
     * 仍是 current —— rotate 别人的链并把 next_tcb 覆盖为次优选择。非 RUNNING
     * 态直接返回。 */
    if ((cur != NULL) && (cur->state == RTOS_TASK_RUNNING)) {
        rtos_prio_t prio = cur->priority;
        rtos_tcb_t *head = rtos_kernel.ready_heads[prio];
        if ((head != NULL) && (head->next_ready != head)) {
            rtos_kernel.ready_heads[prio] = head->next_ready;
            rtos_kernel.next_tcb = rtos_kernel.ready_heads[prio];
            rtos_kernel.switch_count++;
            rtos_port_context_switch();
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

void rtos_task_delay(rtos_tick_t ticks)
{
    if (ticks == 0U) {
        rtos_task_yield();
        return;
    }

    if (RTOS_PORT_IN_ISR()) {
        return; /* ISR 不允许延时 */
    }

    RTOS_PORT_ENTER_CRITICAL();
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    /* 复用 sched_block: 统一处理就绪表移除 + 延时表插入(O(n) 升序) */
    rtos_sched_block(cur, ticks);
    rtos_sched_schedule();
    RTOS_PORT_EXIT_CRITICAL();
}

void rtos_task_delay_until(rtos_tick_t *last_wake_tick, rtos_tick_t period)
{
    if ((last_wake_tick == NULL) || (period == 0U)) {
        return;
    }

    if (RTOS_PORT_IN_ISR()) {
        return; /* ISR 不允许延时/阻塞 */
    }

    RTOS_PORT_ENTER_CRITICAL();
    rtos_tick_t now = rtos_kernel.tick_count;
    rtos_tick_t elapsed = now - *last_wake_tick;
    rtos_tick_t remain = (period > elapsed) ? (period - elapsed) : 0U;
    /* 落后时跳过所有已错过的周期, 直接对齐到最近的未来唤醒点,
     * 避免任务全速追赶饿死低优先级任务。 */
    if (remain == 0U) {
        rtos_tick_t missed = elapsed / period;
        *last_wake_tick += (missed + 1U) * period;
        remain = *last_wake_tick - now;
    } else {
        *last_wake_tick += period;
    }

    if (remain > 0U) {
        rtos_tcb_t *cur = rtos_kernel.current_tcb;
        /* sched_block(cur, remain) 设置 wake_tick = now + remain，与需求一致 */
        rtos_sched_block(cur, remain);
        rtos_sched_schedule();
    }
    RTOS_PORT_EXIT_CRITICAL();
}

rtos_tcb_t *rtos_task_get_current(void)
{
    return rtos_kernel.current_tcb;
}

rtos_prio_t rtos_task_get_priority(const rtos_tcb_t *tcb)
{
    if (tcb == NULL) {
        tcb = rtos_kernel.current_tcb;
    }
    return (tcb != NULL) ? tcb->priority : (rtos_prio_t)RTOS_CONFIG_MAX_PRIORITIES;
}

/* ============================== 等待链重挂(优先级变化后) ============================== */

void rtos_internal_requeue_wait_lists(rtos_tcb_t *tcb)
{
    /* [bug fix PI-2] 优先级变化(用户 set_priority 或内核继承提升/恢复)后,
     * 该任务若正阻塞在某对象等待链上, 必须按新优先级重排——等待链按优先级
     * 排序插入, 不重排则 wake_highest 按旧优先级取链首, 唤醒顺序倒置。 */
    if ((tcb->blocked_on != NULL) && (tcb->wait_node.list_head != NULL)) {
        rtos_wait_node_t **head = tcb->wait_node.list_head;
        rtos_internal_wait_remove(head, &tcb->wait_node);
        rtos_internal_wait_insert_by_prio(head, &tcb->wait_node);
    }
}

rtos_status_t rtos_task_set_priority(rtos_tcb_t *tcb, rtos_prio_t prio)
{
    if (tcb == NULL) {
        tcb = rtos_kernel.current_tcb;
    }
    if ((tcb == NULL) || (prio >= RTOS_CONFIG_MAX_PRIORITIES)) {
        return RTOS_ERR_PARAM;
    }

    RTOS_PORT_ENTER_CRITICAL();
    rtos_bool_t was_ready = (tcb->state == RTOS_TASK_READY) || (tcb->state == RTOS_TASK_RUNNING);

    if (was_ready) {
        rtos_sched_remove_ready(tcb);
    }
    tcb->base_priority = prio;
    /* 重新计算有效优先级: 若持有互斥锁且有等待者优先级更高, 需维持继承的提升。
     * 直接设 priority=prio 会丢弃优先级继承, 导致等待者无法抢占持锁者。 */
    tcb->priority = rtos_internal_mutex_compute_effective_priority(tcb);
    if (was_ready) {
        rtos_sched_add_ready(tcb);
    }
    /* [bug fix PI-2] 优先级变化后重挂对象等待链: 等待链按优先级排序插入,
     * 若该任务正阻塞在某对象上(sem/mutex/queue/event)且优先级被修改,
     * 不重排则对象释放时 wake_highest 仍按旧优先级取链首 → 唤醒顺序倒置
     * (低优先级先得资源)。 */
    rtos_internal_requeue_wait_lists(tcb);

    /* 若提升的优先级高于当前，或当前被降低，触发切换 */
    if (rtos_kernel.sched_state == RTOS_SCHED_RUNNING) {
        rtos_sched_schedule();
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_task_suspend(rtos_tcb_t *tcb)
{
    if (tcb == NULL) {
        /* 自挂起: 不允许在 ISR 中调用。
         * 原因: 自挂起需要立即触发调度切换出去, 但 ISR 末尾才挂起 PendSV,
         *       期间 current_tcb 仍指向被标记为 SUSPENDED 的任务, 状态不一致;
         *       且 ISR 中 rtos_sched_schedule 路径会尝试 context_switch, 与
         *       ISR 自身的栈上下文冲突。挂起其他任务可在 ISR 中调用。 */
        if (RTOS_PORT_IN_ISR()) {
            return RTOS_ERR_ISR;
        }
        tcb = rtos_kernel.current_tcb;
    }
    if (tcb == NULL) {
        return RTOS_ERR_NULL;
    }

    RTOS_PORT_ENTER_CRITICAL();
    rtos_bool_t self_suspend = (tcb == rtos_kernel.current_tcb);

    if (tcb->state == RTOS_TASK_READY || tcb->state == RTOS_TASK_RUNNING) {
        rtos_sched_remove_ready(tcb);
    } else if (tcb->state == RTOS_TASK_DELAYED) {
        /* [fix S-7] 原实现手写复制了 delay_list_remove 的循环(与 rtos_sched.c
         * 重复且漏清 prev_ready) —— 统一调用公共实现(双向 O(1))。 */
        rtos_sched_remove_delayed(tcb);
    }

    /* 若任务正阻塞在某对象(sem/mutex/queue/event)的等待链表上，
     * 必须将其移除，否则对象后续 wake_highest 会误唤醒已挂起任务
     * (导致状态混乱)。notify_wait 无链表(list_head=NULL)，跳过。
     *
     * 必须同时设置 wait_result 为失败: 挂起时本次等待已被中止,
     * 若保留阻塞前的 RTOS_OK, 任务 resume 后从阻塞点恢复执行,
     * sem_take/mutex_take 会直接返回"成功"——但实际上既没有 give
     * 也没有所有权移交, 互斥锁被"偷"进临界区, 互斥语义破坏。
     * 语义沿用 RTOS_ERR_DELETED(对象销毁同款): 等待已作废。 */
    if (tcb->blocked_on != NULL) {
        if (tcb->wait_node.list_head != NULL) {
            rtos_internal_wait_remove(tcb->wait_node.list_head, &tcb->wait_node);
            tcb->wait_node.list_head = NULL;
        }
        /* 若该任务正阻塞在互斥锁上, 等待者减少后重新评估持有者的继承优先级 */
        rtos_internal_mutex_waiter_left(tcb);
        tcb->wait_result = RTOS_ERR_DELETED; /* 等待被挂起中止, 恢复后返回失败 */
        tcb->blocked_on = NULL;
    }

    tcb->state = RTOS_TASK_SUSPENDED;

    /* [bug fix RACE-1] 若该任务已被选为 next_tcb(PendSV 已 pend 但尚未执行,
     * 例如 ISR give 唤醒后又被更高优先级中断 suspend), 必须失效并重选,
     * 否则 PendSV 会把已挂出就绪表的任务强置 RUNNING → 调度链表脏状态。 */
    if (rtos_kernel.next_tcb == tcb) {
        rtos_kernel.next_tcb = NULL;
    }

    if (self_suspend) {
        rtos_sched_schedule();
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_task_resume(rtos_tcb_t *tcb)
{
    if (tcb == NULL) {
        return RTOS_ERR_NULL;
    }

    /* [bug fix BUG-2] 状态检查必须在整个临界区内完成(原实现在临界区外检查,
     * 通过后若被高优先级 ISR 抢占且 ISR 中也 resume 同一任务并完整执行,
     * 本任务恢复后会再次 add_ready 一个已在环中的节点 → 环形链表前驱指针
     * 被改写 → 同优先级其他任务从调度器中永久消失或 HardFault)。 */
    RTOS_PORT_ENTER_CRITICAL();
    if (tcb->state == RTOS_TASK_SUSPENDED) {
        tcb->state = RTOS_TASK_READY;
        rtos_sched_add_ready(tcb);
        if ((rtos_kernel.sched_state == RTOS_SCHED_RUNNING) &&
            (rtos_kernel.current_tcb != NULL) &&
            (tcb->priority < rtos_kernel.current_tcb->priority)) {
            rtos_sched_schedule();
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_task_notify(rtos_tcb_t *tcb, uint32_t value, rtos_notify_type_t type)
{
    if (tcb == NULL) {
        return RTOS_ERR_NULL;
    }
    RTOS_ASSERT_ISR_OK(); /* [guard G-3] 违约优先级中断调用时立即捕获 */

    RTOS_PORT_ENTER_CRITICAL();
    switch (type) {
        case RTOS_NOTIFY_VALUE:
            tcb->notify_value = value;
            break;
        case RTOS_NOTIFY_BIT:
            tcb->notify_value |= value;
            break;
        case RTOS_NOTIFY_INCREMENT:
            tcb->notify_value++;
            break;
        default:
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_ERR_PARAM;
    }
    tcb->notify_state = NOTIFY_PENDING;

    /* 若任务在等待通知, 唤醒它。
     * 跨模块陷阱: notify_wait 用 rtos_sched_block 阻塞, finite timeout 时
     * state 为 DELAYED(挂入延时表), WAIT_FOREVER 时为 BLOCKED。旧实现仅检查
     * BLOCKED, 导致带超时的 notify_wait 永远收不到通知(只能等超时)——这是
     * 单模块检查发现不了的跨模块状态不匹配 bug。blocked_on==(void*)tcb 是
     * notify_wait 的唯一标记, 配合 state 判断可精确识别。 */
    if ((tcb->state == RTOS_TASK_BLOCKED || tcb->state == RTOS_TASK_DELAYED) &&
        tcb->blocked_on == (void *)tcb) {
        tcb->blocked_on = NULL;
        rtos_sched_unblock(tcb);
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_task_notify_wait(rtos_tick_t timeout, uint32_t *recv_value)
{
    if (RTOS_PORT_IN_ISR()) {
        return RTOS_ERR_ISR;
    }

    RTOS_PORT_ENTER_CRITICAL();
    rtos_tcb_t *cur = rtos_kernel.current_tcb;

    if (cur->notify_state == NOTIFY_PENDING) {
        if (recv_value != NULL) {
            *recv_value = cur->notify_value;
        }
        cur->notify_value = 0U;
        cur->notify_state = NOTIFY_IDLE;
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_OK;
    }

    if (timeout == RTOS_NO_WAIT) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_TIMEOUT;
    }

    /* 阻塞等待 */
    cur->blocked_on = (void *)cur; /* 标记在等通知 */
    cur->wait_node.tcb = cur;
    cur->wait_node.wait_obj = (void *)cur;
    cur->wait_node.timed_out = RTOS_FALSE;
    cur->wait_node.next = NULL;
    cur->wait_node.prev = NULL;
    cur->wait_node.list_head = NULL; /* notify 不挂入对象等待链表，超时无需清理 */
    cur->wait_result = RTOS_OK;
    rtos_sched_block(cur, timeout);
    rtos_sched_schedule();
    RTOS_PORT_EXIT_CRITICAL();

    /* 被唤醒后(此处当前任务已恢复运行) */
    RTOS_PORT_ENTER_CRITICAL();
    rtos_status_t result = cur->wait_result;
    if (recv_value != NULL) {
        *recv_value = cur->notify_value;
    }
    /* 仅在确实被通知唤醒(result==OK)时清除通知状态。
     * 超时瞬间到达的通知应保留供下次 notify_wait 取走, 避免丢失:
     *   - 通知先到: result=OK(task_notify 唤醒), 清除合理。
     *   - 超时先到: result=TIMEOUT, 此后通知可能在"超时已设置但任务未运行"
     *     的窗口内到达(notify_state=PENDING), 保留让下次 wait 立即返回。
     * 旧实现无条件清除, 导致超时窗口内的通知被静默丢弃。 */
    if (result == RTOS_OK) {
        cur->notify_value = 0U;
        cur->notify_state = NOTIFY_IDLE;
    }
    RTOS_PORT_EXIT_CRITICAL();
    return result;
}

/* ============================== 空闲任务 ============================== */

void rtos_idle_task(void *arg)
{
    (void)arg;
    uint32_t idle_count = 0U;

    for (;;) {
        /* 清理自删除任务的 TCB 与栈(必须在 idle 任务中, 因为此时 PendSV
         * 早已完成切换, 待释放 TCB 不再被任何上下文引用)。 */
        if (s_pending_free_list != NULL) {
            process_pending_free();
        }

#if RTOS_CONFIG_GENERATE_RUN_TIME_STATS
        rtos_kernel.idle_run_count++;
#endif
        idle_count++;

#if RTOS_CONFIG_USE_IDLE_HOOK
        rtos_idle_hook();
#else
        __asm volatile("wfi");
#endif

        /* 栈溢出检测: 在空闲任务中周期检查所有任务栈
         * [fix G-5] 巡检覆盖全部任务(含 BLOCKED/SUSPENDED 态), 且钩子在临界区
         * 外执行 —— 原实现在临界区内调用 rtos_stack_overflow_hook, 用户钩子里
         * 的任何 RTOS API/中断式 printf 都会在 PendSV 被屏蔽的状态下请求调度,
         * 语义破坏。 */
        if ((idle_count & 0xFFU) == 0U) {
            rtos_tcb_t *overflowed[RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE];
            uint32_t n_overfl = 0U;
            uint32_t i;

            RTOS_PORT_ENTER_CRITICAL();
#if RTOS_CONFIG_USE_PERF_MONITOR
            /* perf 注册表: 覆盖全部存活任务(含 BLOCKED/SUSPENDED) */
            for (i = 0U; i < rtos_perf_task_registry_count(); i++) {
                rtos_tcb_t *tcb = rtos_perf_task_at(i);
                if ((tcb != NULL) &&
                    ((tcb->stack_magic != RTOS_STACK_MAGIC) ||
                     ((uint32_t)tcb->top_of_stack < (uint32_t)tcb->stack_base)) &&
                    (n_overfl < RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE)) {
                    overflowed[n_overfl++] = tcb;
                }
            }
#else
            /* 无 perf: 退化为扫描就绪表(环形)+延时表(单/双向链) */
            for (rtos_prio_t p = 0U; p < RTOS_CONFIG_MAX_PRIORITIES; p++) {
                rtos_tcb_t *head = rtos_kernel.ready_heads[p];
                if (head == NULL) {
                    continue;
                }
                rtos_tcb_t *tcb = head;
                do {
                    if ((tcb->stack_magic != RTOS_STACK_MAGIC) ||
                        ((uint32_t)tcb->top_of_stack < (uint32_t)tcb->stack_base)) {
                        if (n_overfl < RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE) {
                            overflowed[n_overfl++] = tcb;
                        }
                    }
                    tcb = tcb->next_ready;
                } while (tcb != head);
            }
            for (rtos_tcb_t *tcb = rtos_kernel.delay_head; tcb != NULL; tcb = tcb->next_ready) {
                if ((tcb->stack_magic != RTOS_STACK_MAGIC) ||
                    ((uint32_t)tcb->top_of_stack < (uint32_t)tcb->stack_base)) {
                    if (n_overfl < RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE) {
                        overflowed[n_overfl++] = tcb;
                    }
                }
            }
#endif
            RTOS_PORT_EXIT_CRITICAL();

            for (i = 0U; i < n_overfl; i++) {
                rtos_stack_overflow_hook(overflowed[i]); /* 临界区外执行 */
            }
        }
    }
}

/**
 * @file    rtos_wait.c
 * @brief   内核共用的对象等待链、阻塞登记与唤醒机制。
 *
 * @details IPC、超时和优先级继承通过本模块维护等待节点。
 *          调用方持有内核临界区；本模块不改变临界区嵌套层级。
 *          阻塞登记只请求调度，实际任务切换由退出临界区后的 PendSV 完成。
 */
#include "rtos_internal.h"
#include "rtos_task.h"

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
 * @brief 将等待节点尾插到等待链表(O(n))。
 *
 * @param head 等待链表头指针的地址。
 * @param node 待入链节点。
 *
 * @details 与 rtos_internal_wait_insert_by_prio 的区别: 本函数不做优先级定序,
 *          只做尾插。调用方必须同时维护配套的优先级位图, 并在唤醒时改用
 *          rtos_internal_wake_highest_bm —— 两者组合后语义与原有序链等价
 *          (最高优先级、同优先级 FIFO)。
 *
 * @note  同一对象内不可与 insert_by_prio 混用, 否则唤醒顺序失真。
 *        由于未维护独立尾指针，尾插需要遍历到链尾，复杂度为 O(n)。
 *        位图用于唤醒时定位最高优先级，不改变本函数的遍历成本。
 */
void rtos_internal_wait_append(rtos_wait_node_t **head, rtos_wait_node_t *node)
{
    node->next = NULL;

    if (*head == NULL) {
        node->prev = NULL;
        *head = node;
        return;
    }

    rtos_wait_node_t *tail = *head;
    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = node;
    node->prev = tail;
}

/**
 * @brief 从等待链表移除节点。
 */
void rtos_internal_wait_remove(rtos_wait_node_t **head, rtos_wait_node_t *node)
{
    rtos_wait_node_t *next = node->next;
    rtos_wait_node_t *prev = node->prev;

    /* 合法非循环链的邻居不是 node，先快照可避免邻居写入后的重复 SRAM 读取。 */
    if (prev != NULL) {
        prev->next = next;
    } else {
        *head = next;
    }
    if (next != NULL) {
        next->prev = prev;
    }
    node->next = NULL;
    node->prev = NULL;
}

/**
 * @brief 从等待链表移除节点, 并同步维护位图索引链的优先级位图。
 *
 * @param head [in,out] 等待链表头指针的地址。
 * @param node [in]     待移除节点。
 *
 * @details 适用对象: 采用"尾插链表 + 优先级位图"的等待链(信号量/队列)。
 *          位图位在该节点的优先级上可能被**同优先级的其它等待者**共享, 因此
 *          不能无条件清位 —— 先摘链, 再扫描剩余链判断该优先级是否仍有人等待,
 *          无人时才清位。
 *
 *          为什么需要它: `process_delayed_tasks` 的超时清理原先只调
 *          `rtos_internal_wait_remove`, 位图位被留在原处, 使"位图 ⇔ 链"这一
 *          不变式在每次超时后失效(真机实测残留 bit13 = 8192)。
 *
 * @note 调用方需在临界区内。节点不在链上时按 `rtos_internal_wait_remove`
 *       的防御语义处理(空操作)。
 */
void rtos_internal_wait_remove_sync_bitmap(rtos_wait_node_t **head, rtos_wait_node_t *node)
{
    volatile uint32_t *bitmap = node->queue_bitmap;
    const rtos_prio_t   prio   = node->tcb != NULL ? node->tcb->priority : node->queued_priority;

    rtos_internal_wait_remove(head, node);

    /* 非位图链(有序链: 事件组/互斥锁)无需维护位图 */
    if (bitmap == NULL) {
        return;
    }

    rtos_bool_t still_waiting = RTOS_FALSE;
    for (rtos_wait_node_t *p = *head; p != NULL; p = p->next) {
        if (p->tcb != NULL && p->tcb->priority == prio) {
            still_waiting = RTOS_TRUE;
            break;
        }
    }
    if (still_waiting == RTOS_FALSE) {
        *bitmap &= ~(1UL << prio);
    }
}

/**
 * @brief 将当前任务挂入对象等待链表并阻塞。
 * @param obj     所等待对象指针。
 * @param head    对象等待链表头指针的地址。
 * @param timeout 超时 Tick 数；非阻塞语义由调用方在进入本函数前处理。
 * @return RTOS_OK 表示已登记阻塞；RTOS_ERR_SCHED 表示未修改等待状态。
 *
 * @details 调用前必须已进入临界区。函数会:
 *          1. 初始化当前任务的等待节点。
 *          2. 按优先级插入对象等待链表(高优先级在前，便于优先唤醒)。
 *          3. 调用 sched_block 设置阻塞/延时态。
 *          4. 选定候选任务并请求 PendSV。
 *          本函数仍在调用方临界区内返回。调用方退出最外层临界区后，
 *          PendSV 才能切出当前任务；任务恢复运行后再读取 wait_result。
 */
rtos_status_t rtos_internal_block_current_on(void *obj, rtos_wait_node_t **head,
                                             rtos_tick_t timeout)
{
    rtos_tcb_t *cur = rtos_kernel.current_tcb;
    RTOS_ASSERT(cur != NULL);

    /* [缺陷修复 SD-1] 调度器不在运行态(如被 rtos_sched_suspend 挂起)时禁止阻塞:
     * 此时 tick 已停走, 阻塞下去既不会被唤醒也不会有超时 ⇒ 永久挂死。
     * 必须在**任何状态修改之前**返回, 保证不留下半初始化的等待节点。 */
    if (rtos_internal_block_permitted() != RTOS_OK) {
        return RTOS_ERR_SCHED;
    }

    cur->blocked_on = obj;
    cur->wait_node.tcb = cur;
    cur->wait_node.on_timeout = NULL;
    cur->wait_node.wait_obj = obj;
    cur->wait_node.timed_out = RTOS_FALSE;
    cur->wait_node.next = NULL;
    cur->wait_node.prev = NULL;
    /* 注意: transfer_buf 不在此清除 —— 队列零拷贝需在调用本函数前设置并保留。 */
    cur->wait_node.list_head = head; /* 记录所在链表，供超时清理 */
    cur->wait_node.queue_bitmap = NULL; /* 默认有序链; 位图链由调用方覆盖 */
    cur->wait_node.queued_priority = cur->priority;
    cur->wait_result = RTOS_OK;

    /* 按优先级插入对象等待链表 */
    rtos_internal_wait_insert_by_prio(head, &cur->wait_node);

    /* 进入阻塞态(同时处理延时表) */
    rtos_sched_block(cur, timeout);

    /* 登记候选切换；PendSV 在调用方退出最外层临界区后执行。 */
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
 * @brief 唤醒"位图索引等待链"中最高优先级的任务。
 *
 * @param head   等待链表头指针的地址(同优先级内 FIFO)。
 * @param bitmap 存在等待者的优先级位图(调用方维护, 本函数负责清位)。
 * @return 被唤醒的 TCB, NULL 表示无等待者。
 *
 * @details 语义与 rtos_internal_wake_highest 完全等价(取最高优先级等待者,
 *          同优先级取最早入链者), 但"找最高优先级"从遍历链表改为
 *          `__builtin_ctz`(编译为 RBIT+CLZ 两条指令):
 *            - **单节点快路径**: 链上只有 1 个节点时零遍历、零位图运算 ——
 *              这是"每对象 1~2 个等待者"的典型嵌入式负载下最常见的情形;
 *            - 多节点时按位图取最高优先级, 再在该优先级分组内取链首,
 *              分组内扫描长度 = 同优先级等待者数, 与总等待者数无关。
 *
 * @note  位图与链表理论上不会不一致(两者由同一临界区维护); 但为防止
 *        位图位残留导致"取不到牺牲者", 找不到时退化为**全链扫描取最高优先级**
 *        —— 绝不退化为"取链首", 那会唤醒错误的任务并破坏唤醒序语义。
 */
rtos_tcb_t *rtos_internal_wake_highest_bm(rtos_wait_node_t **head, volatile uint32_t *bitmap)
{
    rtos_wait_node_t *victim = *head;

    if (victim == NULL) {
        return NULL;
    }

    if (victim->next != NULL) {
        const rtos_prio_t top = (*bitmap != 0U) ?
            (rtos_prio_t)__builtin_ctz(*bitmap) : RTOS_CONFIG_MAX_PRIORITIES;
        rtos_wait_node_t *cur = *head;

        while ((cur != NULL) && (cur->tcb->priority != top)) {
            cur = cur->next;
        }
        if (cur != NULL) {
            victim = cur;
        } else {
            rtos_prio_t best = victim->tcb->priority;
            for (rtos_wait_node_t *p = *head; p != NULL; p = p->next) {
                if (p->tcb->priority < best) {
                    best = p->tcb->priority;
                    victim = p;
                }
            }
        }
    }

    rtos_internal_wait_remove(head, victim);

    /* 该优先级若已无其它等待者则清位, 避免位图残留导致后续空扫描 */
    {
        rtos_bool_t still_waiting = RTOS_FALSE;
        for (rtos_wait_node_t *p = *head; p != NULL; p = p->next) {
            if (p->tcb->priority == victim->tcb->priority) {
                still_waiting = RTOS_TRUE;
                break;
            }
        }
        if (still_waiting == RTOS_FALSE) {
            *bitmap &= ~(1UL << victim->tcb->priority);
        }
    }

    victim->tcb->blocked_on = NULL;
    victim->tcb->wait_result = RTOS_OK;
    victim->list_head = NULL;

    rtos_sched_unblock(victim->tcb);
    return victim->tcb;
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

/* ============================== 等待链重挂(优先级变化后) ============================== */

void rtos_internal_requeue_wait_lists(rtos_tcb_t *tcb)
{
    /* [bug fix PI-2] 优先级变化(用户 set_priority 或内核继承提升/恢复)后,
     * 该任务若正阻塞在某对象等待链上, 必须让唤醒选择看到新优先级。
     *
     * 两套等待链约定并存, 必须分别处理:
     *   1. 位图索引链(信号量/队列): 顺序由位图决定。重排链表会破坏"同优先级
     *      FIFO"；优先级未变时保留原位，变化时同步位图并尾插至新优先级组。
     *   2. 有序链(事件组/互斥锁): 摘出后按新优先级重插。
     * 不可对位图链调用 insert_by_prio, 否则会把它的 FIFO 语义破坏掉。 */
    if ((tcb->blocked_on == NULL) || (tcb->wait_node.list_head == NULL)) {
        return;
    }

    rtos_wait_node_t **head = tcb->wait_node.list_head;
    const rtos_prio_t new_prio = tcb->priority;
    const rtos_prio_t old_prio = tcb->wait_node.queued_priority;
    if (new_prio == old_prio) return; /* Effective no-op must preserve FIFO. */

    rtos_internal_wait_remove(head, &tcb->wait_node);

    if (tcb->wait_node.queue_bitmap != NULL) {
        /* [缺陷修复 BM-2] 旧位不能无条件清除。
         * 原注释称"若链上仍有同优先级等待者, 它自己的位早已置位, 不受影响",
         * 该论断是错的: 同优先级等待者共享**同一个位**, 清位会把它的位一并
         * 抹掉。反例: A(prio10)、B(prio10) 同链 → bitmap={10}; 把 A 改为 3,
         * 原逻辑清掉 bit10, 于是 B 仍在链上但 bitmap 中已无它的位。
         * 正确做法与超时/删除路径一致: 摘链后扫描剩余链, 确认该优先级确实
         * 无人等待才清位。 */
        rtos_bool_t still_waiting = RTOS_FALSE;
        for (rtos_wait_node_t *p = *head; p != NULL; p = p->next) {
            if ((p->tcb != NULL) && (p->tcb->priority == old_prio)) {
                still_waiting = RTOS_TRUE;
                break;
            }
        }
        if ((still_waiting == RTOS_FALSE) && (old_prio < RTOS_CONFIG_MAX_PRIORITIES)) {
            *tcb->wait_node.queue_bitmap &= ~(1UL << old_prio);
        }
        *tcb->wait_node.queue_bitmap |= (1UL << new_prio);
        tcb->wait_node.queued_priority = new_prio;
        rtos_internal_wait_append(head, &tcb->wait_node);
        return;
    }

    rtos_internal_wait_insert_by_prio(head, &tcb->wait_node);
    tcb->wait_node.queued_priority = new_prio;
}

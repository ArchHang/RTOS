/**
 * @file    rtos_sched.c
 * @brief   调度器与内核全局状态实现
 *
 * @details 实现基于优先级位图的抢占式调度器。核心算法:
 *
 *          1. 就绪表: 一个 32 位 bitmap(每个 bit 对应一个优先级) + 32 条
 *             双向链表。加入就绪: 置位 bitmap[prio], 链入 ready_heads[prio]。
 *          2. 选最高优先级: 用 __builtin_ctz 取 bitmap 的**最低**置位 bit 索引
 *             (优先级 0=最高, 恰在最低位)。Cortex-M3/M4/M7 编译为
 *             RBIT+CLZ 两条指令, O(1)。切勿写成 31-__builtin_clz ——
 *             那是取最高置位 bit(=最低优先级), 方向相反。
 *          3. 时间片: 同优先级链表按 FIFO, 每次 Tick 把当前任务移到链尾,
 *             链头变新当前任务。
 *          4. 延时表: 双向链表按 wake_tick 升序(Tick 中断只查链首)。
 *
 *          临界区策略: 进入临界区时通过 BASEPRI 屏蔽 RTOS 系统调用级中断,
 *          保证调度器数据结构操作原子性, 同时不阻断硬实时中断。
 */
#include "rtos_sched.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "rtos_internal.h"
#include "rtos_perf.h"
#include "rtos_task.h"

/* ============================== 内核全局实例 ============================== */

enum { SCHED_STATE_ALIGNMENT = 32U };

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(offsetof(rtos_kernel_t, ready_heads) == SCHED_STATE_ALIGNMENT,
               "Scheduler state must fit before the ready queues in one 32-byte line");
#endif

/** @brief Align the singleton so its common scheduler fields share one cache line. */
_Alignas(SCHED_STATE_ALIGNMENT) rtos_kernel_t rtos_kernel;

/* ============================== 内部静态数据 ============================== */

/** @brief 空闲任务 TCB(静态分配)。 */
static rtos_tcb_t s_idle_tcb;

/** @brief 空闲任务栈。 */
static rtos_stack_t s_idle_stack[RTOS_CONFIG_IDLE_TASK_STACK_SIZE];

/** @brief 任务 ID 分配计数器。 */
static uint32_t s_next_task_id = 1U;

#if !RTOS_CONFIG_USE_PREEMPTION
/* A cooperative yield grants one dispatch at PendSV, even if an IRQ changes
 * the candidate after the request. The permission is separate from next_tcb. */
static bool s_cooperative_yield_pending;
#endif

/* ============================== 内部函数 ============================== */

/**
 * @brief 计算最低置位 bit 索引(=最高优先级)。
 * @details 优先级约定: 0=最高, 对应 bitmap 最低位(LSB)。
 *          因此"最高优先级就绪任务"= bitmap 中最低置位 bit。
 *          利用 CTZ(Count Trailing Zeros)指令: ARM Cortex-M3/M4/M7 编译为
 *          RBIT+CLZ 两条指令(O(1), 2 周期), 直接给出最低置位 bit 索引。
 *
 *          注意: 切勿用 31-clz, 那会取最高置位 bit(=最低优先级), 方向相反。
 *          例如 bitmap=0x3 (优先级 0/1 都就绪):
 *            - ctz(0x3) = 0  (正确, 选优先级 0)
 *            - 31-clz(0x3) = 1 (错误, 选优先级 1)
 *
 * @param bitmap 就绪位图。
 * @return 最高优先级(0~MAX-1); bitmap=0 时返回 MAX_PRIORITIES(无效值)。
 */
static inline rtos_prio_t find_highest_priority(uint32_t bitmap)
{
    if (bitmap == 0U) {
        return (rtos_prio_t)RTOS_CONFIG_MAX_PRIORITIES;
    }
    /* ctz 取最低置位 bit 索引, 即最高优先级(0=LSB=最高) */
    return (rtos_prio_t)__builtin_ctz(bitmap);
}

/**
 * @brief 将任务链入指定优先级的就绪链表尾部(FIFO)。
 */
static void ready_list_push_tail(rtos_tcb_t *tcb)
{
    rtos_prio_t prio = tcb->priority;
    rtos_tcb_t *head = rtos_kernel.ready_heads[prio];

    if (head == NULL) {
        /* 链表空: 自成环 */
        tcb->next_ready = tcb;
        tcb->prev_ready = tcb;
        rtos_kernel.ready_heads[prio] = tcb;
        rtos_kernel.ready_bitmap |= (1UL << prio);
    } else {
        /* 插入到 head 之前(即尾部)，该优先级的位已经置位。 */
        rtos_tcb_t *tail = head->prev_ready;
        tcb->next_ready = head;
        tcb->prev_ready = tail;
        tail->next_ready = tcb;
        head->prev_ready = tcb;
        /* Idle 只在同级环中没有应用任务时才可成为队头。 */
        if (head == &s_idle_tcb) {
            rtos_kernel.ready_heads[prio] = tcb;
        }
    }
}

/**
 * @brief 从就绪链表中移除任务。
 */
static void ready_list_remove(rtos_tcb_t *tcb)
{
    rtos_prio_t prio = tcb->priority;
    rtos_tcb_t *head = rtos_kernel.ready_heads[prio];

    if (head == NULL) {
        return; /* 不在链表中 */
    }

    if (tcb->next_ready == tcb) {
        /* 链表仅剩此节点 */
        rtos_kernel.ready_heads[prio] = NULL;
        rtos_kernel.ready_bitmap &= ~(1UL << prio);
    } else {
        /* 跳过 tcb */
        tcb->prev_ready->next_ready = tcb->next_ready;
        tcb->next_ready->prev_ready = tcb->prev_ready;
        if (head == tcb) {
            rtos_tcb_t *next = tcb->next_ready;
            if (next == &s_idle_tcb) {
                next = next->next_ready;
            }
            rtos_kernel.ready_heads[prio] = next;
        }
    }
    tcb->next_ready = NULL;
    tcb->prev_ready = NULL;
}

/**
 * @brief 同优先级时间片用尽: 把当前任务移到链尾。
 */
static bool ready_list_rotate(rtos_prio_t prio)
{
    rtos_tcb_t *head = rtos_kernel.ready_heads[prio];
    rtos_tcb_t *next = head->next_ready;
    if (next == &s_idle_tcb) {
        next = next->next_ready;
    }
    if (next == head) {
        return false;
    }
    rtos_kernel.ready_heads[prio] = next;
    return true;
}

/* ============================== Tick 回绕安全比较 ============================== */
/* tick 为 32 位无符号, 1kHz 下约 49.7 天回绕。全部到期/排序比较必须用带符号差值
 * (模 2^32 减法取半平面), 约定单次延时/超时/周期 < 2^31 tick。
 * [bug fix] 原实现用无符号比较: 跨回绕的延时会被排到链首并在下一个 tick 立即
 * "到期"(如 delay(0xFFFFFF00)); 无符号排序在回绕点附近彻底错乱。 */
static inline int32_t sched_tick_after(rtos_tick_t a, rtos_tick_t b)
{
    return (int32_t)(a - b);
}

/**
 * @brief 将任务按 wake_tick 升序插入延时链表。
 *
 * @details 链表按 `wake_tick` 升序、双向(复用 TCB 的 `next_ready` / `prev_ready`,
 *          延时态与就绪态互斥)。插入位置需要搜索, 因此是 O(n); 摘除是 O(1)
 *          (见 delay_list_remove 的 P-5 说明)。
 *
 *          **快路径(本次新增)**: 尾部追加判定。绝大多数延时/超时是"新增一个
 *          比现有全部节点都晚的唤醒点"(周期任务、一次性 delay), 此时只需与
 *          **表尾**比较一次即可 O(1) 追加。表尾通过表头节点的 `prev_ready`
 *          取到 —— P-5 已把该字段维护成双向链的前驱, 对**表头**而言它恰好就是
 *          表尾, 因此无需额外字段。
 *
 *          为什么这条快路径重要: 本函数由 `rtos_sched_block()` 调用, 而
 *          `rtos_sched_block()` 会在 **SysTick 临界区内** 被 `process_delayed_tasks`
 *          之外的路径调用(如任务 delay / 带超时阻塞)。慢路径的线性扫描会直接
 *          拉长关中断窗口 —— 这是重负载下周期控制环偏差的主要原因。
 *
 * @param tcb 待插入任务(已设置 wake_tick)。
 */
static void delay_list_insert(rtos_tcb_t *tcb)
{
    rtos_tcb_t *head = rtos_kernel.delay_head;

    /* ---- 快路径 1: 空链 → O(1) 直接成为唯一节点 ---- */
    if (head == NULL) {
        tcb->next_ready = NULL;
        tcb->prev_ready = NULL;
        rtos_kernel.delay_head = tcb;
        return;
    }

    /* 不变式: 对延时链的**表头**而言, prev_ready 就是表尾(双向链性质)。
     * 该不变式要求链长 >= 2; 链长 == 1 时表头即表尾, prev_ready == NULL。
     * 下面显式处理单节点, 避免依赖该退化情形。 */
    rtos_tcb_t *tail = head->prev_ready;

    /* ---- 快路径 2: 单节点链 → 与唯一节点比较即可 ---- */
    if (head->next_ready == NULL) {
        tail = head;
    } else if (tail == NULL) {
        /* 双节点以上但 prev_ready 为 NULL: 旧数据或构造异常。此处按最坏情况
         * 走慢路径重算, 不静默接受错误的表尾导致链序损坏。 */
        tail = head;
        while (tail->next_ready != NULL) {
            tail = tail->next_ready;
        }
        head->prev_ready = tail; /* 顺带修复不变式 */
    }

    /* ---- 快路径 3: 新唤醒点不早于表尾 → O(1) 追加 ---- */
    if (sched_tick_after(tcb->wake_tick, tail->wake_tick) >= 0) {
        tcb->next_ready = NULL;
        tcb->prev_ready = tail;
        tail->next_ready = tcb;
        head->prev_ready = tcb; /* 维护"表头前驱 = 表尾" */
        return;
    }

    /* ---- 慢路径: 新节点早于表尾, 线性搜索插入点 ----
     *
     * 链表不变式(双向, 复用 next_ready / prev_ready):
     *   - next_ready: 指向 wake_tick 更晚的节点, 表尾为 NULL
     *   - prev_ready: 指向 wake_tick 更早的节点(表头为 NULL);
     *     **额外约定**: 表头节点的 prev_ready 被借用来缓存表尾,
     *     使追加与取表尾都是 O(1), 无需在 kernel 结构里加字段。
     *     链长 >= 2 时: 表头.prev_ready == 表尾, 且表尾.prev_ready == 真实前驱。
     *     链长 == 1 时该约定退化(表头即表尾), 由上面的快路径 2 单独处理,
     *     因此慢路径只在链长 >= 2 时到达, 约定成立。 */
    {
        rtos_tcb_t *prev = NULL;
        rtos_tcb_t *cur = head;

        while ((cur != NULL) && (sched_tick_after(cur->wake_tick, tcb->wake_tick) <= 0)) {
            prev = cur;
            cur = cur->next_ready;
        }

        tcb->next_ready = cur;

        if (prev == NULL) {
            /* 插入到表头之前: 表尾不变, 新表头接管表尾缓存 */
            tcb->prev_ready = tail;
            head->prev_ready = tcb; /* 旧表头的前驱 = 新表头 */
            rtos_kernel.delay_head = tcb;
        } else {
            tcb->prev_ready = prev;
            prev->next_ready = tcb;
            if (cur != NULL) {
                cur->prev_ready = tcb;
            } else {
                head->prev_ready = tcb; /* 新节点成为新表尾, 更新缓存 */
            }
        }
    }
}

/**
 * @brief 从延时链表移除任务。
 *
 * @details [perf P-5] 双向链表 O(1) 摘除(原单向线性搜索 O(n))。
 *          不在链上时为安全空操作(防御)。
 *
 *          额外维护"表头节点缓存表尾"这一约定(见 delay_list_insert 的说明):
 *            - 摘除表头后新表头必须接管表尾缓存;
 *            - 链只剩 1 个节点时该缓存必须清为 NULL(表头即表尾, 无独立缓存);
 *            - 摘除表尾时缓存要回退到新的表尾(通过 next == NULL 判断)。
 *          这些维护动作都是 O(1)。
 */
static void delay_list_remove(rtos_tcb_t *tcb)
{
    rtos_tcb_t *prev = tcb->prev_ready;
    rtos_tcb_t *next = tcb->next_ready;
    const rtos_bool_t is_head = (rtos_kernel.delay_head == tcb) ? RTOS_TRUE : RTOS_FALSE;

    if (is_head != RTOS_FALSE) {
        /* 摘除表头: prev 字段里存的是"表尾缓存", 不是真实前驱 */
        rtos_tcb_t *cached_tail = prev;
        rtos_kernel.delay_head = next;
        if (next != NULL) {
            if (next->next_ready == NULL) {
                /* 链只剩 1 个节点: 它既是表头又是表尾, 无独立缓存 */
                next->prev_ready = NULL;
            } else {
                next->prev_ready = cached_tail; /* 新表头接管表尾缓存 */
            }
        }
    } else if (prev != NULL) {
        prev->next_ready = next;
        if (next != NULL) {
            next->prev_ready = prev;
        } else {
            /* 摘除表尾: 表尾缓存回退到 prev; 但若 prev 就是表头, 它同时
             * 充当表头与表尾, 缓存应清为 NULL。 */
            if (prev == rtos_kernel.delay_head) {
                prev->prev_ready = NULL;
            } else {
                rtos_kernel.delay_head->prev_ready = prev;
            }
        }
    } else {
        return; /* 不在链上(防御, 调用方应保证状态一致) */
    }

    tcb->next_ready = NULL;
    tcb->prev_ready = NULL;
}

/**
 * @brief 唤醒所有到期任务。
 * @details 同时处理对象等待超时: 若到期任务正阻塞在某对象(sem/mutex/queue/event)
 *          的等待链表上，通过 wait_node.list_head 将其从该链表移除，避免悬空节点
 *          被后续 wake_highest/零拷贝直传误用(防止 use-after-free 与重复唤醒)。
 *          notify_wait 不挂入对象链表(list_head=NULL)，跳过移除。
 */
static __attribute__((noinline)) void process_delayed_tasks(rtos_tcb_t *tcb, rtos_tick_t now)
{
    do {
        /* 从延时表摘除链首。
         * 注意: 这里**不能**简单地把新链首的 prev_ready 清为 NULL ——
         * 旧链首的 prev_ready 字段承载的是"表尾缓存"(见 delay_list_insert
         * 的不变式说明), 必须转交给新链首; 只有链长降到 1 时才清空。 */
        {
            rtos_tcb_t *cached_tail = tcb->prev_ready;
            rtos_tcb_t *new_head = tcb->next_ready;

            rtos_kernel.delay_head = new_head;
            if (new_head != NULL) {
                if (new_head->next_ready == NULL) {
                    new_head->prev_ready = NULL; /* 只剩 1 个节点: 表头即表尾 */
                } else {
                    new_head->prev_ready = cached_tail; /* 接管表尾缓存 */
                }
            }
        }
        tcb->next_ready = NULL;
        tcb->prev_ready = NULL;

        /* 对象等待超时: 从对象等待链表移除，防止悬空节点 */
        if (tcb->blocked_on != NULL) {
            if (tcb->wait_node.on_timeout != NULL) {
                tcb->wait_node.on_timeout(&tcb->wait_node);
            }
            tcb->wait_node.timed_out = RTOS_TRUE;
            tcb->wait_result = RTOS_ERR_TIMEOUT;
            if (tcb->wait_node.list_head != NULL) {
                /* [缺陷修复 BM-1] 摘链时同步维护优先级位图。
                 * 原实现只摘链、不清位, 使"位图 = 存在等待者的优先级集合"
                 * 这一不变式在**每次超时**后都被破坏: 位图留有陈旧位, 而链上
                 * 已无该优先级的等待者。真机实测(FIX13 场景): 一次 5 tick 的
                 * 信号量超时后 wait_bitmap 残留 bit13 = 8192。
                 * 后果: rtos_internal_wake_highest_bm 的 ctz 会取到一个空分组,
                 * 只能靠其内部的"全链扫描兜底"保住正确性, 但每次 give 都退化
                 * 为 O(n) 扫描, 且位图不再是可信的观测值。
                 * 与 rtos_sem_deinit 等 deinit 路径里 `wait_bitmap = 0` 的
                 * 处理保持同一口径。 */
                rtos_internal_wait_remove_sync_bitmap(tcb->wait_node.list_head, &tcb->wait_node);
                tcb->wait_node.list_head = NULL;
            }
            if (tcb->wait_node.is_mutex_wait != RTOS_FALSE) {
                rtos_internal_mutex_waiter_left(tcb);
            }

            /* [缺陷修复 BI-1] 清除 blocked_on。
             * 原实现只在唤醒路径清 blocked_on, 超时路径留着不管 —— 于是任务
             * 超时回到就绪态后仍带着"上次阻塞在哪个对象上"的陈旧指针。
             * 两处危害:
             *   1. rtos_task_notify 以 `blocked_on == (void*)tcb` 作为"该任务正在
             *      notify_wait 中"的唯一判据(见 rtos_task.c), 超时后这个标记
             *      仍是哨兵值 → 之后任意一次 notify 会把一个正在普通 delay 的
             *      任务从延时表里摘出来提前唤醒(实测 delay(1000) 提前返回);
             *   2. 对 sem/queue/mutex 超时, 陈旧对象指针会让后续诊断/遍历误判。
             * notify_wait 的哨兵值与对象指针都必须在超时路径一并作废。 */
            tcb->blocked_on = NULL;
        }

        /* 进入就绪态 */
        tcb->state = RTOS_TASK_READY;
        ready_list_push_tail(tcb);
        tcb = rtos_kernel.delay_head;
    } while ((tcb != NULL) && (sched_tick_after(tcb->wake_tick, now) <= 0));
}

/* ============================== 公共 API 实现 ============================== */

rtos_status_t rtos_sched_init(void)
{
    if (rtos_kernel.idle_tcb != NULL) return RTOS_ERR_ALREADY;
    memset(&rtos_kernel, 0, sizeof(rtos_kernel));
    rtos_kernel.sched_state = RTOS_SCHED_NOT_STARTED;

    /* 创建空闲任务(最低优先级, 静态分配: idle 任务必须不依赖堆, 否则堆耗尽
     * 时无法启动 idle 回收, 死锁)。 */
    rtos_status_t st =
        rtos_task_create_static(&s_idle_tcb, s_idle_stack, RTOS_CONFIG_IDLE_TASK_STACK_SIZE,
                                rtos_idle_task, NULL, RTOS_CONFIG_MAX_PRIORITIES - 1U, "IDLE");
    if (st != RTOS_OK) {
        return st;
    }
    rtos_kernel.idle_tcb = &s_idle_tcb;
    s_next_task_id = 2U; /* 1 已给 idle */
    return RTOS_OK;
}

rtos_status_t rtos_sched_start(void)
{
    if (RTOS_PORT_IN_ISR()) {
        return RTOS_ERR_ISR;
    }
    if (rtos_kernel.sched_state != RTOS_SCHED_NOT_STARTED) {
        return RTOS_ERR_ALREADY;
    }
    /* Startup uses BASEPRI even when ordinary critical sections use PRIMASK:
     * SVC must run while SysTick/PendSV wait for the first valid PSP. */
    uint32_t previous_mask;
    const uint32_t mask = (uint32_t)RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY;
    __asm__ __volatile__("mrs %[mask], basepri" : [mask] "=r"(previous_mask) :: "memory");
    __asm__ __volatile__("msr basepri_max, %[mask]\n isb" :: [mask] "r"(mask) : "memory");

    /* 选定第一个任务(最高优先级就绪任务) */
    const rtos_prio_t top = find_highest_priority(rtos_kernel.ready_bitmap);
    rtos_tcb_t *first_tcb = (top < RTOS_CONFIG_MAX_PRIORITIES) ? rtos_kernel.ready_heads[top] : NULL;
    rtos_status_t status = RTOS_ERR_SCHED;
    if (first_tcb != NULL) {
        status = rtos_internal_port_prepare_scheduler();
        if (status == RTOS_OK) {
            first_tcb->state = RTOS_TASK_RUNNING;
            rtos_kernel.current_tcb = first_tcb;
            rtos_kernel.sched_state = RTOS_SCHED_RUNNING;
            rtos_perf_on_scheduler_start();
            rtos_port_start_first_task();
        }
    }

    __asm__ __volatile__("msr basepri, %[mask]\n isb" :: [mask] "r"(previous_mask) : "memory");
    return status;
}

static inline void schedule_ready_priority(rtos_prio_t top, rtos_bool_t is_explicit_yield)
{
    if (top >= RTOS_CONFIG_MAX_PRIORITIES) {
        return; /* 无就绪任务(理论上 idle 总在) */
    }

    rtos_tcb_t *next = rtos_kernel.ready_heads[top];

    rtos_tcb_t *current = rtos_kernel.current_tcb;
    if (next != current) {
#if !RTOS_CONFIG_USE_PREEMPTION
        /* Idle is always dispatchable; it may be waiting for the IRQ that made
         * the selected task ready. An active application task yields itself. */
        if ((is_explicit_yield == RTOS_FALSE) && (current != rtos_kernel.idle_tcb) &&
            (current->state == RTOS_TASK_RUNNING)) {
            return;
        }
        if (is_explicit_yield != RTOS_FALSE) {
            s_cooperative_yield_pending = true;
        }
#else
        (void)is_explicit_yield;
#endif
        rtos_kernel.next_tcb = next;
        rtos_kernel.switch_count++;
        rtos_port_context_switch();
    } else {
        /* Cancel an earlier choice invalidated by a wake or priority change. */
        rtos_kernel.next_tcb = NULL;
        /* A blocked current task can be woken before PendSV saves its stack.
         * It remains on the CPU, so READY must become RUNNING again here. */
        if (next != NULL && next->state == RTOS_TASK_READY) next->state = RTOS_TASK_RUNNING;
    }
}

void rtos_sched_schedule(void)
{
    if (rtos_kernel.sched_state == RTOS_SCHED_RUNNING) {
        schedule_ready_priority(find_highest_priority(rtos_kernel.ready_bitmap), RTOS_FALSE);
    }
}

void rtos_internal_sched_yield(void)
{
    rtos_tcb_t *current = rtos_kernel.current_tcb;
    const rtos_prio_t priority = current->priority;
    rtos_tcb_t *head = rtos_kernel.ready_heads[priority];

    if (head == current) {
        (void)ready_list_rotate(priority);
    } else if (current->next_ready != head) {
        /* A cooperative task can change priority without losing the CPU. */
        ready_list_remove(current);
        ready_list_push_tail(current);
    }
    schedule_ready_priority(find_highest_priority(rtos_kernel.ready_bitmap), RTOS_TRUE);
}

void rtos_sched_add_ready(rtos_tcb_t *tcb)
{
    RTOS_ASSERT(tcb != NULL);
    ready_list_push_tail(tcb);
}

void rtos_sched_remove_ready(rtos_tcb_t *tcb)
{
    RTOS_ASSERT(tcb != NULL);
    ready_list_remove(tcb);
}

void rtos_sched_block(rtos_tcb_t *tcb, rtos_tick_t ticks)
{
    RTOS_ASSERT(tcb != NULL);

    /* 若任务在就绪表，先移除 */
    if (tcb->state == RTOS_TASK_READY || tcb->state == RTOS_TASK_RUNNING) {
        ready_list_remove(tcb);
    }

    /* [bug fix BUG-1] 超过带符号差值可表示域(2^31)的延时统一钳制为永久阻塞:
     * 否则 wake_tick 回绕后带符号差为负, 任务下一个 tick 立即"到期" ——
     * delay(0xFFFFFF00) 本意睡 49.7 天, 实际 1ms 就醒。
     * [缺陷修复 BD-1] 边界取 `> 0x7FFFFFFF`: 判据 sched_tick_after 是**带符号
     * 差值**, 0x7FFFFFFF 恰在该域内(差值 > 0, 语义正确), 原式 `> 0x7FFFFFFE`
     * 把它多钳了一格 —— delay(0x7FFFFFFF)(约 24.8 天)会变成永久阻塞且永不唤醒。 */
    if ((ticks == RTOS_WAIT_FOREVER) || (ticks > 0x7FFFFFFFU)) {
        tcb->wake_tick = 0xFFFFFFFFU; /* 永久阻塞: 不入延时表 */
        tcb->state = RTOS_TASK_BLOCKED;
    } else {
        tcb->wake_tick = rtos_kernel.tick_count + ticks;
        tcb->state = RTOS_TASK_DELAYED;
        delay_list_insert(tcb);
    }
}

void rtos_sched_unblock(rtos_tcb_t *tcb)
{
    RTOS_ASSERT(tcb != NULL);

    /* 从延时表移除(若在其中) */
    if (tcb->state == RTOS_TASK_DELAYED) {
        delay_list_remove(tcb);
    }
    tcb->state = RTOS_TASK_READY;
    ready_list_push_tail(tcb);

    if (rtos_kernel.sched_state != RTOS_SCHED_RUNNING) {
        return;
    }

    /* 就绪位图刚置位。若更高优先级已经就绪，追加任务不影响其队头。
     * 复用本次选择，保留同级 FIFO 及当前任务在 PendSV 前重新就绪的处理。 */
    rtos_prio_t top = (rtos_prio_t)__builtin_ctz(rtos_kernel.ready_bitmap);
    if (top == tcb->priority) {
        schedule_ready_priority(top, RTOS_FALSE);
    }
}

void rtos_sched_remove_delayed(rtos_tcb_t *tcb)
{
    RTOS_ASSERT(tcb != NULL);
    if (tcb->state == RTOS_TASK_DELAYED) {
        delay_list_remove(tcb);
    }
}

void rtos_sched_tick(void)
{
    /* 契约: 调用方必须已进入临界区(与 rtos_sched.h 声明一致)。
     * 唯一调用方 rtos_port_sys_tick_handler 已用 ENTER/EXIT_CRITICAL
     * 包裹, 此处不再重复进入 —— 嵌套临界区虽因计数机制功能正确,
     * 但每个 Tick 多付出一对 basepri 写入 + 屏障 + 计数开销。 */
    if (rtos_kernel.sched_state != RTOS_SCHED_RUNNING) {
        return;
    }

    const rtos_tick_t now = rtos_kernel.tick_count + 1U;
    rtos_kernel.tick_count = now;

    /* The no-expiration path does not enter the due handler's stack frame. */
    rtos_tcb_t *const first = rtos_kernel.delay_head;
    if ((first != NULL) && (sched_tick_after(first->wake_tick, now) <= 0)) {
        process_delayed_tasks(first, now);
    }

#if !RTOS_CONFIG_USE_PREEMPTION
    /* Also preserves an explicit yield already queued before this tick. */
    if ((rtos_kernel.current_tcb != rtos_kernel.idle_tcb) &&
        (rtos_kernel.current_tcb->state == RTOS_TASK_RUNNING)) {
        return;
    }
#endif

    /* 单次调度决策: 选取最高优先级就绪任务。
     * 相比"先时间片轮转再查最高优先级"的旧实现，本路径只做一次
     * find_highest_priority 与至多一次 context_switch，避免冗余轮转
     * 与重复挂起 PendSV。 */
    rtos_prio_t top = find_highest_priority(rtos_kernel.ready_bitmap);
    if (top >= RTOS_CONFIG_MAX_PRIORITIES) {
        return;
    }

    rtos_tcb_t *next = rtos_kernel.ready_heads[top];
    rtos_tcb_t *cur = rtos_kernel.current_tcb;

    if (next != cur) {
        /* 有更高优先级任务就绪 -> 抢占切换 */
        rtos_kernel.next_tcb = next;
        rtos_kernel.switch_count++;
        rtos_port_context_switch();
    } else if ((cur != NULL) && (cur->state == RTOS_TASK_READY)) {
        /* Tick may complete the current task's wait before its first switch. */
        rtos_kernel.next_tcb = NULL;
        cur->state = RTOS_TASK_RUNNING;
    }
#if RTOS_CONFIG_USE_PREEMPTION && RTOS_CONFIG_USE_TIME_SLICING
    else if ((cur != NULL) && (cur->state == RTOS_TASK_RUNNING) && ready_list_rotate(top)) {
        rtos_kernel.next_tcb = rtos_kernel.ready_heads[top];
        rtos_kernel.switch_count++;
        rtos_port_context_switch();
    }
#endif
}

void rtos_sched_suspend(void)
{
    if (RTOS_PORT_IN_ISR()) return;
    RTOS_PORT_ENTER_CRITICAL();
    if (rtos_kernel.sched_state == RTOS_SCHED_RUNNING) {
        rtos_kernel.suspend_depth = 1U;
        rtos_kernel.sched_state = RTOS_SCHED_SUSPENDED;
        rtos_kernel.next_tcb = NULL;
#if !RTOS_CONFIG_USE_PREEMPTION
        s_cooperative_yield_pending = false;
#endif
    } else if (rtos_kernel.sched_state == RTOS_SCHED_SUSPENDED) {
        RTOS_ASSERT(rtos_kernel.suspend_depth != UINT32_MAX);
        if (rtos_kernel.suspend_depth != UINT32_MAX) ++rtos_kernel.suspend_depth;
    }
    RTOS_PORT_EXIT_CRITICAL();
}

void rtos_sched_resume(void)
{
    if (RTOS_PORT_IN_ISR()) return;
    RTOS_PORT_ENTER_CRITICAL();
    if (rtos_kernel.sched_state == RTOS_SCHED_SUSPENDED) {
        if (rtos_kernel.suspend_depth > 1U) {
            --rtos_kernel.suspend_depth;
        } else {
            rtos_kernel.suspend_depth = 0U;
            rtos_kernel.sched_state = RTOS_SCHED_RUNNING;
            rtos_sched_schedule();
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

rtos_tick_t rtos_sched_get_tick_count(void)
{
    return rtos_kernel.tick_count;
}

rtos_sched_state_t rtos_sched_get_state(void)
{
    return rtos_kernel.sched_state;
}

rtos_tcb_t *rtos_sched_commit_context_switch(void)
{
    rtos_tcb_t *current_task = rtos_kernel.current_tcb;

    if (rtos_kernel.sched_state != RTOS_SCHED_RUNNING) {
        rtos_kernel.next_tcb = NULL;
#if !RTOS_CONFIG_USE_PREEMPTION
        s_cooperative_yield_pending = false;
#endif
        return current_task;
    }
#if !RTOS_CONFIG_USE_PREEMPTION
    if (s_cooperative_yield_pending) {
        s_cooperative_yield_pending = false;
        const rtos_prio_t top = find_highest_priority(rtos_kernel.ready_bitmap);
        rtos_tcb_t *next = (top < RTOS_CONFIG_MAX_PRIORITIES) ? rtos_kernel.ready_heads[top] : NULL;
        rtos_kernel.next_tcb = (next != current_task) ? next : NULL;
    }
#endif
    /* 由 PendSV 汇编代码调用本函数前已保存上下文，这里只更新 current */
    if (rtos_kernel.next_tcb != NULL) {
        rtos_tcb_t *prev = current_task;
        rtos_tcb_t *next = rtos_kernel.next_tcb;
#if defined(RTOS_HIL_AUDIT_PENDSV)
        extern void audit_usb_port_pendsv_hook(void);
        audit_usb_port_pendsv_hook();
#endif

        /* 清除 next_tcb: 切换完成后置空, 防止 PendSV 意外重入时
         * 使用过期的 next_tcb 值造成二次切换。 */
        rtos_kernel.next_tcb = NULL;

        /* [bug fix RACE-1 第二层防御] next 必然来自就绪表(或就是 prev)。
         * 若断言命中, 说明存在把非就绪任务选为 next 的路径(已删/已挂起),
         * 会造成"RUNNING 但不在就绪链"的脏状态与后续链表损坏。 */
        RTOS_ASSERT((next->state == RTOS_TASK_READY) || (next == prev));

        if ((prev != NULL) && (prev->state == RTOS_TASK_RUNNING)) {
            prev->state = RTOS_TASK_READY;
        }
        next->state = RTOS_TASK_RUNNING;
        next->switch_count++;
        rtos_kernel.current_tcb = next;

#if RTOS_CONFIG_PERF_HOTPATH_STATS
        /* 性能监视器钩子: 累加 prev 运行周期, 计算调度延迟, 统计切换次数。
         * [perf P-3] 编译期开关(默认关): 全量统计每次切换 ~25 周期,
         * 关闭时切换路径零统计开销(CPU 占比/运行时长/延迟分布不可用)。 */
        rtos_perf_on_context_switch(prev, next);
#endif
        return next;
    }

    return current_task;
}

void rtos_sched_context_switch(void)
{
    /* 保留公共 void 调用约定；移植层直接使用内部入口的 TCB 返回值。 */
    (void)rtos_sched_commit_context_switch();
}

/* ============================== 分配任务 ID(内部接口) ============================== */

uint32_t rtos_internal_alloc_task_id(void)
{
    return s_next_task_id++;
}

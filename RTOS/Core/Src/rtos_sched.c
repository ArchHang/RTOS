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
#include "rtos_task.h"
#include "rtos_internal.h"
#include "rtos_perf.h"
#include <string.h>

/* ============================== 内核全局实例 ============================== */

/** @brief 唯一的内核状态实例。 */
rtos_kernel_t rtos_kernel;

/* ============================== 内部静态数据 ============================== */

/** @brief 空闲任务 TCB(静态分配)。 */
static rtos_tcb_t s_idle_tcb;

/** @brief 空闲任务栈。 */
static rtos_stack_t s_idle_stack[RTOS_CONFIG_IDLE_TASK_STACK_SIZE];

/** @brief 任务 ID 分配计数器。 */
static uint32_t s_next_task_id = 1U;

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
    } else {
        /* 插入到 head 之前(即尾部) */
        rtos_tcb_t *tail = head->prev_ready;
        tcb->next_ready = head;
        tcb->prev_ready = tail;
        tail->next_ready = tcb;
        head->prev_ready = tcb;
    }
    /* 置位就绪位图 */
    rtos_kernel.ready_bitmap |= (1UL << prio);
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
            rtos_kernel.ready_heads[prio] = tcb->next_ready;
        }
    }
    tcb->next_ready = NULL;
    tcb->prev_ready = NULL;
}

/**
 * @brief 同优先级时间片用尽: 把当前任务移到链尾。
 */
static void ready_list_rotate(rtos_prio_t prio)
{
    rtos_tcb_t *head = rtos_kernel.ready_heads[prio];
    if ((head != NULL) && (head->next_ready != head)) {
        /* 把 head 移到尾部: head = head->next */
        rtos_kernel.ready_heads[prio] = head->next_ready;
    }
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
static void process_delayed_tasks(void)
{
    rtos_tick_t now = rtos_kernel.tick_count;

    while (rtos_kernel.delay_head != NULL) {
        rtos_tcb_t *tcb = rtos_kernel.delay_head;
        if (sched_tick_after(tcb->wake_tick, now) > 0) {
            break; /* 链首未到期，后续更晚(带符号差值, 回绕安全) */
        }

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

#if RTOS_CONFIG_USE_DIAGNOSTICS
        rtos_status_t diagnostic_wake_result = tcb->blocked_on != NULL
            ? RTOS_ERR_TIMEOUT : RTOS_OK;
#endif
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
            /* 若超时任务曾阻塞在互斥锁上, 等待者减少后需重新评估持有者的继承优先级 */
            rtos_internal_mutex_waiter_left(tcb);

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
        RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, tcb,
            NULL, 0U, 0U, diagnostic_wake_result, 0U);
    }
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
    if (RTOS_PORT_IN_ISR()) return RTOS_ERR_ISR;
    if (rtos_kernel.sched_state != RTOS_SCHED_NOT_STARTED) return RTOS_ERR_ALREADY;
    /* Startup uses BASEPRI even when ordinary critical sections use PRIMASK:
     * SVC must run while SysTick/PendSV wait for the first valid PSP. */
    uint32_t previous_mask;
    __asm volatile("mrs %0, basepri" : "=r"(previous_mask) :: "memory");

    /* [bug fix RACE-2] 先屏蔽 syscall 级中断再置 RUNNING: 原实现置位后、SVC 加载
     * PSP 前存在 ~0.5µs 裸奔窗口, 若 HAL 时代的 SysTick 恰在此窗口到期, PendSV 会
     * 用未初始化的 PSP 保存上下文(启动即崩, 概率 ~0.05%)。BASEPRI=0x50 屏蔽
     * SysTick/PendSV(优先级 15), SVC(优先级 0)不受影响; SVC handler 末尾已有
     * 清 BASEPRI 逻辑, 首任务仍以全中断运行。 */
    {
        uint32_t mask = (uint32_t)RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY;
        __asm volatile("msr basepri_max, %0\n isb" : : "r"(mask) : "memory");
    }

    rtos_kernel.sched_state = RTOS_SCHED_RUNNING;

    /* 选定第一个任务(最高优先级就绪任务) */
    rtos_prio_t top = find_highest_priority(rtos_kernel.ready_bitmap);
    /* [缺陷修复 BS-1] 补上界判断, 与 rtos_sched_schedule / rtos_sched_tick 同口径。
     * find_highest_priority 在就绪位图为空时返回 MAX_PRIORITIES(=32), 而
     * ready_heads[] 只有 32 个元素 —— 原实现直接索引 ready_heads[32], 是
     * 越界读(该布局下恰好落到相邻的 delay_head 成员, 不会立刻崩, 但语义错误)。
     * 空就绪集在正常流程不可达(idle 恒就绪), 此处属加固。 */
    if (top >= RTOS_CONFIG_MAX_PRIORITIES) {
        rtos_kernel.sched_state = RTOS_SCHED_NOT_STARTED;
        __asm volatile("msr basepri, %0" : : "r"(previous_mask) : "memory");
        return RTOS_ERR_SCHED;
    }
    rtos_kernel.current_tcb = rtos_kernel.ready_heads[top];
    if (rtos_kernel.current_tcb != NULL) {
        rtos_kernel.current_tcb->state = RTOS_TASK_RUNNING;
        RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_RUN, RTOS_DIAG_OP_NONE,
            rtos_kernel.current_tcb, NULL, 0U, 0U, RTOS_OK, 0U);
    }

    /* 性能监视器: 初始化窗口基准(在启动调度器前, 中断尚未开) */
    rtos_perf_on_scheduler_start();

    /* 配置并启动 SysTick/PendSV，加载首个任务栈并切换 */
    rtos_port_start_scheduler();
    rtos_port_start_first_task();

    /* 不应到达此处 */
    while (1) {
    }
}

void rtos_sched_schedule(void)
{
    if (rtos_kernel.sched_state != RTOS_SCHED_RUNNING) return;
    /* 必须在临界区内调用 */
    rtos_prio_t top = find_highest_priority(rtos_kernel.ready_bitmap);

    if (top >= RTOS_CONFIG_MAX_PRIORITIES) {
        return; /* 无就绪任务(理论上 idle 总在) */
    }

    rtos_tcb_t *next = rtos_kernel.ready_heads[top];

    if (next != rtos_kernel.current_tcb) {
#if RTOS_CONFIG_USE_PREEMPTION
        rtos_kernel.next_tcb = next;
        rtos_kernel.switch_count++;
        rtos_port_context_switch();
#else
        /* 非抢占: 仅在当前任务主动让出时切换，由 yield 显式调用 */
        if (rtos_kernel.current_tcb->state != RTOS_TASK_RUNNING) {
            rtos_kernel.next_tcb = next;
            rtos_kernel.switch_count++;
            rtos_port_context_switch();
        }
#endif
    } else {
        /* Cancel an earlier choice invalidated by a wake or priority change. */
        rtos_kernel.next_tcb = NULL;
        /* A blocked current task can be woken before PendSV saves its stack.
         * It remains on the CPU, so READY must become RUNNING again here. */
        if (next != NULL && next->state == RTOS_TASK_READY) {
            next->state = RTOS_TASK_RUNNING;
            RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_RUN, RTOS_DIAG_OP_NONE, next,
                NULL, 0U, 0U, RTOS_OK, 0U);
        }
    }
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
    RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, tcb,
        NULL, 0U, 0U, tcb->wait_result, 0U);

    if (rtos_kernel.sched_state != RTOS_SCHED_RUNNING) {
        return;
    }

    /* 是否需要抢占?
     *
     * [perf G1] 原实现无条件比较 `tcb->priority < current_tcb->priority`,
     * 只要成立就调用 rtos_sched_schedule() —— 而后者会做一次**完整位图选择**
     * (RBIT+CLZ)。问题是"被唤醒者比当前任务优先级高"并不等于"需要切换":
     * 就绪集中可能还有一个**比被唤醒者更高**的任务已经就绪(它在阻塞期间被
     * 唤醒、或被 tick 唤醒)。此时位图选择等于白做。
     *
     * 判据改为直接比较**被唤醒者优先级**与**就绪集中的最高优先级**:
     *   - 就绪位图刚在 ready_list_push_tail 里置位, 位 p 已包含本任务;
     *   - 因此若 ctz(bitmap) == tcb->priority, 则被唤醒者就是新的最高优先级
     *     就绪者, 必然需要切换 → 调 sched_schedule();
     *   - 否则就绪集中已有更高优先级任务, 它要么正在运行(RUNNING), 要么
     *     早前已被选中为 next_tcb —— 两种情况下本次唤醒都不改变"谁该运行",
     *     **无需重算**, 直接返回。
     *
     * 正确性依据: 本函数在临界区内、由 IPC 的 give/set 路径调用。被唤醒者
     * 入链后, 若它不是最高就绪优先级, 则最高就绪优先级在本次调用前后不变,
     * 故"当前正在运行的任务是否仍是最该运行的任务"这一结论也不变。
     *
     * 实测收益(见 OPT_LOG Round 4): 唤醒路径净开销 947 → 见下轮数据。 */
    if (__builtin_ctz(rtos_kernel.ready_bitmap) == (int)tcb->priority) {
        rtos_sched_schedule();
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

    rtos_kernel.tick_count++;

    /* 唤醒到期任务(可能将更高优先级任务放入就绪表) */
    process_delayed_tasks();

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
    }
#if RTOS_CONFIG_USE_TIME_SLICING
    else if ((cur != NULL) && (cur->state == RTOS_TASK_RUNNING)) {
        /* 同优先级时间片轮转: 当前链表多于一个任务时把 head 移到链尾 */
        rtos_tcb_t *head = rtos_kernel.ready_heads[top];
        if ((head != NULL) && (head->next_ready != head)) {
            ready_list_rotate(top);
            rtos_kernel.next_tcb = rtos_kernel.ready_heads[top];
            rtos_kernel.switch_count++;
            rtos_port_context_switch();
        }
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
        return current_task;
    }
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
        RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_RUN, RTOS_DIAG_OP_NONE, next,
            NULL, 0U, 0U, RTOS_OK, 0U);

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
#if RTOS_CONFIG_USE_DIAGNOSTICS
    if ((s_next_task_id == 0U) || (s_next_task_id == UINT32_MAX)) {
        RTOS_DIAG_IDENTITY_EXHAUSTED();
    }
#endif
    return s_next_task_id++;
}

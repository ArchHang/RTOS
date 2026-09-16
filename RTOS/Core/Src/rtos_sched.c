/**
 * @file    rtos_sched.c
 * @brief   调度器与内核全局状态实现
 *
 * @details 实现基于优先级位图的抢占式调度器。核心算法:
 *
 *          1. 就绪表: 一个 32 位 bitmap(每个 bit 对应一个优先级) + 32 条
 *             双向链表。加入就绪: 置位 bitmap[prio], 链入 ready_heads[prio]。
 *          2. 选最高优先级: 用 __builtin_clz 计算 bitmap 的前导零个数,
 *             得到最高置位 bit 索引(Cortex-M7 的 CLZ 指令 1 周期完成)。
 *          3. 时间片: 同优先级链表按 FIFO，每次 Tick 把当前任务移到链尾，
 *             链头变新当前任务。
 *          4. 延时表: 单链表按 wake_tick 升序，Tick 中断时检查链首。
 *
 *          临界区策略: 进入临界区时通过 BASEPRI 屏蔽 RTOS 系统调用级中断，
 *          保证调度器数据结构操作原子性，同时不阻断硬实时中断。
 */
#include "rtos_sched.h"
#include "rtos_task.h"
#include "rtos_internal.h"
#include "rtos_perf.h"
#include "rtos_heap.h"
#include "rtos_timer.h" /* rtos_timer_init (内核初始化时启动定时器服务) */
#include "rtos.h" /* RTOS_VERSION_STRING (rtos_get_version 用) */
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
 * @details 延时链表为简单单向链表，复用 TCB 的 next_ready 字段串接
 *          (此字段仅在任务处于就绪态时才作双向链表用，延时态时单链)。
 *          插入复杂度 O(n)，任务数有限，可接受。
 *          优化: 可使用差分时间轮，本实现为简洁性使用线性链表。
 *
 * @param tcb 待插入任务(已设置 wake_tick)。
 */
static void delay_list_insert(rtos_tcb_t *tcb)
{
    rtos_tcb_t *prev = NULL;
    rtos_tcb_t *cur = rtos_kernel.delay_head;

    /* 找到首个 wake_tick > tcb->wake_tick 的节点，tcb 插在其前(带符号差值, 回绕安全) */
    while ((cur != NULL) && (sched_tick_after(cur->wake_tick, tcb->wake_tick) <= 0)) {
        prev = cur;
        cur = cur->next_ready;
    }

    /* [perf P-5] 双向链表(复用 prev_ready 字段, 延时态与就绪态互斥):
     * 摘除从 O(n) 降为 O(1) —— give 唤醒带超时等待者(unblock 路径)不再
     * 需要线性搜索延时表前驱, 长延时链 + 高频 IPC 场景下唤醒路径恒定。 */
    tcb->next_ready = cur;
    tcb->prev_ready = prev;
    if (prev == NULL) {
        /* 插入表头(含原链表为空的情况: cur==NULL 时 tcb->next_ready 设为 NULL) */
        rtos_kernel.delay_head = tcb;
    } else {
        prev->next_ready = tcb;
    }
    if (cur != NULL) {
        cur->prev_ready = tcb;
    }
}

/**
 * @brief 从延时链表移除任务。
 * @details [perf P-5] 双向链表 O(1) 摘除(原单向线性搜索 O(n))。
 *          不在链上时为安全空操作(防御)。
 */
static void delay_list_remove(rtos_tcb_t *tcb)
{
    rtos_tcb_t *prev = tcb->prev_ready;
    rtos_tcb_t *next = tcb->next_ready;

    if (prev != NULL) {
        prev->next_ready = next;
    } else if (rtos_kernel.delay_head == tcb) {
        rtos_kernel.delay_head = next;
    } else {
        return; /* 不在链上(防御, 调用方应保证状态一致) */
    }
    if (next != NULL) {
        next->prev_ready = prev;
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

        /* 从延时表移除([perf P-5] 双向链: 同步清新链首的 prev) */
        rtos_kernel.delay_head = tcb->next_ready;
        if (rtos_kernel.delay_head != NULL) {
            rtos_kernel.delay_head->prev_ready = NULL;
        }
        tcb->next_ready = NULL;
        tcb->prev_ready = NULL;

        /* 对象等待超时: 从对象等待链表移除，防止悬空节点 */
        if (tcb->blocked_on != NULL) {
            tcb->wait_node.timed_out = RTOS_TRUE;
            tcb->wait_result = RTOS_ERR_TIMEOUT;
            if (tcb->wait_node.list_head != NULL) {
                rtos_internal_wait_remove(tcb->wait_node.list_head, &tcb->wait_node);
                tcb->wait_node.list_head = NULL;
            }
            /* 若超时任务曾阻塞在互斥锁上, 等待者减少后需重新评估持有者的继承优先级 */
            rtos_internal_mutex_waiter_left(tcb);
        }

        /* 进入就绪态 */
        tcb->state = RTOS_TASK_READY;
        ready_list_push_tail(tcb);
    }
}

/* ============================== 公共 API 实现 ============================== */

rtos_status_t rtos_sched_init(void)
{
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
    RTOS_ASSERT(rtos_kernel.sched_state == RTOS_SCHED_NOT_STARTED);

    /* [bug fix RACE-2] 先屏蔽 syscall 级中断再置 RUNNING: 原实现置位后、SVC 加载
     * PSP 前存在 ~0.5µs 裸奔窗口, 若 HAL 时代的 SysTick 恰在此窗口到期, PendSV 会
     * 用未初始化的 PSP 保存上下文(启动即崩, 概率 ~0.05%)。BASEPRI=0x50 屏蔽
     * SysTick/PendSV(优先级 15), SVC(优先级 0)不受影响; SVC handler 末尾已有
     * 清 BASEPRI 逻辑, 首任务仍以全中断运行。 */
    {
        uint32_t mask = (uint32_t)RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY;
        __asm volatile("msr basepri, %0" : : "r"(mask) : "memory");
    }

    rtos_kernel.sched_state = RTOS_SCHED_RUNNING;

    /* 选定第一个任务(最高优先级就绪任务) */
    rtos_prio_t top = find_highest_priority(rtos_kernel.ready_bitmap);
    rtos_kernel.current_tcb = rtos_kernel.ready_heads[top];
    if (rtos_kernel.current_tcb != NULL) {
        rtos_kernel.current_tcb->state = RTOS_TASK_RUNNING;
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

    if (ticks == RTOS_WAIT_FOREVER) {
        /* 永久阻塞: 不入延时表 */
        tcb->wake_tick = 0xFFFFFFFFU;
        tcb->state = RTOS_TASK_BLOCKED;
    } else {
        /* [bug fix BUG-1] 超过带符号差值可表示域(2^31)的延时统一钳制为
         * 永久阻塞: 否则 wake_tick 回绕后带符号差为负, 任务下一个 tick 立即
         * "到期" —— delay(0xFFFFFF00) 本意睡 49.7 天, 实际 1ms 就醒。 */
        if (ticks > 0x7FFFFFFEU) {
            ticks = RTOS_WAIT_FOREVER;
            tcb->wake_tick = 0xFFFFFFFFU;
            tcb->state = RTOS_TASK_BLOCKED;
        } else {
            tcb->wake_tick = rtos_kernel.tick_count + ticks;
            tcb->state = RTOS_TASK_DELAYED;
            delay_list_insert(tcb);
        }
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

    /* 若被唤醒任务优先级高于当前，触发切换 */
    if ((rtos_kernel.sched_state == RTOS_SCHED_RUNNING) &&
        (tcb->priority < rtos_kernel.current_tcb->priority)) {
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
    RTOS_PORT_ENTER_CRITICAL();
    rtos_kernel.sched_state = RTOS_SCHED_SUSPENDED;
    RTOS_PORT_EXIT_CRITICAL();
}

void rtos_sched_resume(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    rtos_kernel.sched_state = RTOS_SCHED_RUNNING;
    /* 挂起期间可能积累待处理切换 */
    rtos_sched_schedule();
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

void rtos_sched_context_switch(void)
{
    /* 由 PendSV 汇编代码调用本函数前已保存上下文，这里只更新 current */
    if (rtos_kernel.next_tcb != NULL) {
        rtos_tcb_t *prev = rtos_kernel.current_tcb;
        rtos_tcb_t *next = rtos_kernel.next_tcb;

        /* 清除 next_tcb: 切换完成后置空, 防止 PendSV 意外重入时
         * 使用过期的 next_tcb 值造成二次切换。 */
        rtos_kernel.next_tcb = NULL;

        /* [bug fix RACE-1 第二层防御] next 必然来自就绪表(或就是 prev)。
         * 若断言命中, 说明存在把非就绪任务选为 next 的路径(已删/已挂起),
         * 会造成"RUNNING 但不在就绪链"的脏状态与后续链表损坏。 */
        RTOS_ASSERT((next->state == RTOS_TASK_READY) || (next == prev));

        if (prev != NULL && prev->state == RTOS_TASK_RUNNING) {
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
    }
}

void rtos_sched_start_first_task(void)
{
    /* 移植层在 rtos_port_start_first_task 中加载栈并异常返回 */
    /* 此函数仅作为调度层入口点，实际工作在 port 层完成 */
}

/* ============================== 内核入口 ============================== */

rtos_status_t rtos_init(void)
{
    /* 性能监视器初始化必须在调度器之前:
     * - 使能 DWT 周期计数器
     * - 清零任务/内存池注册表(s_task_count / s_pool_count)
     *   sched_init 会创建 idle 任务并触发 on_task_created 钩子注册进表,
     *   若 perf_init 在其后调用会把 idle 从注册表清掉。 */
    rtos_status_t st = rtos_perf_init();
    if (st != RTOS_OK) {
        return st;
    }

    /* 初始化动态堆(供 rtos_task_create 等使用)。
     * 必须在 sched_init 之前, 因为 sched_init 内部不依赖堆(idle 用静态分配),
     * 但后续用户代码会用到。 */
    st = rtos_heap_init();
    if (st != RTOS_OK) {
        return st;
    }

    /* 初始化调度器(含空闲任务) */
    st = rtos_sched_init();
    if (st != RTOS_OK) {
        return st;
    }

#if RTOS_CONFIG_USE_TIMERS
    st = rtos_timer_init();
    if (st != RTOS_OK) {
        return st;
    }
#endif
    return RTOS_OK;
}

rtos_status_t rtos_start(void)
{
    return rtos_sched_start();
}

const char *rtos_get_version(void)
{
    return RTOS_VERSION_STRING;
}

/* ============================== 分配任务 ID(内部接口) ============================== */

uint32_t rtos_internal_alloc_task_id(void)
{
    return s_next_task_id++;
}

/* ============================== 钩子弱定义 ============================== */

/** @brief 空闲钩子弱定义，用户可覆盖。 */
__attribute__((weak)) void rtos_idle_hook(void)
{
    /* 默认空实现: 进入低功耗(WFI) */
    __asm volatile("wfi");
}

/** @brief 栈溢出钩子弱定义。 */
__attribute__((weak)) void rtos_stack_overflow_hook(rtos_tcb_t *tcb)
{
    (void)tcb;
    while (1) {
    } /* 默认死循环，用户应实现复位或日志 */
}

/** @brief 任务删除钩子弱定义(仅在 RTOS_CONFIG_USE_DELETE_HOOK=1 时调用)。 */
__attribute__((weak)) void rtos_task_delete_hook(rtos_tcb_t *tcb)
{
    (void)tcb;
}

/** @brief 断言失败弱定义。 */
__attribute__((weak)) void rtos_assert_fail(const char *cond, const char *file, int line)
{
    (void)cond;
    (void)file;
    (void)line;
    while (1) {
    }
}

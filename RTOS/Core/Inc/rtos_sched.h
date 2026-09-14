/**
 * @file    rtos_sched.h
 * @brief   调度器与内核全局状态接口
 *
 * @details 实现基于优先级位图的抢占式调度器。内核维护:
 *            - 就绪位图: 每个优先级对应一位，置位表示该优先级有就绪任务。
 *            - 就绪链表数组: 每个优先级一条双向链表，同优先级任务 FIFO。
 *            - 延时链表: 按唤醒时刻排序，每个 Tick 处理一次。
 *          调度时通过 CLZ(前导零计数)指令在 1 个周期内找到最高优先级位。
 *
 *          Cortex-M3/M4/M7 支持 CLZ 硬件指令，调度算法 O(1)。
 */
#ifndef RTOS_SCHED_H_
#define RTOS_SCHED_H_

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_types.h"
#include "rtos_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== 调度器全局状态 ============================== */

/** @brief 调度器状态。 */
typedef enum {
    RTOS_SCHED_NOT_STARTED = 0, /**< 调度器未启动(内核初始化前)。 */
    RTOS_SCHED_RUNNING = 1, /**< 调度器运行中。 */
    RTOS_SCHED_SUSPENDED = 2, /**< 调度器被挂起(临界区内)。 */
    RTOS_SCHED_STOPPED = 3, /**< 调度器已停止。 */
} rtos_sched_state_t;

/**
 * @struct rtos_kernel_t
 * @brief  内核全局状态。整个内核只有一份实例(rtos_kernel)。
 */
typedef struct rtos_kernel {
    volatile rtos_sched_state_t sched_state; /**< 调度器状态。 */
    volatile rtos_tick_t tick_count; /**< 全局 Tick 计数。 */
    volatile uint32_t schedule_pending; /**< 待调度标志(临界区退出时检查)。 */

    /* 就绪表: 位图 + 每优先级链表头 */
    volatile uint32_t ready_bitmap; /**< 就绪位图(32 位)。 */
    rtos_tcb_t *ready_heads[RTOS_CONFIG_MAX_PRIORITIES]; /**< 各优先级就绪链表头。 */

    /* 延时/超时链表(按 wake_tick 升序) */
    rtos_tcb_t *delay_head; /**< 延时链表头。 */

    /* 当前与下一个任务 */
    rtos_tcb_t *current_tcb; /**< 正在运行的任务。 */
    rtos_tcb_t *next_tcb; /**< 调度选出的下一任务。 */

    /* 空闲任务 TCB 指针 */
    rtos_tcb_t *idle_tcb; /**< 空闲任务(优先级最低)。 */

    /* 统计 */
#if RTOS_CONFIG_GENERATE_RUN_TIME_STATS
    volatile uint32_t idle_run_count; /**< 空闲累计运行计数。 */
    volatile uint32_t total_run_count; /**< 总运行计数。 */
#endif
    volatile uint32_t switch_count; /**< 上下文切换计数。 */

    /* 临界区嵌套计数 */
    volatile uint32_t critical_nesting;

#if RTOS_CONFIG_USE_PERF_MONITOR
    /* ---- 性能监视器字段(条件编译, 关闭时零开销) ---- */
    volatile uint32_t perf_last_switch_cycle; /**< 上次切换的周期计数。 */
    volatile uint32_t perf_pend_cycle; /**< PendSV 挂起时刻周期数。 */
    volatile uint32_t perf_switch_total; /**< 累计切换次数(独立于 switch_count)。 */
    volatile uint32_t perf_latency_max; /**< 最大调度延迟(周期数)。 */
    volatile uint32_t perf_latency_sum; /**< 调度延迟累计(周期数, 求平均用)。 */
    volatile uint32_t perf_latency_count; /**< 调度延迟采样数。 */
    volatile uint32_t perf_idle_cycles; /**< 空闲任务累计运行周期数。 */
    volatile uint32_t perf_total_cycles; /**< 全部任务累计运行周期数。 */
    /* 窗口基准(每次 get_system 查询时更新) */
    volatile uint32_t perf_window_switch_base; /**< 窗口起始切换计数。 */
    volatile uint32_t perf_window_cycle_base; /**< 窗口起始周期计数。 */
    volatile uint32_t perf_window_idle_base; /**< 窗口起始空闲周期数。 */
    volatile uint32_t perf_window_tick_base; /**< 窗口起始 tick 计数。 */
    volatile uint32_t perf_window_latency_sum; /**< 窗口起始延迟累计。 */
    volatile uint32_t perf_window_latency_cnt; /**< 窗口起始延迟采样数。 */
    /* 快照(供 ISR 安全的快速查询) */
    volatile uint32_t perf_cpu_usage_x100; /**< 上次窗口 CPU 使用率快照。 */
    volatile uint32_t perf_switch_rate; /**< 上次窗口切换频率快照。 */
    volatile uint32_t perf_latency_avg_us; /**< 上次窗口平均延迟(us)。 */
    volatile uint32_t perf_latency_max_us; /**< 上次窗口最大延迟(us)。 */
#endif
} rtos_kernel_t;

/** @brief 内核全局状态实例(定义于 rtos_sched.c)。 */
extern rtos_kernel_t rtos_kernel;

/* ============================== 调度器内部 API ============================== */

/**
 * @brief 初始化调度器(清空就绪表/延时表，建立空闲任务)。
 * @return RTOS_OK 或错误。
 */
rtos_status_t rtos_sched_init(void);

/**
 * @brief 启动调度器(不再返回)。
 * @details 创建空闲任务 -> 配置 SysTick/PendSV -> 启动首个任务。
 */
rtos_status_t rtos_sched_start(void) __attribute__((noreturn));

/**
 * @brief 调度器主逻辑: 从就绪表中选取最高优先级任务，必要时触发切换。
 * @details 必须在临界区内调用。若选出任务 != 当前任务，置 next_tcb 并请求 PendSV。
 */
void rtos_sched_schedule(void);

/**
 * @brief 将任务加入就绪表(指定优先级)。
 */
void rtos_sched_add_ready(rtos_tcb_t *tcb);

/**
 * @brief 将任务从就绪表移除。
 */
void rtos_sched_remove_ready(rtos_tcb_t *tcb);

/**
 * @brief 将任务挂入延时链表(按唤醒时刻排序)。
 * @param ticks 延时 Tick 数。
 */
void rtos_sched_block(rtos_tcb_t *tcb, rtos_tick_t ticks);

/**
 * @brief 将任务从延时链表移除并放入就绪表。
 */
void rtos_sched_unblock(rtos_tcb_t *tcb);

/**
 * @brief 仅将任务从延时链表移除(不放入就绪表, 不触发调度)。
 * @details 供任务删除等路径使用: 避免 unblock 将任务加入就绪表并在其优先级
 *          更高时触发调度, 使 next_tcb 指向即将被释放的 TCB(use-after-free)。
 */
void rtos_sched_remove_delayed(rtos_tcb_t *tcb);

/**
 * @brief Tick 中断处理: 推进 Tick 计数、唤醒到期任务、检查时间片。
 * @details 必须在临界区内调用。完成处理后若有就绪任务优先级高于当前，
 *          触发上下文切换。
 */
void rtos_sched_tick(void);

/**
 * @brief 挂起调度器(暂停 Tick 与抢占，仍响应中断)。
 *        用于 ISR 内或临界长流程中防止任务切换。
 */
void rtos_sched_suspend(void);

/** @brief 恢复调度器。 */
void rtos_sched_resume(void);

/** @brief 获取自启动以来的 Tick 数。 */
rtos_tick_t rtos_sched_get_tick_count(void);

/** @brief 获取调度器状态。 */
rtos_sched_state_t rtos_sched_get_state(void);

/** @brief 顶层上下文切换入口(由 PendSV 调用)。 */
void rtos_sched_context_switch(void);

/** @brief 启动后选定第一个任务运行。 */
void rtos_sched_start_first_task(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_SCHED_H_ */

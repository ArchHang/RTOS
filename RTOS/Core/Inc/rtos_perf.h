/**
 * @file    rtos_perf.h
 * @brief   性能监视器接口
 *
 * @details 本模块提供类似计算机系统监视器的功能, 实时采集与查询 RTOS 关键
 *          性能指标, 设计原则:
 *
 *            1. 安全第一: 所有查询接口校验指针, 关键多字段读取在临界区内完成,
 *               不动态分配, 不暴露内核指针, 数组由调用方提供(无内部缓冲溢出)。
 *            2. 最小侵入: 热路径(上下文切换/Tick)只做"读计数器+累加"两三条
 *               指令; 重计算(CPU%/延迟统计)延迟到查询时进行。
 *            3. 零开销裁剪: RTOS_CONFIG_USE_PERF_MONITOR=0 时, 全部 API 与钩子
 *               编译为空, 不增加任何代码或 RAM。
 *            4. 精度可选: Cortex-M3/M4/M7 使用 DWT_CYCCNT(周期级, 推荐);
 *               M0/M0+ 回退到 Tick 计数(1ms 级)。
 *
 *          采集的指标:
 *            - CPU 使用率(总体, 单核)               → rtos_perf_get_cpu_usage
 *            - 各任务 CPU 占用与累计运行时间         → rtos_perf_get_task
 *            - 内存使用率(任务池 + 已注册内存池)     → rtos_perf_get_memory
 *            - 任务切换频率(次/秒)                  → rtos_perf_get_switch_rate
 *            - 调度延迟(平均/最大, PendSV 派发延迟)  → rtos_perf_get_system
 *            - 栈使用量(当前/高水位)                → rtos_perf_get_task
 *            - 系统运行时长(Uptime)                → rtos_perf_get_system
 *
 *          测量原理:
 *            - 运行时间: 每次 PendSV 上下文切换时, 累加 prev 任务的运行周期数
 *              (now_cycle - last_switch_cycle)。32 位 DWT_CYCCNT @168MHz 约 25s
 *              回绕一次, 无符号减法天然处理单次回绕(系统不可能 25s 无切换)。
 *            - CPU 占用: 查询时取窗口[上次查询, 本次查询]内的总周期与空闲周期,
 *              cpu% = (total - idle) / total * 10000。窗口由查询节奏决定。
 *            - 调度延迟: rtos_port_context_switch 置 PENDSVSET 时记录周期,
 *              PendSV 实际执行 rtos_sched_context_switch 时计算差值, 更新
 *              max/sum/count。反映"高优先级中断唤醒任务后到任务实际运行"的延迟。
 *            - 切换频率: 查询窗口内总切换数 / 窗口秒数。
 *
 * @note    查询接口仅可在任务上下文调用(内部使用临界区)。快速接口
 *          rtos_perf_get_cpu_usage / rtos_perf_get_switch_rate 可在 ISR 调用,
 *          但返回的是上次窗口快照值。
 */
#ifndef RTOS_PERF_H_
#define RTOS_PERF_H_

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_types.h"
#include "rtos_mem.h" /* rtos_mempool_t(用于 rtos_perf_register_pool 声明) */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== 公共数据结构 ============================== */

/**
 * @brief 系统级性能快照。
 * @details 所有 *_x100 字段为定点百分比, 范围 0~10000 表示 0.00%~100.00%。
 *          所有 *_us 字段单位为微秒。所有 *_x100 可用 /100 得到整数百分比。
 */
typedef struct {
    uint32_t cpu_usage_x100; /**< CPU 使用率, 0~10000。 */
    uint32_t idle_usage_x100; /**< 空闲率, 0~10000 (=10000 - cpu_usage)。 */
    uint32_t uptime_ticks; /**< 自启动以来的 Tick 数。 */
    uint32_t uptime_ms; /**< 自启动以来的毫秒数(= ticks * 1000 / TICK_RATE)。 */
    uint32_t total_switches; /**< 累计上下文切换次数(自启动)。 */
    uint32_t switch_rate; /**< 切换频率(次/秒, 基于上次查询窗口)。 */
    uint32_t sched_latency_avg_us; /**< 平均调度延迟(微秒)。 */
    uint32_t sched_latency_max_us; /**< 最大调度延迟(微秒)。 */
    uint32_t sched_latency_count; /**< 调度延迟采样数(= 窗口内切换数)。 */
    uint32_t task_count; /**< 已注册任务数。 */
    uint32_t tick_rate_hz; /**< 配置的 Tick 频率。 */
    uint32_t cpu_freq_hz; /**< CPU 主频(SystemCoreClock 快照)。 */
} rtos_perf_system_t;

/**
 * @brief 单任务性能统计。
 * @details run_time_us 为累计运行时间(微秒), 自任务创建起累加。
 *          stack 高水位仅在 RTOS_CONFIG_PERF_STACK_WATERMARK=1 时有效,
 *          否则等于当前栈指针位置(瞬时使用量)。
 */
typedef struct {
    uint32_t task_id; /**< 任务 ID。 */
    char name[RTOS_CONFIG_MAX_TASK_NAME_LEN]; /**< 任务名。 */
    rtos_prio_t priority; /**< 当前优先级。 */
    rtos_task_state_t state; /**< 当前状态。 */
    uint32_t cpu_usage_x100; /**< 该任务 CPU 占用率, 0~10000(窗口内)。 */
    uint32_t run_cycles; /**< 累计运行周期数(DWT 模式)或 Tick 数(回退)。 */
    uint32_t run_time_us; /**< 累计运行时间(微秒)。 */
    uint32_t switch_count; /**< 被调度次数(累计)。 */
    uint32_t stack_total_bytes; /**< 栈总容量(字节)。 */
    uint32_t stack_used_bytes; /**< 栈已用(字节, 高水位或瞬时)。 */
    uint32_t stack_free_bytes; /**< 栈剩余(字节)。 */
} rtos_perf_task_t;

/**
 * @brief 内存使用统计。
 * @details 包含任务对象池使用情况与用户注册的内存池聚合统计。
 */
typedef struct {
    uint32_t task_pool_capacity; /**< 任务池容量(= RTOS_CONFIG_MAX_TASKS)。 */
    uint32_t task_pool_used; /**< 任务池已用(已注册任务数)。 */
    uint32_t pool_total_bytes; /**< 已注册内存池总容量(字节)。 */
    uint32_t pool_used_bytes; /**< 已注册内存池已用(字节)。 */
    uint32_t pool_free_bytes; /**< 已注册内存池空闲(字节)。 */
    uint32_t pool_usage_x100; /**< 内存池使用率, 0~10000。 */
    uint32_t registered_pools; /**< 已注册内存池数。 */
} rtos_perf_memory_t;

/* ============================== 公共 API ============================== */

/**
 * @brief 初始化性能监视器。
 * @details 在 rtos_init() 中自动调用, 用户无需手动调用。使能 DWT, 清零计数器。
 * @return RTOS_OK 或错误码。
 */
rtos_status_t rtos_perf_init(void);

/**
 * @brief 重置所有性能计数器(开始新的测量窗口)。
 * @details 不清除累计值(run_cycles/switch_count), 只重置窗口基准。
 *          累计值在任务创建时从 0 开始, 贯穿任务生命周期。
 * @note  可在任务上下文调用。
 */
void rtos_perf_reset(void);

/**
 * @brief 获取系统级性能快照。
 * @param stats 输出统计结构(不可为 NULL, 由调用方分配)。
 * @return RTOS_OK / RTOS_ERR_NULL。
 * @note  仅可在任务上下文调用(内部临界区)。
 *        调用本函数会推进测量窗口(本次查询成为下次窗口基准)。
 */
rtos_status_t rtos_perf_get_system(rtos_perf_system_t *stats);

/**
 * @brief 获取内存使用统计。
 * @param stats 输出统计结构(不可为 NULL)。
 * @return RTOS_OK / RTOS_ERR_NULL。
 */
rtos_status_t rtos_perf_get_memory(rtos_perf_memory_t *stats);

/**
 * @brief 获取单个任务的性能统计。
 * @param tcb   任务 TCB(NULL 表示当前任务)。
 * @param stats 输出统计结构(不可为 NULL)。
 * @return RTOS_OK / RTOS_ERR_NULL / RTOS_ERR_PARAM。
 * @note  内部临界区读取, 保证多字段一致。
 */
rtos_status_t rtos_perf_get_task(rtos_tcb_t *tcb, rtos_perf_task_t *stats);

/**
 * @brief 获取所有已注册任务的性能统计。
 * @param stats_array 输出数组(由调用方分配, 容量 max_count)。
 * @param max_count   数组容量(避免溢出)。
 * @param actual_count 实际写入的条数(输出, 可为 NULL)。
 * @return RTOS_OK / RTOS_ERR_NULL / RTOS_ERR_PARAM(max_count=0)。
 * @note  遍历任务注册表, 每个任务在临界区内快照。
 */
rtos_status_t rtos_perf_get_all_tasks(rtos_perf_task_t *stats_array, uint32_t max_count,
                                      uint32_t *actual_count);

/**
 * @brief 快速获取 CPU 使用率(0~10000)。
 * @return 上次窗口的 CPU 使用率, 0~10000。
 * @note  ISR 安全(仅读快照)。值在每次 rtos_perf_get_system 调用时更新。
 */
uint32_t rtos_perf_get_cpu_usage(void);

/**
 * @brief 快速获取切换频率(次/秒)。
 * @return 上次窗口的切换频率。
 * @note  ISR 安全(仅读快照)。
 */
uint32_t rtos_perf_get_switch_rate(void);

/* ============================== 内部接口(供内核钩子调用, 用户不直接使用) ============================== */

#if RTOS_CONFIG_USE_PERF_MONITOR

/** @brief 读取高精度周期计数器。
 *         DWT 模式: 返回 DWT_CYCCNT(32 位自由运行, 每时钟周期 +1)。
 *         回退模式: 返回 rtos_kernel.tick_count。
 *         无符号减法天然处理 32 位单次回绕。 */
uint32_t rtos_perf_get_cycles(void);

/** @brief 初始化周期计数器(使能 DWT_CYCCNT)。在 rtos_perf_init 中调用。 */
void rtos_perf_init_cycles(void);

/** @brief 将周期数转换为微秒(基于 SystemCoreClock)。 */
uint32_t rtos_perf_cycles_to_us(uint32_t cycles);

/** @brief 上下文切换钩子(在 rtos_sched_context_switch 中调用, 临界区内)。
 *         累加 prev 任务运行周期, 计算调度延迟, 更新切换计数。 */
void rtos_perf_on_context_switch(rtos_tcb_t *prev, rtos_tcb_t *next);

/** @brief PendSV 触发钩子(在 rtos_port_context_switch 中调用)。
 *         记录 PendSV 挂起时刻, 供调度延迟计算。 */
void rtos_perf_on_pend_switch(void);

/** @brief 任务创建钩子: 加入注册表, 填充栈水印(若启用)。 */
void rtos_perf_on_task_created(rtos_tcb_t *tcb);

/** @brief 任务删除钩子: 从注册表移除。 */
void rtos_perf_on_task_deleted(rtos_tcb_t *tcb);

/** @brief 内存池注册(在 rtos_mempool_init 中自动调用)。 */
void rtos_perf_register_pool(rtos_mempool_t *pool);

/** @brief 内核启动后首次切换钩子: 初始化窗口基准。 */
void rtos_perf_on_scheduler_start(void);

#endif /* RTOS_CONFIG_USE_PERF_MONITOR */

/* ============================== 性能监视器关闭时的零开销桩 ============================== */

#if !RTOS_CONFIG_USE_PERF_MONITOR
#define rtos_perf_init() ((rtos_status_t)RTOS_OK)
#define rtos_perf_reset() ((void)0)
#define rtos_perf_get_system(s) ((void)(s), (rtos_status_t)RTOS_ERR_PARAM)
#define rtos_perf_get_memory(s) ((void)(s), (rtos_status_t)RTOS_ERR_PARAM)
#define rtos_perf_get_task(t, s) ((void)(t), (void)(s), (rtos_status_t)RTOS_ERR_PARAM)
#define rtos_perf_get_all_tasks(a, m, c)                                                           \
    ((void)(a), (void)(m), (void)(c), (rtos_status_t)RTOS_ERR_PARAM)
#define rtos_perf_get_cpu_usage() (0U)
#define rtos_perf_get_switch_rate() (0U)
/* 内部钩子也编译为空 */
#define rtos_perf_get_cycles() (0U)
#define rtos_perf_init_cycles() ((void)0)
#define rtos_perf_cycles_to_us(c) (0U)
#define rtos_perf_on_context_switch(p, n) ((void)0)
#define rtos_perf_on_pend_switch() ((void)0)
#define rtos_perf_on_task_created(t) ((void)0)
#define rtos_perf_on_task_deleted(t) ((void)0)
#define rtos_perf_register_pool(p) ((void)0)
#define rtos_perf_on_scheduler_start() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PERF_H_ */

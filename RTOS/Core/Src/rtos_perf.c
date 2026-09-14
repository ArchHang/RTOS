/**
 * @file    rtos_perf.c
 * @brief   性能监视器实现
 *
 * @details 实现要点:
 *
 *          1. 周期计数器(Cortex-M3/M4/M7 使用 DWT_CYCCNT):
 *             - DWT(Data Watchpoint and Trace)单元含 32 位自由运行周期计数器,
 *               每个 CPU 时钟周期 +1。需先置 DEMCR.TRCENA=1, 再置 DWT_CTRL.CYCCNTENA=1。
 *             - 32 位 @168MHz 约 25.5s 回绕一次。本实现用无符号减法 (now - prev)
 *               天然处理单次回绕(只要两次采样间隔 < 25s, 系统不可能无切换)。
 *             - Cortex-M0/M0+ 无 DWT, 回退到 rtos_kernel.tick_count(1ms 精度)。
 *
 *          2. 任务注册表:
 *             - 静态数组 s_task_registry[], 容量 RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE。
 *             - rtos_task_create 调用 rtos_perf_on_task_created 加入;
 *               rtos_task_delete 调用 rtos_perf_on_task_deleted 移除(swap-with-last)。
 *             - 加删均 O(1), 遍历 O(n)。注册表操作在临界区内。
 *
 *          3. 内存池注册表:
 *             - 静态数组 s_pool_registry[], 容量 RTOS_CONFIG_PERF_POOL_REGISTRY_SIZE。
 *             - rtos_mempool_init 自动调用 rtos_perf_register_pool。
 *
 *          4. 热路径(上下文切换)开销:
 *             - rtos_perf_on_context_switch: 读 DWT_CYCCNT + 1 次减法 + 2 次加法 +
 *               延迟统计(1 减 + 2 比较 + 2 加)。共约 10 条指令, < 60ns @168MHz。
 *             - 不调用任何函数(除 get_cycles 内联), 不进临界区(已在临界区内)。
 *
 *          5. 安全性:
 *             - 所有查询接口校验指针(NULL 返回 RTOS_ERR_NULL)。
 *             - 多字段读取在临界区内, 防止读到中间状态。
 *             - 数组由调用方提供(max_count 限制), 无内部缓冲溢出。
 *             - 不动态分配, 不暴露内核指针(stats 只含值拷贝)。
 *             - 除法前检查除数非零, 避免除零异常。
 *
 *          6. 栈高水位(RTOS_CONFIG_PERF_STACK_WATERMARK=1):
 *             - 任务创建时把栈未用区填 0xA5A5A5A5。
 *             - 查询时从栈底向上扫描, 首个非 0xA5A5A5A5 字即为高水位。
 *             - 仅在查询时扫描(不影响热路径), O(stack_size) 但任务数有限。
 */
#include "rtos_perf.h"

#if RTOS_CONFIG_USE_PERF_MONITOR

#include "rtos_port.h"
#include "rtos_sched.h"
#include "rtos_task.h"
#include "rtos_internal.h"
#include <string.h>

/* ============================== DWT 寄存器定义 ============================== */
/* ARMv7-M 架构手册: DWT 单元地址 0xE0001000。仅 Cortex-M3/M4/M7 有 DWT。 */

#define DWT_BASE (0xE0001000UL)
#define DEMCR_BASE (0xE000EDFCUL)

/** @brief DEMCR(TRCENA 在 bit24)。 */
#define DEMCR_TRCENA (1UL << 24)

/** @brief DWT 控制寄存器(CYCCNTENA 在 bit0)。 */
typedef struct {
    volatile uint32_t CTRL; /**< 0xE0001000: 控制。 */
    volatile uint32_t CYCCNT; /**< 0xE0001004: 周期计数器。 */
    volatile uint32_t CPICNT; /**< 0xE0001008: CPI 计数。 */
    volatile uint32_t EXCCNT; /**< 0xE000100C: 异常计数。 */
} DWT_Type;

#define DWT ((DWT_Type *)DWT_BASE)
#define DEMCR (*(volatile uint32_t *)DEMCR_BASE)

/* ============================== 内部数据 ============================== */

/** @brief 任务注册表(静态数组, 避免动态分配)。 */
static rtos_tcb_t *s_task_registry[RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE];

/** @brief 任务注册表当前条目数。 */
static volatile uint32_t s_task_count = 0U;

/** @brief 内存池注册表。 */
static rtos_mempool_t *s_pool_registry[RTOS_CONFIG_PERF_POOL_REGISTRY_SIZE];

/** @brief 内存池注册表当前条目数。 */
static volatile uint32_t s_pool_count = 0U;

/** @brief 栈水印填充模式。 */
#define RTOS_PERF_STACK_FILL_PATTERN (0xA5A5A5A5UL)

/* ============================== 周期计数器 ============================== */

uint32_t rtos_perf_get_cycles(void)
{
#if RTOS_CONFIG_PERF_USE_DWT
    return DWT->CYCCNT;
#else
    return rtos_kernel.tick_count;
#endif
}

void rtos_perf_init_cycles(void)
{
#if RTOS_CONFIG_PERF_USE_DWT
    /* 使能 DWT 周期计数器:
     *   1. DEMCR.TRCENA=1 (使能 DWT/ITM 全局跟踪)
     *   2. DWT_CYCCNT=0   (清零计数器)
     *   3. DWT_CTRL.CYCCNTENA=1 (使能周期计数) */
    DEMCR |= DEMCR_TRCENA;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= 1U; /* CYCCNTENA */
#endif
}

uint32_t rtos_perf_cycles_to_us(uint32_t cycles)
{
    /* DWT 模式: us = cycles / (cpu_freq / 1e6) = cycles / cpu_mhz
     * 当 cpu_freq = 168MHz: cycles / 168 ≈ cycles * 5.95ns。
     *
     * 回退模式(Tick 计数): cycles 实际是 tick 数,
     * us = ticks * 1e6 / TICK_RATE_HZ, 需 64 位避免溢出。 */
#if RTOS_CONFIG_PERF_USE_DWT
    uint32_t cpu_mhz = SystemCoreClock / 1000000U;
    if (cpu_mhz == 0U) {
        return 0U;
    }
    return cycles / cpu_mhz;
#else
    if (RTOS_CONFIG_TICK_RATE_HZ == 0U) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)cycles * 1000000U) / (uint64_t)RTOS_CONFIG_TICK_RATE_HZ);
#endif
}

/* ============================== 内核钩子 ============================== */

void rtos_perf_on_pend_switch(void)
{
    /* 在 rtos_port_context_switch 中调用: PendSV 即将挂起, 记录时刻。
     * 此处可能在中断上下文(调度器内部触发), 不可阻塞。
     * 仅一次内存写, 开销极小。 */
    rtos_kernel.perf_pend_cycle = rtos_perf_get_cycles();
}

void rtos_perf_on_context_switch(rtos_tcb_t *prev, rtos_tcb_t *next)
{
    /* 在 rtos_sched_context_switch 中调用, 已在临界区内(PendSV 最低优先级,
     * 不会被系统调用中断打断)。不可调用阻塞 API。
     *
     * 开销分析(@168MHz):
     *   - 读 CYCCNT: 1 cycle (LDR from peripheral)
     *   - 减法 + 累加: ~6 cycles
     *   - 延迟统计: ~8 cycles
     *   总计 ~15 cycles ≈ 90ns, 远小于上下文切换本身的 ~200 cycles。
     *
     * next 当前未使用(其运行周期将在下次切换作为 prev 时累加), 保留参数
     * 供未来扩展(如 next 预热缓存)。 */
    (void)next;

    uint32_t now = rtos_perf_get_cycles();
    uint32_t delta = now - rtos_kernel.perf_last_switch_cycle;
    rtos_kernel.perf_last_switch_cycle = now;

    /* 累加 prev 任务运行周期 */
    if (prev != NULL) {
        prev->perf_run_cycles += delta;
        rtos_kernel.perf_total_cycles += delta;
        if (prev == rtos_kernel.idle_tcb) {
            rtos_kernel.perf_idle_cycles += delta;
        }
    }

    /* 调度延迟: 从 PendSV 挂起到本次切换执行的时间差。
     * perf_pend_cycle 在 rtos_port_context_switch 中记录。
     * now - perf_pend_cycle 即派发延迟(含中断嵌套退出时间)。 */
    uint32_t latency = now - rtos_kernel.perf_pend_cycle;
    if (latency > rtos_kernel.perf_latency_max) {
        rtos_kernel.perf_latency_max = latency;
    }
    rtos_kernel.perf_latency_sum += latency;
    rtos_kernel.perf_latency_count++;

    rtos_kernel.perf_switch_total++;
}

void rtos_perf_on_scheduler_start(void)
{
    /* 在 rtos_sched_start 中调用: 内核即将运行, 初始化窗口基准 */
    rtos_kernel.perf_last_switch_cycle = rtos_perf_get_cycles();
    rtos_kernel.perf_pend_cycle = rtos_kernel.perf_last_switch_cycle;
    rtos_kernel.perf_switch_total = 0U;
    rtos_kernel.perf_latency_max = 0U;
    rtos_kernel.perf_latency_sum = 0U;
    rtos_kernel.perf_latency_count = 0U;
    rtos_kernel.perf_idle_cycles = 0U;
    rtos_kernel.perf_total_cycles = 0U;
    rtos_kernel.perf_window_switch_base = 0U;
    rtos_kernel.perf_window_cycle_base = rtos_kernel.perf_last_switch_cycle;
    rtos_kernel.perf_window_idle_base = 0U;
    rtos_kernel.perf_window_tick_base = 0U;
    rtos_kernel.perf_window_latency_sum = 0U;
    rtos_kernel.perf_window_latency_cnt = 0U;
    rtos_kernel.perf_cpu_usage_x100 = 0U;
    rtos_kernel.perf_switch_rate = 0U;
    rtos_kernel.perf_latency_avg_us = 0U;
    rtos_kernel.perf_latency_max_us = 0U;
}

void rtos_perf_on_task_created(rtos_tcb_t *tcb)
{
    if (tcb == NULL) {
        return;
    }

    /* 初始化任务性能字段 */
    tcb->perf_run_cycles = 0U;
    tcb->perf_window_run_base = 0U;

    /* 栈水印填充: 把栈未用区填为 0xA5A5A5A5, 供高水位检测。
     * 栈布局(栈向下生长):
     *   stack_base[0..stack_size-1]  (低地址, 栈底)
     *   ...
     *   top_of_stack                 (当前栈顶, 高地址侧)
     *   stack_base + stack_size      (栈顶上限)
     * top_of_stack 指向初始化帧的最低地址, 之上是已用区(伪现场),
     * 之下(stack_base ~ top_of_stack)是未用区, 可填充模式。
     * 注意: top_of_stack 可能略低于 stack_base + stack_size(对齐消耗)。 */
#if RTOS_CONFIG_PERF_STACK_WATERMARK
    if ((tcb->stack_base != NULL) && (tcb->top_of_stack != NULL)) {
        rtos_stack_t *p = tcb->stack_base;
        rtos_stack_t *end = tcb->top_of_stack; /* 不含, top_of_stack 以上是已用帧 */
        while (p < end) {
            *p = (rtos_stack_t)RTOS_PERF_STACK_FILL_PATTERN;
            p++;
        }
    }
#endif

    /* 加入注册表(临界区保护, 防止与查询并发) */
    RTOS_PORT_ENTER_CRITICAL();
    if (s_task_count < RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE) {
        s_task_registry[s_task_count] = tcb;
        s_task_count++;
    }
    RTOS_PORT_EXIT_CRITICAL();
}

void rtos_perf_on_task_deleted(rtos_tcb_t *tcb)
{
    if (tcb == NULL) {
        return;
    }

    RTOS_PORT_ENTER_CRITICAL();
    /* swap-with-last 移除, O(1) */
    for (uint32_t i = 0U; i < s_task_count; i++) {
        if (s_task_registry[i] == tcb) {
            s_task_count--;
            /* 用最后一个填补空位(若 i 就是最后一个, 自赋值无害) */
            s_task_registry[i] = s_task_registry[s_task_count];
            s_task_registry[s_task_count] = NULL;
            break;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

void rtos_perf_register_pool(rtos_mempool_t *pool)
{
    if (pool == NULL) {
        return;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (s_pool_count < RTOS_CONFIG_PERF_POOL_REGISTRY_SIZE) {
        /* 防重复注册: 检查是否已存在 */
        rtos_bool_t exists = RTOS_FALSE;
        for (uint32_t i = 0U; i < s_pool_count; i++) {
            if (s_pool_registry[i] == pool) {
                exists = RTOS_TRUE;
                break;
            }
        }
        if (!exists) {
            s_pool_registry[s_pool_count] = pool;
            s_pool_count++;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

/* ============================== 初始化与重置 ============================== */

rtos_status_t rtos_perf_init(void)
{
    rtos_perf_init_cycles();

    /* 清零注册表 */
    s_task_count = 0U;
    s_pool_count = 0U;
    memset(s_task_registry, 0, sizeof(s_task_registry));
    memset(s_pool_registry, 0, sizeof(s_pool_registry));

    /* 内核性能字段清零(sched_init 已 memset 整个 rtos_kernel, 这里显式初始化
     * 确保即使 sched_init 先于 perf_init 也正确) */
    rtos_kernel.perf_last_switch_cycle = 0U;
    rtos_kernel.perf_pend_cycle = 0U;
    rtos_kernel.perf_switch_total = 0U;
    rtos_kernel.perf_latency_max = 0U;
    rtos_kernel.perf_latency_sum = 0U;
    rtos_kernel.perf_latency_count = 0U;
    rtos_kernel.perf_idle_cycles = 0U;
    rtos_kernel.perf_total_cycles = 0U;
    rtos_kernel.perf_window_switch_base = 0U;
    rtos_kernel.perf_window_cycle_base = 0U;
    rtos_kernel.perf_window_idle_base = 0U;
    rtos_kernel.perf_window_tick_base = 0U;
    rtos_kernel.perf_window_latency_sum = 0U;
    rtos_kernel.perf_window_latency_cnt = 0U;
    rtos_kernel.perf_cpu_usage_x100 = 0U;
    rtos_kernel.perf_switch_rate = 0U;
    rtos_kernel.perf_latency_avg_us = 0U;
    rtos_kernel.perf_latency_max_us = 0U;

    return RTOS_OK;
}

void rtos_perf_reset(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t now = rtos_perf_get_cycles();
    rtos_kernel.perf_window_switch_base = rtos_kernel.perf_switch_total;
    rtos_kernel.perf_window_cycle_base = now;
    rtos_kernel.perf_window_idle_base = rtos_kernel.perf_idle_cycles;
    rtos_kernel.perf_window_tick_base = rtos_kernel.tick_count;
    rtos_kernel.perf_window_latency_sum = rtos_kernel.perf_latency_sum;
    rtos_kernel.perf_window_latency_cnt = rtos_kernel.perf_latency_count;

    /* 重置各任务窗口基准 */
    for (uint32_t i = 0U; i < s_task_count; i++) {
        rtos_tcb_t *tcb = s_task_registry[i];
        if (tcb != NULL) {
            tcb->perf_window_run_base = tcb->perf_run_cycles;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
}

/* ============================== 查询实现 ============================== */

rtos_status_t rtos_perf_get_system(rtos_perf_system_t *stats)
{
    if (stats == NULL) {
        return RTOS_ERR_NULL;
    }

    RTOS_PORT_ENTER_CRITICAL();

    uint32_t now_cycle = rtos_perf_get_cycles();
    uint32_t total_cycles_window = now_cycle - rtos_kernel.perf_window_cycle_base;
    uint32_t idle_cycles_window = rtos_kernel.perf_idle_cycles - rtos_kernel.perf_window_idle_base;
    uint32_t switches_window = rtos_kernel.perf_switch_total - rtos_kernel.perf_window_switch_base;
    uint32_t ticks_window = rtos_kernel.tick_count - rtos_kernel.perf_window_tick_base;
    uint32_t lat_sum_window = rtos_kernel.perf_latency_sum - rtos_kernel.perf_window_latency_sum;
    uint32_t lat_cnt_window = rtos_kernel.perf_latency_count - rtos_kernel.perf_window_latency_cnt;
    uint32_t lat_max = rtos_kernel.perf_latency_max;

    /* CPU 使用率: (total - idle) / total * 10000
     * 注意: total_cycles_window 是"墙上时间"周期数(从窗口起到现在),
     *       而 idle_cycles_window 是空闲实际运行的周期数。
     *       CPU 占用 = 1 - idle/total。
     * 除零保护: 窗口极短时 total 可能为 0, 返回 0%。 */
    uint32_t cpu_x100 = 0U;
    if (total_cycles_window > 0U) {
        if (idle_cycles_window >= total_cycles_window) {
            /* 空闲周期 >= 总周期(窗口边界或测量误差), CPU=0% */
            cpu_x100 = 0U;
        } else {
            uint32_t busy = total_cycles_window - idle_cycles_window;
            /* busy * 10000 / total, 用 64 位避免溢出(busy 可达 4G) */
            uint64_t busy64 = (uint64_t)busy * 10000U;
            cpu_x100 = (uint32_t)(busy64 / (uint64_t)total_cycles_window);
        }
    }

    /* 切换频率: switches / (ticks / tick_rate)
     * = switches * tick_rate / ticks */
    uint32_t switch_rate = 0U;
    if (ticks_window > 0U) {
        uint64_t rate64 = (uint64_t)switches_window * (uint64_t)RTOS_CONFIG_TICK_RATE_HZ;
        switch_rate = (uint32_t)(rate64 / (uint64_t)ticks_window);
    }

    /* 调度延迟(微秒) */
    uint32_t lat_avg_us = 0U;
    if (lat_cnt_window > 0U) {
        lat_avg_us = rtos_perf_cycles_to_us(lat_sum_window / lat_cnt_window);
    }
    uint32_t lat_max_us = rtos_perf_cycles_to_us(lat_max);

    /* 填充输出 */
    stats->cpu_usage_x100 = cpu_x100;
    stats->idle_usage_x100 = 10000U - cpu_x100;
    stats->uptime_ticks = rtos_kernel.tick_count;
    stats->uptime_ms =
        (uint32_t)(((uint64_t)rtos_kernel.tick_count * 1000U) / (uint64_t)RTOS_CONFIG_TICK_RATE_HZ);
    stats->total_switches = rtos_kernel.perf_switch_total;
    stats->switch_rate = switch_rate;
    stats->sched_latency_avg_us = lat_avg_us;
    stats->sched_latency_max_us = lat_max_us;
    stats->sched_latency_count = lat_cnt_window;
    stats->task_count = s_task_count;
    stats->tick_rate_hz = RTOS_CONFIG_TICK_RATE_HZ;
    stats->cpu_freq_hz = SystemCoreClock;

    /* 更新快照(供 ISR 安全的快速查询) */
    rtos_kernel.perf_cpu_usage_x100 = cpu_x100;
    rtos_kernel.perf_switch_rate = switch_rate;
    rtos_kernel.perf_latency_avg_us = lat_avg_us;
    rtos_kernel.perf_latency_max_us = lat_max_us;

    /* 推进窗口基准 */
    rtos_kernel.perf_window_switch_base = rtos_kernel.perf_switch_total;
    rtos_kernel.perf_window_cycle_base = now_cycle;
    rtos_kernel.perf_window_idle_base = rtos_kernel.perf_idle_cycles;
    rtos_kernel.perf_window_tick_base = rtos_kernel.tick_count;
    rtos_kernel.perf_window_latency_sum = rtos_kernel.perf_latency_sum;
    rtos_kernel.perf_window_latency_cnt = rtos_kernel.perf_latency_count;

    /* 同步推进各任务窗口基准: 保证 rtos_perf_get_task 的分子(任务运行周期窗口)
     * 与分母(墙上周期窗口)起算点一致, 否则 task_cpu% 会因窗口不匹配而 >100%。
     * O(n) 遍历, n 为任务数(通常 <20), 查询路径可接受。 */
    for (uint32_t i = 0U; i < s_task_count; i++) {
        rtos_tcb_t *tcb = s_task_registry[i];
        if (tcb != NULL) {
            tcb->perf_window_run_base = tcb->perf_run_cycles;
        }
    }

    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_perf_get_memory(rtos_perf_memory_t *stats)
{
    if (stats == NULL) {
        return RTOS_ERR_NULL;
    }

    memset(stats, 0, sizeof(*stats));
    stats->task_pool_capacity = RTOS_CONFIG_MAX_TASKS;

    RTOS_PORT_ENTER_CRITICAL();
    stats->task_pool_used = s_task_count;

    /* 聚合所有已注册内存池 */
    for (uint32_t i = 0U; i < s_pool_count; i++) {
        rtos_mempool_t *pool = s_pool_registry[i];
        if (pool != NULL) {
            uint32_t total = pool->block_size * pool->block_count;
            uint32_t free_bytes = pool->block_size * pool->free_count;
            uint32_t used = total - free_bytes;
            stats->pool_total_bytes += total;
            stats->pool_used_bytes += used;
            stats->pool_free_bytes += free_bytes;
            stats->registered_pools++;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();

    /* 使用率(除零保护) */
    if (stats->pool_total_bytes > 0U) {
        uint64_t used64 = (uint64_t)stats->pool_used_bytes * 10000U;
        stats->pool_usage_x100 = (uint32_t)(used64 / (uint64_t)stats->pool_total_bytes);
    }

    return RTOS_OK;
}

/**
 * @brief 测量任务栈高水位(从栈底向上扫描填充模式)。
 * @return 已用栈字数(含初始化帧); 0 表示无法测量。
 */
static uint32_t perf_measure_stack_used(const rtos_tcb_t *tcb)
{
#if RTOS_CONFIG_PERF_STACK_WATERMARK
    if ((tcb == NULL) || (tcb->stack_base == NULL) || (tcb->top_of_stack == NULL)) {
        return 0U;
    }

    /* 栈向下生长: stack_base(低) ... top_of_stack(高, 当前帧底) ... 栈顶上限
     * 未用区: [stack_base, top_of_stack) — 这些在创建时填了 0xA5A5A5A5。
     * 扫描从 stack_base 向上, 找首个非 pattern 字, 即历史最高水位。
     * 已用 = 总字数 - 未用字数(从高水位到 top_of_stack 之间)
     *       但更准确: 已用 = (栈顶上限 - 高水位地址) / sizeof(stack_t)
     * 简化: 已用字数 = stack_size - (high_water - stack_base)
     *   high_water = 首个非 pattern 的字地址
     *   未用 = high_water - stack_base(从栈底到高水位)
     *   已用 = stack_size - 未用 */
    rtos_stack_t *p = tcb->stack_base;
    rtos_stack_t *end = tcb->top_of_stack; /* 上界(不含) */
    while (p < end) {
        if (*p != (rtos_stack_t)RTOS_PERF_STACK_FILL_PATTERN) {
            break; /* 找到高水位 */
        }
        p++;
    }
    /* p 指向首个非 pattern 字(或 end)。
     * 未用字数 = p - stack_base
     * 已用字数 = stack_size - 未用字数 */
    uint32_t unused_words = (uint32_t)(p - tcb->stack_base);
    if (unused_words > tcb->stack_size) {
        unused_words = tcb->stack_size; /* 防御性 */
    }
    return (tcb->stack_size - unused_words);
#else
    /* 不启用水印: 返回当前栈指针位置对应的已用量 */
    if ((tcb == NULL) || (tcb->stack_base == NULL) || (tcb->top_of_stack == NULL)) {
        return 0U;
    }
    /* 栈向下生长: top_of_stack >= stack_base
     * 已用 = (栈顶上限地址 - top_of_stack) / sizeof(stack_t)
     * 栈顶上限 = stack_base + stack_size
     * 已用字数 = stack_size - (top_of_stack - stack_base) */
    uint32_t free_words = (uint32_t)(tcb->top_of_stack - tcb->stack_base);
    if (free_words > tcb->stack_size) {
        return tcb->stack_size; /* 防御性 */
    }
    return (tcb->stack_size - free_words);
#endif
}

rtos_status_t rtos_perf_get_task(rtos_tcb_t *tcb, rtos_perf_task_t *stats)
{
    if (stats == NULL) {
        return RTOS_ERR_NULL;
    }

    if (tcb == NULL) {
        tcb = rtos_kernel.current_tcb;
    }
    if (tcb == NULL) {
        return RTOS_ERR_PARAM;
    }

    RTOS_PORT_ENTER_CRITICAL();

    /* 任务级 CPU 占用: 窗口内该任务运行周期 / 窗口内墙上周期 * 10000。
     * 用墙上周期(total_wall)作分母, 保证各任务 CPU% 之和约等于系统 CPU%。
     * task_run_window = perf_run_cycles(累计) - perf_window_run_base(窗口基准)。 */
    uint32_t task_run_window = tcb->perf_run_cycles - tcb->perf_window_run_base;
    uint32_t now_cycle = rtos_perf_get_cycles();
    uint32_t total_wall_window = now_cycle - rtos_kernel.perf_window_cycle_base;

    uint32_t task_cpu_x100 = 0U;
    if (total_wall_window > 0U) {
        if (task_run_window > total_wall_window) {
            task_run_window = total_wall_window; /* 防御性截断(窗口边界) */
        }
        uint64_t task64 = (uint64_t)task_run_window * 10000U;
        task_cpu_x100 = (uint32_t)(task64 / (uint64_t)total_wall_window);
    }

    /* 填充输出 */
    stats->task_id = tcb->task_id;
    memcpy(stats->name, tcb->name, RTOS_CONFIG_MAX_TASK_NAME_LEN);
    stats->priority = tcb->priority;
    stats->state = tcb->state;
    stats->cpu_usage_x100 = task_cpu_x100;
    stats->run_cycles = tcb->perf_run_cycles;
    stats->run_time_us = rtos_perf_cycles_to_us(tcb->perf_run_cycles);
    stats->switch_count = tcb->switch_count;

    /* 栈统计 */
    uint32_t used_words = perf_measure_stack_used(tcb);
    stats->stack_total_bytes = tcb->stack_size * (uint32_t)sizeof(rtos_stack_t);
    stats->stack_used_bytes = used_words * (uint32_t)sizeof(rtos_stack_t);
    stats->stack_free_bytes =
        (tcb->stack_size > used_words)
            ? ((tcb->stack_size - used_words) * (uint32_t)sizeof(rtos_stack_t))
            : 0U;

    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_perf_get_all_tasks(rtos_perf_task_t *stats_array, uint32_t max_count,
                                      uint32_t *actual_count)
{
    if (stats_array == NULL) {
        return RTOS_ERR_NULL;
    }
    if (max_count == 0U) {
        return RTOS_ERR_PARAM;
    }

    /* 整个遍历持临界区: 注册表写入方(on_task_created/deleted 的
     * swap-with-last 重排)与任务删除(随后的 rtos_heap_free)均在任务
     * 上下文临界区内进行; 若遍历期间让出 CPU, 快照的 TCB 指针可能在
     * get_task 解引用前已被释放(use-after-free 读)。
     * rtos_perf_get_task 内部的 ENTER/EXIT 为嵌套安全(BASEPRI 计数),
     * 栈水印扫描在屏蔽下执行, 典型配置(16 任务 × 512 字栈)约几十 µs。 */
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t count = s_task_count;
    if (count > max_count) {
        count = max_count; /* 不超过调用方数组容量, 防溢出 */
    }

    for (uint32_t i = 0U; i < count; i++) {
        rtos_tcb_t *tcb = s_task_registry[i];
        if (tcb != NULL) {
            rtos_status_t st = rtos_perf_get_task(tcb, &stats_array[i]);
            if (st != RTOS_OK) {
                /* 单个任务查询失败, 填零继续(不中断整体) */
                memset(&stats_array[i], 0, sizeof(stats_array[i]));
            }
        } else {
            memset(&stats_array[i], 0, sizeof(stats_array[i]));
        }
    }
    RTOS_PORT_EXIT_CRITICAL();

    if (actual_count != NULL) {
        *actual_count = count;
    }
    return RTOS_OK;
}

uint32_t rtos_perf_get_cpu_usage(void)
{
    /* ISR 安全: 仅读快照(volatile), 不进临界区 */
    return rtos_kernel.perf_cpu_usage_x100;
}

uint32_t rtos_perf_get_switch_rate(void)
{
    /* ISR 安全: 仅读快照 */
    return rtos_kernel.perf_switch_rate;
}

#endif /* RTOS_CONFIG_USE_PERF_MONITOR */

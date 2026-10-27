/**
 * @file    rtos_kernel.c
 * @brief   内核初始化编排、启动入口、版本信息和默认用户钩子。
 *
 * @details 按依赖顺序初始化性能监视器、堆、调度器和软件定时器服务。
 *          默认钩子使用弱符号，应用可以提供同名实现覆盖。
 */
#include "rtos.h"

/* ============================== 内核入口 ============================== */

rtos_status_t rtos_init(void)
{
    if (rtos_kernel.idle_tcb != NULL) return RTOS_ERR_ALREADY;
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

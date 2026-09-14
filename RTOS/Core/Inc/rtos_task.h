/**
 * @file    rtos_task.h
 * @brief   任务管理接口与 TCB 定义
 *
 * @details 提供任务创建、删除、挂起、恢复、延时、优先级修改及任务通知等功能。
 *          任务控制块(TCB)是内核最核心的数据结构，调度器通过 TCB 链表管理
 *          所有任务的状态切换。本实现采用优先级位图就绪表，O(1) 选取最高
 *          优先级任务。
 */
#ifndef RTOS_TASK_H_
#define RTOS_TASK_H_

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_types.h"
#include "rtos_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== TCB 定义 ============================== */

/**
 * @struct rtos_tcb
 * @brief  任务控制块。每个任务对应一个 TCB 实例。
 *
 * @details 字段排列顺序考虑了缓存友好性: 调度热路径字段(top_of_stack,
 *          priority, state, ready_list)集中在前部。
 *          栈顶指针是上下文切换最先访问的字段，必须位于偏移 0 处
 *          (与 PendSV 汇编代码约定的偏移一致)。
 */
struct rtos_tcb {
    /* ---- 上下文切换热字段 (偏移 0, 与汇编一致) ---- */
    rtos_stack_t *top_of_stack; /**< 当前栈顶(PendSV 第一访问)。 */
    rtos_stack_t *stack_base; /**< 栈底(用于溢出检测)。 */
    uint32_t stack_size; /**< 栈容量(字数)。 */

    /* ---- 调度字段 ---- */
    rtos_prio_t priority; /**< 当前优先级(0 最高)。 */
    rtos_prio_t base_priority; /**< 基础优先级(互斥锁优先级继承用)。 */
    rtos_task_state_t state; /**< 任务状态。 */
    rtos_bool_t is_static; /**< 是否静态分配(决定删除时是否回收 TCB)。 */

    /* ---- 就绪表链表(同一优先级) ---- */
    struct rtos_tcb *next_ready; /**< 同优先级就绪链表后继。 */
    struct rtos_tcb *prev_ready; /**< 同优先级就绪链表前驱。 */

    /* ---- 阻塞字段 ---- */
    rtos_wait_node_t wait_node; /**< 等待节点(挂入对象等待链表)。 */
    rtos_tick_t wake_tick; /**< 唤醒时刻(用于 delay/超时)。 */
    void *blocked_on; /**< 阻塞所在对象(信号量/队列等)。 */
    rtos_status_t wait_result; /**< 唤醒结果(成功/超时)。 */

    /* ---- 任务通知(轻量级同步) ---- */
    volatile uint32_t notify_value; /**< 通知值。 */
    volatile uint8_t notify_state; /**< 通知状态(空闲/待处理/已接收)。 */
    rtos_notify_type_t notify_type; /**< 通知类型。 */

    /* ---- 标识与统计 ---- */
    char name[RTOS_CONFIG_MAX_TASK_NAME_LEN]; /**< 任务名。 */
    uint32_t task_id; /**< 唯一 ID(创建时分配)。 */
#if RTOS_CONFIG_GENERATE_RUN_TIME_STATS
    uint32_t run_time_counter; /**< 累计运行时间(统计用)。 */
#endif
    uint32_t switch_count; /**< 被调度次数。 */

#if RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW
    uint32_t stack_magic; /**< 栈底魔数(溢出检测)。 */
#endif

#if RTOS_CONFIG_USE_PERF_MONITOR
    /* ---- 性能监视器字段(条件编译, 关闭时零开销) ---- */
    volatile uint32_t perf_run_cycles; /**< 累计运行周期数(DWT)或 Tick 数(回退)。 */
    volatile uint32_t perf_window_run_base; /**< 窗口起始运行周期数(算 CPU% 用)。 */
#endif
};

/* ============================== API ============================== */

/**
 * @brief 创建任务(动态分配 TCB 与栈)。
 * @details 默认创建方式: TCB 与栈均从 RTOS 堆分配, 删除任务时自动回收。
 *          适合大多数场景。
 *
 *          分配的内存:
 *            - TCB: sizeof(rtos_tcb_t) 字节
 *            - 栈:  stack_size * sizeof(rtos_stack_t) 字节
 *          失败时返回 RTOS_ERR_NO_MEM, 不分配任何内存。
 *
 * @param tcb_out    输出参数, 接收新建 TCB 指针(不可为 NULL)。
 * @param stack_size 栈大小(字数, 4 字节/字)。建议 >= RTOS_CONFIG_MINIMAL_STACK_SIZE。
 * @param entry      任务入口函数。
 * @param arg        传递给入口函数的参数(可为 NULL)。
 * @param priority   任务优先级(0=最高)。
 * @param name       任务名(可空, 会被复制到 TCB 内部缓冲区)。
 * @return RTOS_OK 或错误码。
 */
rtos_status_t rtos_task_create(rtos_tcb_t **tcb_out, uint32_t stack_size, rtos_task_func_t entry,
                               void *arg, rtos_prio_t priority, const char *name);

/**
 * @brief 创建任务(静态分配栈与 TCB)。
 * @details 用户提供 TCB 与栈存储, 内核不分配堆内存。删除任务时不回收 TCB/栈
 *          (由用户管理其生命周期)。适用于确定性内存布局、无堆场景。
 *
 * @param tcb        静态 TCB 存储指针(不可为 NULL, 需 4 字节对齐)。
 * @param stack_buf  静态栈存储(不可为 NULL, 需 8 字节对齐)。
 * @param stack_size 栈大小(字数)。
 * @param entry      任务入口函数。
 * @param arg        传递给入口函数的参数。
 * @param priority   任务优先级(0=最高)。
 * @param name       任务名(可空)。
 * @return RTOS_OK 或错误码。
 */
rtos_status_t rtos_task_create_static(rtos_tcb_t *tcb, rtos_stack_t *stack_buf, uint32_t stack_size,
                                      rtos_task_func_t entry, void *arg, rtos_prio_t priority,
                                      const char *name);

/**
 * @brief 删除任务。若删除自身，调度器立即切换。
 * @details 对于动态创建的任务(rtos_task_create), 删除时自动
 *          回收 TCB 与栈的堆内存。静态任务不回收(由用户管理)。
 *          删除任务后, tcb 指针变为悬空, 不可再用。
 * @param tcb 任务 TCB。NULL 表示删除当前任务。
 */
rtos_status_t rtos_task_delete(rtos_tcb_t *tcb);

/**
 * @brief 主动让出 CPU(触发一次调度)。
 * @note  可在中断中调用(仅标记待切换，PendSV 在中断退出后执行)。
 */
void rtos_task_yield(void);

/**
 * @brief 阻塞当前任务若干 Tick。
 * @param ticks 阻塞时长。RTOS_NO_WAIT 等价于 yield。
 * @note  不可在中断中调用(ISR 中调用为空操作)。
 */
void rtos_task_delay(rtos_tick_t ticks);

/**
 * @brief 阻塞到指定绝对时刻。
 * @param tick_to_wake 唤醒时刻的 Tick 计数。
 * @note  不可在中断中调用(ISR 中调用为空操作)。
 */
void rtos_task_delay_until(rtos_tick_t *last_wake_tick, rtos_tick_t period);

/** @brief 获取当前任务 TCB。 */
rtos_tcb_t *rtos_task_get_current(void);

/** @brief 获取任务优先级。 */
rtos_prio_t rtos_task_get_priority(const rtos_tcb_t *tcb);

/** @brief 设置任务优先级(可能触发立即抢占)。 */
rtos_status_t rtos_task_set_priority(rtos_tcb_t *tcb, rtos_prio_t prio);

/**
 * @brief 挂起任务(不参与调度)。
 * @note  可在中断中调用(挂起其他任务)。挂起自身时中断退出后切换。
 */
rtos_status_t rtos_task_suspend(rtos_tcb_t *tcb);

/**
 * @brief 恢复挂起的任务。
 * @note  可在中断中调用(典型用法: ISR 中唤醒等待数据的任务)。
 *        若恢复任务优先级更高，中断退出后自动切换。
 */
rtos_status_t rtos_task_resume(rtos_tcb_t *tcb);

/**
 * @brief 通知任务(发送值/位/计数)。
 * @note  可在中断中调用(典型用法: ISR 中通知任务有事件发生)。
 *        这是 ISR→任务同步的最快路径(无独立对象，直接写 TCB)。
 */
rtos_status_t rtos_task_notify(rtos_tcb_t *tcb, uint32_t value, rtos_notify_type_t type);

/**
 * @brief 等待通知。
 * @note  不可在中断中调用(会阻塞)。
 */
rtos_status_t rtos_task_notify_wait(rtos_tick_t timeout, uint32_t *recv_value);

/** @brief 申请空闲钩子(用户可重新定义 rtos_idle_hook)。 */
void rtos_idle_hook(void);

/** @brief 空闲任务主体(内部使用)。 */
void rtos_idle_task(void *arg);

/** @brief 栈溢出检测钩子(用户可重定义 rtos_stack_overflow_hook)。 */
void rtos_stack_overflow_hook(rtos_tcb_t *tcb);

/** @brief 任务删除钩子(仅在 RTOS_CONFIG_USE_DELETE_HOOK=1 时调用, 用户可重定义)。 */
void rtos_task_delete_hook(rtos_tcb_t *tcb);

/** @brief 断言失败处理(用户可重定义)。 */
void rtos_assert_fail(const char *cond, const char *file, int line);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_TASK_H_ */

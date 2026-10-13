/**
 * @file    kern_port.h
 * @brief   内核适配层: 用统一接口屏蔽自研 RTOS 与 FreeRTOS 的 API 差异。
 *
 * @details 目的: 让同一份场景测试代码 (scenarios.c) 能编译链接到两套内核上,
 *          实现"逐项镜像"的系统级对比。本头文件只暴露 C 原生类型与句柄,
 *          绝不包含任何内核私有类型, 因此两侧的实现文件各自包含自己的内核头,
 *          互不污染。
 *
 *          适配的两处语义差异:
 *            1. 优先级方向: 自研 RTOS 数值小=优先级高(0 最高); FreeRTOS 数值
 *               大=优先级高。本层统一采用【自研语义】(数值小=高), 由 FreeRTOS
 *               侧的实现负责翻转 (见 kern_port_freertos.c 中的 KERN_PRIO 换算)。
 *            2. 对象类型: 自研的 sem/mutex/queue/event 是静态结构体由调用方
 *               提供存储; FreeRTOS 全部是动态分配的句柄。适配层统一为
 *               "init 时由适配层负责创建, 调用方只持有不透明句柄"。
 *
 * @note    计时统一用 DWT CYCCNT (1 cycle ≈ 5.952 ns @168MHz), 两侧同源。
 *          本层不提供 printf, 由各工程的 syscalls 重定向负责。
 */
#ifndef KERN_PORT_H_
#define KERN_PORT_H_

#include <stdint.h>

/* ============================== 优先级约定 ============================== */

/**
 * @brief 统一优先级刻度(自研语义: 数值小 = 优先级高)。
 * @details 两侧实现各自映射到本内核的实际优先级空间。刻度取值集中在此,
 *          场景代码只用这些宏, 便于两工程保持相同拓扑。
 */
#define KERN_PRIO_HIGHEST   2U  /**< 最高(周期控制环) */
#define KERN_PRIO_HIGH      4U  /**< 高(高优先级等待者/ISR 消费者) */
#define KERN_PRIO_ABOVE     6U  /**< 中高(中等优先级干扰者) */
#define KERN_PRIO_NORMAL    8U  /**< 常规(生产者) */
#define KERN_PRIO_BELOW    10U  /**< 中低(消费者/工作线程) */
#define KERN_PRIO_LOW      12U  /**< 低(持锁者/普通等待者) */
#define KERN_PRIO_LOWEST   14U  /**< 最低(填充用) */

/** @brief 无限等待的超时值。 */
#define KERN_WAIT_FOREVER 0xFFFFFFFFU

/** @brief 不等待。 */
#define KERN_NO_WAIT 0U

/* ============================== 时间基准 ============================== */

/** @brief 当前 CPU 周期计数(DWT CYCCNT)。 */
uint32_t kern_cycles(void);

/** @brief 使能 DWT 周期计数器。 */
void kern_cycles_init(void);

/** @brief 由周期数换算微秒(168MHz 下 168 周期 = 1µs)。 */
uint32_t kern_cycles_to_us(uint32_t cycles);

/** @brief 当前 tick 计数。 */
uint32_t kern_ticks(void);

/** @brief 各内核的 tick 频率(Hz), 用于结果归一化。 */
uint32_t kern_tick_rate(void);

/* ============================== 内核生命周期 ============================== */

/** @brief 初始化内核(不启动调度器)。 */
void kern_init(void);

/** @brief 启动调度器(不返回)。 */
void kern_start(void) __attribute__((noreturn));

/** @brief 当前任务让出 CPU。 */
void kern_yield(void);

/** @brief 当前任务延时指定 tick。 */
void kern_delay(uint32_t ticks);

/**
 * @brief 延时到指定绝对时刻(周期任务用)。
 * @param last_wake_tick [in,out] 上次唤醒时刻, 首次调用前须置为 0。
 * @param period         周期(tick)。
 */
void kern_delay_until(uint32_t *last_wake_tick, uint32_t period);

/** @brief 当前 tick 周期内的中断屏蔽窗口起点(进入临界区)。 */
void kern_enter_critical(void);

/** @brief 退出临界区。 */
void kern_exit_critical(void);

/* ============================== 任务 ============================== */

/** @brief 任务函数原型。 */
typedef void (*kern_task_func_t)(void *arg);

/**
 * @brief 创建一个任务。
 * @param func       任务入口。
 * @param arg        传给入口的参数。
 * @param stack_words 栈大小(以 32 位字为单位), 两侧语义一致。
 * @param prio       统一优先级刻度(KERN_PRIO_*)。
 * @param name       任务名(<= 8 字符, 便于日志对齐)。
 * @return 0 成功, 负值失败。
 */
int32_t kern_task_create(kern_task_func_t func, void *arg, uint32_t stack_words, uint32_t prio,
                         const char *name);

/** @brief 删除当前任务。 */
void kern_task_delete_self(void);

/** @brief 当前任务的统一刻度优先级(用于复现性检查)。 */
uint32_t kern_task_priority_current(void);

/* ============================== 同步对象 ============================== */

/**
 * @brief 在调用方提供的存储上初始化一个内核对象。
 * @param storage 调用方提供的存储(两侧实现自行决定如何使用, 见各实现文件)。
 * @param size    storage 的字节数。
 * @return 0 成功, 负值失败。
 *
 * @details 自研 RTOS 直接把 storage 当作对象结构体使用; FreeRTOS 侧从 storage
 *          中取出一块句柄槽并动态创建对象。两侧调用方代码完全一致。
 */
int32_t kern_sem_init(void *storage, uint32_t size, uint32_t initial_count, uint32_t max_count);

/** @brief 获取信号量(阻塞等待)。 */
int32_t kern_sem_take(void *storage, uint32_t timeout_ticks);

/** @brief 释放信号量(可在中断中调用)。 */
int32_t kern_sem_give(void *storage);

/**
 * @brief 初始化互斥锁(支持优先级继承)。
 * @return 0 成功, 负值失败。
 */
int32_t kern_mutex_init(void *storage, uint32_t size);

/** @brief 获取互斥锁。 */
int32_t kern_mutex_take(void *storage, uint32_t timeout_ticks);

/** @brief 释放互斥锁。 */
int32_t kern_mutex_give(void *storage);

/**
 * @brief 初始化事件标志组。
 * @return 0 成功, 负值失败。
 */
int32_t kern_event_init(void *storage, uint32_t size);

/** @brief 置位事件标志(可在中断中调用)。 */
uint32_t kern_event_set(void *storage, uint32_t bits);

/* ============================== 中断注入 ============================== */

/**
 * @brief 配置并启动一个周期定时器中断作为确定性中断源(TIM6)。
 * @param period_us 中断周期(微秒)。
 * @param prio      中断优先级(NVIC 数值, 两侧一致: 数值越大优先级越低)。
 * @return 0 成功, 负值失败。
 *
 * @details 两侧都使用 TIM6_DAC_IRQHandler, 由各自实现文件提供 ISR 实体,
 *          调用场景注册的回调 kern_isr_set_handler()。
 */
int32_t kern_timer_isr_start(uint32_t period_us, uint32_t prio);

/** @brief 停止定时器中断。 */
void kern_timer_isr_stop(void);

/** @brief ISR 回调原型(在中断上下文执行)。 */
typedef void (*kern_isr_handler_t)(void);

/** @brief 注册 ISR 回调(两侧 ISR 实体在中断内调用它)。 */
void kern_isr_set_handler(kern_isr_handler_t handler);

/** @brief 当前是否处于中断上下文(两侧实现各自的判定)。 */
uint32_t kern_in_isr(void);

/*
 * 适配实现选路: 由构建系统定义 KERN_IS_CUSTOM_RTOS 选择内核。
 *   KERN_IS_CUSTOM_RTOS = 1 -> 使用 kern_port_rtos.c(自研 RTOS)
 *   未定义                   -> 使用 kern_port_freertos.c(FreeRTOS)
 * 两个 .c 文件都参与编译, 但各自内部用下面的守卫包裹, 同一固件只有一份生效。
 */
#if defined(KERN_IS_CUSTOM_RTOS)
#define KERN_IMPL_FILE "kern_port_rtos.c"
#else
#define KERN_IMPL_FILE "kern_port_freertos.c"
#endif

#endif /* KERN_PORT_H_ */

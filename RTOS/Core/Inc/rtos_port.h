/**
 * @file    rtos_port.h
 * @brief   处理器移植层接口
 *
 * @details 定义内核与处理器架构(Cortex-M)之间的抽象接口。移植到新架构时
 *          只需实现本文件声明的一组函数与宏，内核其余代码无需修改。
 *
 *          当前实现(Port/GCC/ARM_CM7/port.c)面向 Cortex-M4F / M7F 内核，
 *          覆盖 STM32F4/F7/L4/H7 等系列的同内核型号。移植层需处理:
 *            1. 上下文切换(保存/恢复 R4-R11, S16-S31, PSP, EXC_RETURN)
 *            2. 临界区进出(BASEPRI 屏蔽中等优先级中断)
 *            3. SysTick 与 PendSV 中断配置
 *            4. 栈初始化(模拟一次中断返回以启动首个任务)
 *            5. 计数屏障与 FPU 上下文延迟保存(LAZY stacking)
 *
 *          移植层不负责时钟配置与时钟树初始化。SystemCoreClock 全局变量
 *          由用户工程(如 STM32CubeMX 的 system_stm32xx.c)提供，移植层
 *          仅读取以配置 SysTick 重载值。
 */
#ifndef RTOS_PORT_H_
#define RTOS_PORT_H_

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== 系统时钟(由用户工程提供) ============================== */

/**
 * @brief 系统主频(Hz)。
 * @details  由用户工程(如 STM32CubeMX 生成的 system_stm32xx.c)定义并初始化。
 *           移植层在 rtos_port_start_scheduler() 中读取此值配置 SysTick。
 *           用户必须在启动调度器前完成时钟配置并设置此变量。
 */
extern uint32_t SystemCoreClock;

/* ============================== 临界区宏 ============================== */

/* [perf P-1] 临界区嵌套计数(全局唯一, port.c 定义)。
 * 原实现为 port.c 的 static 变量 + out-of-line 函数: 每次 enter/exit 付出
 * 函数调用序言/尾声, 且 exit 侧带 isb。改为头文件内联 + exit 去 isb
 * (FreeRTOS vPortSetBASEPRI 同判: 裸 msr 即可), 每对节省 ~10 周期。 */
extern volatile uint32_t rtos_port_crit_nest;

/**
 * @brief 进入临界区: 屏蔽 RTOS 系统调用级中断(内联, 支持嵌套)。
 * @details 使用 BASEPRI 寄存器屏蔽优先级数值 >= MAX_SYSCALL_INTERRUPT_PRIORITY
 *          的中断。这样高优先级中断(如硬实时控制环)仍可抢占内核临界区，
 *          保证实时性。
 *          isb 必须: 屏蔽需立即生效, 否则临界区头部指令仍可能被抢占。
 */
static inline void rtos_port_enter_critical(void)
{
#if RTOS_CONFIG_USE_BASEPRI
    __asm volatile(" msr basepri, %0 \n"
                   " isb             \n" :: "r"(RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY)
                   : "memory");
#else
    __asm volatile(" cpsid i \n" ::: "memory");
#endif
    rtos_port_crit_nest++;
}

/**
 * @brief 退出临界区: 当嵌套计数归零时恢复 BASEPRI(内联)。
 * @details [perf P-1] 解除屏蔽的 msr basepri,0 后不需要 isb:
 *          FreeRTOS vPortExitCritical 同判 —— 寄存器写入对后续取指
 *          自然生效, 已 pending 的中断晚几条指令被识别, 无正确性影响。
 */
static inline void rtos_port_exit_critical(void)
{
    rtos_port_crit_nest--;
    if (rtos_port_crit_nest == 0U) {
#if RTOS_CONFIG_USE_BASEPRI
        __asm volatile(" msr basepri, %0 \n" :: "r"(0U) : "memory");
#else
        __asm volatile(" cpsie i \n" ::: "memory");
#endif
    }
}

/**
 * @brief 在中断处理函数中是否处于中断上下文([perf P-4] 内联, 消除函数调用)。
 */
static inline rtos_bool_t rtos_port_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile(" mrs %0, ipsr \n" : "=r"(ipsr) :: "memory");
    return (ipsr != 0U) ? RTOS_TRUE : RTOS_FALSE;
}

/** @brief 进入临界区(宏形式, 兼容既有调用)。 */
#define RTOS_PORT_ENTER_CRITICAL() rtos_port_enter_critical()

/** @brief 退出临界区(宏形式, 兼容既有调用)。 */
#define RTOS_PORT_EXIT_CRITICAL() rtos_port_exit_critical()

/** @brief 中断上下文判断(宏形式, 兼容既有调用)。 */
#define RTOS_PORT_IN_ISR() rtos_port_in_isr()

/** @brief 禁用全局中断(最高级别保护，尽量少用)。 */
#define RTOS_PORT_DISABLE_INTERRUPTS() rtos_port_disable_interrupts()

/** @brief 恢复全局中断(与 DISABLE 配对)。 */
#define RTOS_PORT_ENABLE_INTERRUPTS() rtos_port_enable_interrupts()

/* ============================== 栈与对齐 ============================== */

/** @brief 栈数据单元类型(Cortex-M 为 32 位)。 */
typedef uint32_t rtos_stack_t;

/** @brief 栈对齐掩码(8 字节对齐)。 */
#define RTOS_PORT_STACK_ALIGN_MASK ((rtos_stack_t)(RTOS_CONFIG_STACK_ALIGNMENT - 1U))

/** @brief EXC_RETURN 值: 返回线程模式 + 使用 PSP + 基本 FPU 帧。
 *         bit4=0(基本帧 0x20 字), bit2=1(PSP), bit3=1(线程模式), bit4=0(有 FPU)。
 *         0xFFFFFFFD = 无 FPU 上下文返回线程模式 PSP。
 *         0xFFFFFFED = 带 FPU 上下文返回线程模式 PSP。 */
#define RTOS_PORT_EXC_RETURN_NO_FPU (0xFFFFFFFDUL)
#define RTOS_PORT_EXC_RETURN_WITH_FPU (0xFFFFFFEDUL)

/* ============================== 移植层函数声明 ============================== */

/**
 * @brief 初始化任务栈，使其看起来像被中断打断的任务。
 * @param stack_top    栈顶指针(已对齐)。
 * @param stack_size   栈大小(字数)。
 * @param entry        任务入口函数。
 * @param arg          传递给任务的参数。
 * @return 初始化后的栈指针(指向应保存的最低地址)。
 *
 * @details 在栈中按以下布局(低地址 -> 高地址)构造伪现场:
 *            [R3 填充][EXC_RETURN][R4..R11][(S16..S31)][R0..R3|R12|LR|PC|xPSR]
 *            ^返回 sp                                          ^栈顶
 *          压栈顺序(高地址 -> 低地址，使用 *--sp):
 *            xPSR, PC(=entry), LR(=exit), R12, R3, R2, R1, R0(=arg),
 *            (S31..S16 若启用 FPU), R11..R4, EXC_RETURN, R3 填充。
 *          顺序必须与 PendSV 的 STMDB 保存顺序一致，保证首次切换
 *          出栈即从 entry 开始执行，并接收 arg 作为参数。
 */
rtos_stack_t *rtos_port_init_stack(rtos_stack_t *stack_top, uint32_t stack_size,
                                   rtos_task_func_t entry, void *arg);

/**
 * @brief 触发上下文切换(请求 PendSV)。
 * @details 设置 ICSR.PENDSVSET 位，PendSV 优先级最低，会在所有中断退出后执行。
 */
void rtos_port_context_switch(void);

/**
 * @brief 从首个任务启动(不再返回)。
 * @details 加载第一个任务的栈指针，设置 PSP，触发异常返回进入线程模式。
 */
void rtos_port_start_first_task(void) __attribute__((noreturn));

/**
 * @brief 启动调度器: 使能 FPU 惰性堆栈, 配置 SysTick 周期中断。
 * @details 中断优先级由用户工程配置(见移植文档), 本函数不设置 PendSV/SysTick 优先级。
 */
void rtos_port_start_scheduler(void);

/**
 * @brief SysTick 中断处理函数(由移植层提供，调用内核 Tick 处理)。
 */
void rtos_port_sys_tick_handler(void);

/**
 * @brief PendSV 中断处理函数(由移植层汇编实现上下文切换)。
 */
void rtos_port_pend_sv_handler(void) __attribute__((naked));

/**
 * @brief SVC 中断处理函数(由移植层汇编实现首个任务启动)。
 * @details 在 Handler 模式下加载首个任务栈并异常返回进入线程模式。
 *          由 rtos_port_start_first_task() 触发 SVC 进入。
 *
 *          必须在 Handler 模式下执行 BX LR 才能触发异常返回,
 *          Thread 模式下 BX 到 EXC_RETURN 魔数值会触发 HardFault。
 */
void rtos_port_svc_handler(void) __attribute__((naked));

/** @brief 关闭全局中断。 */
void rtos_port_disable_interrupts(void);

/** @brief 开启全局中断。 */
void rtos_port_enable_interrupts(void);

/**
 * @brief [guard G-3] 返回当前激活中断的 NVIC 优先级(已移位到高 4 位)。
 * @details 线程模式返回 0。用于 ISR-API 入口断言: 优先级数值高于
 *          MAX_SYSCALL(即数值 < 0x50)的中断违规调用内核 API 时,
 *          BASEPRI 屏蔽失效, 会静默破坏内核数据结构(FreeRTOS 的
 *          vPortValidateInterruptPriority 等价物)。
 */
uint32_t rtos_port_isr_priority(void);

/**
 * @brief 获取当前中断优先级屏蔽后的允许状态(用于 ISR 内核调用保护)。
 */
uint32_t rtos_port_get_interrupt_mask(void);

/**
 * @brief 设置中断屏蔽级别。
 */
void rtos_port_set_interrupt_mask(uint32_t mask);

/**
 * @brief 原子化的临界区计数读取(用于断言 ISR 中不可调用阻塞 API)。
 */
uint32_t rtos_port_get_critical_nesting(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PORT_H_ */

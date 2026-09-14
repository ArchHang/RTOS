/**
 * @file    rtos_config.h
 * @brief   RTOS 配置参数定义
 *
 * @details 本文件集中定义 RTOS 内核的编译期可配置参数。用户可根据目标芯片
 *          的实际资源(Flash/RAM 大小、任务规模、Tick 频率等)在此处裁剪内核。
 *          所有数值均为编译期常量，不占用运行时 RAM。
 *
 *          本 RTOS 仅提供内核与移植层源码，不捆绑启动文件/链接脚本/Makefile/
 *          时钟配置。这些属于目标 SDK(如 STM32CubeMX 生成的工程)的职责。
 *          用户只需将 Core/ 与 Port/ 源文件加入 SDK 工程编译即可。
 *
 *          移植层自动识别 ARM 内核(通过编译器预定义宏 __ARM_ARCH_7EM__ 等)，
 *          无需用户指定芯片型号。当前 port.c 支持 Cortex-M4F/M7F(覆盖
 *          STM32F4/F7/L4/H7 等系列的同内核型号)。
 *
 *          配置原则:
 *            - 任务优先级数与栈大小直接影响 RAM 占用，需按实际任务数评估。
 *            - Tick 频率越高，调度精度越高但 CPU 开销越大。常用 1kHz。
 *            - 关闭未使用的功能模块可在资源紧张时节省 Flash。
 */
#ifndef RTOS_CONFIG_H_
#define RTOS_CONFIG_H_

#include <stdint.h>

/* ============================== 调度器配置 ============================== */

/** @brief 内核 Tick 频率(Hz)。1kHz 对应 1ms 调度粒度。 */
#define RTOS_CONFIG_TICK_RATE_HZ (1000U)

/** @brief 支持的最大任务优先级数(0 ~ N-1)。数值越大 RAM 中就绪表越大。 */
#define RTOS_CONFIG_MAX_PRIORITIES (32U)

/** @brief 是否启用时间片轮转(同优先级任务轮流执行)。1=启用, 0=禁用。 */
#define RTOS_CONFIG_USE_TIME_SLICING (1)

/** @brief 是否启用抢占式调度。1=启用(高优先级可立即抢占低优先级)。 */
#define RTOS_CONFIG_USE_PREEMPTION (1)

/* ============================== 任务配置 ============================== */

/** @brief 默认任务栈大小(单位: 字, 即 4 字节)。Cortex-M EABI 要求 8 字节栈对齐。 */
#define RTOS_CONFIG_MINIMAL_STACK_SIZE (128U)

/** @brief 空闲任务栈大小(单位: 字)。 */
#define RTOS_CONFIG_IDLE_TASK_STACK_SIZE (128U)

/** @brief 系统空闲时调用的钩子函数开关。1=启用 rtos_idle_hook()。 */
#define RTOS_CONFIG_USE_IDLE_HOOK (1)

/** @brief 任务删除时的清理钩子开关。1=启用 rtos_task_delete_hook()。 */
#define RTOS_CONFIG_USE_DELETE_HOOK (0)

/** @brief 任务名最大长度(含结束符)。 */
#define RTOS_CONFIG_MAX_TASK_NAME_LEN (16U)

/* ============================== 内存配置 ============================== */

/** @brief 是否使用静态分配的任务池(1)或动态堆分配(0)。 */
#define RTOS_CONFIG_USE_STATIC_ALLOCATION (1)

/** @brief 静态任务池容量(最大任务数)。 */
#define RTOS_CONFIG_MAX_TASKS (16U)

/** @brief 动态内存堆大小(字节)，仅当 USE_STATIC_ALLOCATION=0 时生效。 */
#define RTOS_CONFIG_HEAP_SIZE (32U * 1024U)

/* ============================== 通信机制配置 ============================== */

/** @brief 最大信号量数量(静态分配时)。 */
#define RTOS_CONFIG_MAX_SEMAPHORES (8U)

/** @brief 最大互斥锁数量(静态分配时)。 */
#define RTOS_CONFIG_MAX_MUTEXES (8U)

/** @brief 最大消息队列数量(静态分配时)。 */
#define RTOS_CONFIG_MAX_QUEUES (8U)

/** @brief 最大事件标志组数量(静态分配时)。 */
#define RTOS_CONFIG_MAX_EVENT_GROUPS (8U)

/** @brief 是否启用软件定时器。1=启用。 */
#define RTOS_CONFIG_USE_TIMERS (1)

/** @brief 软件定时器任务栈大小(字)。 */
#define RTOS_CONFIG_TIMER_TASK_STACK_SIZE (256U)

/** @brief 软件定时器任务优先级(建议较高)。 */
#define RTOS_CONFIG_TIMER_TASK_PRIORITY ((RTOS_CONFIG_MAX_PRIORITIES - 2U))

/* ============================== 调试与统计 ============================== */

/** @brief 是否启用 CPU 利用率统计(基于空闲任务运行计数)。 */
#define RTOS_CONFIG_GENERATE_RUN_TIME_STATS (1)

/** @brief 是否启用栈溢出检测。0=关闭, 1=方法1(快速), 2=方法2(更全面)。 */
#define RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW (2)

/** @brief 是否启用断言。调试阶段建议启用，发布时关闭以节省开销。 */
#define RTOS_CONFIG_ASSERT_ENABLED (1)

/** @brief 断言失败处理宏。可重定向到自定义处理器。 */
#if RTOS_CONFIG_ASSERT_ENABLED
#define RTOS_ASSERT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond))                                                                               \
            rtos_assert_fail(#cond, __FILE__, __LINE__);                                           \
    } while (0)
#else
#define RTOS_ASSERT(cond) ((void)0)
#endif

/* ============================== 性能监视器 ============================== */

/** @brief 是否启用性能监视器(1=启用, 0=关闭)。
 *         启用后提供 CPU 使用率/任务运行时间/切换频率/调度延迟等指标的实时查询。
 *         关闭时所有性能 API 编译为空, 零开销。 */
#define RTOS_CONFIG_USE_PERF_MONITOR (1)

/** @brief 性能监视器是否使用 DWT 周期计数器(Cortex-M3/M4/M7 有 DWT)。
 *         1=使用 DWT_CYCCNT(周期级精度), 0=回退到 Tick 级精度(1ms)。
 *         Cortex-M0/M0+ 无 DWT, 必须设为 0。 */
#define RTOS_CONFIG_PERF_USE_DWT (1)

/** @brief 性能监视器任务注册表容量(>= RTOS_CONFIG_MAX_TASKS)。
 *         用于支持遍历所有任务统计。 */
#define RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE (RTOS_CONFIG_MAX_TASKS + 2U)

/** @brief 性能监视器内存池注册表容量(用户注册的自定义内存池数上限)。 */
#define RTOS_CONFIG_PERF_POOL_REGISTRY_SIZE (8U)

/** @brief 是否在任务创建时填充栈为固定模式以支持栈高水位检测。
 *         1=填充 0xA5A5A5A5, 可测量历史最大栈使用量;
 *         0=不填充(仅测量当前栈指针位置, 开销更小)。 */
#define RTOS_CONFIG_PERF_STACK_WATERMARK (1)

/* ============================== USB 子系统 ============================== */

/** @brief 是否启用 USB 子系统(1=启用, 0=关闭)。
 *         启用后提供 USB OSAL 适配层, 可对接 CherryUSB 等外部 USB 协议栈。
 *         关闭时所有 USB API 编译为空, 零代码零 RAM, 不影响其他模块。 */
#define RTOS_CONFIG_USE_USB (1)

/** @brief USB 设备模式适配(1=允许创建设备栈线程/对象, 0=禁用)。 */
#define RTOS_CONFIG_USB_DEVICE_MODE (1)
/** @brief USB 主机模式适配(1=允许创建主机栈线程/对象, 0=禁用)。 */
#define RTOS_CONFIG_USB_HOST_MODE (0)

/** @brief USB OSAL 信号量对象池容量。 */
#define RTOS_CONFIG_USB_OSAL_SEM_COUNT (8U)
/** @brief USB OSAL 互斥锁对象池容量。 */
#define RTOS_CONFIG_USB_OSAL_MUTEX_COUNT (8U)
/** @brief USB OSAL 消息队列对象池容量。 */
#define RTOS_CONFIG_USB_OSAL_MQ_COUNT (4U)
/** @brief USB OSAL 线程对象池容量(每个对象含内嵌栈, 勿设过大以免浪费 RAM)。 */
#define RTOS_CONFIG_USB_OSAL_THREAD_COUNT (4U)
/** @brief USB OSAL 定时器对象池容量。 */
#define RTOS_CONFIG_USB_OSAL_TIMER_COUNT (8U)

/** @brief USB 线程内嵌栈大小(字, 4 字节/字)。建议 >= 512 (2KB)。
 *         CherryUSB hub/class 线程通常需 1~2KB 栈。 */
#define RTOS_CONFIG_USB_THREAD_STACK_SIZE (512U)

/** @brief USB 线程默认优先级(建议中等, 低于实时控制任务)。
 *         0=最高。设为 MAX_PRIORITIES/2 为中等。 */
#define RTOS_CONFIG_USB_THREAD_PRIORITY (RTOS_CONFIG_MAX_PRIORITIES / 2U)

/** @brief USB 消息队列默认深度(条数)。CherryUSB mq 传递 uintptr_t 事件。 */
#define RTOS_CONFIG_USB_MQ_DEPTH (16U)

/** @brief USB ISR 注册表容量(支持的中断号映射数, 通常 = USB 控制器数)。 */
#define RTOS_CONFIG_USB_ISR_TABLE_SIZE (2U)

/** @brief 是否启用 MSC 大容量存储(与 CDC 组成复合设备, 1=启用, 0=仅 CDC)。
 *         启用后单个 USB 设备同时枚举出虚拟串口 + U盘(复合设备)。 */
#define RTOS_CONFIG_USB_USE_MSC (1)

/** @brief 是否启用 HID 人机接口设备(自定义厂商定义报告, 64B IN/OUT)。
 *         与 CDC/MSC 组成复合设备: 虚拟串口 + U盘 + HID 设备三合一。
 *         注意: CDC+MSC+HID 共需 4 个 IN 端点 FIFO, 已通过 DWC2 自定义
 *         FIFO 划分适配 F446 OTG_FS(320 字 FIFO, 端点号 <= 5)。 */
#define RTOS_CONFIG_USB_USE_HID (1)

/** @brief MSC RAM 盘大小(字节, 必须是 512 的整数倍)。
 *         上电后主机可将其格式化为 FAT 等文件系统(断电丢数据)。 */
#define RTOS_CONFIG_USB_MSC_RAM_DISK_SIZE (32U * 1024U)

/** @brief MSC 是否只读(1=主机侧只读, 写请求返回失败; 0=可读写)。 */
#define RTOS_CONFIG_USB_MSC_READONLY (0)

/** @brief USB 堆总大小(字节), 用于 rtos_usb_malloc / usb_osal_malloc。
 *         分配 URB / 描述符缓冲 / class 结构等大小不一的对象。
 *         内部采用双档固定块池实现: 小块(<=64B)与大块(<=512B), O(1) 无碎片。 */
#define RTOS_CONFIG_USB_HEAP_SIZE (8U * 1024U)

/* ============================== 对齐与临界区 ============================== */

/** @brief 栈按 8 字节对齐(Cortex-M EABI 要求 8 字节栈对齐)。 */
#define RTOS_CONFIG_STACK_ALIGNMENT (8U)

/** @brief 是否使用 BASEPRI 屏蔽中断(1)或 PRIMASK 全屏蔽(0)。
 *         Cortex-M3/M4/M7 推荐使用 BASEPRI，保留高优先级中断响应能力。
 *         Cortex-M0/M0+ 无 BASEPRI 寄存器，必须设为 0(用 PRIMASK)。 */
#define RTOS_CONFIG_USE_BASEPRI (1)

/** @brief BASEPRI 屏蔽阈值。优先级数值 >= 此值的中断被屏蔽。
 *         STM32 系列均使用 4 位优先级(0~15)，0 为最高。设为 5 可屏蔽 5~15，
 *         保留 0~4 给高优先级实时中断(如电机控制)。
 * @note   阈值已左移 4 位(高 4 位有效)，即 (5 << 4) = 0x50。 */
#define RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY (5U << 4)

#endif /* RTOS_CONFIG_H_ */

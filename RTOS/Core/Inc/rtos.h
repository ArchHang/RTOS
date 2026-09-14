/**
 * @file    rtos.h
 * @brief   RTOS 顶层 API 头文件
 *
 * @details 用户应用只需 #include "rtos.h" 即可使用全部 RTOS 功能。
 *          本文件汇总了内核各子模块的公共头文件，并提供版本信息。
 *
 *          架构总览:
 *
 *            +-------------------------------------------+
 *            |              用户应用层                   |
 *            +-------------------------------------------+
 *            | rtos_task | rtos_sem | rtos_mutex | ...   |  <-- 公共 API
 *            +-------------------------------------------+
 *            |         rtos_sched (调度器)               |  <-- 内核核心
 *            +-------------------------------------------+
 *            |         rtos_port (Cortex-M4F/M7F 移植)  |  <-- 架构相关
 *            +-------------------------------------------+
 *            |        用户 SDK + Cortex-M 硬件           |
 *            +-------------------------------------------+
 *
 *          数据流(典型任务切换):
 *            1. SysTick 触发 -> rtos_port_sys_tick_handler()
 *            2. 调度器更新 tick_count, 检查延时表
 *            3. 若更高优先级任务就绪 -> 置 schedule_pending
 *            4. 退出中断时 PendSV 触发上下文切换
 *            5. PendSV 保存当前任务 R4-R11/S16-S31, 加载下一任务
 */
#ifndef RTOS_H_
#define RTOS_H_

#include "rtos_config.h"
#include "rtos_types.h"
#include "rtos_port.h"
#include "rtos_sched.h"
#include "rtos_task.h"
#include "rtos_sem.h"
#include "rtos_mutex.h"
#include "rtos_queue.h"
#include "rtos_event.h"
#include "rtos_timer.h"
#include "rtos_mem.h"
#include "rtos_heap.h"
#include "rtos_perf.h"
#include "rtos_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief RTOS 版本: 主.次.补丁。 */
#define RTOS_VERSION_MAJOR (1U)
#define RTOS_VERSION_MINOR (0U)
#define RTOS_VERSION_PATCH (0U)
#define RTOS_VERSION_STRING "1.0.0"

/** @brief 获取版本号字符串。 */
const char *rtos_get_version(void);

/**
 * @brief 内核初始化: 初始化调度器、内存池、各对象池。
 * @return RTOS_OK 或错误码。
 * @note  必须在 rtos_sched_start() 之前调用，且只在系统启动时调用一次。
 */
rtos_status_t rtos_init(void);

/**
 * @brief 启动 RTOS 调度器(不返回)。
 * @details 内部调用 rtos_sched_start()，启动后空闲任务首先运行。
 */
rtos_status_t rtos_start(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* RTOS_H_ */

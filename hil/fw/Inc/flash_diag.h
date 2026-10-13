/**
 * @file flash_diag.h
 * @brief 测试固件的 STM32F446 Flash 指令缓存诊断接口。
 *
 * 回调在中断开启时运行，可阻塞和触发 PendSV。诊断不改变生产固件配置。
 */
#ifndef F446_RTOS_HIL_FW_INC_FLASH_DIAG_H_
#define F446_RTOS_HIL_FW_INC_FLASH_DIAG_H_

#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 完整运行一次基准，并清理本次创建的任务和对象。
 *
 * 回调负责自身计时与结果输出，必须保持调用者的中断屏蔽状态、临界区深度
 * 和调度器状态；不得改变 Flash 配置，也不得进行 Flash 编程或擦除。
 */
typedef void (*flash_diag_benchmark_t)(void);

/**
 * @brief 在同一 ELF 内按指令缓存开启、关闭、开启的顺序运行基准。
 *
 * @param[in] benchmark_callback 完整基准回调，不可为 NULL。
 * @return RTOS_OK 表示诊断检查通过且原始 FLASH_ACR 已恢复；RTOS_ERR_NULL
 *         表示回调为空；RTOS_ERR_SCHED 表示运行上下文不允许阻塞；
 *         RTOS_ERR_PARAM 表示缓存状态、资源清理或诊断输出检查失败。
 *
 * 仅限 FIRMWARE_FLASH_DIAG=1 的 STM32F446 Cortex-M4 测试固件。调用方必须处于
 * 特权任务上下文，调度器正在运行，PRIMASK、BASEPRI、FAULTMASK 和临界区深度
 * 均为零。缓存切换短暂屏蔽 IRQ；基准执行期间保持 IRQ 开启。仅操作 ICEN/ICRST，
 * 保留 LATENCY、PRFTEN 和 DCEN。正常和失败退出均核验原始 ACR 恢复情况。
 * 上下文或空回调拒绝发生在任何缓存修改之前，不在不安全上下文打印串口日志。
 */
rtos_status_t flash_diag_run(flash_diag_benchmark_t benchmark_callback);

#ifdef __cplusplus
}
#endif

#endif /* F446_RTOS_HIL_FW_INC_FLASH_DIAG_H_ */

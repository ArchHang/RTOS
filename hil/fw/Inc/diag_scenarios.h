/**
 * @file    diag_scenarios.h
 * @brief   HIL 诊断场景接口(仅用于测试夹具的自我核准, 不属于产品代码)。
 *
 * @details 与 test_scenarios.c 共用 test_framework.c 的统计/打印设施。
 *          由 hil\build_fw.ps1 -Variant diag 编译进固件, 通过
 *          TEST_SCENARIO_SELECT 选择:
 *            11 -> D1: S1 争用取证(复刻 S1 顺序, 记录是否真发生争用)
 *            16 -> D4: event_wait 陈旧 queue_bitmap 不变式
 *            17 -> D5: timer_change_period 周期上界
 *            18 -> D6: 初始栈帧 PC 的 bit0(Thumb 位)是否必须清零
 *            19 -> D7: delay() 成本随延时链长度的变化(O(n) 插入探测)
 *            20 -> D8: sem_take 阻塞成本随等待者数的变化(O(n) 尾插探测)
 *            21 -> D9: 最坏关中断窗口(WCET)探测
 *            22 -> D10: sched_suspend 期间阻塞 API 的拒绝契约
 *
 *          各场景的结论状态(哪些数字可引用)见 hil\README.md §4.1。
 *
 * @note    本文件不参与正式产品构建: 默认固件的 main.c 不会引用它。
 */
#ifndef DIAG_SCENARIOS_H_
#define DIAG_SCENARIOS_H_

#include <stdint.h>

/** @brief D1: 记录 S1 夹具是否真正制造了互斥锁争用。 */
int32_t diag_s1_forensics(void);

/** @brief D4: 事件等待期间 queue_bitmap 必须为 NULL(陈旧指针检查)。 */
int32_t diag_event_stale_bitmap(void);

/** @brief D5: timer_change_period 必须拒绝超出带符号域的周期。 */
int32_t diag_timer_period_bound(void);

/** @brief D6: 判定初始栈帧 PC 的 bit0(Thumb 位)是否必须清零。 */
int32_t diag_pc_thumb_bit(void);

/** @brief D7: delay() 成本随延时链长度的变化(O(n) 插入探测)。⚠ 已实现, 尚未采集完整数据。 */
int32_t diag_delay_list_scaling(void);

/** @brief D8: sem_take 阻塞成本随等待者数的变化(O(n) 尾插探测)。⚠ 已实现, 尚未采集完整数据。 */
int32_t diag_wait_chain_scaling(void);

/** @brief D9: 最坏关中断窗口探测 —— ⚠ 口径未收敛, 数字不可引用(见 .c 内说明)。 */
int32_t diag_interrupt_blackout(void);

/** @brief D10: 挂起调度器期间, 阻塞 API 必须被拒绝、非阻塞路径不受影响 —— 已上板验证。 */
int32_t diag_sched_suspend(void);

#endif /* DIAG_SCENARIOS_H_ */
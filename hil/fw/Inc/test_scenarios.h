/**
 * @file    test_scenarios.h
 * @brief   系统级真实场景测试套件(自研 RTOS 与 FreeRTOS 共用同一份实现)。
 *
 * @details 与既有 benchmark.c 的区别: benchmark.c 测的是单个内核 API 的
 *          单次调用延迟(微基准); 本套件测的是多个任务、多个同步对象与中断
 *          共同工作时的系统行为 —— 吞吐、抖动、最坏情况, 以及随任务数增长
 *          的伸缩性。
 *
 *          全部代码只调用 kern_port.h 的统一接口, 不出现任何内核专有类型,
 *          因此同一份 .c 可编译进两套固件, 保证对比口径完全一致。
 *
 *          场景清单:
 *            S1 优先级反转   : 低优先级持锁 + 中优先级干扰者时的头阻塞延迟
 *            S2 等待者伸缩性 : N 个任务阻塞在同一信号量上的 give 成本
 *            S3 混合负载管线 : 3 生产 + 2 消费 + 5ms 控制环的吞吐与抖动
 *            S4 周期控制抖动 : 高优先级控制环 20s 长跑的唤醒偏差
 *            S5 内存足迹     : 建立等价任务集所需的内核对象创建能力
 */
#ifndef TEST_SCENARIOS_H_
#define TEST_SCENARIOS_H_

#include <stdint.h>

/**
 * @brief 本次固件运行哪一个场景(由构建脚本按 -D 传入, 也可在此改)。
 * @details 每次固件只跑一个场景: 避免场景之间的时序干扰, 也让每次烧录的
 *          运行时间可控。取值 1..5。
 */
#ifndef TEST_SCENARIO_SELECT
#define TEST_SCENARIO_SELECT 1
#endif

/**
 * @brief 运行指定编号的场景。
 * @param scenario_id 1..5, 见文件头清单。
 * @return 0 成功, 负值失败(参数非法或内核对象创建失败)。
 */
int32_t test_scenario_run(uint32_t scenario_id);

/**
 * @brief 场景测试任务入口(由各工程的 main 创建)。
 * @param arg 未使用。
 *
 * @details 按 TEST_SCENARIO_SELECT 选择场景, 打印 META/START/DONE 行后自删除,
 *          便于上位机判定运行结束。
 */
void test_scenario_task(void *arg);

#endif /* TEST_SCENARIOS_H_ */

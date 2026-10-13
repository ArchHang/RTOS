/**
 * @file    boundary_test.h
 * @brief   自研 RTOS 边界与稳定性能测试夹具接口
 *
 * @details 与 benchmark.c(纯性能微基准) 和 selftest.c(全量功能回归) 分工不同:
 *          本夹具专注于**契约边界与非法输入的稳定性**, 并顺带复测各快路径的
 *          周期开销。用例按"有效值 / 下边界 / 上边界 / 超出上边界 /
 *          超出下边界 / 空值"六类组织, 每类覆盖多个 API。
 *
 *          输出格式(USART2 115200):
 *            BT|META|<target>|<limits...>
 *            BT|<分类>|<用例>|<PASS/FAIL>|<实际值>|<期望值>
 *            BT|PERF|<指标>|avg=<周期>|min=<>|max=<>
 *            BT|SUMMARY|pass=<n> fail=<n>
 *
 * @note    与 selftest 的边界用例**有意不重复**: selftest 覆盖"典型非法值"
 *          (如 stack=39、prio=32), 本夹具覆盖**契约极值**
 *          (如 stack 恰好 96 与 95、堆上限恰好等于与超出 1 字、UINT32_MAX)。
 */
#ifndef BOUNDARY_TEST_H_
#define BOUNDARY_TEST_H_

#include <stdint.h>

/**
 * @brief 边界与稳定性测试任务入口。
 * @param arg 未使用(按 rtos_task_func_t 约定接收任务参数)。
 *
 * @details 以优先级 9 创建, 会自行完成全部用例并打印 SUMMARY, 之后进入
 *          长期 delay 循环(不返回、不自杀, 与 benchmark/bench_task 一致,
 *          便于上位机在 SUMMARY 之后继续观察系统稳定性)。
 */
void boundary_test_task(void *arg);

#endif /* BOUNDARY_TEST_H_ */

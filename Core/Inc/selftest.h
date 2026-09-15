/**
 * @file    selftest.h
 * @brief   RTOS 真机自测试固件接口(HIL)
 *
 * @details 三种角色:
 *            1. selftest_pre_start(): 在 rtos_start() 之前调用(main 中),
 *               测试"调度器未启动"上下文的安全 API 行为。
 *            2. selftest_task(): 调度器启动后运行的全量功能测试主任务。
 *            3. selftest_uart_isr_hook(): 由 USART2 RX 中断回调调用,
 *               在真实 ISR 上下文中执行 API 错误码矩阵测试。
 *
 *          输出格式(USART2 115200): ST|<模块>|<用例>|<PASS/FAIL/INFO>|<详情>
 */
#ifndef SELFTEST_H_
#define SELFTEST_H_

#include <stdint.h>

/** 在 rtos_init() 之后、rtos_start() 之前调用(main 上下文)。 */
void selftest_pre_start(void);

/** 主测试任务入口(优先级 9, 栈 1024 字)。 */
void selftest_task(void *arg);

/**
 * USART2 RX 中断钩子。
 * @param byte 收到的字节。
 * @return 1 = 已消费(测试触发字节 'X'), 调用方应继续 HAL 接收并直接返回;
 *         0 = 未消费, 调用方按原逻辑处理。
 */
uint8_t selftest_uart_isr_hook(uint8_t byte);

#endif /* SELFTEST_H_ */

/**
 * @file    benchmark.h
 * @brief   RTOS 性能基准测试接口(与 FreeRTOS 版镜像同构)
 */
#ifndef BENCHMARK_H_
#define BENCHMARK_H_

#include <stdint.h>

/** 基准主任务入口(优先级 9, 栈 1024 字)。 */
void bench_task(void *arg);

#endif /* BENCHMARK_H_ */

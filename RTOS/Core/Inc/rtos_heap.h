/**
 * @file    rtos_heap.h
 * @brief   动态堆内存分配器接口
 *
 * @details 提供线程安全的通用堆分配器, 用于动态创建任务 TCB 与栈
 *          等场景。分配器采用 first-fit 策略 + 块合并,
 *          在保证实时性(O(n) 最坏复杂度, n = 空闲块数)的同时减少碎片。
 *
 *          堆内存在 rtos_init() 阶段一次性初始化, 使用静态缓冲区
 *          RTOS_CONFIG_HEAP_SIZE 字节, 不依赖 C 标准库 malloc。
 *
 *          线程安全: 内部使用临界区(BASEPRI)保护空闲链表,
 *          可在任务上下文与中断上下文(短时)中调用。建议仅在任务上下文使用。
 *
 *          块布局(已对齐):
 *            ┌──────────┐
 *            │ header_t │  size + magic(0xA5A5A5A5)
 *            ├──────────┤
 *            │ user data│  返回给用户的指针指向此处
 *            │ ...      │
 *            └──────────┘
 *          header 大小 = sizeof(header_t), 8 字节对齐保证用户数据 8 字节对齐。
 */
#ifndef RTOS_HEAP_H_
#define RTOS_HEAP_H_

#include <stdint.h>
#include <stddef.h>
#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化堆。
 * @details 使用内部静态缓冲区(RTOS_CONFIG_HEAP_SIZE 字节)构建空闲链表。
 *          必须在 rtos_init() 中调用一次, 之后 rtos_heap_alloc/free 可用。
 *          重复调用会被忽略(返回 RTOS_OK)。
 */
rtos_status_t rtos_heap_init(void);

/**
 * @brief 分配 size 字节内存。
 * @param size 请求字节数, 内部向上对齐到 8 字节边界。
 * @return 成功返回 8 字节对齐的指针; 失败(堆空/size=0)返回 NULL。
 *
 * @note 线程安全, 可在任务上下文调用。中断中调用需谨慎(临界区开销)。
 */
void *rtos_heap_alloc(uint32_t size);

/**
 * @brief 释放 rtos_heap_alloc 返回的指针。
 * @param ptr 之前分配的指针, NULL 时无操作。
 * @note 释放后会与相邻空闲块合并以减少碎片。
 */
void rtos_heap_free(void *ptr);

/**
 * @brief 查询堆空闲字节数(粗略, 不含 header 开销)。
 */
uint32_t rtos_heap_get_free(void);

/**
 * @brief 查询当前**单次可分配的最大字节数**。
 *
 * @return 当前**最大单块连续空闲块**可供用户使用的字节数(已扣除该块的块头,
 *         并向下对齐到 8 字节); 堆未初始化时返回 0。
 *
 * @details 与 rtos_heap_get_free 的区别: 后者是空闲字节总和, 直接用它会高估 ——
 *          分配器是 first-fit, 只会在**一个连续块**内满足请求, 碎片化时空闲
 *          总和远大于任何单块。本函数返回的是"在堆状态不变的前提下, 一次
 *          rtos_heap_alloc(size) 当且仅当 size <= 本返回值时可能成功"。
 *
 *          用途: 调用方(如 rtos_task_create)需要在校验阶段判断"某尺寸是否
 *          可能满足", 用本函数可避免给出实际不可达的假上界
 *          (见 rtos_task_create 的边界修复说明)。
 *
 * @note 本函数在临界区内遍历块链, 复杂度 O(空闲块数), 与 rtos_heap_alloc
 *       同级; 仅供创建/校验等非热路径使用。
 */
uint32_t rtos_heap_get_max_alloc(void);

/** @brief 查询累计分配次数(调试用)。 */
uint32_t rtos_heap_get_alloc_count(void);

/** @brief 查询累计释放次数(调试用)。 */
uint32_t rtos_heap_get_free_count(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_HEAP_H_ */

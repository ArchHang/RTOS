/**
 * @file    rtos_heap.h
 * @brief   动态堆内存分配器接口
 *
 * @details 提供线程安全的通用堆分配器, 用于动态创建任务 TCB 与栈
 *          等场景。分配器采用 first-fit 策略 + 块合并,
 *          分配最坏 O(n)，n 为物理块总数，释放 O(1)。
 *
 *          堆内存在 rtos_init() 阶段一次性初始化, 使用静态缓冲区
 *          RTOS_CONFIG_HEAP_SIZE 字节, 不依赖 C 标准库 malloc。
 *
 *          线程安全: 内部使用临界区(BASEPRI)保护空闲链表,
 *          分配、释放及最大单块查询仅允许任务上下文或系统调用级中断调用。
 *          BASEPRI 配置下, 中断优先级数值必须 >=
 *          RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY(默认 NVIC 优先级 5~15)。
 *          优先级 0~4 的中断禁止调用这些操作; 关闭断言后仍须遵守此契约。
 *          遍历耗时与堆形状有关, 建议仅在任务上下文使用。
 *
 *          块布局(已对齐):
 *            ┌──────────┐
 *            │ block hdr│  size + magic(0xA5A5A5A5)
 *            ├──────────┤
 *            │ user data│  返回给用户的指针指向此处
 *            │ ...      │
 *            └──────────┘
 *          块头为 8 字节，用户数据为 8 字节对齐。
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
 * @details 使用内部静态缓冲区(RTOS_CONFIG_HEAP_SIZE 字节)构建首块，容量向下对齐到 8 字节。
 *          必须在 rtos_init() 中调用一次, 之后 rtos_heap_alloc/free 可用。
 *          仅供启动阶段调用, 不允许与运行中的堆操作并发。
 * @return 成功或重复调用返回 RTOS_OK，向下对齐后容量不足或超限返回 RTOS_ERR_PARAM。
 */
rtos_status_t rtos_heap_init(void);

/**
 * @brief 分配 size 字节内存。
 * @param size 请求字节数, 内部向上对齐到 8 字节边界。
 * @return 成功返回 8 字节对齐的指针，无法满足请求或 size=0 时返回 NULL。
 *
 * @note 线程安全, ISR 调用须满足本模块的系统调用优先级约束。
 *       临界区包含 O(n) 遍历，需要时合并连续空闲块，建议在任务上下文调用。
 */
void *rtos_heap_alloc(uint32_t size);

/**
 * @brief 释放 rtos_heap_alloc 返回的指针。
 * @param ptr 之前分配的指针, NULL 时无操作。
 * @note O(1)，最多合并紧邻的下一块。调用方必须传入仍有效的原始分配地址或 NULL。
 *       ISR 调用须满足本模块的系统调用优先级约束。
 */
void rtos_heap_free(void *ptr);

/**
 * @brief 查询堆空闲字节数(粗略, 不含 header 开销)。
 */
uint32_t rtos_heap_get_free(void);

/**
 * @brief 查询当前**单次可分配的最大字节数**。
 *
 * @return 当前最大连续空闲段合并后可提供的 8 字节对齐用户容量，未初始化时返回 0。
 *
 * @details 对连续空闲段只扣一个块头。rtos_heap_get_free 汇总现有空闲块的用户容量，
 *          排除所有现存块头。本查询不修改块布局，也不保留分配空间。
 *
 * @note 本函数在临界区内遍历块链，复杂度 O(物理块总数)，与 rtos_heap_alloc
 *       同级; 仅供创建/校验等非热路径使用。ISR 调用须满足本模块的
 *       系统调用优先级约束, 关闭断言不放宽该约束。
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

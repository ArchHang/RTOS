/**
 * @file    rtos_mem.h
 * @brief   内存管理接口
 *
 * @details 提供两种内存管理方案:
 *            1. 固定块内存池(Fixed Block Pool): O(1) 分配/释放，无碎片，
 *               适合实时系统。每个池内块大小固定。
 *            2. (可选)动态堆: 仅在 RTOS_CONFIG_USE_STATIC_ALLOCATION=0 时启用。
 *          本实现默认采用静态分配策略，对象池(TCB/信号量等)在编译期固定。
 *          固定块池用于应用层需要动态分配等长结构的场景(如网络数据包)。
 */
#ifndef RTOS_MEM_H_
#define RTOS_MEM_H_

#include <stdint.h>
#include <stddef.h>
#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct rtos_mempool
 * @brief  固定块内存池。
 */
typedef struct rtos_mempool {
    uint8_t *pool_base; /**< 池起始地址。 */
    uint32_t block_size; /**< 单块大小(已含对齐填充)。 */
    uint32_t block_count; /**< 块数。 */
    volatile uint32_t free_count; /**< 空闲块数。 */
    void *free_list; /**< 空闲链表头(指向第一个空闲块)。 */
} rtos_mempool_t;

/**
 * @brief 初始化内存池。
 * @param pool        池对象。
 * @param base        存储区基址(用户分配，需 4 字节对齐)。
 * @param block_size  每块大小(字节)。
 * @param block_count 块数。
 * @note 存储区大小需 >= block_size * block_count (内部会按 4 字节向上对齐)。
 */
rtos_status_t rtos_mempool_init(rtos_mempool_t *pool, void *base, uint32_t block_size,
                                uint32_t block_count);

/**
 * @brief 分配一块(固定大小，O(1))。
 * @return 块指针，NULL 表示池空。
 */
void *rtos_mempool_alloc(rtos_mempool_t *pool);

/**
 * @brief 释放块。
 * @param pool 池。
 * @param block 之前分配的块指针。
 */
rtos_status_t rtos_mempool_free(rtos_mempool_t *pool, void *block);

/** @brief 查询空闲块数。 */
uint32_t rtos_mempool_get_free(const rtos_mempool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_MEM_H_ */

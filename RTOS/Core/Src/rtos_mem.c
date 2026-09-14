/**
 * @file    rtos_mem.c
 * @brief   固定块内存池实现
 *
 * @details 固定块内存池提供 O(1) 分配/释放，无碎片，适合实时系统。
 *          原理:
 *            - 池内所有块大小相同(向上对齐到 4 字节)。
 *            - 空闲块用单向链表串联，每个空闲块的起始 4 字节存下一个空闲块指针。
 *            - 分配: 取链表头，head = head->next。
 *            - 释放: 块->next = head; head = 块。
 *          分配/释放均为常数时间，且无外部碎片(块大小固定)。
 *
 *          STM32 系列通常有较大 SRAM，可划分多个内存池服务于不同模块
 *          (如网络缓冲、显示帧缓冲)。具体容量由芯片型号决定。
 */
#include "rtos_mem.h"
#include "rtos_port.h"
#include "rtos_perf.h"
#include <string.h>

/** @brief 空闲链表节点(复用空闲块空间)。 */
typedef struct free_node {
    struct free_node *next;
} free_node_t;

rtos_status_t rtos_mempool_init(rtos_mempool_t *pool, void *base, uint32_t block_size,
                                uint32_t block_count)
{
    if ((pool == NULL) || (base == NULL) || (block_size == 0U) || (block_count == 0U)) {
        return RTOS_ERR_PARAM;
    }

    /* 块大小向上对齐到 4 字节(保证指针存储对齐) */
    uint32_t aligned_size = (block_size + 3U) & ~3U;
    if (aligned_size < sizeof(free_node_t)) {
        aligned_size = sizeof(free_node_t);
    }

    pool->pool_base = (uint8_t *)base;
    pool->block_size = aligned_size;
    pool->block_count = block_count;
    pool->free_count = block_count;

    /* 构建空闲链表: 每块起始存下一块指针 */
    free_node_t *prev = NULL;
    pool->free_list = NULL;
    for (uint32_t i = 0U; i < block_count; i++) {
        free_node_t *node = (free_node_t *)((uint8_t *)base + i * aligned_size);
        node->next = prev;
        prev = node;
    }
    pool->free_list = prev; /* 链表头 = 最后一块 */

    /* 性能监视器: 注册内存池, 供 rtos_perf_get_memory 聚合统计。
     * 内部有防重复注册与容量检查, 安全。 */
    rtos_perf_register_pool(pool);

    return RTOS_OK;
}

void *rtos_mempool_alloc(rtos_mempool_t *pool)
{
    if (pool == NULL) {
        return NULL;
    }

    void *block = NULL;
    RTOS_PORT_ENTER_CRITICAL();

    free_node_t *node = (free_node_t *)pool->free_list;
    if (node != NULL) {
        pool->free_list = node->next;
        pool->free_count--;
        block = (void *)node;
    }

    RTOS_PORT_EXIT_CRITICAL();
    return block;
}

rtos_status_t rtos_mempool_free(rtos_mempool_t *pool, void *block)
{
    if ((pool == NULL) || (block == NULL)) {
        return RTOS_ERR_NULL;
    }

    /* 合法性检查: 块地址是否在池范围内 */
    uint8_t *p = (uint8_t *)block;
    if ((p < pool->pool_base) || (p >= pool->pool_base + pool->block_size * pool->block_count)) {
        return RTOS_ERR_PARAM;
    }

    RTOS_PORT_ENTER_CRITICAL();
    free_node_t *node = (free_node_t *)block;
    node->next = (free_node_t *)pool->free_list;
    pool->free_list = node;
    pool->free_count++;
    RTOS_PORT_EXIT_CRITICAL();

    return RTOS_OK;
}

uint32_t rtos_mempool_get_free(const rtos_mempool_t *pool)
{
    return (pool != NULL) ? pool->free_count : 0U;
}

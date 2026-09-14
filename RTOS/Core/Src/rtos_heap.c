/**
 * @file    rtos_heap.c
 * @brief   动态堆内存分配器实现
 *
 * @details 采用 first-fit + 块合并策略, 实现简单且适合 RTOS 中等规模分配。
 *
 *          设计权衡:
 *            - 复杂度: 分配 O(n)/释放 O(1) 合并 O(n), n=空闲块数。
 *              对于 RTOS_CONFIG_HEAP_SIZE=32KB 与典型任务数(<30), 性能足够。
 *            - 碎片: 通过合并相邻空闲块减少外部碎片; 内部碎片由对齐产生(<=7B/块)。
 *            - 实时性: 临界区持有时间 = O(n) 链表遍历, 可预测。
 *
 *          块布局(已 8 字节对齐):
 *            ┌─────────────────┐
 *            │ header_t        │  magic + size(高 bit = free 标志)
 *            ├─────────────────┤
 *            │ user data       │  <- rtos_heap_alloc 返回此处
 *            │ ...             │
 *            └─────────────────┘
 *
 *          header_t 大小 = 8 字节(uint32_t magic + uint32_t size), 用户数据天然
 *          8 字节对齐。size 字段最高 bit(0x80000000)作为空闲标志:
 *            - 1 = 空闲
 *            - 0 = 已分配
 *          实际块大小 = size & ~FREE_BIT。
 *
 *          合并策略: 释放时检查下一块(通过当前块大小定位)是否空闲并合并。
 *          前向合并需遍历空闲链表查找前驱, 暂不实现(单向遍历成本高),
 *          在 alloc 路径中已部分抵消(连续空闲会被请求消耗)。
 *
 *          线程安全: 所有操作在 RTOS_PORT_ENTER/EXIT_CRITICAL 内完成。
 *          BASEPRI 屏蔽 syscall 级中断, 保证链表操作原子性。
 */
#include "rtos_heap.h"
#include "rtos_port.h"
#include "rtos_config.h"
#include <string.h>

/* ============================== 内部常量与类型 ============================== */

/** @brief 块 header 魔数(用于检测越界/双重释放)。 */
#define RTOS_HEAP_MAGIC (0xA5A5A5A5UL)

/** @brief size 字段空闲标志位(最高 bit)。 */
#define RTOS_HEAP_FREE_BIT (0x80000000UL)

/** @brief 块大小掩码(去掉空闲位)。 */
#define RTOS_HEAP_SIZE_MASK (~RTOS_HEAP_FREE_BIT)

/** @brief 最小分配大小(字节, 含 header)。小于此值的请求按此值分配。 */
#define RTOS_HEAP_MIN_BLOCK (16U)

/** @brief 对齐字节(8 字节, 与 Cortex-M EABI 栈对齐一致)。 */
#define RTOS_HEAP_ALIGN (8U)

/** @brief header 结构。 */
typedef struct {
    uint32_t magic; /**< 魔数(校验合法性)。 */
    uint32_t size; /**< 块总大小(含 header), 高 bit 为空闲标志。 */
} header_t;

/* ============================== 内部状态 ============================== */

/** @brief 堆静态缓冲区(RTOS_CONFIG_HEAP_SIZE 字节, 8 字节对齐)。 */
static uint8_t s_heap_buf[RTOS_CONFIG_HEAP_SIZE] __attribute__((aligned(RTOS_HEAP_ALIGN)));

/** @brief 已初始化标志(防止重复初始化)。 */
static rtos_bool_t s_inited = RTOS_FALSE;

/** @brief 堆总大小(字节)。 */
static uint32_t s_heap_total = 0U;

/** @brief 堆空闲字节数(粗略, 不含 header 开销)。 */
static uint32_t s_heap_free = 0U;

/** @brief 累计分配次数(统计用)。 */
static uint32_t s_alloc_count = 0U;

/** @brief 累计释放次数(统计用)。 */
static uint32_t s_free_count = 0U;

/* ============================== 内部函数 ============================== */

/** @brief 向上对齐到 RTOS_HEAP_ALIGN 字节。 */
static inline uint32_t align_up(uint32_t n)
{
    return (n + (RTOS_HEAP_ALIGN - 1U)) & ~(uint32_t)(RTOS_HEAP_ALIGN - 1U);
}

/** @brief 计算块用户数据指针(跳过 header)。 */
static inline void *block_to_user(header_t *hdr)
{
    return (void *)((uint8_t *)hdr + sizeof(header_t));
}

/** @brief 由用户指针反查 header。 */
static inline header_t *user_to_block(void *ptr)
{
    return (header_t *)((uint8_t *)ptr - sizeof(header_t));
}

/** @brief 取下一块(按当前块大小定位)。NULL 表示到达堆末尾。 */
static inline header_t *block_next(header_t *hdr)
{
    uint8_t *next = (uint8_t *)hdr + (hdr->size & RTOS_HEAP_SIZE_MASK);
    if (next >= s_heap_buf + s_heap_total) {
        return NULL;
    }
    return (header_t *)next;
}

/** @brief 标记块为空闲/已分配。 */
static inline void block_set_free(header_t *hdr, rtos_bool_t is_free)
{
    if (is_free) {
        hdr->size |= RTOS_HEAP_FREE_BIT;
    } else {
        hdr->size &= ~RTOS_HEAP_FREE_BIT;
    }
}

/** @brief 块是否空闲。 */
static inline rtos_bool_t block_is_free(const header_t *hdr)
{
    return (hdr->size & RTOS_HEAP_FREE_BIT) ? RTOS_TRUE : RTOS_FALSE;
}

/**
 * @brief 分裂块: 若空闲块远大于请求, 将尾部切分为新空闲块。
 * @param hdr   待分裂的空闲块(将被分配)。
 * @param need  实际需要的块大小(含 header, 已对齐)。
 */
static void block_split(header_t *hdr, uint32_t need)
{
    uint32_t total = hdr->size & RTOS_HEAP_SIZE_MASK;
    /* 仅当剩余 >= MIN_BLOCK + sizeof(header) 才分裂(否则浪费空间在 header 上) */
    if (total >= need + RTOS_HEAP_MIN_BLOCK + sizeof(header_t)) {
        header_t *new_blk = (header_t *)((uint8_t *)hdr + need);
        new_blk->magic = RTOS_HEAP_MAGIC;
        new_blk->size = total - need;
        block_set_free(new_blk, RTOS_TRUE);
        hdr->size = need; /* 已分配块不带 FREE 位 */
    }
    /* 否则不分裂, 整块分配(略浪费, 但避免产生过小碎片) */
}

/**
 * @brief 与下一块合并(若下一块空闲)。
 * @param hdr 当前块(已空闲)。
 */
static void block_merge_next(header_t *hdr)
{
    header_t *next = block_next(hdr);
    if ((next != NULL) && block_is_free(next)) {
        /* 合并: 当前块大小 += 下一块总大小 */
        uint32_t next_size = next->size & RTOS_HEAP_SIZE_MASK;
        hdr->size = (hdr->size & RTOS_HEAP_SIZE_MASK) + next_size;
        block_set_free(hdr, RTOS_TRUE);
        /* 清除下一块魔数(防止误用, 防御性) */
        next->magic = 0;
    }
}

/* ============================== 公共 API ============================== */

rtos_status_t rtos_heap_init(void)
{
    if (s_inited) {
        return RTOS_OK; /* 已初始化, 幂等 */
    }

    /* 首块 header 占据堆起始, 用户数据紧跟其后 */
    s_heap_total = RTOS_CONFIG_HEAP_SIZE;
    /* 确保总大小至少能容纳 header + 最小块 */
    if (s_heap_total < sizeof(header_t) + RTOS_HEAP_MIN_BLOCK) {
        return RTOS_ERR_PARAM;
    }

    header_t *first = (header_t *)s_heap_buf;
    first->magic = RTOS_HEAP_MAGIC;
    first->size = s_heap_total;
    block_set_free(first, RTOS_TRUE);

    s_heap_free = s_heap_total - sizeof(header_t);
    s_alloc_count = 0U;
    s_free_count = 0U;
    s_inited = RTOS_TRUE;
    return RTOS_OK;
}

void *rtos_heap_alloc(uint32_t size)
{
    if (!s_inited || (size == 0U)) {
        return NULL;
    }

    /* 块总大小 = header + 用户数据(向上对齐 8) */
    uint32_t need = align_up(size) + sizeof(header_t);
    if (need < RTOS_HEAP_MIN_BLOCK) {
        need = RTOS_HEAP_MIN_BLOCK;
    }

    void *user = NULL;
    RTOS_PORT_ENTER_CRITICAL();

    /* first-fit 遍历: 从堆起始扫描, 找第一个足够大的空闲块 */
    header_t *hdr = (header_t *)s_heap_buf;
    while ((uint8_t *)hdr < s_heap_buf + s_heap_total) {
        if (hdr->magic != RTOS_HEAP_MAGIC) {
            /* 堆损坏(越界写) */
            break;
        }

        uint32_t blk_size = hdr->size & RTOS_HEAP_SIZE_MASK;
        if (block_is_free(hdr) && (blk_size >= need)) {
            /* 命中: 分裂(若可), 标记已分配 */
            block_split(hdr, need);
            block_set_free(hdr, RTOS_FALSE);
            s_heap_free -= (hdr->size & RTOS_HEAP_SIZE_MASK) - sizeof(header_t);
            s_alloc_count++;
            user = block_to_user(hdr);
            break;
        }

        /* 跳到下一块 */
        if (blk_size == 0U) {
            break; /* 防御性: 避免死循环 */
        }
        hdr = (header_t *)((uint8_t *)hdr + blk_size);
    }

    RTOS_PORT_EXIT_CRITICAL();
    return user;
}

void rtos_heap_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (!s_inited) {
        return;
    }

    /* 合法性检查: 指针必须在堆范围内 */
    uint8_t *p = (uint8_t *)ptr;
    if ((p < s_heap_buf + sizeof(header_t)) || (p >= s_heap_buf + s_heap_total)) {
        return; /* 越界指针, 忽略 */
    }

    RTOS_PORT_ENTER_CRITICAL();

    header_t *hdr = user_to_block(ptr);
    if (hdr->magic != RTOS_HEAP_MAGIC) {
        /* 双重释放或越界写 */
        RTOS_PORT_EXIT_CRITICAL();
        return;
    }
    if (block_is_free(hdr)) {
        /* 双重释放 */
        RTOS_PORT_EXIT_CRITICAL();
        return;
    }

    /* 标记为空闲, 累计空闲字节 */
    uint32_t blk_size = hdr->size & RTOS_HEAP_SIZE_MASK;
    s_heap_free += blk_size - sizeof(header_t);
    s_free_count++;
    block_set_free(hdr, RTOS_TRUE);

    /* 与下一块合并(若下一块也空闲) */
    block_merge_next(hdr);

    RTOS_PORT_EXIT_CRITICAL();
}

uint32_t rtos_heap_get_free(void)
{
    return s_heap_free;
}

uint32_t rtos_heap_get_alloc_count(void)
{
    return s_alloc_count;
}

uint32_t rtos_heap_get_free_count(void)
{
    return s_free_count;
}

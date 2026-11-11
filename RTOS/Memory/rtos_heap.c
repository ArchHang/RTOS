/**
 * @file    rtos_heap.c
 * @brief   动态堆内存分配器实现
 *
 * @details 采用 first-fit + 块合并策略, 实现简单且适合 RTOS 中等规模分配。
 *
 *          设计权衡:
 *            - 复杂度: 分配 O(n)、释放 O(1)，n 为物理块总数。
 *            - 碎片: 合并相邻空闲块减少外部碎片，对齐和无法拆分的尾部产生内部碎片。
 *            - 实时性: 临界区持有时间 = O(n) 链表遍历, 可预测。
 *
 *          块布局(已 8 字节对齐):
 *            ┌─────────────────┐
 *            │ block header    │  magic + size(高 bit = free 标志)
 *            ├─────────────────┤
 *            │ user data       │  <- rtos_heap_alloc 返回此处
 *            │ ...             │
 *            └─────────────────┘
 *
 *          块头大小 = 8 字节(uint32_t magic + uint32_t size), 用户数据天然
 *          8 字节对齐。size 字段最高 bit(0x80000000)作为空闲标志:
 *            - 1 = 空闲
 *            - 0 = 已分配
 *          实际块大小 = size & ~FREE_BIT。
 *
 *          合并策略: 释放时最多合并紧邻的下一块，保持固定成本。
 *          分配时按地址扫描，单块不够时合并足够长的连续空闲段。
 *          最大单次分配查询按相同规则统计空闲段，不修改块布局。
 *
 *          线程安全: 所有操作在 RTOS_PORT_ENTER/EXIT_CRITICAL 内完成。
 *          BASEPRI 屏蔽 syscall 级中断, 保证链表操作原子性。
 */
#include "rtos_heap.h"
#include "rtos_port.h"
#include "rtos_config.h"
#include "rtos_task.h" /* rtos_assert_fail declaration for ISR API guards */

/* ============================== 内部常量与类型 ============================== */

/** @brief 块 header 魔数(用于检测越界/双重释放)。 */
#define RTOS_HEAP_MAGIC UINT32_C(0xA5A5A5A5)

/** @brief size 字段空闲标志位(最高 bit)。 */
#define RTOS_HEAP_FREE_BIT UINT32_C(0x80000000)

/** @brief 块大小掩码(去掉空闲位)。 */
#define RTOS_HEAP_SIZE_MASK UINT32_C(0x7FFFFFFF)

/** @brief 最小分配大小(字节, 含 header)。小于此值的请求按此值分配。 */
#define RTOS_HEAP_MIN_BLOCK (16U)

/** @brief 对齐字节(8 字节, 与 Cortex-M EABI 栈对齐一致)。 */
#define RTOS_HEAP_ALIGN (8U)

/** @brief header 结构。 */
typedef struct heap_block_header {
    uint32_t magic; /**< 魔数(校验合法性)。 */
    uint32_t size; /**< 块总大小(含 header), 高 bit 为空闲标志。 */
} heap_block_header_t;

#define RTOS_HEAP_HEADER_SIZE ((uint32_t)sizeof(heap_block_header_t))

_Static_assert(sizeof(heap_block_header_t) == RTOS_HEAP_ALIGN,
               "heap header must preserve payload alignment");

/* ============================== 内部状态 ============================== */

/** @brief 堆静态缓冲区(RTOS_CONFIG_HEAP_SIZE 字节, 8 字节对齐)。 */
static uint8_t s_heap_buf[RTOS_CONFIG_HEAP_SIZE] __attribute__((aligned(RTOS_HEAP_ALIGN)));

/** @brief 已初始化标志(防止重复初始化)。 */
static rtos_bool_t s_inited = RTOS_FALSE;

/** @brief 去除末尾不足一个对齐单位的字节后，堆的可用容量。 */
static const size_t s_heap_total = sizeof(s_heap_buf) & ~((size_t)RTOS_HEAP_ALIGN - 1U);

/** @brief 堆空闲字节数(粗略, 不含 header 开销)。 */
static uint32_t s_heap_free = 0U;

/** @brief 累计分配次数(统计用)。 */
static uint32_t s_alloc_count = 0U;

/** @brief 累计释放次数(统计用)。 */
static uint32_t s_free_count = 0U;

/* ============================== 内部函数 ============================== */

/** @brief 向上对齐到 RTOS_HEAP_ALIGN 字节。 */
static inline uint32_t align_up(uint32_t size)
{
    return (size + (RTOS_HEAP_ALIGN - 1U)) & ~(uint32_t)(RTOS_HEAP_ALIGN - 1U);
}

static inline heap_block_header_t *block_at(uint32_t offset)
{
    return (heap_block_header_t *)(s_heap_buf + offset);
}

/* The caller supplies an aligned offset no greater than s_heap_total. */
static inline uint32_t block_checked_size(uint32_t offset)
{
    uint32_t remaining = (uint32_t)s_heap_total - offset;
    if (remaining < RTOS_HEAP_MIN_BLOCK) {
        return 0U;
    }

    const heap_block_header_t *header = block_at(offset);
    uint32_t size = header->size & RTOS_HEAP_SIZE_MASK;
    if ((header->magic != RTOS_HEAP_MAGIC) || (size < RTOS_HEAP_MIN_BLOCK) ||
        (size > remaining) || ((size & (RTOS_HEAP_ALIGN - 1U)) != 0U)) {
        return 0U;
    }
    return size;
}

static inline rtos_bool_t block_is_free(const heap_block_header_t *header)
{
    return (header->size & RTOS_HEAP_FREE_BIT) != 0U;
}

/* Split only when the tail is large enough; return the actual allocated size. */
static uint32_t block_split(uint32_t offset, uint32_t size, uint32_t need)
{
    uint32_t remaining = size - need;
    if (remaining < RTOS_HEAP_MIN_BLOCK + RTOS_HEAP_HEADER_SIZE) {
        return size;
    }

    heap_block_header_t *tail = block_at(offset + need);
    tail->magic = RTOS_HEAP_MAGIC;
    tail->size = remaining | RTOS_HEAP_FREE_BIT;
    s_heap_free -= RTOS_HEAP_HEADER_SIZE;
    return need;
}

/* Commit a run already validated under the same lock, reclaiming each absorbed header. */
static void block_join_run(uint32_t offset, uint32_t first_size, uint32_t run_size)
{
    for (uint32_t size = first_size; size < run_size;) {
        heap_block_header_t *next = block_at(offset + size);
        size += next->size & RTOS_HEAP_SIZE_MASK;
        next->magic = 0U;
        s_heap_free += RTOS_HEAP_HEADER_SIZE;
    }
    block_at(offset)->size = run_size | RTOS_HEAP_FREE_BIT;
}

/* ============================== 公共 API ============================== */

rtos_status_t rtos_heap_init(void)
{
    if (s_inited) {
        return RTOS_OK; /* 已初始化, 幂等 */
    }

    if ((s_heap_total < RTOS_HEAP_HEADER_SIZE + RTOS_HEAP_MIN_BLOCK) ||
        (s_heap_total > RTOS_HEAP_SIZE_MASK)) {
        return RTOS_ERR_PARAM;
    }

    uint32_t usable = (uint32_t)s_heap_total;
    heap_block_header_t *first = block_at(0U);
    first->magic = RTOS_HEAP_MAGIC;
    first->size = usable | RTOS_HEAP_FREE_BIT;

    s_heap_free = usable - RTOS_HEAP_HEADER_SIZE;
    s_alloc_count = 0U;
    s_free_count = 0U;
    s_inited = RTOS_TRUE;
    return RTOS_OK;
}

void *rtos_heap_alloc(uint32_t size)
{
    RTOS_ASSERT_ISR_OK();
    if (!s_inited || (size == 0U) || (size > s_heap_total - RTOS_HEAP_HEADER_SIZE)) {
        return NULL;
    }

    uint32_t need = align_up(size) + RTOS_HEAP_HEADER_SIZE;
    void *user = NULL;
    RTOS_PORT_ENTER_CRITICAL();

    for (uint32_t offset = 0U; offset < s_heap_total;) {
        uint32_t block_size = block_checked_size(offset);
        if (block_size == 0U) {
            break;
        }
        heap_block_header_t *header = block_at(offset);
        if (!block_is_free(header)) {
            offset += block_size;
            continue;
        }

        /* Ordinary single-block fits do no run scan. Leave failed runs intact. */
        uint32_t run_size = block_size;
        while (run_size < need) {
            uint32_t next_offset = offset + run_size;
            uint32_t next_size = block_checked_size(next_offset);
            if ((next_size == 0U) || !block_is_free(block_at(next_offset))) {
                break;
            }
            run_size += next_size;
        }

        if (run_size >= need) {
            if (run_size != block_size) {
                block_join_run(offset, block_size, run_size);
            }
            uint32_t used = block_split(offset, run_size, need);
            header->size = used;
            s_heap_free -= used - RTOS_HEAP_HEADER_SIZE;
            s_alloc_count++;
            user = s_heap_buf + offset + RTOS_HEAP_HEADER_SIZE;
            break;
        }
        offset += run_size;
    }

    RTOS_PORT_EXIT_CRITICAL();
    return user;
}

void rtos_heap_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    RTOS_ASSERT_ISR_OK();
    if (!s_inited) {
        return;
    }

    uintptr_t address = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)s_heap_buf;
    if (address < base) {
        return;
    }
    uintptr_t payload_offset = address - base;
    if ((payload_offset < RTOS_HEAP_HEADER_SIZE) || (payload_offset >= s_heap_total) ||
        ((payload_offset & (RTOS_HEAP_ALIGN - 1U)) != 0U)) {
        return;
    }

    RTOS_PORT_ENTER_CRITICAL();

    uint32_t offset = (uint32_t)payload_offset - RTOS_HEAP_HEADER_SIZE;
    uint32_t size = block_checked_size(offset);
    heap_block_header_t *header = block_at(offset);
    if ((size == 0U) || block_is_free(header)) {
        RTOS_PORT_EXIT_CRITICAL();
        return;
    }

    header->size = size | RTOS_HEAP_FREE_BIT;
    s_heap_free += size - RTOS_HEAP_HEADER_SIZE;
    s_free_count++;

    uint32_t next_offset = offset + size;
    uint32_t next_size = block_checked_size(next_offset);
    if ((next_size != 0U) && block_is_free(block_at(next_offset))) {
        block_join_run(offset, size, size + next_size);
    }

    RTOS_PORT_EXIT_CRITICAL();
}

uint32_t rtos_heap_get_free(void)
{
    return s_heap_free;
}

uint32_t rtos_heap_get_max_alloc(void)
{
    RTOS_ASSERT_ISR_OK();
    if (!s_inited) {
        return 0U;
    }

    RTOS_PORT_ENTER_CRITICAL();

    uint32_t largest = 0U;
    uint32_t run_size = 0U;
    for (uint32_t offset = 0U; offset < s_heap_total;) {
        uint32_t size = block_checked_size(offset);
        if (size == 0U) {
            break;
        }

        if (block_is_free(block_at(offset))) {
            run_size += size;
            if (run_size > largest) {
                largest = run_size;
            }
        } else {
            run_size = 0U;
        }
        offset += size;
    }

    RTOS_PORT_EXIT_CRITICAL();

    return (largest == 0U) ? 0U : largest - RTOS_HEAP_HEADER_SIZE;
}

uint32_t rtos_heap_get_alloc_count(void)
{
    return s_alloc_count;
}

uint32_t rtos_heap_get_free_count(void)
{
    return s_free_count;
}

/**
 * @file    rtos_heap.c
 * @brief   线性堆内存分配器
 *
 * @details 采用按地址 first-fit、按需合并及前向释放合并。
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
 *          缓存上次成功位置及此前最大空闲段，跳过已知不足的前缀。
 *          最大单次分配查询按相同规则统计空闲段，不修改块布局。
 *
 *          线程安全: 所有操作在 RTOS_PORT_ENTER/EXIT_CRITICAL 内完成。
 *          BASEPRI 屏蔽 syscall 级中断, 保证链表操作原子性。
 */
#include "rtos_heap.h"

#include "rtos_config.h"

#include <stdatomic.h>

#include "rtos_port.h"
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

/* Offset zero ignores the prefix maximum. An interior checkpoint follows a
 * live block. At the heap end the maximum covers the whole arena. */
static uint32_t s_scan_offset;
static uint32_t s_scan_prefix_max;
/* All blocks strictly before this physical boundary are in use. */
static uint32_t s_live_prefix_end;

/** @brief 堆空闲字节数(粗略, 不含 header 开销)。 */
static _Atomic uint32_t s_heap_free;

/** @brief 累计分配次数(统计用)。 */
static _Atomic uint32_t s_alloc_count;

/** @brief 累计释放次数(统计用)。 */
static _Atomic uint32_t s_free_count;

/* ============================== 内部函数 ============================== */

static inline uint32_t heap_lock(void)
{
#if defined(RTOS_PORT_ENTER_LOCAL_CRITICAL)
    return RTOS_PORT_ENTER_LOCAL_CRITICAL();
#else
    RTOS_PORT_ENTER_CRITICAL();
    return 0U;
#endif
}

static inline void heap_unlock(uint32_t mask)
{
#if defined(RTOS_PORT_EXIT_LOCAL_CRITICAL)
    RTOS_PORT_EXIT_LOCAL_CRITICAL(mask);
#else
    (void)mask;
    RTOS_PORT_EXIT_CRITICAL();
#endif
}

/** @brief 向上对齐到 RTOS_HEAP_ALIGN 字节。 */
static inline uint32_t align_up(uint32_t size)
{
    return (size + (RTOS_HEAP_ALIGN - 1U)) & ~(uint32_t)(RTOS_HEAP_ALIGN - 1U);
}

static inline heap_block_header_t *block_at(uint32_t offset)
{
    return (heap_block_header_t *)(s_heap_buf + offset);
}

/* The caller supplies an aligned offset. Reject short tails before forming a pointer. */
static inline uint32_t block_checked_tag(uint32_t offset)
{
    if ((s_heap_total < RTOS_HEAP_MIN_BLOCK) || (offset > s_heap_total - RTOS_HEAP_MIN_BLOCK)) {
        return 0U;
    }
    const uint32_t size_limit = (uint32_t)s_heap_total - RTOS_HEAP_MIN_BLOCK - offset;

    const heap_block_header_t *header = block_at(offset);
    const uint32_t tag = header->size;
    const uint32_t size = tag & RTOS_HEAP_SIZE_MASK;
    /* A size smaller than the minimum wraps above the valid range. */
    if ((header->magic != RTOS_HEAP_MAGIC) || (size - RTOS_HEAP_MIN_BLOCK > size_limit) ||
        ((size & (RTOS_HEAP_ALIGN - 1U)) != 0U)) {
        return 0U;
    }
    return tag;
}

/* Callers establish that the complete minimum block fits before reading the header. */
static inline rtos_bool_t block_read_scan_tag(const heap_block_header_t *header,
                                              uint32_t size_limit, uint32_t *tag_out)
{
    const heap_block_header_t *const aligned =
        (const heap_block_header_t *)__builtin_assume_aligned(header, RTOS_HEAP_ALIGN);
    const heap_block_header_t value = *aligned;
    const uint32_t tag = value.size;
    const uint32_t size = tag & RTOS_HEAP_SIZE_MASK;
    /* Retain exactly the three low alignment bits in the combined check. */
    const uint32_t header_errors = (value.magic ^ RTOS_HEAP_MAGIC) | (tag << 29U);
    if ((header_errors != 0U) || (size - RTOS_HEAP_MIN_BLOCK > size_limit)) {
        return RTOS_FALSE;
    }
    *tag_out = tag;
    return RTOS_TRUE;
}

/* Writers already serialize in the port critical section. Relaxed access
 * keeps the independent statistics observable without an atomic RMW. */
static inline void increment_counter(_Atomic uint32_t *counter)
{
    const uint32_t count = atomic_load_explicit(counter, memory_order_relaxed);
    atomic_store_explicit(counter, count + 1U, memory_order_relaxed);
}

/* Split only when the tail is large enough; return the actual allocated size. */
static uint32_t block_split(uint32_t offset, uint32_t size, uint32_t need, uint32_t *free_bytes)
{
    const uint32_t min_tail = RTOS_HEAP_MIN_BLOCK + RTOS_HEAP_HEADER_SIZE;
    const uint32_t remaining = size - need;
    if ((s_heap_total < RTOS_HEAP_MIN_BLOCK + min_tail) || (remaining < min_tail)) {
        return size;
    }

    heap_block_header_t *tail = block_at(offset + need);
    tail->magic = RTOS_HEAP_MAGIC;
    tail->size = remaining | RTOS_HEAP_FREE_BIT;
    *free_bytes -= RTOS_HEAP_HEADER_SIZE;
    return need;
}

/* Commit a run already validated under the same lock, reclaiming each absorbed header. */
static void block_join_run(uint32_t offset, uint32_t first_size, uint32_t run_size,
                           uint32_t *free_bytes)
{
    for (uint32_t size = first_size; size < run_size;) {
        heap_block_header_t *next = block_at(offset + size);
        size += next->size & RTOS_HEAP_SIZE_MASK;
        next->magic = 0U;
        *free_bytes += RTOS_HEAP_HEADER_SIZE;
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

    atomic_store_explicit(&s_heap_free, usable - RTOS_HEAP_HEADER_SIZE, memory_order_relaxed);
    atomic_store_explicit(&s_alloc_count, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_free_count, 0U, memory_order_relaxed);
    s_inited = RTOS_TRUE;
    return RTOS_OK;
}

void *rtos_heap_alloc(uint32_t size)
{
    RTOS_ASSERT_ISR_OK();
    if (!s_inited || (size == 0U) || (size > s_heap_total - RTOS_HEAP_HEADER_SIZE)) {
        return NULL;
    }

    const uint32_t need = align_up(size + RTOS_HEAP_HEADER_SIZE);
    void *user = NULL;
    const uint32_t mask = heap_lock();

    const uint32_t checkpoint = s_scan_offset;
    uint32_t offset = checkpoint;
    uint32_t prefix_max = (offset != 0U) ? s_scan_prefix_max : 0U;
    if ((need <= prefix_max) || (offset < s_live_prefix_end)) {
        offset = s_live_prefix_end;
        prefix_max = 0U;
    }
    const uint32_t scan_limit = (s_heap_total >= RTOS_HEAP_MIN_BLOCK) ?
                                (uint32_t)s_heap_total - RTOS_HEAP_MIN_BLOCK : 0U;
    while (offset <= scan_limit) {
        heap_block_header_t *const header = block_at(offset);
        uint32_t tag;
        if (!block_read_scan_tag(header, scan_limit - offset, &tag)) {
            s_scan_offset = 0U;
            s_live_prefix_end = 0U;
            goto exit_critical;
        }
        if ((tag & RTOS_HEAP_FREE_BIT) == 0U) {
            offset += tag;
            continue;
        }
        const uint32_t block_size = tag & RTOS_HEAP_SIZE_MASK;

        /* Ordinary single-block fits do no run scan. Leave failed runs intact. */
        uint32_t run_size = block_size;
        while (run_size < need) {
            const uint32_t next_offset = offset + run_size;
            uint32_t next_tag;
            if ((next_offset > scan_limit) ||
                !block_read_scan_tag(block_at(next_offset), scan_limit - next_offset, &next_tag)) {
                break;
            }
            if ((next_tag & RTOS_HEAP_FREE_BIT) == 0U) {
                if (run_size > prefix_max) {
                    prefix_max = run_size;
                }
                /* A checked live block closes this too-small run. */
                offset = next_offset + next_tag;
                goto scan_next;
            }
            run_size += next_tag & RTOS_HEAP_SIZE_MASK;
        }

        if (run_size >= need) {
            uint32_t free_bytes = atomic_load_explicit(&s_heap_free, memory_order_relaxed);
            if (run_size != block_size) {
                block_join_run(offset, block_size, run_size, &free_bytes);
            }
            uint32_t used = block_split(offset, run_size, need, &free_bytes);
            header->size = used;
            free_bytes -= used - RTOS_HEAP_HEADER_SIZE;
            atomic_store_explicit(&s_heap_free, free_bytes, memory_order_relaxed);
            increment_counter(&s_alloc_count);
            user = s_heap_buf + offset + RTOS_HEAP_HEADER_SIZE;
            if (prefix_max == 0U) {
                s_live_prefix_end = offset + used;
            }
            if (offset != checkpoint) {
                s_scan_offset = offset;
                s_scan_prefix_max = prefix_max;
            }
            goto exit_critical;
        }
        if (run_size > prefix_max) {
            prefix_max = run_size;
        }
        /* Recheck an invalid header at the outer boundary without stepping past it. */
        offset += run_size;
    scan_next:
        continue;
    }

    if (offset != s_heap_total) {
        s_scan_offset = 0U;
        s_live_prefix_end = 0U;
    } else if ((checkpoint != offset) &&
               ((checkpoint == 0U) || (prefix_max <= s_scan_prefix_max))) {
        /* Keep a checkpoint that can still accelerate a successful allocation. */
        s_scan_prefix_max = prefix_max;
        s_scan_offset = offset;
    }
exit_critical:
    heap_unlock(mask);
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

    /* Unsigned subtraction also rejects addresses below the first payload. */
    uintptr_t header_offset = (uintptr_t)ptr - (uintptr_t)s_heap_buf - RTOS_HEAP_HEADER_SIZE;
    if ((s_heap_total < RTOS_HEAP_MIN_BLOCK) ||
        (header_offset > s_heap_total - RTOS_HEAP_MIN_BLOCK) ||
        ((header_offset & (RTOS_HEAP_ALIGN - 1U)) != 0U)) {
        return;
    }

    const uint32_t mask = heap_lock();

    const uint32_t offset = (uint32_t)header_offset;
    heap_block_header_t *header = block_at(offset);
    uint32_t size = header->size;
    const uint32_t size_limit = (uint32_t)s_heap_total - RTOS_HEAP_MIN_BLOCK - offset;
    /* The unsigned range also rejects the free bit in the unmasked size. */
    if ((header->magic != RTOS_HEAP_MAGIC) || (size - RTOS_HEAP_MIN_BLOCK > size_limit) ||
        ((size & (RTOS_HEAP_ALIGN - 1U)) != 0U)) {
        heap_unlock(mask);
        return;
    }

    if (offset < s_scan_offset) {
        s_scan_offset = 0U;
    }
    if (offset < s_live_prefix_end) {
        s_live_prefix_end = offset;
    }

    uint32_t recovered = size - RTOS_HEAP_HEADER_SIZE;
    const uint32_t next_offset = offset + size;
    const uint32_t next_tag = block_checked_tag(next_offset);
    if ((next_tag & RTOS_HEAP_FREE_BIT) != 0U) {
        block_at(next_offset)->magic = 0U;
        size += next_tag & RTOS_HEAP_SIZE_MASK;
        recovered += RTOS_HEAP_HEADER_SIZE;
    }
    header->size = size | RTOS_HEAP_FREE_BIT;
    const uint32_t free_bytes = atomic_load_explicit(&s_heap_free, memory_order_relaxed);
    atomic_store_explicit(&s_heap_free, free_bytes + recovered, memory_order_relaxed);
    increment_counter(&s_free_count);

    heap_unlock(mask);
}

uint32_t rtos_heap_get_free(void)
{
    return atomic_load_explicit(&s_heap_free, memory_order_relaxed);
}

uint32_t rtos_heap_get_max_alloc(void)
{
    RTOS_ASSERT_ISR_OK();
    if (!s_inited) {
        return 0U;
    }

    const uint32_t mask = heap_lock();

    const uint32_t checkpoint = s_scan_offset;
    const uint32_t prefix_max = (checkpoint != 0U) ? s_scan_prefix_max : 0U;
    uint32_t largest = prefix_max;
    if (checkpoint != s_heap_total) {
        /* The internal checkpoint follows a live block, so runs cannot cross it. */
        uint32_t offset = checkpoint;
        uint32_t run_size = 0U;
        for (;;) {
            const uint32_t tag = block_checked_tag(offset);
            if (tag == 0U) {
                break;
            }

            const uint32_t size = tag & RTOS_HEAP_SIZE_MASK;
            if ((tag & RTOS_HEAP_FREE_BIT) != 0U) {
                run_size += size;
                if (run_size > largest) {
                    largest = run_size;
                }
            } else {
                run_size = 0U;
            }
            offset += size;
        }
        /* Keep an interior checkpoint if it can accelerate a successful
         * allocation that would have to start at zero with an end checkpoint. */
        if (offset != s_heap_total) {
            s_scan_offset = 0U;
            s_live_prefix_end = 0U;
        } else if ((checkpoint == 0U) || (largest == prefix_max)) {
            s_scan_prefix_max = largest;
            s_scan_offset = offset;
        }
    }

    heap_unlock(mask);

    return (largest == 0U) ? 0U : largest - RTOS_HEAP_HEADER_SIZE;
}

uint32_t rtos_heap_get_alloc_count(void)
{
    return atomic_load_explicit(&s_alloc_count, memory_order_relaxed);
}

uint32_t rtos_heap_get_free_count(void)
{
    return atomic_load_explicit(&s_free_count, memory_order_relaxed);
}

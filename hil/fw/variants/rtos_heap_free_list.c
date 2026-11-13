/**
 * @file    rtos_heap_free_list.c
 * @brief   Address-ordered free-list allocator for HIL comparison.
 *
 * @details 在空闲载荷中保存下一块偏移，保留按地址 first-fit、按需合并与右向合并。
 *          分配只访问空闲链节点，释放在同一链中寻找插入位置，两者最坏均为 O(F)。
 *          F 为空闲物理块数。最大容量查询在无缓存时遍历物理块。
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
 *          合并策略: 释放时最多合并紧邻右块。分配只合并刚够请求的连续空闲块。
 *          完整失败扫描核对空闲账目后可缓存全堆最大跨度。成功修改后使缓存失效。
 *
 *          Uses the same local critical section as the production heap.
 *          The HIL build selects this translation unit as a heap replacement.
 */
#include "rtos_heap.h"

#include <stdatomic.h>
#include <string.h>

#include "rtos_config.h"
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
#define RTOS_HEAP_LINK_NONE UINT32_MAX
#define RTOS_HEAP_MAX_UNKNOWN UINT32_MAX

_Static_assert(sizeof(heap_block_header_t) == RTOS_HEAP_ALIGN,
               "heap header must preserve payload alignment");
_Static_assert(RTOS_HEAP_MIN_BLOCK >= RTOS_HEAP_HEADER_SIZE + sizeof(uint32_t),
               "a free block must hold its next offset");

typedef struct heap_free_node {
    uint32_t size;
    uint32_t next_offset;
} heap_free_node_t;

typedef struct heap_fit {
    uint32_t previous_offset;
    uint32_t offset;
    uint32_t first_size;
    uint32_t run_size;
    uint32_t next_offset;
} heap_fit_t;

typedef enum heap_search_result {
    HEAP_SEARCH_MISS = 0,
    HEAP_SEARCH_FIT,
    HEAP_SEARCH_CORRUPT
} heap_search_result_t;

/* ============================== 内部状态 ============================== */

/** @brief 堆静态缓冲区(RTOS_CONFIG_HEAP_SIZE 字节, 8 字节对齐)。 */
static uint8_t s_heap_buf[RTOS_CONFIG_HEAP_SIZE] __attribute__((aligned(RTOS_HEAP_ALIGN)));

/** @brief 已初始化标志(防止重复初始化)。 */
static rtos_bool_t s_inited = RTOS_FALSE;

/** @brief 去除末尾不足一个对齐单位的字节后，堆的可用容量。 */
static const size_t s_heap_total = sizeof(s_heap_buf) & ~((size_t)RTOS_HEAP_ALIGN - 1U);

/* The list contains each free physical block exactly once in address order. */
static uint32_t s_free_head;
static uint32_t s_max_free_span;

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

static inline rtos_bool_t is_free_link_offset(uint32_t offset)
{
    return (offset == RTOS_HEAP_LINK_NONE) ||
           ((s_heap_total >= RTOS_HEAP_MIN_BLOCK) &&
            (offset <= s_heap_total - RTOS_HEAP_MIN_BLOCK) &&
            ((offset & (RTOS_HEAP_ALIGN - 1U)) == 0U));
}

/* Only read a link after the caller has validated the containing block. */
static inline rtos_bool_t read_free_link(uint32_t offset, uint32_t size, uint32_t *next_offset)
{
    uint32_t next;
    memcpy(&next, s_heap_buf + offset + RTOS_HEAP_HEADER_SIZE, sizeof(next));
    if ((next < offset + size) || !is_free_link_offset(next)) {
        return RTOS_FALSE;
    }
    *next_offset = next;
    return RTOS_TRUE;
}

static inline rtos_bool_t read_free_node(uint32_t offset, heap_free_node_t *node)
{
    if ((offset & (RTOS_HEAP_ALIGN - 1U)) != 0U) {
        return RTOS_FALSE;
    }
    const uint32_t tag = block_checked_tag(offset);
    if ((tag & RTOS_HEAP_FREE_BIT) == 0U) {
        return RTOS_FALSE;
    }
    node->size = tag & RTOS_HEAP_SIZE_MASK;
    return read_free_link(offset, node->size, &node->next_offset);
}

static inline void write_free_link(uint32_t offset, uint32_t next_offset)
{
    if (offset == RTOS_HEAP_LINK_NONE) {
        s_free_head = next_offset;
    } else {
        memcpy(s_heap_buf + offset + RTOS_HEAP_HEADER_SIZE, &next_offset, sizeof(next_offset));
    }
}

static heap_search_result_t find_free_fit(uint32_t need, heap_fit_t *fit, uint32_t *largest)
{
    uint32_t previous_offset = RTOS_HEAP_LINK_NONE;
    uint32_t offset = s_free_head;
    uint32_t scanned_bytes = 0U;
    *largest = 0U;
    while (offset != RTOS_HEAP_LINK_NONE) {
        heap_free_node_t node;
        if (!read_free_node(offset, &node)) {
            return HEAP_SEARCH_CORRUPT;
        }
        const uint32_t first_size = node.size;
        uint32_t run_size = first_size;
        uint32_t last_offset = offset;
        scanned_bytes += first_size - RTOS_HEAP_HEADER_SIZE;
        while ((run_size < need) && (node.next_offset == offset + run_size)) {
            last_offset = node.next_offset;
            if (!read_free_node(last_offset, &node)) {
                return HEAP_SEARCH_CORRUPT;
            }
            run_size += node.size;
            scanned_bytes += node.size - RTOS_HEAP_HEADER_SIZE;
        }
        if (run_size >= need) {
            *fit = (heap_fit_t){
                .previous_offset = previous_offset,
                .offset = offset,
                .first_size = first_size,
                .run_size = run_size,
                .next_offset = node.next_offset
            };
            return HEAP_SEARCH_FIT;
        }
        if (run_size > *largest) {
            *largest = run_size;
        }
        previous_offset = last_offset;
        offset = node.next_offset;
    }
    const uint32_t free_bytes = atomic_load_explicit(&s_heap_free, memory_order_relaxed);
    return (scanned_bytes == free_bytes) ? HEAP_SEARCH_MISS : HEAP_SEARCH_CORRUPT;
}

static void *commit_free_fit(uint32_t need, const heap_fit_t *fit)
{
    uint32_t free_bytes = atomic_load_explicit(&s_heap_free, memory_order_relaxed);
    if (fit->run_size != fit->first_size) {
        block_join_run(fit->offset, fit->first_size, fit->run_size, &free_bytes);
    }
    const uint32_t used = block_split(fit->offset, fit->run_size, need, &free_bytes);
    uint32_t successor = fit->next_offset;
    if (used != fit->run_size) {
        successor = fit->offset + used;
        write_free_link(successor, fit->next_offset);
    }
    write_free_link(fit->previous_offset, successor);
    block_at(fit->offset)->size = used;
    free_bytes -= used - RTOS_HEAP_HEADER_SIZE;
    atomic_store_explicit(&s_heap_free, free_bytes, memory_order_relaxed);
    increment_counter(&s_alloc_count);
    s_max_free_span = RTOS_HEAP_MAX_UNKNOWN;
    return s_heap_buf + fit->offset + RTOS_HEAP_HEADER_SIZE;
}

static rtos_bool_t find_release_links(uint32_t offset, uint32_t end,
                                    uint32_t *previous_offset, uint32_t *next_offset)
{
    uint32_t previous = RTOS_HEAP_LINK_NONE;
    uint32_t current = s_free_head;
    if (!is_free_link_offset(current)) {
        return RTOS_FALSE;
    }
    while (current < offset) {
        heap_free_node_t node;
        if (!read_free_node(current, &node) || (node.size > offset - current)) {
            return RTOS_FALSE;
        }
        previous = current;
        current = node.next_offset;
    }
    if (current < end) {
        return RTOS_FALSE;
    }
    *previous_offset = previous;
    *next_offset = current;
    return RTOS_TRUE;
}

static void release_live_block(uint32_t offset, uint32_t size)
{
    const uint32_t right_offset = offset + size;
    uint32_t previous_offset;
    uint32_t next_offset;
    if (!find_release_links(offset, right_offset, &previous_offset, &next_offset)) {
        s_max_free_span = RTOS_HEAP_MAX_UNKNOWN;
        return;
    }
    const uint32_t right_tag = block_checked_tag(right_offset);
    const rtos_bool_t is_right_free = (right_tag & RTOS_HEAP_FREE_BIT) != 0U;
    if (is_right_free != (next_offset == right_offset)) {
        s_max_free_span = RTOS_HEAP_MAX_UNKNOWN;
        return;
    }
    uint32_t recovered = size - RTOS_HEAP_HEADER_SIZE;
    if (is_right_free) {
        const uint32_t right_size = right_tag & RTOS_HEAP_SIZE_MASK;
        if (!read_free_link(right_offset, right_size, &next_offset)) {
            s_max_free_span = RTOS_HEAP_MAX_UNKNOWN;
            return;
        }
        block_at(right_offset)->magic = 0U;
        size += right_size;
        recovered += RTOS_HEAP_HEADER_SIZE;
    }
    block_at(offset)->size = size | RTOS_HEAP_FREE_BIT;
    write_free_link(offset, next_offset);
    write_free_link(previous_offset, offset);
    const uint32_t free_bytes = atomic_load_explicit(&s_heap_free, memory_order_relaxed);
    atomic_store_explicit(&s_heap_free, free_bytes + recovered, memory_order_relaxed);
    increment_counter(&s_free_count);
    s_max_free_span = RTOS_HEAP_MAX_UNKNOWN;
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
    s_free_head = 0U;
    write_free_link(0U, RTOS_HEAP_LINK_NONE);
    s_max_free_span = RTOS_HEAP_MAX_UNKNOWN;

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

    if ((s_max_free_span == RTOS_HEAP_MAX_UNKNOWN) || (need <= s_max_free_span)) {
        heap_fit_t fit;
        uint32_t largest;
        const heap_search_result_t result = find_free_fit(need, &fit, &largest);
        if (result == HEAP_SEARCH_FIT) {
            user = commit_free_fit(need, &fit);
        } else if (result == HEAP_SEARCH_MISS) {
            s_max_free_span = largest;
        } else {
            s_max_free_span = RTOS_HEAP_MAX_UNKNOWN;
        }
    }

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

    release_live_block(offset, size);

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

    uint32_t largest = s_max_free_span;
    if (largest == RTOS_HEAP_MAX_UNKNOWN) {
        uint32_t offset = 0U;
        uint32_t run_size = 0U;
        largest = 0U;
        while (offset < s_heap_total) {
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
        if (offset == s_heap_total) {
            s_max_free_span = largest;
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

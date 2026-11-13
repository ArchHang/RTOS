/** @file current.c
 * @brief Compile the production allocator into a separate benchmark arena.
 */
#include "industry_backend/adapter.h"
#include "rtos_heap.h"

#define rtos_heap_init industry_private_init
#define rtos_heap_alloc industry_heap_alloc
#define rtos_heap_free industry_heap_free
#define rtos_heap_get_free industry_private_get_free
#define rtos_heap_get_max_alloc industry_private_get_max_alloc
#define rtos_heap_get_alloc_count industry_private_get_alloc_count
#define rtos_heap_get_free_count industry_private_get_free_count

#include "industry_backend/heap.c"

_Static_assert(sizeof(s_heap_buf) == 32U * 1024U, "F446 comparison requires a 32 KiB arena");

bool industry_heap_init(void)
{
    /* The included allocator's static state belongs to the benchmark instance. */
    s_inited = RTOS_FALSE;
    s_scan_offset = 0U;
    s_scan_prefix_max = 0U;
    s_live_prefix_end = 0U;
    return industry_private_init() == RTOS_OK;
}

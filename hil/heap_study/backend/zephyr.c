/** @file zephyr.c
 * @brief Benchmark adapter for the unmodified Zephyr v4.2.0 sys_heap implementation.
 */
#include "industry_backend/adapter.h"
#include "rtos_port.h"
#include "rtos_task.h"

/* Preserve the upstream helper's unused context parameter verbatim. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "industry_backend/upstream/heap.c"
#pragma GCC diagnostic pop

#define INDUSTRY_ZEPHYR_ARENA_SIZE (32U * 1024U)
#define INDUSTRY_ZEPHYR_ALIGNMENT 8U

static _Alignas(INDUSTRY_ZEPHYR_ALIGNMENT) uint8_t s_industry_arena[INDUSTRY_ZEPHYR_ARENA_SIZE];
static struct sys_heap s_industry_heap;

bool industry_heap_init(void)
{
    sys_heap_init(&s_industry_heap, s_industry_arena, sizeof(s_industry_arena));
    return s_industry_heap.heap != NULL;
}

void *industry_heap_alloc(uint32_t size)
{
    const uint32_t mask = RTOS_PORT_ENTER_LOCAL_CRITICAL();
    void *const address = sys_heap_aligned_alloc(&s_industry_heap, INDUSTRY_ZEPHYR_ALIGNMENT, size);
    RTOS_PORT_EXIT_LOCAL_CRITICAL(mask);
    return address;
}

void industry_heap_free(void *address)
{
    const uint32_t mask = RTOS_PORT_ENTER_LOCAL_CRITICAL();
    sys_heap_free(&s_industry_heap, address);
    RTOS_PORT_EXIT_LOCAL_CRITICAL(mask);
}

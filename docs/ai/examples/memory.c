/**
 * @file memory.c
 * @brief Matched heap ownership and a persistent fixed-block pool.
 */
#include "ai_examples.h"

#include <string.h>

static rtos_mempool_t sample_pool;
static _Alignas(8) uint8_t pool_storage[4][64];

rtos_status_t ai_example_heap_use(uint32_t bytes, uint32_t *last_byte)
{
    if (last_byte == NULL) {
        return RTOS_ERR_NULL;
    }
    if (bytes == 0U) {
        return RTOS_ERR_PARAM;
    }
    uint8_t *buffer = (uint8_t *)rtos_heap_alloc(bytes);
    if (buffer == NULL) {
        return RTOS_ERR_NO_MEM;
    }
    memset(buffer, 0x5A, (size_t)bytes);
    uint32_t value = (uint32_t)buffer[bytes - 1U];
    rtos_heap_free(buffer);
    *last_byte = value;
    return RTOS_OK;
}

rtos_status_t ai_example_prepare_pool(void)
{
    /* The performance registry can retain &sample_pool for later observations. */
    return rtos_mempool_init(&sample_pool, pool_storage, 64U, 4U);
}

rtos_status_t ai_example_pool_use(void)
{
    void *block = rtos_mempool_alloc(&sample_pool);
    if (block == NULL) {
        return RTOS_ERR_NO_MEM;
    }
    memset(block, 0x3C, 64U);
    return rtos_mempool_free(&sample_pool, block);
}

/** @file adapter.h
 * @brief Shared allocator interface for the isolated F446 HIL fixtures.
 */
#ifndef INDUSTRY_ADAPTER_H
#define INDUSTRY_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Reset the dedicated arena between traces with no live allocations. */
bool industry_heap_init(void);

/** @brief Allocate at least size bytes with 8-byte payload alignment. */
void *industry_heap_alloc(uint32_t size);

/** @brief Release a block from the dedicated arena. */
void industry_heap_free(void *address);

#endif

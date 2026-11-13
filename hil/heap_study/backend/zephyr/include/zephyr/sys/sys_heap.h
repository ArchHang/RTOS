/** @brief ABI subset of the Zephyr sys_heap public header for the HIL adapter. */
#ifndef INDUSTRY_ZEPHYR_SYS_HEAP_H
#define INDUSTRY_ZEPHYR_SYS_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sys_heap {
    struct z_heap *heap;
    void *init_mem;
    size_t init_bytes;
};

void sys_heap_init(struct sys_heap *heap, void *mem, size_t bytes);
void *sys_heap_alloc(struct sys_heap *heap, size_t bytes);
void *sys_heap_aligned_alloc(struct sys_heap *heap, size_t align, size_t bytes);
void sys_heap_free(struct sys_heap *heap, void *mem);
size_t sys_heap_usable_size(struct sys_heap *heap, void *mem);

#endif

/** @brief Standalone configuration for the Zephyr sys_heap HIL adapter. */
#ifndef INDUSTRY_ZEPHYR_KERNEL_H
#define INDUSTRY_ZEPHYR_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#define CONFIG_SYS_HEAP_ALLOC_LOOPS 3
#define CONFIG_SYS_HEAP_RUNTIME_STATS 1
#define CONFIG_SYS_HEAP_ARRAY_SIZE 0
#define CONFIG_SYS_HEAP_SMALL_ONLY 0
#define CONFIG_SYS_HEAP_BIG_ONLY 0

/* Keep the upstream routine assertions enabled through the RTOS assert sink. */
#define __ASSERT(condition, ...)                              \
    do {                                                      \
        if (!(condition)) {                                   \
            rtos_assert_fail(#condition, __FILE__, __LINE__); \
            __builtin_trap();                                 \
        }                                                     \
    } while (0)

#endif

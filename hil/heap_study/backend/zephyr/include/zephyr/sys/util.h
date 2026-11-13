/** @brief Utility macro subset required by the Zephyr sys_heap HIL adapter. */
#ifndef INDUSTRY_ZEPHYR_UTIL_H
#define INDUSTRY_ZEPHYR_UTIL_H

#include <stdint.h>

#define ARG_UNUSED(value) ((void)(value))
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define BIT(position) (UINT32_C(1) << (position))
#define BIT_MASK(bits) (BIT(bits) - UINT32_C(1))
#define MIN(left, right) ((left) < (right) ? (left) : (right))
#define MAX(left, right) ((left) > (right) ? (left) : (right))
#define ROUND_UP(value, alignment) \
    (((uintptr_t)(value) + (uintptr_t)(alignment) - 1U) & ~((uintptr_t)(alignment) - 1U))
#define ROUND_DOWN(value, alignment) ((uintptr_t)(value) & ~((uintptr_t)(alignment) - 1U))

/* The compatibility config defines the options read by IS_ENABLED as 0 or 1. */
#define IS_ENABLED(option) ((option) == 1)
/* The HIL firmware runs without the upstream MSAN hooks. */
#define IF_ENABLED(option, code)

#endif

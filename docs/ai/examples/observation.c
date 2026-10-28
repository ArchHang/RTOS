/**
 * @file observation.c
 * @brief Error-aware performance snapshots and a balanced critical section.
 */
#include "ai_examples.h"

rtos_status_t ai_example_observe(rtos_perf_system_t *system, rtos_perf_memory_t *memory)
{
    if ((system == NULL) || (memory == NULL)) {
        return RTOS_ERR_NULL;
    }
    rtos_status_t status = rtos_perf_get_system(system);
    if (status != RTOS_OK) {
        return status;
    }
    return rtos_perf_get_memory(memory);
}

rtos_status_t ai_example_critical_exchange(volatile uint32_t *target, uint32_t value,
    uint32_t *previous)
{
    if ((target == NULL) || (previous == NULL)) {
        return RTOS_ERR_NULL;
    }
    if ((uintptr_t)target == (uintptr_t)previous) {
        return RTOS_ERR_PARAM;
    }
    RTOS_PORT_ENTER_CRITICAL();
    uint32_t original = *target;
    *target = value;
    *previous = original;
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

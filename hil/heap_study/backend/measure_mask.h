/** @brief Instrument allocator-local mask boundaries for the F446 TIM5 fixtures. */
#ifndef INDUSTRY_MEASURE_MASK_H
#define INDUSTRY_MEASURE_MASK_H

#include "rtos_port.h"

void heap_irq_mask_begin(void);
void heap_irq_mask_end(void);

static inline uint32_t measure_enter_heap_mask(void)
{
    const uint32_t previous = rtos_port_enter_local_critical();
    heap_irq_mask_begin();
    return previous;
}

static inline void measure_exit_heap_mask(uint32_t previous)
{
    heap_irq_mask_end();
    rtos_port_exit_local_critical(previous);
}

#undef RTOS_PORT_ENTER_LOCAL_CRITICAL
#undef RTOS_PORT_EXIT_LOCAL_CRITICAL
#define RTOS_PORT_ENTER_LOCAL_CRITICAL() measure_enter_heap_mask()
#define RTOS_PORT_EXIT_LOCAL_CRITICAL(mask) measure_exit_heap_mask((mask))

#endif


#define RTOS_PORT_H_
#include <stdint.h>

typedef uint32_t rtos_stack_t;

#define RTOS_PORT_ENTER_CRITICAL() ((void)0)
#define RTOS_PORT_EXIT_CRITICAL() ((void)0)
#define RTOS_PORT_IN_ISR() 0U

static inline uint32_t rtos_port_isr_priority(void)
{
    return 0U;
}

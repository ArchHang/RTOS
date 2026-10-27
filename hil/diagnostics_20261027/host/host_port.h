/** @file host_port.h
 * @brief Single-thread native exclusion model, never an ARM timing emulator.
 */
#ifndef DIAGNOSTIC_HOST_PORT_H_
#define DIAGNOSTIC_HOST_PORT_H_

#define RTOS_PORT_H_
#include <stdint.h>
#include "rtos_types.h"

typedef uint32_t rtos_stack_t;

extern uint32_t host_isr;
extern uint32_t host_priority;
extern uint32_t host_nesting;
extern uint32_t host_enter_count;
extern uint32_t host_exit_count;
extern uint32_t host_reset_at_exit;

void host_enter_critical(void);
void host_exit_critical(void);
rtos_bool_t rtos_port_in_isr(void);
uint32_t rtos_port_isr_priority(void);

#define RTOS_PORT_ENTER_CRITICAL() host_enter_critical()
#define RTOS_PORT_EXIT_CRITICAL() host_exit_critical()
#define RTOS_PORT_IN_ISR() rtos_port_in_isr()

#endif /* DIAGNOSTIC_HOST_PORT_H_ */

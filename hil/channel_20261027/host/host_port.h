/**
 * @file host_port.h
 * @brief Native test exclusion and scripted scheduler, not an ARM port.
 */
#ifndef CHANNEL_HOST_PORT_H_
#define CHANNEL_HOST_PORT_H_

#define RTOS_PORT_H_
#include <stdint.h>
#include "rtos_types.h"

typedef uint32_t rtos_stack_t;

extern volatile uint32_t rtos_port_crit_nest;
extern volatile uint32_t rtos_port_saved_mask;
extern uint32_t host_basepri;
extern uint32_t host_primask;
extern uint32_t host_isr;
extern uint32_t host_priority;
extern uint32_t host_wait_calls;
extern rtos_tick_t host_wait_budgets[32];
extern uint32_t host_wait_advance;
extern void (*host_schedule_action)(void);

void rtos_port_enter_critical(void);
void rtos_port_exit_critical(void);
rtos_bool_t rtos_port_in_isr(void);
uint32_t rtos_port_isr_priority(void);
uint32_t rtos_port_get_interrupt_mask(void);
uint32_t rtos_port_get_global_interrupt_mask(void);
void rtos_port_set_interrupt_mask(uint32_t mask);
void host_reset(void);

#define RTOS_PORT_ENTER_CRITICAL() rtos_port_enter_critical()
#define RTOS_PORT_EXIT_CRITICAL() rtos_port_exit_critical()
#define RTOS_PORT_IN_ISR() rtos_port_in_isr()

#endif /* CHANNEL_HOST_PORT_H_ */

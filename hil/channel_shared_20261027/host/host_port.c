/**
 * @file host_port.c
 * @brief Serial scripted waits for native channel tests; no simulated IRQ claim.
 */
#include "host_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtos_task.h"
#include "rtos_sched.h"
#include "rtos_internal.h"

rtos_kernel_t rtos_kernel;
volatile uint32_t rtos_port_crit_nest;
volatile uint32_t rtos_port_saved_mask;
uint32_t host_basepri;
uint32_t host_primask;
uint32_t host_isr;
uint32_t host_priority;
uint32_t host_wait_calls;
rtos_tick_t host_wait_budgets[32];
uint32_t host_wait_advance;
void (*host_schedule_action)(void);

static rtos_tcb_t host_task;
static uint32_t pending_wait;
static rtos_tick_t pending_timeout;

static void host_remove_wait(rtos_wait_node_t *node)
{
    if (node->list_head != NULL) {
        if (node->prev != NULL) {
            node->prev->next = node->next;
        } else {
            *node->list_head = node->next;
        }
        if (node->next != NULL) {
            node->next->prev = node->prev;
        }
    }
    node->list_head = NULL;
    node->next = NULL;
    node->prev = NULL;
    node->tcb->blocked_on = NULL;
}

void host_reset(void)
{
    if (rtos_port_crit_nest != 0U || pending_wait != 0U) {
        (void)fprintf(stderr, "HOST|FAIL|unbalanced_exclusion_or_pending_wait\n");
        abort();
    }
    memset(&rtos_kernel, 0, sizeof(rtos_kernel));
    memset(&host_task, 0, sizeof(host_task));
    memset(host_wait_budgets, 0, sizeof(host_wait_budgets));
    rtos_kernel.sched_state = RTOS_SCHED_RUNNING;
    rtos_kernel.current_tcb = &host_task;
    host_task.state = RTOS_TASK_RUNNING;
    host_task.priority = 4U;
    host_basepri = 0U;
    host_primask = 0U;
    host_isr = 0U;
    host_priority = 0x50U;
    host_wait_calls = 0U;
    host_wait_advance = 0U;
    host_schedule_action = NULL;
}

void rtos_port_enter_critical(void)
{
    if (rtos_port_crit_nest == UINT32_MAX) {
        abort();
    }
    rtos_port_crit_nest++;
}

void rtos_port_exit_critical(void)
{
    if (rtos_port_crit_nest == 0U) {
        abort();
    }
    rtos_port_crit_nest--;
    if (rtos_port_crit_nest == 0U && pending_wait != 0U) {
        pending_wait = 0U;
        if (host_wait_calls < 32U) {
            host_wait_budgets[host_wait_calls] = pending_timeout;
        }
        host_wait_calls++;
        rtos_kernel.tick_count += host_wait_advance;
        if (host_schedule_action != NULL) {
            host_schedule_action();
        }
        if (host_task.wait_node.list_head != NULL) {
            if (pending_timeout == RTOS_WAIT_FOREVER) {
                (void)fprintf(stderr, "HOST|FAIL|unscripted_infinite_wait\n");
                abort();
            }
            if (host_wait_advance == 0U) {
                rtos_kernel.tick_count += pending_timeout;
            }
            host_remove_wait(&host_task.wait_node);
            host_task.wait_result = RTOS_ERR_TIMEOUT;
        }
        host_task.state = RTOS_TASK_RUNNING;
    }
}

rtos_bool_t rtos_port_in_isr(void)
{
    return host_isr != 0U ? RTOS_TRUE : RTOS_FALSE;
}

uint32_t rtos_port_isr_priority(void)
{
    return host_priority;
}

uint32_t rtos_port_get_interrupt_mask(void)
{
    return host_basepri;
}

uint32_t rtos_port_get_global_interrupt_mask(void)
{
    return host_primask;
}

void rtos_port_set_interrupt_mask(uint32_t mask)
{
    host_basepri = mask;
}

rtos_tick_t rtos_sched_get_tick_count(void)
{
    return rtos_kernel.tick_count;
}

rtos_status_t rtos_internal_block_current_on(void *object, rtos_wait_node_t **head,
                                            rtos_tick_t timeout)
{
    if (rtos_internal_block_permitted() != RTOS_OK) {
        return RTOS_ERR_SCHED;
    }
    if (pending_wait != 0U || host_task.wait_node.list_head != NULL) {
        abort();
    }
    host_task.blocked_on = object;
    host_task.wait_result = RTOS_OK;
    host_task.state = RTOS_TASK_BLOCKED;
    host_task.wait_node.tcb = &host_task;
    host_task.wait_node.list_head = head;
    host_task.wait_node.prev = NULL;
    host_task.wait_node.next = *head;
    if (*head != NULL) {
        (*head)->prev = &host_task.wait_node;
    }
    *head = &host_task.wait_node;
    pending_timeout = timeout;
    pending_wait = 1U;
    return RTOS_OK;
}

void rtos_internal_wake_all(rtos_wait_node_t **head, rtos_status_t reason)
{
    while (*head != NULL) {
        rtos_wait_node_t *node = *head;
        host_remove_wait(node);
        node->tcb->wait_result = reason;
        node->tcb->state = RTOS_TASK_READY;
    }
}

void rtos_assert_fail(const char *condition, const char *file, int line)
{
    (void)fprintf(stderr, "HOST|ASSERT|%s:%d:%s\n", file, line, condition);
    abort();
}

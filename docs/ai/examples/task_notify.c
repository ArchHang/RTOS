/**
 * @file task_notify.c
 * @brief Persistent static worker and legal-IRQ notification example.
 */
#include "ai_examples.h"

static rtos_tcb_t worker;
static _Alignas(8) rtos_stack_t worker_stack[256];
static volatile uint32_t latest_value;

static void worker_entry(void *argument)
{
    (void)argument;
    for (;;) {
        uint32_t value = 0U;
        rtos_status_t status = rtos_task_notify_wait(RTOS_WAIT_FOREVER, &value);
        if (status == RTOS_OK) {
            latest_value = value;
        } else {
            /* Do not consume a possibly changed error-path output as new work. */
            rtos_task_delay(1U);
        }
    }
}

rtos_status_t ai_example_prepare_worker(void)
{
    /* Once only. Keep TCB and stack alive while the task can execute.
     * The name is copied by create and only needs to be valid during that call. */
    return rtos_task_create_static(&worker, worker_stack, 256U, worker_entry,
        NULL, 10U, "ai-worker");
}

rtos_status_t ai_example_notify_from_irq(uint32_t value)
{
    /* Caller must publish the worker only after create returned RTOS_OK. */
    return rtos_task_notify(&worker, value, RTOS_NOTIFY_VALUE);
}

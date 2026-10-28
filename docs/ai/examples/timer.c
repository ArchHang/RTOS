/**
 * @file timer.c
 * @brief Persistent timer storage and asynchronous command completion semantics.
 */
#include "ai_examples.h"

static rtos_timer_t sample_timer;
static volatile uint32_t callback_count;

static void timer_callback(void *argument)
{
    volatile uint32_t *count = (volatile uint32_t *)argument;
    if (count != NULL) {
        /* The service task is the sole writer; no blocking work is done here. */
        (*count)++;
    }
}

rtos_status_t ai_example_prepare_timer(void)
{
    /* rtos_init auto-initializes timers only when USE_TIMERS is enabled.
     * Otherwise the caller must explicitly initialize the timer subsystem. */
    return rtos_timer_create(&sample_timer, "ai-timer", timer_callback,
        (void *)&callback_count, 100U, RTOS_TIMER_PERIODIC);
}

rtos_status_t ai_example_start_timer(void)
{
    return rtos_timer_start(&sample_timer);
}

rtos_status_t ai_example_stop_timer(void)
{
    return rtos_timer_stop(&sample_timer);
}

rtos_status_t ai_example_request_timer_delete(void)
{
    /* Both callback argument and timer object remain alive after this returns. */
    return rtos_timer_delete(&sample_timer);
}

/**
 * @file ipc.c
 * @brief Separate, persistent queue/semaphore/mutex/event usage examples.
 */
#include "ai_examples.h"

static rtos_queue_t sample_queue;
static uint32_t queue_storage[4];
static rtos_sem_t signal;
static rtos_mutex_t sample_mutex;
static uint32_t protected_value;
static rtos_event_t sample_event;

rtos_status_t ai_example_prepare_queue(void)
{
    return rtos_queue_init(&sample_queue, queue_storage,
        (uint32_t)sizeof(queue_storage[0]), 4U);
}

rtos_status_t ai_example_queue_roundtrip(uint32_t sample, uint32_t *received)
{
    if (received == NULL) {
        return RTOS_ERR_NULL;
    }
    rtos_status_t status = rtos_queue_send(&sample_queue, &sample,
        RTOS_NO_WAIT, RTOS_FALSE);
    if (status != RTOS_OK) {
        return status;
    }
    uint32_t result = 0U;
    status = rtos_queue_recv(&sample_queue, &result, RTOS_NO_WAIT);
    if (status == RTOS_OK) {
        *received = result;
    }
    return status;
}

rtos_status_t ai_example_queue_send_from_irq(uint32_t sample)
{
    return rtos_queue_send(&sample_queue, &sample, RTOS_NO_WAIT, RTOS_FALSE);
}

rtos_status_t ai_example_prepare_signal(void)
{
    return rtos_sem_init(&signal, 0U, 1U);
}

rtos_status_t ai_example_signal_from_irq(void)
{
    return rtos_sem_give(&signal);
}

rtos_status_t ai_example_wait_signal(void)
{
    return rtos_sem_take(&signal, 10U);
}

rtos_status_t ai_example_prepare_mutex(void)
{
    return rtos_mutex_init(&sample_mutex);
}

rtos_status_t ai_example_mutex_update(uint32_t value)
{
    rtos_status_t status = rtos_mutex_take(&sample_mutex, 10U);
    if (status != RTOS_OK) {
        return status;
    }
    protected_value = value;
    return rtos_mutex_give(&sample_mutex);
}

rtos_status_t ai_example_prepare_event(void)
{
    return rtos_event_init(&sample_event);
}

rtos_event_bits_t ai_example_set_event_from_irq(void)
{
    return rtos_event_set(&sample_event, 3U);
}

rtos_event_bits_t ai_example_wait_event(void)
{
    return rtos_event_wait(&sample_event, 3U,
        RTOS_EVENT_WAIT_ALL | RTOS_EVENT_CLEAR_ON_EXIT, 10U);
}

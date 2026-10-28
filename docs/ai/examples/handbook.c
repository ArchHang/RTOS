/**
 * @file handbook.c
 * @brief Persistent task/IRQ-to-queue pipeline and alternative startup for the design handbook.
 *
 * Objects are initialized once before scheduling/IRQ publication and never destroyed here.
 * This is an isolated application example, not an additional kernel service or daily firmware.
 */
#include "ai_examples.h"

#define HANDBOOK_QUEUE_CAPACITY (8U)
#define HANDBOOK_STACK_WORDS (256U)

typedef struct handbook_message {
    uint32_t sequence;
    int32_t sample;
} handbook_message_t;

static rtos_queue_t handbook_queue;
static handbook_message_t handbook_storage[HANDBOOK_QUEUE_CAPACITY];
static rtos_mutex_t handbook_result_mutex;
static rtos_event_t handbook_events;
static rtos_tcb_t handbook_consumer;
static rtos_tcb_t handbook_producer;
static _Alignas(8) rtos_stack_t handbook_consumer_stack[HANDBOOK_STACK_WORDS];
static _Alignas(8) rtos_stack_t handbook_producer_stack[HANDBOOK_STACK_WORDS];
static ai_example_handbook_result_t handbook_result;
static bool handbook_has_init_attempted;
static bool handbook_is_ready;

/** @brief Record a persistent state hint, not a counted error/message stream. */
static void handbook_mark_failure(void)
{
    /* event_set returns a snapshot, not an operation status. No consumer clears this hint. */
    (void)rtos_event_set(&handbook_events, AI_EXAMPLE_HANDBOOK_OPERATION_FAILED);
}

/** @brief Store one completed result under the mutex, preserving a consistent snapshot. */
static rtos_status_t handbook_store_result(const handbook_message_t *message)
{
    rtos_status_t status = rtos_mutex_take(&handbook_result_mutex, RTOS_WAIT_FOREVER);
    if (status != RTOS_OK) {
        return status;
    }
    handbook_result.sequence = message->sequence;
    handbook_result.sample = message->sample;
    if (handbook_result.processed_count < UINT32_MAX) {
        handbook_result.processed_count++;
    }
    return rtos_mutex_give(&handbook_result_mutex);
}

/** @brief Drain copied messages; all callback/storage lifetimes are permanent in this example. */
static void handbook_consumer_task(void *argument)
{
    (void)argument;
    for (;;) {
        handbook_message_t message;
        rtos_status_t status = rtos_queue_recv(&handbook_queue, &message, RTOS_WAIT_FOREVER);
        if (status != RTOS_OK) {
            handbook_mark_failure();
            rtos_task_delay(1U);
            continue;
        }
        status = handbook_store_result(&message);
        if (status != RTOS_OK) {
            /* The message has been consumed; this hint does not claim successful processing. */
            handbook_mark_failure();
            rtos_task_delay(1U);
            continue;
        }
        /* Repeated completions coalesce into a state bit; results are read under the mutex. */
        (void)rtos_event_set(&handbook_events, AI_EXAMPLE_HANDBOOK_RESULT_READY);
    }
}

rtos_status_t ai_example_handbook_pipeline_init(void)
{
    if (handbook_has_init_attempted) {
        return RTOS_ERR_ALREADY;
    }
    /* Startup is one-shot. On failure do not enable sources/start the scheduler or retry init. */
    handbook_has_init_attempted = true;
    rtos_status_t status = rtos_queue_init(&handbook_queue, handbook_storage,
        (uint32_t)sizeof(handbook_storage[0]), HANDBOOK_QUEUE_CAPACITY);
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_mutex_init(&handbook_result_mutex);
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_event_init(&handbook_events);
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_task_create_static(&handbook_consumer, handbook_consumer_stack,
        HANDBOOK_STACK_WORDS, handbook_consumer_task, NULL, 10U, "book-consumer");
    if (status == RTOS_OK) {
        /* No concurrent caller exists yet. This flag stays immutable after scheduling starts. */
        handbook_is_ready = true;
    }
    return status;
}

rtos_status_t ai_example_handbook_publish(uint32_t sequence, int32_t sample)
{
    if (!handbook_is_ready) {
        return RTOS_ERR_SCHED;
    }
    handbook_message_t message = {sequence, sample};
    /* The stack message stays alive until the potentially blocking send returns. */
    return rtos_queue_send(&handbook_queue, &message, 10U, RTOS_FALSE);
}

rtos_status_t ai_example_handbook_publish_from_isr(uint32_t sequence, int32_t sample)
{
    if (!handbook_is_ready) {
        return RTOS_ERR_SCHED;
    }
    handbook_message_t message = {sequence, sample};
    return rtos_queue_send(&handbook_queue, &message, RTOS_NO_WAIT, RTOS_FALSE);
}

rtos_status_t ai_example_handbook_read_result(ai_example_handbook_result_t *result)
{
    if (result == NULL) {
        return RTOS_ERR_NULL;
    }
    if (!handbook_is_ready) {
        return RTOS_ERR_SCHED;
    }
    rtos_status_t status = rtos_mutex_take(&handbook_result_mutex, 10U);
    if (status != RTOS_OK) {
        return status;
    }
    ai_example_handbook_result_t snapshot = handbook_result;
    status = rtos_mutex_give(&handbook_result_mutex);
    if (status != RTOS_OK) {
        return status;
    }
    if (snapshot.processed_count == 0U) {
        return RTOS_ERR_TIMEOUT;
    }
    *result = snapshot;
    return RTOS_OK;
}

rtos_event_bits_t ai_example_handbook_wait_hint(rtos_tick_t timeout)
{
    if (!handbook_is_ready) {
        return 0U;
    }
    return rtos_event_wait(&handbook_events, AI_EXAMPLE_HANDBOOK_RESULT_READY,
        RTOS_EVENT_WAIT_ANY | RTOS_EVENT_CLEAR_ON_EXIT, timeout);
}

void ai_example_handbook_periodic(void *argument)
{
    (void)argument;
    rtos_tick_t phase = rtos_sched_get_tick_count();
    uint32_t sequence = 0U;
    for (;;) {
        rtos_status_t status = ai_example_handbook_publish(sequence, 42);
        if (status != RTOS_OK) {
            /* Explicit lossy backpressure policy: skip this sample; no infinite retry. */
            handbook_mark_failure();
        }
        sequence++; /* Message identity is modulo 2^32, independent of acceptance. */
        rtos_task_delay_until(&phase, 20U);
    }
}

rtos_status_t ai_example_handbook_start(void)
{
    rtos_status_t status = rtos_init();
    if (status != RTOS_OK) {
        return status;
    }
    status = ai_example_handbook_pipeline_init();
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_task_create_static(&handbook_producer, handbook_producer_stack,
        HANDBOOK_STACK_WORDS, ai_example_handbook_periodic, NULL, 12U, "book-producer");
    if (status != RTOS_OK) {
        return status;
    }
    /* BSP clock/exception routing is the caller's responsibility; success never returns. */
    return rtos_start();
}

rtos_status_t ai_example_handbook_control_worker(rtos_tcb_t *worker, rtos_prio_t priority)
{
    if (RTOS_PORT_IN_ISR()) {
        return RTOS_ERR_ISR;
    }
    if (worker == NULL) {
        return RTOS_ERR_NULL;
    }
    if (priority >= RTOS_CONFIG_MAX_PRIORITIES || worker == rtos_task_get_current()) {
        return RTOS_ERR_PARAM;
    }
    rtos_status_t status = rtos_task_suspend(worker);
    if (status != RTOS_OK) {
        return status;
    }
    status = rtos_task_set_priority(worker, priority);
    rtos_status_t resume_status = rtos_task_resume(worker);
    if (resume_status != RTOS_OK) {
        return resume_status;
    }
    return status;
}

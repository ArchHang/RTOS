/**
 * @file ai_examples.h
 * @brief Independent documentation examples; not part of the application build.
 *
 * Preconditions and lifetime rules are specified in the adjacent README.
 * Include bool/types before rtos.h so its existing USB-OFF macros have types.
 */
#ifndef RTOS_DOCS_AI_EXAMPLES_H_
#define RTOS_DOCS_AI_EXAMPLES_H_

#include <stdbool.h>
#include <stdint.h>

#include "rtos.h"

/** @brief Create one persistent worker after rtos_init, before IRQ publication.
 * @return Actual static-task creation result; no IRQ may use a failed object.
 */
rtos_status_t ai_example_prepare_worker(void);
/** @brief Notify that worker from a legal syscall IRQ after successful creation.
 * @param value [in] Latest value; repeated VALUE notifications may overwrite it.
 * @return The actual notification status; this is not a counted message queue.
 */
rtos_status_t ai_example_notify_from_irq(uint32_t value);

/** @brief Initialize the private queue once, before publishing it to callers.
 * @return Actual queue initialization result.
 */
rtos_status_t ai_example_prepare_queue(void);
/** @brief Send then receive on a private, exclusively owned empty queue.
 * @param sample [in] Value copied by the queue.
 * @param received [out] Valid output; unchanged if either operation fails.
 * @return Actual queue status. Concurrent producers/consumers are not supported here.
 */
rtos_status_t ai_example_queue_roundtrip(uint32_t sample, uint32_t *received);
/** @brief Attempt a queue send from a legal syscall IRQ; never waits.
 * @param sample [in] Value to copy; a full queue returns TIMEOUT.
 * @return Actual queue status; the caller decides its drop/retry policy.
 */
rtos_status_t ai_example_queue_send_from_irq(uint32_t sample);
/** @brief Initialize a binary semaphore once before use.
 * @return Actual semaphore initialization result.
 */
rtos_status_t ai_example_prepare_signal(void);
/** @brief Give the signal from a legal syscall IRQ; OVERFLOW is not success.
 * @return Actual give result; the caller handles saturation according to its policy.
 */
rtos_status_t ai_example_signal_from_irq(void);
/** @brief Wait up to ten ticks for one signal from normal task context.
 * @return Actual take result, including timeout or cancellation.
 */
rtos_status_t ai_example_wait_signal(void);
/** @brief Initialize a mutex before use.
 * @return Actual mutex initialization result.
 */
rtos_status_t ai_example_prepare_mutex(void);
/** @brief Update a private value while holding its mutex in a running task.
 * @param value [in] Replacement value; no update occurs if take fails.
 * @return take failure or the subsequent give result.
 */
rtos_status_t ai_example_mutex_update(uint32_t value);
/** @brief Initialize an event object before use.
 * @return Actual event initialization result.
 */
rtos_status_t ai_example_prepare_event(void);
/** @brief Set the low two event bits from a legal syscall IRQ.
 * @return Actual post-operation event snapshot, not a status code.
 */
rtos_event_bits_t ai_example_set_event_from_irq(void);
/** @brief Wait for both low bits, clearing consumed bits, for at most ten ticks.
 * @return Snapshot; test (result & 3U) == 3U to determine condition satisfaction.
 */
rtos_event_bits_t ai_example_wait_event(void);

/** @brief Allocate, use and return one heap buffer within this call.
 * @param bytes [in] Positive byte count, subject to available heap capacity.
 * @param last_byte [out] Valid output; unchanged on invalid input/allocation failure.
 * @return NULL/PARAM/NO_MEM or OK; caller owns no heap pointer on return.
 */
rtos_status_t ai_example_heap_use(uint32_t bytes, uint32_t *last_byte);
/** @brief Initialize one persistent four-block memory pool once.
 * @return Actual pool initialization result.
 */
rtos_status_t ai_example_prepare_pool(void);
/** @brief Borrow, fill and return one block; pool control/storage stay alive.
 * @return NO_MEM if empty, otherwise the actual block-return result.
 */
rtos_status_t ai_example_pool_use(void);

/** @brief Create a persistent timer after its subsystem has been initialized.
 * @return Actual timer creation result; this does not start the timer.
 */
rtos_status_t ai_example_prepare_timer(void);
/** @brief Submit start; OK means command accepted, not callback already executed.
 * @return Actual timer-start result.
 */
rtos_status_t ai_example_start_timer(void);
/** @brief Submit stop; this is not a generic callback-completion barrier.
 * @return Actual timer-stop result.
 */
rtos_status_t ai_example_stop_timer(void);
/** @brief Submit delete while keeping all timer storage alive permanently.
 * @return Actual timer-delete result; no storage is released on OK.
 */
rtos_status_t ai_example_request_timer_delete(void);

/** @brief Query system then memory snapshots in task context.
 * @param system [out] Valid system snapshot storage.
 * @param memory [out] Valid memory snapshot storage.
 * @return First failure; the two outputs are not an atomic combined snapshot.
 */
rtos_status_t ai_example_observe(rtos_perf_system_t *system, rtos_perf_memory_t *memory);
/** @brief Send CDC bytes on an initialized device from normal task context.
 * @param busid [in] Valid initialized bus.
 * @param data [in] Readable bytes, valid throughout this call.
 * @param bytes [in] Positive accessible byte count, at most INT32_MAX.
 * @return Actual byte count; zero is ambiguous. USB-OFF sample returns zero.
 */
int32_t ai_example_cdc_write(uint8_t busid, const uint8_t *data, uint32_t bytes);
/** @brief Exchange one word inside a balanced, nonblocking critical section.
 * @param target [in,out] Naturally aligned accessible word.
 * @param value [in] Replacement value.
 * @param previous [out] Distinct naturally aligned accessible word.
 * @return NULL/PARAM or OK; legal task/syscall-IRQ contexts only.
 */
rtos_status_t ai_example_critical_exchange(volatile uint32_t *target, uint32_t value,
    uint32_t *previous);

#endif /* RTOS_DOCS_AI_EXAMPLES_H_ */

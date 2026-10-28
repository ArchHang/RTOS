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

/** @brief Last completed pipeline result; processed_count saturates at UINT32_MAX. */
typedef struct ai_example_handbook_result {
    uint32_t processed_count;
    uint32_t sequence;
    int32_t sample;
} ai_example_handbook_result_t;

#define AI_EXAMPLE_HANDBOOK_RESULT_READY ((rtos_event_bits_t)1U)
#define AI_EXAMPLE_HANDBOOK_OPERATION_FAILED ((rtos_event_bits_t)2U)

/** @brief Initialize persistent pipeline objects/worker once before rtos_start.
 * @return Actual initialization status; a failed startup requires application recovery/reset.
 * @note rtos_init must have succeeded. No concurrent caller/IRQ may use this pipeline yet.
 */
rtos_status_t ai_example_handbook_pipeline_init(void);
/** @brief Publish a copied message from a running task, waiting at most ten ticks.
 * @param sequence [in] Application message identity; wrap policy belongs to the application.
 * @param sample [in] Value copied into the message; no borrowed payload pointer.
 * @return Actual queue status; TIMEOUT means this message was not accepted.
 */
rtos_status_t ai_example_handbook_publish(uint32_t sequence, int32_t sample);
/** @brief Attempt one copied send from a legal syscall IRQ after pipeline publication.
 * @param sequence [in] Message identity assigned by this producer.
 * @param sample [in] Value copied during the call.
 * @return Actual queue status; caller must count/drop/retry a rejected message appropriately.
 */
rtos_status_t ai_example_handbook_publish_from_isr(uint32_t sequence, int32_t sample);
/** @brief Read the last completed result under a mutex from a running task.
 * @param result [out] Accessible snapshot; unchanged on failure or before the first result.
 * @return OK, TIMEOUT before a result/after lock timeout, or the actual lock error.
 */
rtos_status_t ai_example_handbook_read_result(ai_example_handbook_result_t *result);
/** @brief Wait for a coalesced result hint, clearing only the ready bit when satisfied.
 * @param timeout [in] Tick budget, NO_WAIT or FOREVER; normal finite limit applies.
 * @return Raw event snapshot; test RESULT_READY explicitly. FAILED alone is not completion.
 * @note Running task context only. The result mutex, not this hint, protects snapshot access.
 */
rtos_event_bits_t ai_example_handbook_wait_hint(rtos_tick_t timeout);
/** @brief Periodic producer task; never returns and uses a twenty-tick phase.
 * @param argument [in] Unused, may be NULL; queue storage must remain alive.
 */
void ai_example_handbook_periodic(void *argument);
/** @brief Alternative application entry after BSP preparation; starts this example only.
 * @return Initialization/creation error; successful scheduler start never returns.
 * @note Once only, before any RTOS-dependent application IRQ is enabled. Not daily main.c.
 */
rtos_status_t ai_example_handbook_start(void);
/** @brief Suspend/change priority/resume a caller-owned worker at a safe application boundary.
 * @param worker [in,out] Live, non-idle, non-current worker with persistent storage.
 * @param priority [in] New valid base priority.
 * @return Actual failure, otherwise OK; this is not a transactional business rollback.
 * @note One controller owns lifecycle. Worker is quiescent, holds no resource and is not waiting.
 *       No concurrent suspend/delete/resume is permitted. Originally suspended state is not kept.
 */
rtos_status_t ai_example_handbook_control_worker(rtos_tcb_t *worker, rtos_prio_t priority);

#endif /* RTOS_DOCS_AI_EXAMPLES_H_ */

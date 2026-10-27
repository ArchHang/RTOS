/**
 * @file rtos_diag.h
 * @brief Optional bounded flight recorder and conservative wait explanations.
 *
 * Freeze before reading. A snapshot is immutable until reset; pass its session
 * to every read/explain call. Reset invalidates old sessions and discards history.
 * Addresses are opaque labels within a wait episode, never lifetime handles.
 */
#ifndef RTOS_DIAG_H_
#define RTOS_DIAG_H_

#include "rtos_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rtos_diag_event {
    RTOS_DIAG_EVENT_WAIT = 1,
    RTOS_DIAG_EVENT_WAKE,
    RTOS_DIAG_EVENT_RUN,
    RTOS_DIAG_EVENT_RETURN,
    RTOS_DIAG_EVENT_SUSPEND,
    RTOS_DIAG_EVENT_RESUME,
    RTOS_DIAG_EVENT_DELETE,
    RTOS_DIAG_EVENT_CREATE,
    RTOS_DIAG_EVENT_MARK
} rtos_diag_event_t;

typedef enum rtos_diag_operation {
    RTOS_DIAG_OP_NONE = 0,
    RTOS_DIAG_OP_DELAY,
    RTOS_DIAG_OP_SEM,
    RTOS_DIAG_OP_QUEUE_SEND,
    RTOS_DIAG_OP_QUEUE_RECEIVE,
    RTOS_DIAG_OP_MUTEX,
    RTOS_DIAG_OP_EVENT,
    RTOS_DIAG_OP_NOTIFY,
    RTOS_DIAG_OP_CHANNEL_ACQUIRE,
    RTOS_DIAG_OP_CHANNEL_RECEIVE
} rtos_diag_operation_t;

typedef enum rtos_diag_phase {
    RTOS_DIAG_PHASE_UNKNOWN = 0,
    RTOS_DIAG_PHASE_WAITING,
    RTOS_DIAG_PHASE_READY,
    RTOS_DIAG_PHASE_RUNNING,
    RTOS_DIAG_PHASE_RETURNED,
    RTOS_DIAG_PHASE_SUSPENDED,
    RTOS_DIAG_PHASE_DELETED
} rtos_diag_phase_t;

typedef enum rtos_diag_trigger {
    RTOS_DIAG_TRIGGER_USER = 1,
    RTOS_DIAG_TRIGGER_FAULT,
    RTOS_DIAG_TRIGGER_SEQUENCE_EXHAUSTED,
    RTOS_DIAG_TRIGGER_IDENTITY_EXHAUSTED
} rtos_diag_trigger_t;

typedef enum rtos_diag_cause {
    RTOS_DIAG_CAUSE_NONE = 0,
    RTOS_DIAG_CAUSE_RESOURCE,
    RTOS_DIAG_CAUSE_RETRY_NOTIFICATION,
    RTOS_DIAG_CAUSE_DELAY_ELAPSED,
    RTOS_DIAG_CAUSE_TIMEOUT,
    RTOS_DIAG_CAUSE_OBJECT_DELETED,
    RTOS_DIAG_CAUSE_CHANNEL_CLOSED,
    RTOS_DIAG_CAUSE_TASK_SUSPENDED,
    RTOS_DIAG_CAUSE_TASK_DELETED,
    RTOS_DIAG_CAUSE_OTHER
} rtos_diag_cause_t;

#define RTOS_DIAG_FLAG_ISR (1U)
#define RTOS_DIAG_FLAG_INCOMPLETE (1U)
#define RTOS_DIAG_FLAG_PARTIAL_WRITE (2U)
#define RTOS_DIAG_FLAG_FAULT (4U)

typedef struct rtos_diag_record {
    uint32_t sequence;
    rtos_tick_t tick;
    uint32_t task_id;
    uintptr_t object;
    uint32_t peer_id;
    uint32_t value;
    uint32_t detail;
    int32_t result;
    uint8_t event;
    uint8_t operation;
    uint8_t priority;
    uint8_t flags;
} rtos_diag_record_t;

typedef struct rtos_diag_info {
    uint32_t session;
    uint32_t capacity;
    uint32_t count;
    uint32_t first_sequence;
    uint32_t last_sequence;
    uint32_t overwritten;
    uint32_t flags;
    uint32_t trigger;
    uint32_t fault_code;
    uintptr_t fault_pc;
} rtos_diag_info_t;

typedef struct rtos_diag_explanation {
    uint32_t session;
    uint32_t task_id;
    uint32_t episode; /**< Sequence of WAIT, zero if its beginning is missing. */
    uintptr_t object;
    uint32_t peer_id; /**< Mutex entry owner ID, or event wait mode. Not a live query. */
    uint32_t timeout;
    uint32_t detail; /**< Event mask for event waits; otherwise operation detail. */
    rtos_tick_t wait_tick;
    rtos_tick_t wake_tick;
    rtos_tick_t run_tick;
    int32_t wake_result;
    int32_t return_result;
    uint32_t flags;
    rtos_diag_operation_t operation;
    rtos_diag_phase_t phase;
    rtos_diag_cause_t cause;
} rtos_diag_explanation_t;

/** @brief Clear logical history and start a fresh session. Thread only.
 *  @return OK; ISR in an exception; OVERFLOW if identity/counters are exhausted;
 *          DISABLED when compiled out. Does not erase old RAM bytes.
 */
rtos_status_t rtos_diag_reset(void);
/** @brief Freeze history, idempotently. Thread or legal syscall IRQ.
 *  @param trigger [in] RTOS_DIAG_TRIGGER_USER; internal triggers are reserved.
 *  @return OK; PARAM for a reserved trigger; ISR for forbidden IRQ; DISABLED.
 */
rtos_status_t rtos_diag_freeze(uint32_t trigger);
/** @brief Record one application marker without formatting or allocation.
 *  @param value [in] Application event value; no pointer is dereferenced.
 *  @param detail [in] Application-defined detail word.
 *  @return OK; BUSY when frozen; ISR for forbidden IRQ; DISABLED.
 */
rtos_status_t rtos_diag_mark(uint32_t value, uint32_t detail);
/** @brief Obtain frozen snapshot metadata. Thread only; O(capacity).
 *  @param info [out] Writable metadata; unchanged on failure.
 *  @return OK; PARAM for NULL; BUSY before freeze; STALE on a racing reset;
 *          ISR in an exception; DISABLED.
 */
rtos_status_t rtos_diag_get_info(rtos_diag_info_t *info);
/** @brief Copy one frozen record without following historical pointers.
 *  @param session [in] Session returned by get_info.
 *  @param sequence [in] Nonzero sequence within that snapshot.
 *  @param record [out] Writable record; unchanged on failure.
 *  @return OK; PARAM for NULL/zero sequence; BUSY before freeze; STALE for old
 *          session/overwritten/uncommitted record; ISR in exception; DISABLED.
 */
rtos_status_t rtos_diag_read(uint32_t session, uint32_t sequence,
                            rtos_diag_record_t *record);
/** @brief Explain the latest retained wait episode. Thread only; O(capacity).
 *  @param session [in] Frozen snapshot session.
 *  @param task_id [in] Nonzero task identity, never a TCB pointer.
 *  @param explanation [out] Writable result; unchanged on failure. On OK, check
 *         flags: missing evidence is UNKNOWN/INCOMPLETE, not a guessed cause.
 *  @return OK; PARAM for NULL/zero ID; BUSY before freeze; STALE after reset;
 *          OVERFLOW for an inconsistent range; ISR in exception; DISABLED.
 */
rtos_status_t rtos_diag_explain(uint32_t session, uint32_t task_id,
                               rtos_diag_explanation_t *explanation);
/** @brief Freeze from a terminal fault without locks or TCB dereferences.
 * Must not return to interrupted code. NMI must not call this entry point.
 * Keeps the first fault. SRAM contents require a debugger after the halt.
 * @param fault_code [in] Platform fault status; preserved as an opaque word.
 * @param fault_pc [in] Known fault PC, or zero if not safely available.
 * @return No value. Compiled-out implementation has no effect.
 */
void rtos_diag_fault_freeze(uint32_t fault_code, uintptr_t fault_pc);

#ifdef __cplusplus
}
#endif
#endif /* RTOS_DIAG_H_ */

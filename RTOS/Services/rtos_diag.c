/**
 * @file rtos_diag.c
 * @brief Single-core bounded recorder. No allocation, I/O or object traversal.
 */
#include "rtos_diag.h"

#include <stdatomic.h>
#include <string.h>

#include "rtos_diag_internal.h"
#include "rtos_port.h"
#include "rtos_sched.h"
#include "rtos_task.h"

#if RTOS_CONFIG_USE_DIAGNOSTICS

#if (RTOS_CONFIG_DIAGNOSTIC_CAPACITY < 8U) || (RTOS_CONFIG_DIAGNOSTIC_CAPACITY > 1024U) || \
    ((RTOS_CONFIG_DIAGNOSTIC_CAPACITY & (RTOS_CONFIG_DIAGNOSTIC_CAPACITY - 1U)) != 0U)
#error "Diagnostic capacity must be a power of two in [8, 1024]"
#endif

typedef struct diag_slot {
    volatile uint32_t commit;
    rtos_diag_record_t record;
} diag_slot_t;

static diag_slot_t diag_slots[RTOS_CONFIG_DIAGNOSTIC_CAPACITY];
static uint32_t diag_session = 1U;
static uint32_t diag_next_sequence = 1U;
static uint32_t diag_session_first = 1U;
static uint32_t diag_identity_exhausted;
static uint32_t diag_sequence_exhausted;
static volatile uint32_t diag_frozen;
static volatile uint32_t diag_trigger;
static volatile uint32_t diag_writing;
static volatile uint32_t diag_fault_valid;
static volatile uint32_t diag_fault_code;
static volatile uintptr_t diag_fault_pc;

#if defined(RTOS_HIL_DIAG_WRITE_STAGE)
extern void rtos_hil_diag_write_stage(uint32_t stage);
#define DIAG_WRITE_STAGE(stage) rtos_hil_diag_write_stage(stage)
#else
#define DIAG_WRITE_STAGE(stage) ((void)0)
#endif

static rtos_status_t diag_context_permitted(rtos_bool_t allow_isr)
{
    if (RTOS_PORT_IN_ISR() == RTOS_FALSE) {
        return RTOS_OK;
    }
    if ((allow_isr == RTOS_FALSE) ||
        (rtos_port_isr_priority() < RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY)) {
        return RTOS_ERR_ISR;
    }
    return RTOS_OK;
}

/* Kernel writers are serialized by existing exclusion. Only legal syscall
 * contexts may enter here. High IRQs/NMI cannot use the normal recorder.
 * commit is the last publication; a terminal fault can leave an invalid slot.
 */
void rtos_diag_emit_locked(rtos_diag_event_t event, rtos_diag_operation_t operation,
                           const rtos_tcb_t *task, const rtos_diag_payload_t *payload)
{
    if (diag_frozen != 0U) {
        return;
    }
    uint32_t sequence = diag_next_sequence;
    diag_slot_t *slot = &diag_slots[(sequence - 1U) &
        (RTOS_CONFIG_DIAGNOSTIC_CAPACITY - 1U)];

    diag_writing = 1U;
    slot->commit = 0U;
    atomic_signal_fence(memory_order_seq_cst);
    DIAG_WRITE_STAGE(1U);
    slot->record.sequence = sequence;
    slot->record.tick = rtos_kernel.tick_count;
    slot->record.task_id = task != NULL ? task->task_id : 0U;
    slot->record.object = (uintptr_t)payload->object;
    slot->record.peer_id = payload->peer_id;
    slot->record.value = payload->value;
    slot->record.detail = payload->detail;
    slot->record.result = (int32_t)payload->result;
    slot->record.event = (uint8_t)event;
    slot->record.operation = (uint8_t)operation;
    slot->record.priority = task != NULL ? (uint8_t)task->priority : UINT8_MAX;
    slot->record.flags = RTOS_PORT_IN_ISR() != RTOS_FALSE ? RTOS_DIAG_FLAG_ISR : 0U;
    atomic_signal_fence(memory_order_seq_cst);
    DIAG_WRITE_STAGE(2U);
    slot->commit = sequence;
    atomic_signal_fence(memory_order_seq_cst);
    DIAG_WRITE_STAGE(3U);
    diag_writing = 0U;
    if (sequence == UINT32_MAX) {
        diag_sequence_exhausted = 1U;
        diag_trigger = RTOS_DIAG_TRIGGER_SEQUENCE_EXHAUSTED;
        diag_frozen = 1U;
    } else {
        diag_next_sequence = sequence + 1U;
    }
}

void rtos_diag_identity_exhausted_locked(void)
{
    diag_identity_exhausted = 1U;
    if (diag_frozen == 0U) {
        diag_trigger = RTOS_DIAG_TRIGGER_IDENTITY_EXHAUSTED;
        diag_frozen = 1U;
    }
}

void rtos_internal_diag_return_record(rtos_diag_operation_t operation,
    const rtos_tcb_t *task, uint32_t value, rtos_status_t result)
{
    RTOS_PORT_ENTER_CRITICAL();
    RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_RETURN, operation, task, NULL,
        value, 0U, result, 0U);
    RTOS_PORT_EXIT_CRITICAL();
}

rtos_status_t rtos_internal_diag_return_status(rtos_diag_operation_t operation,
    const rtos_tcb_t *task, rtos_status_t result)
{
    rtos_internal_diag_return_record(operation, task, 0U, result);
    return result;
}

uint32_t rtos_internal_diag_return_value(rtos_diag_operation_t operation,
    const rtos_tcb_t *task, uint32_t value, rtos_status_t result)
{
    rtos_internal_diag_return_record(operation, task, value, result);
    return value;
}

rtos_status_t rtos_diag_reset(void)
{
    rtos_status_t status = diag_context_permitted(RTOS_FALSE);

    if (status != RTOS_OK) {
        return status;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if ((diag_session == UINT32_MAX) || (diag_identity_exhausted != 0U) ||
        (diag_sequence_exhausted != 0U)) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_OVERFLOW;
    }
    /* Logical invalidation: sequence never rewinds, so no bulk RAM clearing
     * (and no capacity-dependent interrupt masking) is necessary. */
    diag_session++;
    diag_session_first = diag_next_sequence;
    diag_writing = 0U;
    diag_fault_valid = 0U;
    diag_fault_code = 0U;
    diag_fault_pc = 0U;
    diag_trigger = 0U;
    diag_frozen = 0U;
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_diag_freeze(uint32_t trigger)
{
    rtos_status_t status = diag_context_permitted(RTOS_TRUE);

    if (status != RTOS_OK) {
        return status;
    }
    if (trigger != RTOS_DIAG_TRIGGER_USER) {
        return RTOS_ERR_PARAM;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (diag_frozen == 0U) {
        diag_trigger = trigger;
        diag_frozen = 1U;
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_diag_mark(uint32_t value, uint32_t detail)
{
    rtos_status_t status = diag_context_permitted(RTOS_TRUE);

    if (status != RTOS_OK) {
        return status;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (diag_frozen != 0U) {
        status = RTOS_ERR_BUSY;
    } else {
        const rtos_tcb_t *task = RTOS_PORT_IN_ISR() == RTOS_FALSE ?
            rtos_kernel.current_tcb : NULL;
        RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_MARK, RTOS_DIAG_OP_NONE, task,
            NULL, value, detail, RTOS_OK, 0U);
    }
    RTOS_PORT_EXIT_CRITICAL();
    return status;
}

/* Does not trust global head/count at a terminal fault interruption. Each
 * committed slot is self-identifying, and all counters fit one native word.
 */
static rtos_status_t diag_copy_slot(uint32_t session, uint32_t index,
                                   rtos_diag_record_t *record, uint32_t *has_record)
{
    rtos_status_t status = RTOS_OK;

    RTOS_PORT_ENTER_CRITICAL();
    if (session != diag_session) {
        status = RTOS_ERR_STALE;
    } else if (diag_frozen == 0U) {
        status = RTOS_ERR_BUSY;
    } else {
        const diag_slot_t *slot = &diag_slots[index];
        uint32_t commit = slot->commit;

        atomic_signal_fence(memory_order_seq_cst);
        *has_record = 0U;
        if ((commit >= diag_session_first) && (commit != 0U) &&
            (commit == slot->record.sequence)) {
            *record = slot->record;
            *has_record = 1U;
        }
    }
    RTOS_PORT_EXIT_CRITICAL();
    return status;
}

rtos_status_t rtos_diag_get_info(rtos_diag_info_t *info)
{
    rtos_status_t status = diag_context_permitted(RTOS_FALSE);
    rtos_diag_info_t snapshot = {0};
    uint32_t session_first;

    if (status != RTOS_OK) {
        return status;
    }
    if (info == NULL) {
        return RTOS_ERR_PARAM;
    }
    RTOS_PORT_ENTER_CRITICAL();
    if (diag_frozen == 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_BUSY;
    }
    snapshot.session = diag_session;
    snapshot.capacity = RTOS_CONFIG_DIAGNOSTIC_CAPACITY;
    session_first = diag_session_first;
    snapshot.trigger = diag_trigger;
    snapshot.flags = diag_writing != 0U ? RTOS_DIAG_FLAG_PARTIAL_WRITE : 0U;
    if (diag_fault_valid != 0U) {
        snapshot.flags |= RTOS_DIAG_FLAG_FAULT;
        snapshot.fault_code = diag_fault_code;
        snapshot.fault_pc = diag_fault_pc;
    }
    RTOS_PORT_EXIT_CRITICAL();
    for (uint32_t index = 0U; index < RTOS_CONFIG_DIAGNOSTIC_CAPACITY; index++) {
        rtos_diag_record_t record = {0};
        uint32_t has_record;

        status = diag_copy_slot(snapshot.session, index, &record, &has_record);
        if (status != RTOS_OK) {
            return status;
        }
        if (has_record != 0U) {
            if ((snapshot.count == 0U) || (record.sequence < snapshot.first_sequence)) {
                snapshot.first_sequence = record.sequence;
            }
            if (record.sequence > snapshot.last_sequence) {
                snapshot.last_sequence = record.sequence;
            }
            snapshot.count++;
        }
    }
    if (snapshot.count != 0U) {
        snapshot.overwritten = snapshot.last_sequence - session_first + 1U - snapshot.count;
    }
    *info = snapshot;
    return RTOS_OK;
}

rtos_status_t rtos_diag_read(uint32_t session, uint32_t sequence,
                            rtos_diag_record_t *record)
{
    rtos_status_t status = diag_context_permitted(RTOS_FALSE);
    rtos_diag_record_t snapshot;
    uint32_t has_record;

    if (status != RTOS_OK) {
        return status;
    }
    if ((record == NULL) || (sequence == 0U)) {
        return RTOS_ERR_PARAM;
    }
    status = diag_copy_slot(session, (sequence - 1U) &
        (RTOS_CONFIG_DIAGNOSTIC_CAPACITY - 1U), &snapshot, &has_record);
    if (status != RTOS_OK) {
        return status;
    }
    if ((has_record == 0U) || (snapshot.sequence != sequence)) {
        return RTOS_ERR_STALE;
    }
    *record = snapshot;
    return RTOS_OK;
}

static void diag_begin_explanation(rtos_diag_explanation_t *explanation,
                                    const rtos_diag_record_t *record)
{
    uint32_t session = explanation->session;

    memset(explanation, 0, sizeof(*explanation));
    explanation->session = session;
    explanation->task_id = record->task_id;
    explanation->episode = record->sequence;
    explanation->object = record->object;
    explanation->peer_id = record->peer_id;
    explanation->timeout = record->value;
    explanation->detail = record->detail;
    explanation->wait_tick = record->tick;
    explanation->operation = (rtos_diag_operation_t)record->operation;
    explanation->phase = RTOS_DIAG_PHASE_WAITING;
}

static rtos_diag_cause_t diag_wake_cause(const rtos_diag_explanation_t *explanation,
                                        int32_t result)
{
    if (result == RTOS_OK) {
        if (explanation->operation == RTOS_DIAG_OP_DELAY) {
            return RTOS_DIAG_CAUSE_DELAY_ELAPSED;
        }
        if ((explanation->operation == RTOS_DIAG_OP_CHANNEL_ACQUIRE) ||
            (explanation->operation == RTOS_DIAG_OP_CHANNEL_RECEIVE)) {
            return RTOS_DIAG_CAUSE_RETRY_NOTIFICATION;
        }
        return RTOS_DIAG_CAUSE_RESOURCE;
    }
    switch (result) {
        case RTOS_ERR_TIMEOUT:
            return RTOS_DIAG_CAUSE_TIMEOUT;
        case RTOS_ERR_DELETED:
            return RTOS_DIAG_CAUSE_OBJECT_DELETED;
        case RTOS_ERR_CLOSED:
            return RTOS_DIAG_CAUSE_CHANNEL_CLOSED;
        default:
            return RTOS_DIAG_CAUSE_OTHER;
    }
}

static void diag_apply_record(rtos_diag_explanation_t *explanation,
                               const rtos_diag_record_t *record)
{
    if (record->event == RTOS_DIAG_EVENT_WAIT) {
        diag_begin_explanation(explanation, record);
        return;
    }
    if (explanation->episode == 0U) {
        return;
    }
    /* Explain the last wait episode, not the task's later application state.
     * Parking/deleting a task after the API has returned cannot un-return it.
     */
    if ((explanation->phase == RTOS_DIAG_PHASE_RETURNED) ||
        (explanation->phase == RTOS_DIAG_PHASE_DELETED)) {
        return;
    }
    switch ((rtos_diag_event_t)record->event) {
        case RTOS_DIAG_EVENT_WAKE:
            explanation->wake_tick = record->tick;
            explanation->wake_result = record->result;
            explanation->cause = diag_wake_cause(explanation, record->result);
            explanation->phase = RTOS_DIAG_PHASE_READY;
            break;
        case RTOS_DIAG_EVENT_RUN:
            if (explanation->phase == RTOS_DIAG_PHASE_READY) {
                explanation->run_tick = record->tick;
                explanation->phase = RTOS_DIAG_PHASE_RUNNING;
            }
            break;
        case RTOS_DIAG_EVENT_RETURN:
            if ((rtos_diag_operation_t)record->operation != explanation->operation) {
                explanation->flags |= RTOS_DIAG_FLAG_INCOMPLETE;
                break;
            }
            explanation->return_result = record->result;
            if (explanation->phase != RTOS_DIAG_PHASE_RUNNING) {
                explanation->flags |= RTOS_DIAG_FLAG_INCOMPLETE;
            }
            explanation->phase = RTOS_DIAG_PHASE_RETURNED;
            break;
        case RTOS_DIAG_EVENT_SUSPEND:
            if (record->value != 0U) {
                explanation->wake_result = record->result;
                explanation->cause = RTOS_DIAG_CAUSE_TASK_SUSPENDED;
            }
            explanation->phase = RTOS_DIAG_PHASE_SUSPENDED;
            break;
        case RTOS_DIAG_EVENT_RESUME:
            explanation->phase = RTOS_DIAG_PHASE_READY;
            break;
        case RTOS_DIAG_EVENT_DELETE:
            explanation->phase = RTOS_DIAG_PHASE_DELETED;
            explanation->cause = RTOS_DIAG_CAUSE_TASK_DELETED;
            break;
        default:
            break;
    }
}

rtos_status_t rtos_diag_explain(uint32_t session, uint32_t task_id,
                               rtos_diag_explanation_t *explanation)
{
    rtos_diag_info_t info;
    rtos_diag_explanation_t snapshot = {0};
    rtos_status_t status;

    if (diag_context_permitted(RTOS_FALSE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if ((explanation == NULL) || (task_id == 0U)) {
        return RTOS_ERR_PARAM;
    }
    status = rtos_diag_get_info(&info);
    if (status != RTOS_OK) {
        return status;
    }
    if (session != info.session) {
        return RTOS_ERR_STALE;
    }
    snapshot.session = session;
    snapshot.task_id = task_id;
    uint32_t span = info.count != 0U ? info.last_sequence - info.first_sequence + 1U : 0U;
    if (span > RTOS_CONFIG_DIAGNOSTIC_CAPACITY) {
        return RTOS_ERR_OVERFLOW;
    }
    for (uint32_t offset = 0U; offset < span; offset++) {
        rtos_diag_record_t record;
        uint32_t sequence = info.first_sequence + offset;

        status = rtos_diag_read(session, sequence, &record);
        if (status == RTOS_ERR_STALE) {
            snapshot.flags |= RTOS_DIAG_FLAG_INCOMPLETE;
            continue;
        }
        if (status != RTOS_OK) {
            return status;
        }
        if (record.task_id == task_id) {
            diag_apply_record(&snapshot, &record);
        }
    }
    /* A concurrent reset must not turn a mixed snapshot into an explanation. */
    RTOS_PORT_ENTER_CRITICAL();
    status = session != diag_session ? RTOS_ERR_STALE : RTOS_OK;
    RTOS_PORT_EXIT_CRITICAL();
    if (status != RTOS_OK) {
        return status;
    }
    if ((snapshot.episode == 0U) || (snapshot.flags != 0U) ||
        ((info.flags & RTOS_DIAG_FLAG_PARTIAL_WRITE) != 0U)) {
        snapshot.flags |= RTOS_DIAG_FLAG_INCOMPLETE;
        snapshot.phase = RTOS_DIAG_PHASE_UNKNOWN;
    }
    *explanation = snapshot;
    return RTOS_OK;
}

void rtos_diag_fault_freeze(uint32_t fault_code, uintptr_t fault_pc)
{
    /* Terminal fault only: an interrupted normal writer never resumes. Do not
     * touch nesting, a TCB, its stack, or an object which may itself be corrupt.
     * NMI is deliberately excluded; it needs a separate application policy.
     */
    uint32_t was_frozen = diag_frozen;

    diag_frozen = 1U;
    atomic_signal_fence(memory_order_seq_cst);
    if (diag_fault_valid == 0U) {
        diag_fault_code = fault_code;
        diag_fault_pc = fault_pc;
        atomic_signal_fence(memory_order_seq_cst);
        diag_fault_valid = 1U;
        if (was_frozen == 0U) {
            diag_trigger = RTOS_DIAG_TRIGGER_FAULT;
        }
    }
}

#else

rtos_status_t rtos_diag_reset(void)
{
    return RTOS_ERR_DISABLED;
}

rtos_status_t rtos_diag_freeze(uint32_t trigger)
{
    (void)trigger;
    return RTOS_ERR_DISABLED;
}

rtos_status_t rtos_diag_mark(uint32_t value, uint32_t detail)
{
    (void)value;
    (void)detail;
    return RTOS_ERR_DISABLED;
}

rtos_status_t rtos_diag_get_info(rtos_diag_info_t *info)
{
    (void)info;
    return RTOS_ERR_DISABLED;
}

rtos_status_t rtos_diag_read(uint32_t session, uint32_t sequence,
                            rtos_diag_record_t *record)
{
    (void)session;
    (void)sequence;
    (void)record;
    return RTOS_ERR_DISABLED;
}

rtos_status_t rtos_diag_explain(uint32_t session, uint32_t task_id,
                               rtos_diag_explanation_t *explanation)
{
    (void)session;
    (void)task_id;
    (void)explanation;
    return RTOS_ERR_DISABLED;
}

void rtos_diag_fault_freeze(uint32_t fault_code, uintptr_t fault_pc)
{
    (void)fault_code;
    (void)fault_pc;
}

#endif /* RTOS_CONFIG_USE_DIAGNOSTICS */

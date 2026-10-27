/** @file diagnostic_model.c
 * @brief Independent bounded FIFO oracle plus explicit interrupted-write tests.
 *
 * Includes the actual production implementation so sequence/session exhaustion
 * can be injected without exposing test-only public APIs. Mock exclusion does
 * not validate Cortex-M timing, memory ordering, PendSV or actual IRQ nesting.
 */
#include "host_port.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "../../../RTOS/Services/rtos_diag.c"

rtos_kernel_t rtos_kernel;
uint32_t host_isr;
uint32_t host_priority = 0x50U;
uint32_t host_nesting;
uint32_t host_enter_count;
uint32_t host_exit_count;
uint32_t host_reset_at_exit;

static uint32_t checks;
static uint32_t failures;
static uint32_t random_state = 0x97F44621U;
static uint32_t interrupt_stage;
static jmp_buf fault_escape;
static rtos_tcb_t task;
static uint32_t object_label;

void host_enter_critical(void)
{
    host_nesting++;
    host_enter_count++;
}

void host_exit_critical(void)
{
    host_nesting--;
    host_exit_count++;
    if ((host_reset_at_exit != 0U) && (--host_reset_at_exit == 0U)) {
        (void)rtos_diag_reset();
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

void rtos_hil_diag_write_stage(uint32_t stage)
{
    if (stage == interrupt_stage) {
        rtos_diag_fault_freeze(0xA55AF00DU, (uintptr_t)0x08001235U);
        longjmp(fault_escape, 1);
    }
}

static void expect(const char *name, uint32_t condition)
{
    checks++;
    if (condition == 0U) {
        failures++;
        (void)printf("HOST|FAIL|id=%lu name=%s\n", (unsigned long)checks, name);
    }
}

static void clean_state(void)
{
    memset(diag_slots, 0, sizeof(diag_slots));
    diag_session = 1U;
    diag_next_sequence = 1U;
    diag_session_first = 1U;
    diag_identity_exhausted = 0U;
    diag_sequence_exhausted = 0U;
    diag_frozen = 0U;
    diag_trigger = 0U;
    diag_writing = 0U;
    diag_fault_valid = 0U;
    diag_fault_code = 0U;
    diag_fault_pc = 0U;
    host_isr = 0U;
    host_priority = 0x50U;
    host_nesting = 0U;
    host_enter_count = 0U;
    host_exit_count = 0U;
    host_reset_at_exit = 0U;
    interrupt_stage = 0U;
    memset(&rtos_kernel, 0, sizeof(rtos_kernel));
    memset(&task, 0, sizeof(task));
    task.task_id = 72U;
    task.priority = 8U;
    rtos_kernel.current_tcb = &task;
}

static uint32_t random_word(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return random_state;
}

static rtos_diag_info_t freeze_info(void)
{
    rtos_diag_info_t info = {0};

    expect("freeze", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    expect("frozen_info", rtos_diag_get_info(&info) == RTOS_OK);
    return info;
}

static void emit(rtos_diag_event_t event, rtos_diag_operation_t operation,
                  uint32_t value, uint32_t detail, rtos_status_t result)
{
    RTOS_PORT_ENTER_CRITICAL();
    RTOS_DIAG_RECORD(event, operation, &task, &object_label, value, detail, result, 44U);
    RTOS_PORT_EXIT_CRITICAL();
}

static void test_parameters(void)
{
    rtos_diag_info_t info;
    rtos_diag_info_t old_info;
    rtos_diag_record_t record;
    rtos_diag_record_t old_record;
    rtos_diag_explanation_t explanation;
    rtos_diag_explanation_t old_explanation;

    clean_state();
    memset(&info, 0xA5, sizeof(info));
    old_info = info;
    memset(&record, 0xA5, sizeof(record));
    old_record = record;
    memset(&explanation, 0xA5, sizeof(explanation));
    old_explanation = explanation;
    expect("info_null", rtos_diag_get_info(NULL) == RTOS_ERR_PARAM);
    expect("info_busy", rtos_diag_get_info(&info) == RTOS_ERR_BUSY);
    expect("info_busy_unchanged", memcmp(&info, &old_info, sizeof(info)) == 0);
    expect("read_null", rtos_diag_read(1U, 1U, NULL) == RTOS_ERR_PARAM);
    expect("read_zero", rtos_diag_read(1U, 0U, &record) == RTOS_ERR_PARAM);
    expect("read_busy", rtos_diag_read(1U, 1U, &record) == RTOS_ERR_BUSY);
    expect("read_busy_unchanged", memcmp(&record, &old_record, sizeof(record)) == 0);
    expect("explain_null", rtos_diag_explain(1U, 1U, NULL) == RTOS_ERR_PARAM);
    expect("explain_zero", rtos_diag_explain(1U, 0U, &explanation) == RTOS_ERR_PARAM);
    expect("explain_busy", rtos_diag_explain(1U, 1U, &explanation) == RTOS_ERR_BUSY);
    expect("explain_busy_unchanged", memcmp(&explanation, &old_explanation,
        sizeof(explanation)) == 0);
    expect("invalid_trigger", rtos_diag_freeze(0U) == RTOS_ERR_PARAM);
    info = freeze_info();
    expect("empty_info", info.count == 0U && info.first_sequence == 0U
        && info.last_sequence == 0U && info.capacity == RTOS_CONFIG_DIAGNOSTIC_CAPACITY);
    expect("empty_read", rtos_diag_read(info.session, 1U, &record) == RTOS_ERR_STALE);
    expect("mark_frozen", rtos_diag_mark(1U, 2U) == RTOS_ERR_BUSY);
    expect("empty_explain", rtos_diag_explain(info.session, task.task_id,
        &explanation) == RTOS_OK && explanation.phase == RTOS_DIAG_PHASE_UNKNOWN
        && (explanation.flags & RTOS_DIAG_FLAG_INCOMPLETE) != 0U);
    expect("reset", rtos_diag_reset() == RTOS_OK);
    expect("old_session_read", rtos_diag_read(info.session, 1U, &record) == RTOS_ERR_STALE);
    rtos_diag_info_t next = freeze_info();
    expect("session_increment", next.session == info.session + 1U);
    expect("old_session_explain", rtos_diag_explain(info.session, task.task_id,
        &explanation) == RTOS_ERR_STALE);
    (void)printf("HOST|GROUP|parameters=PASS\n");
}

static void test_fifo_model(void)
{
    uint32_t expected_values[RTOS_CONFIG_DIAGNOSTIC_CAPACITY];
    uint32_t expected_details[RTOS_CONFIG_DIAGNOSTIC_CAPACITY];
    uint32_t total_operations = 0U;

    clean_state();
    for (uint32_t round = 0U; round < 160U; round++) {
        expect("model_reset", rtos_diag_reset() == RTOS_OK);
        uint32_t first_sequence = diag_next_sequence;
        uint32_t count = random_word() % (RTOS_CONFIG_DIAGNOSTIC_CAPACITY * 3U + 7U);

        for (uint32_t index = 0U; index < count; index++) {
            uint32_t value = random_word();
            uint32_t detail = random_word();
            uint32_t position = index % RTOS_CONFIG_DIAGNOSTIC_CAPACITY;

            rtos_kernel.tick_count = UINT32_MAX - index;
            expect("model_mark", rtos_diag_mark(value, detail) == RTOS_OK);
            expected_values[position] = value;
            expected_details[position] = detail;
            total_operations++;
        }
        rtos_diag_info_t info = freeze_info();
        uint32_t retained = count < RTOS_CONFIG_DIAGNOSTIC_CAPACITY ?
            count : RTOS_CONFIG_DIAGNOSTIC_CAPACITY;
        uint32_t overwritten = count - retained;

        expect("model_count", info.count == retained && info.overwritten == overwritten);
        expect("model_range", count == 0U ?
            (info.first_sequence == 0U && info.last_sequence == 0U) :
            (info.first_sequence == first_sequence + overwritten
                && info.last_sequence == first_sequence + count - 1U));
        for (uint32_t index = overwritten; index < count; index++) {
            uint32_t position = index % RTOS_CONFIG_DIAGNOSTIC_CAPACITY;
            rtos_diag_record_t record = {0};

            expect("model_read", rtos_diag_read(info.session, first_sequence + index,
                &record) == RTOS_OK);
            expect("model_fields", record.sequence == first_sequence + index
                && record.value == expected_values[position]
                && record.detail == expected_details[position]
                && record.tick == UINT32_MAX - index && record.task_id == task.task_id
                && record.event == RTOS_DIAG_EVENT_MARK && record.flags == 0U);
        }
        if (overwritten != 0U) {
            rtos_diag_record_t record = {0};

            expect("model_old_rejected", rtos_diag_read(info.session, first_sequence,
                &record) == RTOS_ERR_STALE);
        }
    }
    (void)printf("HOST|MODEL|rounds=160 operations=%lu capacity=%u status=PASS\n",
        (unsigned long)total_operations, (unsigned int)RTOS_CONFIG_DIAGNOSTIC_CAPACITY);
}

static void test_explanation(void)
{
    const rtos_diag_operation_t operations[] = {RTOS_DIAG_OP_DELAY, RTOS_DIAG_OP_SEM,
        RTOS_DIAG_OP_QUEUE_SEND, RTOS_DIAG_OP_QUEUE_RECEIVE, RTOS_DIAG_OP_MUTEX,
        RTOS_DIAG_OP_EVENT, RTOS_DIAG_OP_NOTIFY, RTOS_DIAG_OP_CHANNEL_ACQUIRE,
        RTOS_DIAG_OP_CHANNEL_RECEIVE};
    const rtos_diag_event_t events[] = {RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_EVENT_WAKE,
        RTOS_DIAG_EVENT_RUN, RTOS_DIAG_EVENT_RETURN, RTOS_DIAG_EVENT_SUSPEND,
        RTOS_DIAG_EVENT_RESUME, RTOS_DIAG_EVENT_DELETE};
    const rtos_diag_phase_t phases[] = {RTOS_DIAG_PHASE_WAITING, RTOS_DIAG_PHASE_READY,
        RTOS_DIAG_PHASE_RUNNING, RTOS_DIAG_PHASE_RETURNED, RTOS_DIAG_PHASE_RETURNED,
        RTOS_DIAG_PHASE_RETURNED, RTOS_DIAG_PHASE_RETURNED};

    for (uint32_t operation = 0U; operation < sizeof(operations) / sizeof(operations[0]);
         operation++) {
        for (uint32_t step = 0U; step < sizeof(events) / sizeof(events[0]); step++) {
            clean_state();
            for (uint32_t index = 0U; index <= step; index++) {
                rtos_kernel.tick_count = index + 91U;
                emit(events[index], (index == 0U || index == 3U) ? operations[operation]
                    : RTOS_DIAG_OP_NONE,
                    index == 0U ? 27U : 0U, index == 0U ? 0xA55AU : 0U,
                    index == 1U ? RTOS_OK : RTOS_ERR_TIMEOUT);
            }
            rtos_diag_info_t info = freeze_info();
            rtos_diag_explanation_t result = {0};

            expect("explain_status", rtos_diag_explain(info.session, task.task_id,
                &result) == RTOS_OK);
            expect("explain_episode", result.episode == 1U && result.task_id == task.task_id
                && result.object == (uintptr_t)&object_label && result.timeout == 27U
                && result.detail == 0xA55AU && result.peer_id == 44U
                && result.wait_tick == 91U && result.operation == operations[operation]);
            expect("explain_phase", result.phase == phases[step] && result.flags == 0U);
            rtos_diag_cause_t expected_cause = RTOS_DIAG_CAUSE_RESOURCE;
            if (operations[operation] == RTOS_DIAG_OP_DELAY) {
                expected_cause = RTOS_DIAG_CAUSE_DELAY_ELAPSED;
            } else if (operations[operation] == RTOS_DIAG_OP_CHANNEL_ACQUIRE
                || operations[operation] == RTOS_DIAG_OP_CHANNEL_RECEIVE) {
                expected_cause = RTOS_DIAG_CAUSE_RETRY_NOTIFICATION;
            }
            if (step == 0U) {
                expected_cause = RTOS_DIAG_CAUSE_NONE;
            }
            expect("explain_cause", result.cause == expected_cause);
            if (step >= 3U) {
                expect("actual_return_distinct", result.wake_result == RTOS_OK
                    && result.return_result == RTOS_ERR_TIMEOUT);
            }
        }
    }
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 3U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_RETURN, RTOS_DIAG_OP_SEM, 0U, 0U, RTOS_ERR_DELETED);
    uint32_t old_episode = 1U;
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_QUEUE_RECEIVE, 9U, 7U, RTOS_OK);
    rtos_diag_info_t info = freeze_info();
    rtos_diag_explanation_t result = {0};

    expect("new_episode_status", rtos_diag_explain(info.session, task.task_id, &result) == RTOS_OK);
    expect("same_address_new_episode", result.episode > old_episode
        && result.operation == RTOS_DIAG_OP_QUEUE_RECEIVE && result.timeout == 9U
        && result.phase == RTOS_DIAG_PHASE_WAITING);
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
    for (uint32_t index = 0U; index < RTOS_CONFIG_DIAGNOSTIC_CAPACITY; index++) {
        expect("overwrite_mark", rtos_diag_mark(index, 0U) == RTOS_OK);
    }
    emit(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    info = freeze_info();
    expect("lost_begin_status", rtos_diag_explain(info.session, task.task_id, &result) == RTOS_OK);
    expect("lost_begin_unknown", result.episode == 0U
        && result.phase == RTOS_DIAG_PHASE_UNKNOWN
        && (result.flags & RTOS_DIAG_FLAG_INCOMPLETE) != 0U);
    (void)printf("HOST|GROUP|explanation=PASS operations=9 phases=7\n");
}

static void test_conservative_history(void)
{
    const rtos_status_t wake_results[] = {RTOS_ERR_TIMEOUT, RTOS_ERR_DELETED,
        RTOS_ERR_CLOSED, RTOS_ERR_PARAM};
    const rtos_diag_cause_t causes[] = {RTOS_DIAG_CAUSE_TIMEOUT,
        RTOS_DIAG_CAUSE_OBJECT_DELETED, RTOS_DIAG_CAUSE_CHANNEL_CLOSED, RTOS_DIAG_CAUSE_OTHER};

    for (uint32_t index = 0U; index < sizeof(wake_results) / sizeof(wake_results[0]); index++) {
        clean_state();
        emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
        emit(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, 0U, 0U, wake_results[index]);
        rtos_diag_info_t info = freeze_info();
        rtos_diag_explanation_t result = {0};

        expect("wake_cause_status", rtos_diag_explain(info.session, task.task_id,
            &result) == RTOS_OK);
        expect("wake_cause_exact", result.cause == causes[index]
            && result.wake_result == wake_results[index] && result.phase == RTOS_DIAG_PHASE_READY);
    }
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_SUSPEND, RTOS_DIAG_OP_NONE, 1U, 0U, RTOS_ERR_DELETED);
    rtos_diag_info_t info = freeze_info();
    rtos_diag_explanation_t result = {0};

    expect("suspend_cause_status", rtos_diag_explain(info.session, task.task_id,
        &result) == RTOS_OK);
    expect("suspend_distinct_from_object_deinit", result.cause == RTOS_DIAG_CAUSE_TASK_SUSPENDED
        && result.wake_result == RTOS_ERR_DELETED && result.phase == RTOS_DIAG_PHASE_SUSPENDED);
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_RUN, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_RETURN, RTOS_DIAG_OP_QUEUE_RECEIVE, 0U, 0U, RTOS_ERR_TIMEOUT);
    info = freeze_info();
    expect("mismatched_return_status", rtos_diag_explain(info.session, task.task_id,
        &result) == RTOS_OK);
    expect("mismatched_return_unknown", result.phase == RTOS_DIAG_PHASE_UNKNOWN
        && (result.flags & RTOS_DIAG_FLAG_INCOMPLETE) != 0U);
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_RETURN, RTOS_DIAG_OP_SEM, 0U, 0U, RTOS_OK);
    info = freeze_info();
    expect("missing_dispatch_status", rtos_diag_explain(info.session, task.task_id,
        &result) == RTOS_OK);
    expect("missing_dispatch_unknown", result.phase == RTOS_DIAG_PHASE_UNKNOWN
        && (result.flags & RTOS_DIAG_FLAG_INCOMPLETE) != 0U);
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_RUN, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    /* Emulate a damaged/incomplete committed history. Reader must never join
     * separated records into apparently complete proof of a wait episode. */
    diag_slots[1].commit = 0U;
    info = freeze_info();
    expect("hole_status", rtos_diag_explain(info.session, task.task_id, &result) == RTOS_OK);
    expect("hole_unknown", result.phase == RTOS_DIAG_PHASE_UNKNOWN
        && (result.flags & RTOS_DIAG_FLAG_INCOMPLETE) != 0U);
    clean_state();
    expect("read_race_marker", rtos_diag_mark(5U, 6U) == RTOS_OK);
    info = freeze_info();
    rtos_diag_info_t output;
    rtos_diag_info_t old_output;

    memset(&output, 0xA5, sizeof(output));
    old_output = output;
    host_reset_at_exit = 1U;
    expect("reset_during_info_stale", rtos_diag_get_info(&output) == RTOS_ERR_STALE);
    expect("reset_during_info_output_unchanged", memcmp(&output, &old_output, sizeof(output)) == 0);
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
    info = freeze_info();
    rtos_diag_explanation_t old_result;

    memset(&result, 0x5A, sizeof(result));
    old_result = result;
    host_reset_at_exit = RTOS_CONFIG_DIAGNOSTIC_CAPACITY + 1U;
    expect("reset_during_explain_stale", rtos_diag_explain(info.session, task.task_id,
        &result) == RTOS_ERR_STALE);
    expect("reset_during_explain_output_unchanged", memcmp(&result, &old_result,
        sizeof(result)) == 0);
    (void)printf("HOST|GROUP|conservative_history=PASS\n");
}

static void test_episode_lifecycle(void)
{
    const rtos_diag_operation_t operations[] = {RTOS_DIAG_OP_SEM, RTOS_DIAG_OP_DELAY};

    for (uint32_t index = 0U; index < sizeof(operations) / sizeof(operations[0]); index++) {
        clean_state();
        emit(RTOS_DIAG_EVENT_WAIT, operations[index], 9U, 0U, RTOS_OK);
        emit(RTOS_DIAG_EVENT_SUSPEND, RTOS_DIAG_OP_NONE, 1U, 0U, RTOS_ERR_DELETED);
        emit(RTOS_DIAG_EVENT_RESUME, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
        emit(RTOS_DIAG_EVENT_RUN, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
        rtos_status_t returned = operations[index] == RTOS_DIAG_OP_DELAY
            ? RTOS_OK : RTOS_ERR_DELETED;

        emit(RTOS_DIAG_EVENT_RETURN, operations[index], 0U, 0U, returned);
        rtos_diag_info_t info = freeze_info();
        rtos_diag_explanation_t result = {0};

        expect("cancel_resume_status", rtos_diag_explain(info.session, task.task_id,
            &result) == RTOS_OK);
        expect("cancel_resume_returned", result.phase == RTOS_DIAG_PHASE_RETURNED
            && result.wake_result == RTOS_ERR_DELETED && result.return_result == returned
            && result.cause == RTOS_DIAG_CAUSE_TASK_SUSPENDED);
    }
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_SUSPEND, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_RESUME, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_RUN, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_RETURN, RTOS_DIAG_OP_SEM, 0U, 0U, RTOS_OK);
    rtos_diag_info_t info = freeze_info();
    rtos_diag_explanation_t result = {0};

    expect("ready_suspend_resume_status", rtos_diag_explain(info.session, task.task_id,
        &result) == RTOS_OK);
    expect("ready_suspend_keeps_grant", result.phase == RTOS_DIAG_PHASE_RETURNED
        && result.wake_result == RTOS_OK && result.return_result == RTOS_OK
        && result.cause == RTOS_DIAG_CAUSE_RESOURCE);
    clean_state();
    emit(RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_OP_SEM, 9U, 0U, RTOS_OK);
    emit(RTOS_DIAG_EVENT_DELETE, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_ERR_DELETED);
    emit(RTOS_DIAG_EVENT_WAKE, RTOS_DIAG_OP_NONE, 0U, 0U, RTOS_OK);
    info = freeze_info();
    expect("deleted_status", rtos_diag_explain(info.session, task.task_id, &result) == RTOS_OK);
    expect("deleted_episode_terminal", result.phase == RTOS_DIAG_PHASE_DELETED
        && result.cause == RTOS_DIAG_CAUSE_TASK_DELETED);
    (void)printf("HOST|GROUP|episode_lifecycle=PASS\n");
}

static void test_irq_contract(void)
{
    const uint32_t priorities[] = {0U, 0x40U, 0x50U, 0xF0U};

    for (uint32_t index = 0U; index < sizeof(priorities) / sizeof(priorities[0]); index++) {
        clean_state();
        host_isr = 1U;
        host_priority = priorities[index];
        uint32_t legal = host_priority >= RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY;
        rtos_status_t expected = legal != 0U ? RTOS_OK : RTOS_ERR_ISR;
        rtos_diag_info_t info = {0};
        rtos_diag_record_t record = {0};
        rtos_diag_explanation_t result = {0};

        expect("irq_mark", rtos_diag_mark(11U, 12U) == expected);
        expect("irq_freeze", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == expected);
        expect("irq_reset_rejected", rtos_diag_reset() == RTOS_ERR_ISR);
        expect("irq_info_rejected", rtos_diag_get_info(&info) == RTOS_ERR_ISR);
        expect("irq_read_rejected", rtos_diag_read(1U, 1U, &record) == RTOS_ERR_ISR);
        expect("irq_explain_rejected", rtos_diag_explain(1U, task.task_id,
            &result) == RTOS_ERR_ISR);
        expect("illegal_irq_no_lock", legal != 0U || host_enter_count == 0U);
        host_isr = 0U;
        info = freeze_info();
        expect("irq_count", info.count == legal);
        if (legal != 0U) {
            expect("irq_read", rtos_diag_read(info.session, info.first_sequence,
                &record) == RTOS_OK);
            expect("irq_tag", record.flags == RTOS_DIAG_FLAG_ISR && record.task_id == 0U);
        }
    }
    (void)printf("HOST|GROUP|irq_contract=PASS priorities=4\n");
}

static uint32_t interrupt_write(uint32_t stage)
{
    interrupt_stage = stage;
    if (setjmp(fault_escape) == 0) {
        (void)rtos_diag_mark(99U, 88U);
        return 0U;
    }
    return 1U;
}

static void test_interrupted_publication(void)
{
    for (uint32_t is_full = 0U; is_full < 2U; is_full++) {
        for (uint32_t stage = 1U; stage <= 3U; stage++) {
            clean_state();
            uint32_t before = is_full != 0U ? RTOS_CONFIG_DIAGNOSTIC_CAPACITY : 2U;

            for (uint32_t index = 0U; index < before; index++) {
                expect("before_fault_mark", rtos_diag_mark(index, 0U) == RTOS_OK);
            }
            expect("fault_must_escape", interrupt_write(stage) == 1U);
            /* A real terminal handler never returns. The host reader stands for
             * a debugger inspecting immutable SRAM, not resuming the writer. */
            interrupt_stage = 0U;
            host_nesting = 0U;
            uint32_t enter_before = host_enter_count;

            rtos_kernel.current_tcb = (rtos_tcb_t *)(uintptr_t)1U;
            rtos_diag_fault_freeze(0xBADU, (uintptr_t)1U);
            expect("fault_no_lock", host_enter_count == enter_before);
            rtos_diag_info_t info = {0};

            expect("fault_info", rtos_diag_get_info(&info) == RTOS_OK);
            expect("fault_first_preserved", info.fault_code == 0xA55AF00DU
                && info.fault_pc == (uintptr_t)0x08001235U
                && info.trigger == RTOS_DIAG_TRIGGER_FAULT);
            expect("fault_partial_visible", (info.flags & (RTOS_DIAG_FLAG_FAULT
                | RTOS_DIAG_FLAG_PARTIAL_WRITE)) == (RTOS_DIAG_FLAG_FAULT
                    | RTOS_DIAG_FLAG_PARTIAL_WRITE));
            uint32_t expected_count = stage == 3U ? (is_full != 0U ? before : before + 1U)
                : (is_full != 0U ? before - 1U : before);

            expect("fault_committed_count", info.count == expected_count);
            rtos_diag_record_t record = {0};
            rtos_status_t expected = stage == 3U ? RTOS_OK : RTOS_ERR_STALE;

            expect("fault_never_reads_half_slot", rtos_diag_read(info.session, before + 1U,
                &record) == expected);
            rtos_diag_explanation_t result = {0};

            expect("fault_explain", rtos_diag_explain(info.session, task.task_id,
                &result) == RTOS_OK && result.phase == RTOS_DIAG_PHASE_UNKNOWN
                && (result.flags & RTOS_DIAG_FLAG_INCOMPLETE) != 0U);
        }
    }
    (void)printf("HOST|GROUP|terminal_publication=PASS stages=3 occupancy=2\n");
}

static void test_counter_exhaustion(void)
{
    clean_state();
    diag_next_sequence = UINT32_MAX - 1U;
    diag_session_first = diag_next_sequence;
    expect("near_max_mark", rtos_diag_mark(1U, 0U) == RTOS_OK);
    expect("max_mark", rtos_diag_mark(2U, 0U) == RTOS_OK);
    rtos_diag_info_t info = {0};

    expect("sequence_exhaustion_info", rtos_diag_get_info(&info) == RTOS_OK
        && info.trigger == RTOS_DIAG_TRIGGER_SEQUENCE_EXHAUSTED
        && info.last_sequence == UINT32_MAX && info.count == 2U);
    expect("sequence_no_wrap", rtos_diag_mark(3U, 0U) == RTOS_ERR_BUSY
        && diag_next_sequence == UINT32_MAX);
    expect("sequence_reset_blocked", rtos_diag_reset() == RTOS_ERR_OVERFLOW);
    rtos_diag_fault_freeze(99U, (uintptr_t)0x08002223U);
    expect("fault_cannot_reenable_sequence", rtos_diag_reset() == RTOS_ERR_OVERFLOW);
    clean_state();
    diag_session = UINT32_MAX;
    expect("session_reset_blocked", rtos_diag_reset() == RTOS_ERR_OVERFLOW);
    expect("session_still_records", rtos_diag_mark(1U, 0U) == RTOS_OK);
    info = freeze_info();
    expect("session_no_wrap", info.session == UINT32_MAX);
    clean_state();
    rtos_diag_identity_exhausted_locked();
    info = freeze_info();
    expect("identity_frozen", info.trigger == RTOS_DIAG_TRIGGER_IDENTITY_EXHAUSTED);
    expect("identity_reset_blocked", rtos_diag_reset() == RTOS_ERR_OVERFLOW);
    rtos_diag_fault_freeze(77U, (uintptr_t)3U);
    expect("fault_cannot_reenable_identity", rtos_diag_reset() == RTOS_ERR_OVERFLOW);
    (void)printf("HOST|GROUP|counter_exhaustion=PASS\n");
}

int main(void)
{
    test_parameters();
    test_fifo_model();
    test_explanation();
    test_conservative_history();
    test_episode_lifecycle();
    test_irq_contract();
    test_interrupted_publication();
    test_counter_exhaustion();
    expect("exclusion_balanced", host_nesting == 0U);
    (void)printf("HOST|SUMMARY|checks=%lu failures=%lu sanitizer=none\n",
        (unsigned long)checks, (unsigned long)failures);
    (void)printf("HOST|DONE|status=%lu\n", failures == 0U ? 0UL : 1UL);
    return failures == 0U ? 0 : 1;
}

/**
 * @file diagnostic_checks.c
 * @brief Real-scheduler wait episodes and bounded diagnostic snapshot contracts.
 *
 * The controller runs at priority 9 and a worker at 11. This permits an exact
 * observation of READY before the worker receives CPU time. Return snapshots are
 * frozen by the worker immediately after the actual API returns. No test writes
 * scheduler state, constructs fake trace records, or changes production hooks.
 */
#include "audit_regressions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "rtos.h"
#include "rtos_diag.h"

#define DG_STACK_WORDS (384U)
#define DG_PAYLOAD (0x23554477U)
#define DG_SEED (0x10203040U)
#define DG_FINITE_WAIT (15U)
#define DG_POLL_LIMIT (100U)

#if RTOS_CONFIG_USE_DIAGNOSTICS && !defined(DG_RING_ONLY)
#define DG_FULL_TESTS (1)
#else
#define DG_FULL_TESTS (0)
#endif

#if !RTOS_CONFIG_USE_DIAGNOSTICS
#define DG_FIXTURE_MODE "disabled"
#elif defined(DG_RING_ONLY)
#define DG_FIXTURE_MODE "ring"
#else
#define DG_FIXTURE_MODE "functional"
#endif

typedef enum dg_operation {
    DG_OP_SEM = 0,
    DG_OP_QUEUE_SEND,
    DG_OP_QUEUE_RECEIVE,
    DG_OP_MUTEX,
    DG_OP_EVENT_ANY,
    DG_OP_EVENT_ALL,
    DG_OP_NOTIFY,
    DG_OP_DELAY,
    DG_OP_CHANNEL_ACQUIRE,
    DG_OP_CHANNEL_RECEIVE,
    DG_OP_COUNT
} dg_operation_t;

typedef struct dg_result {
    rtos_status_t status;
    uint32_t value;
    rtos_tick_t elapsed;
    uint32_t finished;
    uint32_t cleanup_valid;
} dg_result_t;

#if DG_FULL_TESTS
static rtos_sem_t resource_sem;
static rtos_sem_t done_sem;
static rtos_queue_t queue;
static uint32_t queue_storage;
static rtos_mutex_t mutex;
static rtos_event_t event;
static rtos_channel_t channel;
static uint8_t channel_storage[32] __attribute__((aligned(8)));
static rtos_channel_slot_t channel_slot[1];
static rtos_channel_writer_t held_writer;
static uint32_t holder_active;
static uint32_t channel_expected_closed;
static rtos_tcb_t worker;
static rtos_stack_t worker_stack[DG_STACK_WORDS] __attribute__((aligned(8)));
static uint32_t worker_active;
static dg_operation_t worker_operation;
static rtos_tick_t worker_timeout;
static uint32_t worker_freeze_return;
static dg_result_t worker_result;
#endif
static uint32_t pass_count;
static uint32_t fail_count;

static void dg_expect(const char *name, uint32_t condition)
{
    if (condition != 0U) {
        pass_count++;
    } else {
        fail_count++;
    }
    (void)printf("DG|CHECK|%s|id=%lu name=%s\r\n",
        condition != 0U ? "PASS" : "FAIL",
        (unsigned long)(pass_count + fail_count), name);
}

#if DG_FULL_TESTS
static rtos_diag_operation_t diagnostic_operation(dg_operation_t operation)
{
    static const rtos_diag_operation_t operations[DG_OP_COUNT] = {
        RTOS_DIAG_OP_SEM, RTOS_DIAG_OP_QUEUE_SEND, RTOS_DIAG_OP_QUEUE_RECEIVE,
        RTOS_DIAG_OP_MUTEX, RTOS_DIAG_OP_EVENT, RTOS_DIAG_OP_EVENT,
        RTOS_DIAG_OP_NOTIFY, RTOS_DIAG_OP_DELAY, RTOS_DIAG_OP_CHANNEL_ACQUIRE,
        RTOS_DIAG_OP_CHANNEL_RECEIVE
    };

    return operations[operation];
}

static const void *wait_object(dg_operation_t operation)
{
    switch (operation) {
        case DG_OP_SEM:
            return &resource_sem;
        case DG_OP_QUEUE_SEND:
        case DG_OP_QUEUE_RECEIVE:
            return &queue;
        case DG_OP_MUTEX:
            return &mutex;
        case DG_OP_EVENT_ANY:
        case DG_OP_EVENT_ALL:
            return &event;
        case DG_OP_NOTIFY:
            return &worker;
        case DG_OP_CHANNEL_ACQUIRE:
        case DG_OP_CHANNEL_RECEIVE:
            return &channel;
        case DG_OP_DELAY:
        default:
            return NULL;
    }
}

static uint32_t wait_blocked(void)
{
    for (uint32_t poll = 0U; poll < DG_POLL_LIMIT; poll++) {
        RTOS_PORT_ENTER_CRITICAL();
        uint32_t blocked = (worker.state == RTOS_TASK_BLOCKED
            || worker.state == RTOS_TASK_DELAYED)
            && worker.blocked_on == wait_object(worker_operation);
        RTOS_PORT_EXIT_CRITICAL();
        if (blocked != 0U) {
            return 1U;
        }
        rtos_task_delay(1U);
    }
    return 0U;
}

static rtos_status_t perform_operation(rtos_channel_writer_t *writer,
                                      rtos_channel_reader_t *reader)
{
    switch (worker_operation) {
        case DG_OP_SEM:
            return rtos_sem_take(&resource_sem, worker_timeout);
        case DG_OP_QUEUE_SEND:
            worker_result.value = DG_PAYLOAD;
            return rtos_queue_send(&queue, &worker_result.value, worker_timeout, RTOS_TRUE);
        case DG_OP_QUEUE_RECEIVE:
            return rtos_queue_recv(&queue, &worker_result.value, worker_timeout);
        case DG_OP_MUTEX:
            return rtos_mutex_take(&mutex, worker_timeout);
        case DG_OP_EVENT_ANY:
        case DG_OP_EVENT_ALL:
            worker_result.value = rtos_event_wait(&event, 3U,
                worker_operation == DG_OP_EVENT_ALL ? RTOS_EVENT_WAIT_ALL : RTOS_EVENT_WAIT_ANY,
                worker_timeout);
            return RTOS_OK;
        case DG_OP_NOTIFY:
            return rtos_task_notify_wait(worker_timeout, &worker_result.value);
        case DG_OP_DELAY:
            rtos_task_delay(worker_timeout);
            return RTOS_OK;
        case DG_OP_CHANNEL_ACQUIRE:
            return rtos_channel_acquire(&channel, writer, worker_timeout);
        case DG_OP_CHANNEL_RECEIVE:
            return rtos_channel_receive(&channel, reader, worker_timeout);
        default:
            return RTOS_ERR_PARAM;
    }
}

static void finish_operation(rtos_channel_writer_t *writer, rtos_channel_reader_t *reader)
{
    uint32_t valid = 1U;

    if (worker_result.status == RTOS_OK) {
        switch (worker_operation) {
            case DG_OP_MUTEX:
                valid = rtos_mutex_give(&mutex) == RTOS_OK;
                break;
            case DG_OP_CHANNEL_ACQUIRE:
                valid = rtos_channel_cancel(&channel, writer) == RTOS_OK;
                break;
            case DG_OP_CHANNEL_RECEIVE:
                valid = reader->length == sizeof(uint32_t) && reader->tag == DG_PAYLOAD;
                valid &= rtos_channel_release(&channel, reader) == RTOS_OK;
                break;
            default:
                break;
        }
    }
    worker_result.cleanup_valid = valid;
}

static void worker_entry(void *argument)
{
    (void)argument;
    rtos_channel_writer_t writer = {0};
    rtos_channel_reader_t reader = {0};
    rtos_tick_t start_tick = rtos_sched_get_tick_count();

    worker_result.status = perform_operation(&writer, &reader);
    worker_result.elapsed = rtos_sched_get_tick_count() - start_tick;
    if (worker_freeze_return != 0U) {
        (void)rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER);
    }
    finish_operation(&writer, &reader);
    worker_result.finished = 1U;
    (void)rtos_sem_give(&done_sem);
    (void)rtos_task_suspend(NULL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static uint32_t prepare_objects(dg_operation_t operation)
{
    rtos_channel_config_t config = {channel_storage, sizeof(channel_storage), channel_slot,
        sizeof(channel_slot), sizeof(channel_storage), 1U};
    uint32_t valid = rtos_sem_init(&resource_sem, 0U, 1U) == RTOS_OK;

    valid &= rtos_sem_init(&done_sem, 0U, 1U) == RTOS_OK;
    valid &= rtos_queue_init(&queue, &queue_storage, sizeof(queue_storage), 1U) == RTOS_OK;
    valid &= rtos_mutex_init(&mutex) == RTOS_OK;
    valid &= rtos_event_init(&event) == RTOS_OK;
    valid &= rtos_channel_init(&channel, &config) == RTOS_OK;
    holder_active = 0U;
    channel_expected_closed = 0U;
    if (operation == DG_OP_QUEUE_SEND) {
        uint32_t seed = DG_SEED;
        valid &= rtos_queue_send(&queue, &seed, RTOS_NO_WAIT, RTOS_FALSE) == RTOS_OK;
    } else if (operation == DG_OP_MUTEX) {
        valid &= rtos_mutex_take(&mutex, RTOS_NO_WAIT) == RTOS_OK;
    } else if (operation == DG_OP_EVENT_ALL) {
        valid &= rtos_event_set(&event, 1U) == 1U;
    } else if (operation == DG_OP_CHANNEL_ACQUIRE) {
        holder_active = rtos_channel_acquire(&channel, &held_writer, RTOS_NO_WAIT) == RTOS_OK;
        valid &= holder_active;
    }
    return valid;
}

static uint32_t start_worker(dg_operation_t operation, rtos_tick_t timeout,
                             uint32_t freeze_return)
{
    worker_operation = operation;
    worker_timeout = timeout;
    worker_freeze_return = freeze_return;
    worker_active = 0U;
    memset(&worker_result, 0, sizeof(worker_result));
    dg_expect("history_reset", rtos_diag_reset() == RTOS_OK);
    uint32_t valid = prepare_objects(operation);

    dg_expect("objects_prepared", valid);
    if (valid == 0U) {
        return 0U;
    }
    rtos_status_t status = rtos_task_create_static(&worker, worker_stack, DG_STACK_WORDS,
        worker_entry, NULL, 11U, "diag-worker");
    worker_active = status == RTOS_OK;
    dg_expect("worker_created", worker_active);
    if (worker_active == 0U) {
        return 0U;
    }
    valid = wait_blocked();
    dg_expect("worker_really_blocked", valid);
    return valid;
}

static uint32_t fulfill_operation(void)
{
    uint32_t value = DG_PAYLOAD;

    switch (worker_operation) {
        case DG_OP_SEM:
            return rtos_sem_give(&resource_sem) == RTOS_OK;
        case DG_OP_QUEUE_SEND:
            return rtos_queue_recv(&queue, &value, RTOS_NO_WAIT) == RTOS_OK && value == DG_SEED;
        case DG_OP_QUEUE_RECEIVE:
            return rtos_queue_send(&queue, &value, RTOS_NO_WAIT, RTOS_TRUE) == RTOS_OK;
        case DG_OP_MUTEX:
            return rtos_mutex_give(&mutex) == RTOS_OK;
        case DG_OP_EVENT_ANY:
        case DG_OP_EVENT_ALL:
            value = rtos_event_set(&event, 2U);
            return (value & 2U) != 0U;
        case DG_OP_NOTIFY:
            return rtos_task_notify(&worker, value, RTOS_NOTIFY_VALUE) == RTOS_OK;
        case DG_OP_DELAY:
            return 1U;
        case DG_OP_CHANNEL_ACQUIRE:
            holder_active = 0U;
            return rtos_channel_cancel(&channel, &held_writer) == RTOS_OK;
        case DG_OP_CHANNEL_RECEIVE: {
            rtos_channel_writer_t writer = {0};
            if (rtos_channel_acquire(&channel, &writer, RTOS_NO_WAIT) != RTOS_OK) {
                return 0U;
            }
            memcpy(writer.data, &value, sizeof(value));
            return rtos_channel_publish(&channel, &writer, sizeof(value), DG_PAYLOAD) == RTOS_OK;
        }
        default:
            return 0U;
    }
}

static void cleanup_objects(void)
{
    uint32_t valid = 1U;

    if (worker_active != 0U) {
        valid = rtos_task_delete(&worker) == RTOS_OK;
        worker_active = 0U;
    }
    if (holder_active != 0U) {
        valid &= rtos_channel_cancel(&channel, &held_writer) == RTOS_OK;
        holder_active = 0U;
    }
    if (mutex.owner == rtos_task_get_current()) {
        valid &= rtos_mutex_give(&mutex) == RTOS_OK;
    }
    /* A late receive must retain its queued message. Drain it explicitly;
     * diagnostics must never reclaim a message as a side effect of a timeout. */
    rtos_channel_reader_t reader = {0};
    rtos_status_t received = rtos_channel_receive(&channel, &reader, RTOS_NO_WAIT);
    if (received == RTOS_OK) {
        valid &= rtos_channel_release(&channel, &reader) == RTOS_OK;
    }
    rtos_channel_stats_t stats = {0};
    valid &= rtos_channel_get_stats(&channel, &stats) == RTOS_OK;
    valid &= stats.capacity == 1U && stats.free == 1U && stats.writing == 0U
        && stats.queued == 0U && stats.reading == 0U && stats.waiting_writers == 0U
        && stats.waiting_readers == 0U && stats.closed == channel_expected_closed;
    /* The close scenario intentionally leaves this instance closed. A repeated
     * close reports ALREADY; verify its ledger, then deinit that known state. */
    if (stats.closed == 0U) {
        valid &= rtos_channel_close(&channel) == RTOS_OK;
    }
    valid &= rtos_channel_deinit(&channel) == RTOS_OK;
    valid &= rtos_event_deinit(&event) == RTOS_OK;
    valid &= rtos_mutex_deinit(&mutex) == RTOS_OK;
    valid &= rtos_queue_deinit(&queue) == RTOS_OK;
    valid &= rtos_sem_deinit(&done_sem) == RTOS_OK;
    valid &= rtos_sem_deinit(&resource_sem) == RTOS_OK;
    dg_expect("objects_cleaned", valid);
}

static uint32_t snapshot(rtos_diag_info_t *info, rtos_diag_explanation_t *explanation)
{
    dg_expect("snapshot_info", rtos_diag_get_info(info) == RTOS_OK);
    rtos_status_t status = rtos_diag_explain(info->session, worker.task_id, explanation);

    dg_expect("snapshot_explained", status == RTOS_OK);
    dg_expect("snapshot_identity", status == RTOS_OK
        && explanation->task_id == worker.task_id && explanation->session == info->session
        && explanation->operation == diagnostic_operation(worker_operation)
        && explanation->object == (uintptr_t)wait_object(worker_operation));
    dg_expect("snapshot_complete", explanation->episode != 0U
        && (explanation->flags & RTOS_DIAG_FLAG_INCOMPLETE) == 0U);
    return status == RTOS_OK;
}

static void inspect_episode(const rtos_diag_info_t *info, const rtos_diag_explanation_t *explain)
{
    uint32_t wait_sequence = 0U;
    uint32_t wake_sequence = 0U;
    uint32_t run_sequence = 0U;
    uint32_t return_sequence = 0U;
    uint32_t valid = 1U;

    for (uint32_t offset = 0U; offset < info->count; offset++) {
        rtos_diag_record_t record = {0};
        uint32_t sequence = info->first_sequence + offset;
        valid &= rtos_diag_read(info->session, sequence, &record) == RTOS_OK;
        valid &= record.sequence == sequence;
        if (record.task_id != worker.task_id) {
            continue;
        }
        switch (record.event) {
            case RTOS_DIAG_EVENT_WAIT:
                valid &= record.operation == (uint8_t)diagnostic_operation(worker_operation)
                    && record.value == worker_timeout;
                wait_sequence = sequence;
                wake_sequence = 0U;
                run_sequence = 0U;
                return_sequence = 0U;
                break;
            case RTOS_DIAG_EVENT_WAKE:
                wake_sequence = sequence;
                break;
            case RTOS_DIAG_EVENT_RUN:
                if (wake_sequence != 0U) {
                    run_sequence = sequence;
                }
                break;
            case RTOS_DIAG_EVENT_RETURN:
                return_sequence = sequence;
                valid &= record.result == explain->return_result;
                if (worker_operation == DG_OP_EVENT_ANY || worker_operation == DG_OP_EVENT_ALL) {
                    valid &= record.value == worker_result.value;
                }
                break;
            default:
                break;
        }
    }
    dg_expect("raw_sequences_contiguous", valid);
    dg_expect("raw_wait_episode_matches", wait_sequence == explain->episode);
    dg_expect("raw_wait_wake_run_return_order", wait_sequence != 0U
        && wait_sequence < wake_sequence && wake_sequence < run_sequence
        && run_sequence < return_sequence);
}

static void test_phase(rtos_diag_phase_t phase)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};

    if (start_worker(DG_OP_SEM, RTOS_WAIT_FOREVER, 0U) == 0U) {
        return;
    }
    if (phase == RTOS_DIAG_PHASE_READY) {
        dg_expect("resource_wakes_worker", fulfill_operation());
    }
    dg_expect("freeze_first_trigger", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    (void)snapshot(&info, &explain);
    dg_expect("phase_matches_scheduler", explain.phase == phase
        && explain.cause == (phase == RTOS_DIAG_PHASE_READY
            ? RTOS_DIAG_CAUSE_RESOURCE : RTOS_DIAG_CAUSE_NONE)
        && worker_result.finished == 0U
        && worker.state == (phase == RTOS_DIAG_PHASE_READY
            ? RTOS_TASK_READY : RTOS_TASK_BLOCKED));
    dg_expect("phase_forever_budget", explain.timeout == RTOS_WAIT_FOREVER);
    (void)printf("DG|CASE|name=phase operation=%u phase=%u cause=%u episode=%lu\r\n",
        (unsigned int)worker_operation, (unsigned int)phase, (unsigned int)explain.cause,
        (unsigned long)explain.episode);
    cleanup_objects();
}

static uint32_t returned_value_valid(dg_operation_t operation)
{
    switch (operation) {
        case DG_OP_QUEUE_SEND:
        case DG_OP_QUEUE_RECEIVE:
        case DG_OP_NOTIFY:
            return worker_result.value == DG_PAYLOAD;
        case DG_OP_EVENT_ANY:
            return worker_result.value == 2U;
        case DG_OP_EVENT_ALL:
            return worker_result.value == 3U;
        default:
            return 1U;
    }
}

static void test_return(dg_operation_t operation)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};
    rtos_tick_t timeout = operation == DG_OP_DELAY ? DG_FINITE_WAIT : RTOS_WAIT_FOREVER;

    if (start_worker(operation, timeout, 1U) == 0U) {
        return;
    }
    dg_expect("resource_wakes_worker", fulfill_operation());
    dg_expect("worker_return_observed", rtos_sem_take(&done_sem, DG_POLL_LIMIT) == RTOS_OK
        && worker_result.finished == 1U);
    (void)snapshot(&info, &explain);
    dg_expect("api_return_success", worker_result.status == RTOS_OK
        && worker_result.cleanup_valid == 1U && returned_value_valid(operation));
    rtos_diag_cause_t cause = operation == DG_OP_DELAY ? RTOS_DIAG_CAUSE_DELAY_ELAPSED
        : (operation >= DG_OP_CHANNEL_ACQUIRE ? RTOS_DIAG_CAUSE_RETRY_NOTIFICATION
            : RTOS_DIAG_CAUSE_RESOURCE);
    dg_expect("return_phase_matches", explain.phase == RTOS_DIAG_PHASE_RETURNED
        && explain.wake_result == RTOS_OK && explain.return_result == RTOS_OK
        && explain.cause == cause);
    dg_expect("return_timeout_preserved", explain.timeout == timeout);
    if (operation == DG_OP_EVENT_ANY || operation == DG_OP_EVENT_ALL) {
        dg_expect("event_wait_mask", (explain.detail & 3U) == 3U
            && explain.peer_id == (operation == DG_OP_EVENT_ALL
                ? RTOS_EVENT_WAIT_ALL : RTOS_EVENT_WAIT_ANY));
    }
    if (operation == DG_OP_MUTEX) {
        dg_expect("mutex_owner_at_wait", explain.peer_id == rtos_task_get_current()->task_id);
    }
    inspect_episode(&info, &explain);
    (void)printf("DG|CASE|name=returned operation=%u phase=%u cause=%u wake=%ld result=%ld "
        "episode=%lu\r\n", (unsigned int)operation, (unsigned int)explain.phase,
        (unsigned int)explain.cause, (long)explain.wake_result, (long)explain.return_result,
        (unsigned long)explain.episode);
    cleanup_objects();
}

static void test_timeout(dg_operation_t operation)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};

    if (start_worker(operation, DG_FINITE_WAIT * 3U, 1U) == 0U) {
        return;
    }
    dg_expect("timeout_worker_returned", rtos_sem_take(&done_sem, DG_POLL_LIMIT) == RTOS_OK
        && worker_result.finished == 1U && worker_result.elapsed >= DG_FINITE_WAIT * 3U);
    (void)snapshot(&info, &explain);
    dg_expect("timeout_cause_preserved", explain.phase == RTOS_DIAG_PHASE_RETURNED
        && explain.wake_result == RTOS_ERR_TIMEOUT
        && explain.return_result == RTOS_ERR_TIMEOUT && explain.cause == RTOS_DIAG_CAUSE_TIMEOUT);
    dg_expect("timeout_api_result", worker_result.cleanup_valid == 1U
        && (operation == DG_OP_EVENT_ALL
            ? worker_result.value == 1U : worker_result.status == RTOS_ERR_TIMEOUT));
    inspect_episode(&info, &explain);
    (void)printf("DG|CASE|name=timeout operation=%u phase=%u wake=%ld result=%ld value=%lu\r\n",
        (unsigned int)operation, (unsigned int)explain.phase, (long)explain.wake_result,
        (long)explain.return_result, (unsigned long)worker_result.value);
    cleanup_objects();
}

static uint32_t count_task_events(const rtos_diag_info_t *info, rtos_diag_event_t event_type)
{
    uint32_t count = 0U;

    for (uint32_t offset = 0U; offset < info->count; offset++) {
        rtos_diag_record_t record = {0};
        if (rtos_diag_read(info->session, info->first_sequence + offset, &record) == RTOS_OK
            && record.task_id == worker.task_id && record.event == (uint8_t)event_type) {
            count++;
        }
    }
    return count;
}

static void test_cancellation(uint32_t suspended, uint32_t deleted)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};

    if (start_worker(DG_OP_SEM, RTOS_WAIT_FOREVER, 1U) == 0U) {
        return;
    }
    if (deleted != 0U) {
        dg_expect("delete_blocked_task", rtos_task_delete(&worker) == RTOS_OK);
        worker_active = 0U;
    } else if (suspended != 0U) {
        dg_expect("suspend_blocked_task", rtos_task_suspend(&worker) == RTOS_OK);
    } else {
        dg_expect("deinit_waited_object", rtos_sem_deinit(&resource_sem) == RTOS_OK);
    }
    if (suspended != 0U || deleted != 0U) {
        dg_expect("freeze_cancelled_task", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    } else {
        dg_expect("deinitialized_wait_returns", rtos_sem_take(&done_sem, DG_POLL_LIMIT) == RTOS_OK
            && worker_result.status == RTOS_ERR_DELETED && worker_result.finished == 1U);
    }
    (void)snapshot(&info, &explain);
    rtos_diag_phase_t expected_phase = deleted != 0U ? RTOS_DIAG_PHASE_DELETED
        : (suspended != 0U ? RTOS_DIAG_PHASE_SUSPENDED : RTOS_DIAG_PHASE_RETURNED);
    rtos_diag_cause_t expected_cause = deleted != 0U ? RTOS_DIAG_CAUSE_TASK_DELETED
        : (suspended != 0U ? RTOS_DIAG_CAUSE_TASK_SUSPENDED : RTOS_DIAG_CAUSE_OBJECT_DELETED);
    dg_expect("cancellation_phase_is_explicit", explain.phase == expected_phase
        && explain.cause == expected_cause);
    dg_expect("cancelled_wait_detached", resource_sem.wait_head == NULL
        && resource_sem.wait_bitmap == 0U);
    dg_expect("suspend_is_not_object_deinit", resource_sem.is_initialized == (suspended | deleted)
        && count_task_events(&info, RTOS_DIAG_EVENT_SUSPEND) == suspended
        && count_task_events(&info, RTOS_DIAG_EVENT_DELETE) == deleted);
    dg_expect("cancelled_task_return_evidence", (suspended | deleted) != 0U
        ? count_task_events(&info, RTOS_DIAG_EVENT_RETURN) == 0U
        : (explain.wake_result == RTOS_ERR_DELETED && explain.return_result == RTOS_ERR_DELETED));
    (void)printf("DG|CASE|name=cancellation suspended=%lu deleted=%lu phase=%u\r\n",
        (unsigned long)suspended, (unsigned long)deleted, (unsigned int)explain.phase);
    cleanup_objects();
}

static uint32_t delay_cancel_order(const rtos_diag_info_t *info)
{
    static const uint8_t expected[] = {RTOS_DIAG_EVENT_WAIT, RTOS_DIAG_EVENT_SUSPEND,
        RTOS_DIAG_EVENT_RESUME, RTOS_DIAG_EVENT_RUN, RTOS_DIAG_EVENT_RETURN};
    uint32_t matched = 0U;

    for (uint32_t offset = 0U; offset < info->count; offset++) {
        rtos_diag_record_t record = {0};
        if (rtos_diag_read(info->session, info->first_sequence + offset, &record) != RTOS_OK) {
            return 0U;
        }
        if (record.task_id == worker.task_id && matched < sizeof(expected)
            && record.event == expected[matched]) {
            matched++;
        }
    }
    return matched == sizeof(expected);
}

static void test_delay_suspend(rtos_tick_t timeout)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};

    if (start_worker(DG_OP_DELAY, timeout, 1U) == 0U) {
        return;
    }
    dg_expect("pure_delay_suspended", rtos_task_suspend(&worker) == RTOS_OK);
    dg_expect("pure_delay_resumed", rtos_task_resume(&worker) == RTOS_OK);
    dg_expect("interrupted_delay_returned", rtos_sem_take(&done_sem, DG_POLL_LIMIT) == RTOS_OK
        && worker_result.finished == 1U && worker_result.cleanup_valid == 1U);
    (void)snapshot(&info, &explain);
    dg_expect("pure_delay_suspend_cause", explain.phase == RTOS_DIAG_PHASE_RETURNED
        && explain.cause == RTOS_DIAG_CAUSE_TASK_SUSPENDED
        && explain.wake_result == RTOS_ERR_DELETED);
    dg_expect("void_delay_return_is_separate", worker_result.status == RTOS_OK
        && explain.return_result == RTOS_OK);
    dg_expect("interrupted_delay_not_elapsed", explain.timeout == timeout
        && (timeout == RTOS_WAIT_FOREVER || worker_result.elapsed < timeout));
    dg_expect("delay_cancel_events_exact", count_task_events(&info, RTOS_DIAG_EVENT_SUSPEND) == 1U
        && count_task_events(&info, RTOS_DIAG_EVENT_RESUME) == 1U
        && count_task_events(&info, RTOS_DIAG_EVENT_WAKE) == 0U);
    dg_expect("delay_cancel_trace_order", delay_cancel_order(&info));
    (void)printf("DG|CASE|name=delay_suspend timeout=%lu phase=%u cause=%u result=%ld\r\n",
        (unsigned long)timeout, (unsigned int)explain.phase, (unsigned int)explain.cause,
        (long)explain.return_result);
    cleanup_objects();
}

static void test_ready_suspend(void)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};

    if (start_worker(DG_OP_SEM, RTOS_WAIT_FOREVER, 0U) == 0U) {
        return;
    }
    dg_expect("ready_suspend_resource_granted", fulfill_operation());
    dg_expect("already_ready_task_suspended", rtos_task_suspend(&worker) == RTOS_OK);
    dg_expect("ready_suspend_frozen", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    (void)snapshot(&info, &explain);
    dg_expect("ready_suspend_preserves_wake_cause", explain.phase == RTOS_DIAG_PHASE_SUSPENDED
        && explain.cause == RTOS_DIAG_CAUSE_RESOURCE && explain.wake_result == RTOS_OK);
    uint32_t not_cancelled = 0U;
    for (uint32_t offset = 0U; offset < info.count; offset++) {
        rtos_diag_record_t record = {0};
        if (rtos_diag_read(info.session, info.first_sequence + offset, &record) == RTOS_OK
            && record.task_id == worker.task_id && record.event == RTOS_DIAG_EVENT_SUSPEND) {
            not_cancelled += record.value == 0U ? 1U : 0U;
        }
    }
    dg_expect("ready_suspend_did_not_cancel_wait", not_cancelled == 1U
        && worker.state == RTOS_TASK_SUSPENDED && worker_result.finished == 0U);
    (void)printf("DG|CASE|name=ready_suspend phase=%u cause=%u wake=%ld\r\n",
        (unsigned int)explain.phase, (unsigned int)explain.cause, (long)explain.wake_result);
    cleanup_objects();
}

static void test_channel_close(uint32_t recreate)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};

    if (start_worker(DG_OP_CHANNEL_RECEIVE, RTOS_WAIT_FOREVER, 1U) == 0U) {
        return;
    }
    dg_expect("channel_close_wakes_waiter", rtos_channel_close(&channel) == RTOS_OK);
    channel_expected_closed = 1U;
    if (recreate != 0U) {
        rtos_channel_config_t config = {channel_storage, sizeof(channel_storage), channel_slot,
            sizeof(channel_slot), sizeof(channel_storage), 1U};
        dg_expect("channel_old_instance_removed", rtos_channel_deinit(&channel) == RTOS_OK);
        dg_expect("channel_same_address_recreated", rtos_channel_init(&channel, &config)
            == RTOS_OK);
        channel_expected_closed = 0U;
    }
    rtos_status_t expected_result = recreate != 0U ? RTOS_ERR_DELETED : RTOS_ERR_CLOSED;
    dg_expect("closed_wait_returned", rtos_sem_take(&done_sem, DG_POLL_LIMIT) == RTOS_OK
        && worker_result.finished == 1U && worker_result.status == expected_result);
    (void)snapshot(&info, &explain);
    dg_expect("close_wake_and_return_distinguished", explain.phase == RTOS_DIAG_PHASE_RETURNED
        && explain.wake_result == RTOS_ERR_CLOSED && explain.return_result == expected_result
        && explain.cause == RTOS_DIAG_CAUSE_CHANNEL_CLOSED);
    inspect_episode(&info, &explain);
    (void)printf("DG|CASE|name=channel_close recreate=%lu wake=%ld result=%ld episode=%lu\r\n",
        (unsigned long)recreate, (long)explain.wake_result, (long)explain.return_result,
        (unsigned long)explain.episode);
    cleanup_objects();
}

static void test_channel_late(dg_operation_t operation)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};

    if (start_worker(operation, DG_FINITE_WAIT * 3U, 1U) == 0U) {
        return;
    }
    dg_expect("channel_resource_notification", fulfill_operation());
    rtos_tick_t ready_tick = rtos_sched_get_tick_count();
    while (rtos_sched_get_tick_count() - ready_tick < DG_FINITE_WAIT * 3U) {
        __WFI();
    }
    dg_expect("late_worker_still_ready", worker.state == RTOS_TASK_READY
        && worker_result.finished == 0U);
    dg_expect("late_wait_returned", rtos_sem_take(&done_sem, DG_POLL_LIMIT) == RTOS_OK
        && worker_result.finished == 1U && worker_result.status == RTOS_ERR_TIMEOUT);
    (void)snapshot(&info, &explain);
    dg_expect("notification_is_not_granted_ownership", explain.phase == RTOS_DIAG_PHASE_RETURNED
        && explain.wake_result == RTOS_OK && explain.return_result == RTOS_ERR_TIMEOUT
        && explain.cause == RTOS_DIAG_CAUSE_RETRY_NOTIFICATION);
    inspect_episode(&info, &explain);
    (void)printf("DG|CASE|name=channel_late operation=%u wake=%ld result=%ld elapsed=%lu\r\n",
        (unsigned int)operation, (long)explain.wake_result, (long)explain.return_result,
        (unsigned long)worker_result.elapsed);
    cleanup_objects();
}

#endif /* DG_FULL_TESTS */

#if RTOS_CONFIG_USE_DIAGNOSTICS
static void test_api_contract(void)
{
    rtos_diag_info_t info;
    rtos_diag_info_t before_info;
    rtos_diag_record_t record;
    rtos_diag_record_t before_record;
    rtos_diag_explanation_t explain;
    rtos_diag_explanation_t before_explain;

    memset(&info, 0xA5, sizeof(info));
    memcpy(&before_info, &info, sizeof(info));
    memset(&record, 0xA5, sizeof(record));
    memcpy(&before_record, &record, sizeof(record));
    memset(&explain, 0xA5, sizeof(explain));
    memcpy(&before_explain, &explain, sizeof(explain));
    dg_expect("api_reset", rtos_diag_reset() == RTOS_OK);
    dg_expect("live_info_is_busy", rtos_diag_get_info(&info) == RTOS_ERR_BUSY
        && memcmp(&info, &before_info, sizeof(info)) == 0);
    dg_expect("live_read_old_session_rejected", rtos_diag_read(1U, 1U, &record) == RTOS_ERR_STALE
        && memcmp(&record, &before_record, sizeof(record)) == 0);
    dg_expect("live_explain_is_busy", rtos_diag_explain(1U,
        rtos_task_get_current()->task_id, &explain)
        == RTOS_ERR_BUSY && memcmp(&explain, &before_explain, sizeof(explain)) == 0);
    dg_expect("marker_accepted", rtos_diag_mark(11U, 22U) == RTOS_OK);
    dg_expect("invalid_trigger_rejected", rtos_diag_freeze(0U) == RTOS_ERR_PARAM);
    dg_expect("user_freeze", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    dg_expect("freeze_is_idempotent", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    dg_expect("frozen_marker_refused", rtos_diag_mark(33U, 44U) == RTOS_ERR_BUSY);
    dg_expect("frozen_info_available", rtos_diag_get_info(&info) == RTOS_OK
        && info.capacity == RTOS_CONFIG_DIAGNOSTIC_CAPACITY
        && info.trigger == RTOS_DIAG_TRIGGER_USER && info.count == 1U);
    dg_expect("marker_read_exact", rtos_diag_read(info.session, info.first_sequence, &record)
        == RTOS_OK && record.event == RTOS_DIAG_EVENT_MARK
        && record.value == 11U && record.detail == 22U
        && record.task_id == rtos_task_get_current()->task_id);
    dg_expect("null_info_rejected", rtos_diag_get_info(NULL) == RTOS_ERR_PARAM);
    dg_expect("null_record_rejected", rtos_diag_read(info.session, info.first_sequence, NULL)
        == RTOS_ERR_PARAM);
    dg_expect("zero_sequence_rejected", rtos_diag_read(info.session, 0U, &record)
        == RTOS_ERR_PARAM);
    dg_expect("unknown_sequence_rejected", rtos_diag_read(info.session, info.last_sequence + 1U,
        &record) == RTOS_ERR_STALE);
    dg_expect("null_explanation_rejected", rtos_diag_explain(info.session, 1U, NULL)
        == RTOS_ERR_PARAM);
    dg_expect("zero_task_id_rejected", rtos_diag_explain(info.session, 0U, &explain)
        == RTOS_ERR_PARAM);
    dg_expect("unknown_task_is_incomplete", rtos_diag_explain(info.session, UINT32_MAX, &explain)
        == RTOS_OK && explain.phase == RTOS_DIAG_PHASE_UNKNOWN && explain.episode == 0U
        && (explain.flags & RTOS_DIAG_FLAG_INCOMPLETE) != 0U);
    uint32_t old_session = info.session;
    uint32_t old_sequence = info.first_sequence;
    dg_expect("new_session_started", rtos_diag_reset() == RTOS_OK);
    dg_expect("new_session_frozen", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    dg_expect("old_read_session_rejected", rtos_diag_read(old_session, old_sequence, &record)
        == RTOS_ERR_STALE);
    dg_expect("old_explain_session_rejected", rtos_diag_explain(old_session, 1U, &explain)
        == RTOS_ERR_STALE);
}

static void test_ring_overwrite(void)
{
    rtos_diag_info_t baseline = {0};
    rtos_diag_info_t info = {0};
    rtos_diag_record_t record = {0};
    uint32_t valid = 1U;

    dg_expect("ring_baseline_reset", rtos_diag_reset() == RTOS_OK);
    dg_expect("ring_baseline_marker", rtos_diag_mark(0U, 0U) == RTOS_OK);
    dg_expect("ring_baseline_frozen", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    dg_expect("ring_baseline_info", rtos_diag_get_info(&baseline) == RTOS_OK);
    dg_expect("ring_collection_reset", rtos_diag_reset() == RTOS_OK);
    for (uint32_t index = 0U; index < RTOS_CONFIG_DIAGNOSTIC_CAPACITY + 5U; index++) {
        valid &= rtos_diag_mark(index, index ^ DG_PAYLOAD) == RTOS_OK;
    }
    dg_expect("ring_markers_accepted", valid);
    dg_expect("ring_collection_frozen", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    dg_expect("ring_info_exact", rtos_diag_get_info(&info) == RTOS_OK
        && info.count == RTOS_CONFIG_DIAGNOSTIC_CAPACITY && info.overwritten == 5U
        && info.last_sequence - info.first_sequence + 1U == info.count);
    dg_expect("overwritten_sequence_rejected", rtos_diag_read(info.session,
        baseline.last_sequence + 1U, &record) == RTOS_ERR_STALE);
    for (uint32_t offset = 0U; offset < info.count; offset++) {
        valid &= rtos_diag_read(info.session, info.first_sequence + offset, &record) == RTOS_OK;
        valid &= record.event == RTOS_DIAG_EVENT_MARK && record.value == offset + 5U
            && record.detail == ((offset + 5U) ^ DG_PAYLOAD);
    }
    dg_expect("retained_ring_payloads_exact", valid);
    (void)printf("DG|CASE|name=ring count=%lu overwritten=%lu first=%lu last=%lu\r\n",
        (unsigned long)info.count, (unsigned long)info.overwritten,
        (unsigned long)info.first_sequence, (unsigned long)info.last_sequence);
}

#if DG_FULL_TESTS
static void test_missing_wait(uint32_t reset)
{
    rtos_diag_info_t info = {0};
    rtos_diag_explanation_t explain = {0};
    uint32_t valid = 1U;

    if (start_worker(DG_OP_SEM, RTOS_WAIT_FOREVER, 1U) == 0U) {
        return;
    }
    if (reset != 0U) {
        valid = rtos_diag_reset() == RTOS_OK;
    } else {
        for (uint32_t index = 0U; index < RTOS_CONFIG_DIAGNOSTIC_CAPACITY + 5U; index++) {
            valid &= rtos_diag_mark(index, 0U) == RTOS_OK;
        }
    }
    dg_expect("history_deliberately_lost", valid);
    dg_expect("lost_history_resource_wakes", fulfill_operation());
    dg_expect("lost_history_worker_returns", rtos_sem_take(&done_sem, DG_POLL_LIMIT) == RTOS_OK
        && worker_result.finished == 1U && worker_result.status == RTOS_OK);
    dg_expect("lost_history_info", rtos_diag_get_info(&info) == RTOS_OK);
    dg_expect("missing_wait_never_invented", rtos_diag_explain(info.session, worker.task_id,
        &explain) == RTOS_OK && explain.episode == 0U
        && explain.phase == RTOS_DIAG_PHASE_UNKNOWN
        && (explain.flags & RTOS_DIAG_FLAG_INCOMPLETE) != 0U);
    (void)printf("DG|CASE|name=missing_wait reset=%lu phase=%u incomplete=%lu\r\n",
        (unsigned long)reset, (unsigned int)explain.phase,
        (unsigned long)(explain.flags & RTOS_DIAG_FLAG_INCOMPLETE));
    cleanup_objects();
}
#endif

static volatile uint32_t irq_done;
static volatile rtos_status_t irq_status[6];
static volatile uint32_t irq_masks_preserved;

/** @brief Exercise only diagnostic public APIs from a real software-pended IRQ. */
void TIM7_IRQHandler(void)
{
    rtos_diag_info_t info;
    rtos_diag_record_t record;
    rtos_diag_explanation_t explain;
    uint32_t primask = __get_PRIMASK();
    uint32_t basepri = __get_BASEPRI();
    uint32_t nesting = rtos_port_crit_nest;

    irq_status[0] = rtos_diag_mark((uint32_t)NVIC_GetPriority(TIM7_IRQn), DG_PAYLOAD);
    irq_status[1] = rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER);
    irq_status[2] = rtos_diag_reset();
    irq_status[3] = rtos_diag_get_info(&info);
    irq_status[4] = rtos_diag_read(1U, 1U, &record);
    irq_status[5] = rtos_diag_explain(1U, 1U, &explain);
    irq_masks_preserved = primask == __get_PRIMASK() && basepri == __get_BASEPRI()
        && nesting == rtos_port_crit_nest;
    irq_done = 1U;
}

static void run_irq(uint32_t priority)
{
    rtos_diag_info_t info = {0};
    uint32_t markers = 0U;
    uint32_t valid = 1U;

    dg_expect("irq_history_reset", rtos_diag_reset() == RTOS_OK);
    irq_done = 0U;
    irq_masks_preserved = 0U;
    NVIC_SetPriority(TIM7_IRQn, priority);
    NVIC_EnableIRQ(TIM7_IRQn);
    NVIC_SetPendingIRQ(TIM7_IRQn);
    __DSB();
    __ISB();
    for (uint32_t poll = 0U; poll < 1000000U && irq_done == 0U; poll++) {
        __NOP();
    }
    NVIC_DisableIRQ(TIM7_IRQn);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    dg_expect("irq_executed", irq_done == 1U);
    for (uint32_t index = 0U; index < 6U; index++) {
        rtos_status_t expected = priority >= 5U && index < 2U ? RTOS_OK : RTOS_ERR_ISR;
        valid &= irq_status[index] == expected;
    }
    dg_expect("irq_context_contracts", valid && irq_masks_preserved == 1U);
    dg_expect("irq_snapshot_frozen", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_OK);
    dg_expect("irq_snapshot_info", rtos_diag_get_info(&info) == RTOS_OK);
    for (uint32_t offset = 0U; offset < info.count; offset++) {
        rtos_diag_record_t record = {0};
        if (rtos_diag_read(info.session, info.first_sequence + offset, &record) == RTOS_OK
            && record.event == RTOS_DIAG_EVENT_MARK && record.detail == DG_PAYLOAD) {
            markers++;
            valid &= record.task_id == 0U && record.value == priority
                && (record.flags & RTOS_DIAG_FLAG_ISR) != 0U;
        }
    }
    dg_expect("irq_marker_identity", valid && markers == (priority >= 5U ? 1U : 0U));
    (void)printf("DG|IRQ|priority=%lu completed=%lu markers=%lu preserved=%lu\r\n",
        (unsigned long)priority, (unsigned long)irq_done, (unsigned long)markers,
        (unsigned long)irq_masks_preserved);
}

static void test_irq_contract(void)
{
    uint32_t priority = NVIC_GetPriority(TIM7_IRQn);
    uint32_t enabled = NVIC_GetEnableIRQ(TIM7_IRQn);
    uint32_t pending = NVIC_GetPendingIRQ(TIM7_IRQn);

    dg_expect("irq_line_unused", enabled == 0U && pending == 0U);
    run_irq(0U);
    run_irq(4U);
    run_irq(5U);
    NVIC_SetPriority(TIM7_IRQn, priority);
    if (enabled != 0U) {
        NVIC_EnableIRQ(TIM7_IRQn);
    }
    if (pending != 0U) {
        NVIC_SetPendingIRQ(TIM7_IRQn);
    }
    dg_expect("irq_line_restored", NVIC_GetPriority(TIM7_IRQn) == priority
        && NVIC_GetEnableIRQ(TIM7_IRQn) == enabled
        && NVIC_GetPendingIRQ(TIM7_IRQn) == pending);
}

#else

static void test_disabled(void)
{
    rtos_diag_info_t info;
    rtos_diag_info_t before_info;
    rtos_diag_record_t record;
    rtos_diag_record_t before_record;
    rtos_diag_explanation_t explain;
    rtos_diag_explanation_t before_explain;

    memset(&info, 0xA5, sizeof(info));
    memcpy(&before_info, &info, sizeof(info));
    memset(&record, 0xA5, sizeof(record));
    memcpy(&before_record, &record, sizeof(record));
    memset(&explain, 0xA5, sizeof(explain));
    memcpy(&before_explain, &explain, sizeof(explain));
    dg_expect("disabled_feature_selected", RTOS_CONFIG_USE_DIAGNOSTICS == 0);
    dg_expect("disabled_reset", rtos_diag_reset() == RTOS_ERR_DISABLED);
    dg_expect("disabled_freeze", rtos_diag_freeze(RTOS_DIAG_TRIGGER_USER) == RTOS_ERR_DISABLED);
    dg_expect("disabled_marker", rtos_diag_mark(1U, 2U) == RTOS_ERR_DISABLED);
    dg_expect("disabled_info", rtos_diag_get_info(&info) == RTOS_ERR_DISABLED);
    dg_expect("disabled_read", rtos_diag_read(1U, 1U, &record) == RTOS_ERR_DISABLED);
    dg_expect("disabled_explain", rtos_diag_explain(1U, 1U, &explain) == RTOS_ERR_DISABLED);
    dg_expect("disabled_info_unchanged", memcmp(&info, &before_info, sizeof(info)) == 0);
    dg_expect("disabled_record_unchanged", memcmp(&record, &before_record, sizeof(record)) == 0);
    dg_expect("disabled_explanation_unchanged", memcmp(&explain, &before_explain,
        sizeof(explain)) == 0);
    rtos_diag_fault_freeze(1U, (uintptr_t)DG_PAYLOAD);
    dg_expect("disabled_fault_is_noop", rtos_diag_mark(1U, 2U) == RTOS_ERR_DISABLED);
}

#endif /* RTOS_CONFIG_USE_DIAGNOSTICS */

/** @brief Emit the immutable fixture configuration before the scheduler starts. */
void audit_pre_start(void)
{
    (void)printf("DG|META|mode=%s diagnostics=%u capacity=%u assertions=%u mask=%u\r\n",
        DG_FIXTURE_MODE, (unsigned int)RTOS_CONFIG_USE_DIAGNOSTICS,
        (unsigned int)RTOS_CONFIG_DIAGNOSTIC_CAPACITY,
        (unsigned int)RTOS_CONFIG_ASSERT_ENABLED, (unsigned int)RTOS_CONFIG_USE_BASEPRI);
}

/** @brief Run deterministic real-kernel scenarios, then retain the board for inspection.
 *  @param argument [in] Unused audit task argument.
 */
void audit_regressions_task(void *argument)
{
    (void)argument;
#if DG_FULL_TESTS
    test_phase(RTOS_DIAG_PHASE_WAITING);
    test_phase(RTOS_DIAG_PHASE_READY);
    for (uint32_t operation = 0U; operation < (uint32_t)DG_OP_COUNT; operation++) {
        test_return((dg_operation_t)operation);
    }
    test_timeout(DG_OP_SEM);
    test_timeout(DG_OP_EVENT_ALL);
    test_timeout(DG_OP_NOTIFY);
    test_cancellation(0U, 0U);
    test_cancellation(1U, 0U);
    test_cancellation(0U, 1U);
    test_delay_suspend(DG_FINITE_WAIT * 10U);
    test_delay_suspend(RTOS_WAIT_FOREVER);
    test_ready_suspend();
    test_channel_close(0U);
    test_channel_close(1U);
    test_channel_late(DG_OP_CHANNEL_ACQUIRE);
    test_channel_late(DG_OP_CHANNEL_RECEIVE);
#endif
#if RTOS_CONFIG_USE_DIAGNOSTICS
    test_api_contract();
    test_ring_overwrite();
#if DG_FULL_TESTS
    test_missing_wait(0U);
    test_missing_wait(1U);
#endif
    test_irq_contract();
#else
    test_disabled();
#endif
    (void)printf("DG|SUMMARY|mode=%s pass=%lu fail=%lu\r\n", DG_FIXTURE_MODE,
        (unsigned long)pass_count, (unsigned long)fail_count);
    (void)printf("DG|DONE|status=%lu\r\n", fail_count == 0U ? 0UL : 1UL);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

/**
 * @file channel_checks.c
 * @brief Public channel state, ownership, cancellation and real-IRQ contracts.
 *
 * The only white-box injection is the isolated ticket exhaustion boundary.
 * Waiting workers use the real scheduler, SysTick and PendSV. A ready worker
 * receives no reserved buffer; deleting or suspending it must not lose a wake.
 */
#include "audit_regressions.h"
#include "channel_fixture.h"

#define CH_BLOCK_SIZE (64U)
#define CH_BLOCK_COUNT (4U)
#define CH_WORKERS (4U)
#define CH_STACK_WORDS (256U)

typedef struct ch_worker_result {
    rtos_status_t status;
    rtos_tick_t elapsed;
    uint32_t valid;
    uint32_t finished;
} ch_worker_result_t;

static rtos_channel_t channel;
static rtos_channel_t other_channel;
static uint8_t storage[CH_BLOCK_COUNT * CH_BLOCK_SIZE] __attribute__((aligned(8)));
static uint8_t other_storage[CH_BLOCK_SIZE] __attribute__((aligned(8)));
static rtos_channel_slot_t slots[CH_BLOCK_COUNT];
static rtos_channel_slot_t other_slots[1];
static rtos_tcb_t workers[CH_WORKERS];
static rtos_stack_t stacks[CH_WORKERS][CH_STACK_WORDS] __attribute__((aligned(8)));
static ch_worker_result_t results[CH_WORKERS];
static rtos_sem_t done;
static uint32_t worker_mask;
static uint32_t worker_acquire;
static rtos_tick_t worker_timeout;
static volatile uint32_t irq_done;
static volatile uint32_t irq_legal;
static volatile rtos_status_t irq_status[10];
static rtos_channel_writer_t irq_writer;
static rtos_channel_reader_t irq_reader;
static uint32_t irq_priority;
static uint32_t irq_enabled;
static uint32_t irq_pending;

static rtos_channel_config_t main_config(uint32_t count)
{
    rtos_channel_config_t config = {storage, sizeof(storage), slots, sizeof(slots),
        CH_BLOCK_SIZE, count};

    return config;
}

static uint32_t initialize_main(uint32_t count)
{
    rtos_channel_config_t config = main_config(count);
    rtos_status_t status = rtos_channel_init(&channel, &config);

    ch_expect("channel_init", status == RTOS_OK);
    return status == RTOS_OK;
}

static void inspect_free(const char *name, uint32_t expected)
{
    rtos_channel_stats_t stats = {0};
    rtos_status_t status = rtos_channel_get_stats(&channel, &stats);

    ch_expect(name, status == RTOS_OK && stats.free == expected && ch_conserved(&stats)
        && stats.writing == 0U && stats.queued == 0U && stats.reading == 0U);
}

static void close_empty(void)
{
    ch_expect("close_empty", rtos_channel_close(&channel) == RTOS_OK);
    ch_expect("deinit_empty", rtos_channel_deinit(&channel) == RTOS_OK);
}

static void test_parameters(void)
{
    rtos_channel_config_t config = main_config(CH_BLOCK_COUNT);
    rtos_channel_config_t invalid = config;
    rtos_channel_t snapshot;

    memset(&channel, 0xA5, sizeof(channel));
    snapshot = channel;
    ch_expect("null_channel", rtos_channel_init(NULL, &config) == RTOS_ERR_PARAM);
    ch_expect("null_config", rtos_channel_init(&channel, NULL) == RTOS_ERR_PARAM);
    invalid.storage = storage + 1U;
    ch_expect("storage_alignment", rtos_channel_init(&channel, &invalid) == RTOS_ERR_PARAM);
    invalid = config;
    invalid.storage_size = sizeof(storage) - 1U;
    ch_expect("storage_capacity", rtos_channel_init(&channel, &invalid) == RTOS_ERR_PARAM);
    invalid = config;
    invalid.slots_size = sizeof(slots) - 1U;
    ch_expect("metadata_capacity", rtos_channel_init(&channel, &invalid) == RTOS_ERR_PARAM);
    invalid = config;
    invalid.block_size = 0U;
    ch_expect("zero_block_size", rtos_channel_init(&channel, &invalid) == RTOS_ERR_PARAM);
    invalid = config;
    invalid.block_count = 0U;
    ch_expect("zero_block_count", rtos_channel_init(&channel, &invalid) == RTOS_ERR_PARAM);
    ch_expect("rejected_init_preserves_control", memcmp(&snapshot, &channel,
        sizeof(channel)) == 0);
    if (initialize_main(CH_BLOCK_COUNT) == 0U) {
        return;
    }
    ch_expect("duplicate_init", rtos_channel_init(&channel, &config) == RTOS_ERR_ALREADY);
    ch_expect("open_deinit_busy", rtos_channel_deinit(&channel) == RTOS_ERR_BUSY);
    ch_expect("acquire_null_output", rtos_channel_acquire(&channel, NULL, 0U)
        == RTOS_ERR_PARAM);
    ch_expect("receive_null_output", rtos_channel_receive(&channel, NULL, 0U)
        == RTOS_ERR_PARAM);
    ch_expect("null_stats", rtos_channel_get_stats(&channel, NULL) == RTOS_ERR_PARAM);
    ch_expect("aliased_output_rejected", rtos_channel_acquire(&channel,
        (rtos_channel_writer_t *)(void *)storage, 0U) == RTOS_ERR_PARAM);
    inspect_free("failed_calls_preserve_ledger", CH_BLOCK_COUNT);
    close_empty();
}

static void test_tickets_and_data(void)
{
    rtos_channel_config_t config = {other_storage, sizeof(other_storage), other_slots,
        sizeof(other_slots), CH_BLOCK_SIZE, 1U};
    rtos_channel_writer_t writers[CH_BLOCK_COUNT] = {0};
    rtos_channel_writer_t saved[CH_BLOCK_COUNT] = {0};
    rtos_channel_reader_t reader = {0};
    rtos_channel_stats_t stats = {0};
    uint64_t last_sequence = 0U;

    if (initialize_main(CH_BLOCK_COUNT) == 0U) {
        return;
    }
    ch_expect("other_channel_init", rtos_channel_init(&other_channel, &config) == RTOS_OK);
    for (uint32_t index = 0U; index < CH_BLOCK_COUNT; index++) {
        rtos_status_t status = rtos_channel_acquire(&channel, &writers[index], 0U);
        ch_expect("writer_acquired", status == RTOS_OK);
        if (status != RTOS_OK) {
            continue;
        }
        saved[index] = writers[index];
        ch_expect("writer_ticket_odd", (writers[index].ticket.generation & 1U) != 0U);
        ch_expect("writer_pointer_identity", writers[index].data
            == &storage[writers[index].ticket.slot * CH_BLOCK_SIZE]);
        ch_expect("writer_capacity", writers[index].capacity == CH_BLOCK_SIZE);
        ch_fill(writers[index].data, CH_BLOCK_SIZE, index + 1U);
    }
    rtos_channel_writer_t unchanged;
    memset(&unchanged, 0xA5, sizeof(unchanged));
    rtos_channel_writer_t sentinel = unchanged;
    ch_expect("full_acquire_timeout", rtos_channel_acquire(&channel, &unchanged, 0U)
        == RTOS_ERR_TIMEOUT);
    ch_expect("failed_acquire_output_unchanged", memcmp(&unchanged, &sentinel,
        sizeof(sentinel)) == 0);
    ch_expect("wrong_channel_publish", rtos_channel_publish(&other_channel, &writers[0],
        CH_BLOCK_SIZE, 1U) == RTOS_ERR_STALE);
    ch_expect("oversize_publish", rtos_channel_publish(&channel, &writers[0],
        CH_BLOCK_SIZE + 1U, 1U) == RTOS_ERR_PARAM);
    ch_expect("failed_publish_keeps_writer", writers[0].data == saved[0].data
        && writers[0].ticket.generation == saved[0].ticket.generation);
    for (uint32_t index = 0U; index < CH_BLOCK_COUNT; index++) {
        uint32_t length = index == 0U ? 0U : CH_BLOCK_SIZE;
        ch_expect("publish", rtos_channel_publish(&channel, &writers[index], length,
            index + 1U) == RTOS_OK);
        ch_expect("publish_invalidates_view", writers[index].data == NULL
            && writers[index].ticket.generation == 0U && writers[index].capacity == 0U);
        ch_expect("duplicate_publish_rejected", rtos_channel_publish(&channel, &saved[index],
            length, index + 1U) == RTOS_ERR_STALE);
    }
    ch_expect("queued_ledger", rtos_channel_get_stats(&channel, &stats) == RTOS_OK
        && stats.queued == CH_BLOCK_COUNT && stats.free == 0U && ch_conserved(&stats));
    for (uint32_t index = 0U; index < CH_BLOCK_COUNT; index++) {
        rtos_status_t status = rtos_channel_receive(&channel, &reader, 0U);
        ch_expect("receive", status == RTOS_OK);
        if (status != RTOS_OK) {
            continue;
        }
        rtos_channel_reader_t old_reader = reader;
        ch_expect("receive_fifo_tag", reader.tag == index + 1U);
        ch_expect("receive_length", reader.length == (index == 0U ? 0U : CH_BLOCK_SIZE));
        ch_expect("receive_pointer_identity", reader.data == saved[index].data);
        ch_expect("reader_ticket_even", (reader.ticket.generation & 1U) == 0U
            && reader.ticket.generation == saved[index].ticket.generation + 1U);
        ch_expect("monotonic_sequence", reader.sequence > last_sequence);
        last_sequence = reader.sequence;
        ch_expect("in_place_payload", ch_verify(reader.data, reader.length, index + 1U));
        ch_expect("writer_cannot_cancel_reader", rtos_channel_cancel(&channel, &saved[index])
            == RTOS_ERR_STALE);
        ch_expect("release", rtos_channel_release(&channel, &reader) == RTOS_OK);
        ch_expect("release_invalidates_view", reader.data == NULL
            && reader.ticket.generation == 0U && reader.length == 0U);
        ch_expect("duplicate_release_rejected", rtos_channel_release(&channel, &old_reader)
            == RTOS_ERR_STALE);
    }
    inspect_free("all_data_returned", CH_BLOCK_COUNT);
    ch_expect("other_channel_close", rtos_channel_close(&other_channel) == RTOS_OK);
    ch_expect("other_channel_deinit", rtos_channel_deinit(&other_channel) == RTOS_OK);
    close_empty();
}

static void test_close_and_recreate(void)
{
    rtos_channel_writer_t writer = {0};
    rtos_channel_writer_t old_writer;
    rtos_channel_reader_t reader = {0};
    rtos_channel_reader_t old_reader;
    rtos_channel_stats_t stats = {0};

    if (initialize_main(2U) == 0U) {
        return;
    }
    ch_expect("close_writer_acquire", rtos_channel_acquire(&channel, &writer, 0U) == RTOS_OK);
    old_writer = writer;
    ch_fill(writer.data, CH_BLOCK_SIZE, 77U);
    ch_expect("close_writer_publish", rtos_channel_publish(&channel, &writer,
        CH_BLOCK_SIZE, 77U) == RTOS_OK);
    ch_expect("close_second_acquire", rtos_channel_acquire(&channel, &writer, 0U) == RTOS_OK);
    ch_expect("close_with_inflight", rtos_channel_close(&channel) == RTOS_OK);
    ch_expect("close_repeat", rtos_channel_close(&channel) == RTOS_ERR_ALREADY);
    ch_expect("close_does_not_reclaim", rtos_channel_get_stats(&channel, &stats) == RTOS_OK
        && stats.writing == 1U && stats.queued == 1U && stats.free == 0U);
    ch_expect("closed_deinit_busy", rtos_channel_deinit(&channel) == RTOS_ERR_BUSY);
    ch_expect("closed_publish_rejected", rtos_channel_publish(&channel, &writer, 1U, 1U)
        == RTOS_ERR_CLOSED);
    ch_expect("closed_publish_keeps_ownership", writer.data != NULL);
    ch_expect("closed_cancel_allowed", rtos_channel_cancel(&channel, &writer) == RTOS_OK);
    ch_expect("closed_queue_can_drain", rtos_channel_receive(&channel, &reader, 0U) == RTOS_OK);
    old_reader = reader;
    ch_expect("drained_data_valid", reader.tag == 77U
        && ch_verify(reader.data, reader.length, 77U));
    ch_expect("reading_prevents_deinit", rtos_channel_deinit(&channel) == RTOS_ERR_BUSY);
    ch_expect("closed_release_allowed", rtos_channel_release(&channel, &reader) == RTOS_OK);
    ch_expect("closed_empty_receive", rtos_channel_receive(&channel, &reader, 0U)
        == RTOS_ERR_CLOSED);
    ch_expect("closed_free_acquire", rtos_channel_acquire(&channel, &writer, 0U)
        == RTOS_ERR_CLOSED);
    ch_expect("closed_empty_deinit", rtos_channel_deinit(&channel) == RTOS_OK);
    ch_expect("unregistered_call", rtos_channel_acquire(&channel, &writer, 0U)
        == RTOS_ERR_DELETED);
    if (initialize_main(2U) == 0U) {
        return;
    }
    ch_expect("old_writer_rejected_after_reinit", rtos_channel_cancel(&channel, &old_writer)
        == RTOS_ERR_STALE);
    ch_expect("old_reader_rejected_after_reinit", rtos_channel_release(&channel, &old_reader)
        == RTOS_ERR_STALE);
    inspect_free("new_instance_unchanged", 2U);
    close_empty();
}

static void worker_entry(void *argument)
{
    uint32_t index = (uint32_t)(uintptr_t)argument;
    rtos_channel_writer_t writer = {0};
    rtos_channel_reader_t reader = {0};
    rtos_tick_t start = rtos_sched_get_tick_count();
    rtos_status_t status;
    uint32_t valid = 1U;

    if (worker_acquire != 0U) {
        status = rtos_channel_acquire(&channel, &writer, worker_timeout);
        if (status == RTOS_OK) {
            valid = rtos_channel_cancel(&channel, &writer) == RTOS_OK;
        }
    } else {
        status = rtos_channel_receive(&channel, &reader, worker_timeout);
        if (status == RTOS_OK) {
            valid = ch_verify(reader.data, reader.length, reader.tag);
            valid &= rtos_channel_release(&channel, &reader) == RTOS_OK;
        }
    }
    RTOS_PORT_ENTER_CRITICAL();
    results[index].status = status;
    results[index].elapsed = rtos_sched_get_tick_count() - start;
    results[index].valid = valid;
    results[index].finished = 1U;
    RTOS_PORT_EXIT_CRITICAL();
    if (rtos_sem_give(&done) != RTOS_OK) {
        results[index].valid = 0U;
    }
    if (rtos_task_suspend(NULL) != RTOS_OK) {
        results[index].valid = 0U;
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static uint32_t prepare_workers(uint32_t acquire, rtos_tick_t timeout)
{
    worker_mask = 0U;
    worker_acquire = acquire;
    worker_timeout = timeout;
    memset(results, 0, sizeof(results));
    rtos_status_t status = rtos_sem_init(&done, 0U, CH_WORKERS);

    ch_expect("done_semaphore_init", status == RTOS_OK);
    return status == RTOS_OK;
}

static uint32_t create_worker(uint32_t index)
{
    rtos_status_t status = rtos_task_create_static(&workers[index], stacks[index],
        CH_STACK_WORDS, worker_entry, (void *)(uintptr_t)index, 11U + index, "ch-wait");

    ch_expect("waiter_created", status == RTOS_OK);
    if (status == RTOS_OK) {
        worker_mask |= 1UL << index;
        ch_expect("waiter_actually_blocked", ch_wait_blocked(&workers[index], &channel));
    }
    return status == RTOS_OK;
}

static void cleanup_workers(void)
{
    for (uint32_t index = 0U; index < CH_WORKERS; index++) {
        if ((worker_mask & (1UL << index)) != 0U) {
            ch_expect("waiter_deleted", rtos_task_delete(&workers[index]) == RTOS_OK);
        }
    }
    ch_expect("done_semaphore_deinit", rtos_sem_deinit(&done) == RTOS_OK);
    worker_mask = 0U;
}

static void test_ticket_exhaustion(void)
{
    rtos_channel_writer_t writer;
    rtos_channel_writer_t sentinel;

    if (initialize_main(1U) == 0U) {
        return;
    }
    memset(&writer, 0xA5, sizeof(writer));
    sentinel = writer;
    /* White-box injection avoids billions of real loan operations. No loan exists. */
    RTOS_PORT_ENTER_CRITICAL();
    uint64_t saved = channel.next_ticket;
    channel.next_ticket = UINT64_MAX - 1U;
    RTOS_PORT_EXIT_CRITICAL();
    ch_expect("ticket_exhaustion_refused", rtos_channel_acquire(&channel, &writer, 0U)
        == RTOS_ERR_OVERFLOW);
    ch_expect("ticket_exhaustion_output_unchanged", memcmp(&writer, &sentinel,
        sizeof(writer)) == 0);
    RTOS_PORT_ENTER_CRITICAL();
    channel.next_ticket = saved;
    RTOS_PORT_EXIT_CRITICAL();
    (void)printf("CH|INJECT|boundary=ticket_exhaustion method=whitebox\r\n");
    inspect_free("exhaustion_preserves_ledger", 1U);
    close_empty();
}

static void test_wait_contexts(void)
{
    rtos_channel_writer_t writer = {0};
    rtos_channel_reader_t reader = {0};
    rtos_tick_t start;

    if (initialize_main(1U) == 0U) {
        return;
    }
    start = rtos_sched_get_tick_count();
    ch_expect("finite_empty_receive", rtos_channel_receive(&channel, &reader, 7U)
        == RTOS_ERR_TIMEOUT);
    ch_expect("receive_timeout_elapsed", rtos_sched_get_tick_count() - start >= 7U);
    rtos_sched_suspend();
    rtos_status_t status = rtos_channel_receive(&channel, &reader, 1U);
    rtos_sched_resume();
    ch_expect("scheduler_suspended_refuses_block", status == RTOS_ERR_SCHED);
    RTOS_PORT_ENTER_CRITICAL();
    status = rtos_channel_receive(&channel, &reader, 1U);
    RTOS_PORT_EXIT_CRITICAL();
    ch_expect("outer_critical_refuses_block", status == RTOS_ERR_SCHED);
    uint32_t saved_mask = __get_PRIMASK();
    __disable_irq();
    status = rtos_channel_receive(&channel, &reader, 1U);
    __set_PRIMASK(saved_mask);
    __ISB();
    ch_expect("primask_refuses_block", status == RTOS_ERR_SCHED);
    ch_expect("timeout_holder_acquire", rtos_channel_acquire(&channel, &writer, 0U) == RTOS_OK);
    start = rtos_sched_get_tick_count();
    ch_expect("finite_full_acquire", rtos_channel_acquire(&channel, &irq_writer, 7U)
        == RTOS_ERR_TIMEOUT);
    ch_expect("acquire_timeout_elapsed", rtos_sched_get_tick_count() - start >= 7U);
    ch_expect("timeout_holder_cancel", rtos_channel_cancel(&channel, &writer) == RTOS_OK);
    close_empty();
}

static void test_total_budget(void)
{
    rtos_channel_writer_t writer = {0};

    if (initialize_main(1U) == 0U || prepare_workers(1U, 12U) == 0U) {
        return;
    }
    ch_expect("budget_holder_acquire", rtos_channel_acquire(&channel, &writer, 0U) == RTOS_OK);
    if (create_worker(0U) != 0U) {
        uint32_t releases_ok = 1U;
        uint32_t steals_ok = 1U;
        uint32_t retry_wakes = 0U;
        for (uint32_t round = 0U; round < 9U; round++) {
            RTOS_PORT_ENTER_CRITICAL();
            retry_wakes += workers[0].state == RTOS_TASK_DELAYED;
            RTOS_PORT_EXIT_CRITICAL();
            releases_ok &= rtos_channel_cancel(&channel, &writer) == RTOS_OK;
            steals_ok &= rtos_channel_acquire(&channel, &writer, 0U) == RTOS_OK;
            rtos_task_delay(2U);
        }
        /* UART output here would delay the lower-priority worker and invalidate
         * its return-latency bound, so only report after the retry window. */
        ch_expect("budget_release", releases_ok);
        ch_expect("budget_steal_before_waiter", steals_ok);
        ch_expect("budget_multiple_retries_observed", retry_wakes >= 2U);
        ch_expect("budget_wait_completed", rtos_sem_take(&done, 20U) == RTOS_OK);
        ch_expect("retry_preserves_total_budget", results[0].status == RTOS_ERR_TIMEOUT
            && results[0].elapsed >= 12U && results[0].elapsed <= 15U);
        (void)printf("CH|BUDGET|elapsed=%lu requested=12\r\n",
            (unsigned long)results[0].elapsed);
    }
    ch_expect("budget_final_cancel", rtos_channel_cancel(&channel, &writer) == RTOS_OK);
    cleanup_workers();
    close_empty();
}

static void make_deadline_resource(uint32_t acquire, rtos_channel_writer_t *holder)
{
    if (acquire != 0U) {
        ch_expect("deadline_holder_cancel", rtos_channel_cancel(&channel, holder) == RTOS_OK);
    } else {
        ch_expect("deadline_writer_acquire", rtos_channel_acquire(&channel, holder, 0U)
            == RTOS_OK);
        ch_fill(holder->data, CH_BLOCK_SIZE, 88U);
        ch_expect("deadline_writer_publish", rtos_channel_publish(&channel, holder,
            CH_BLOCK_SIZE, 88U) == RTOS_OK);
    }
}

static void test_late_grant(uint32_t acquire, uint32_t early_wake)
{
    rtos_channel_writer_t holder = {0};

    if (initialize_main(1U) == 0U || prepare_workers(acquire, 50U) == 0U) {
        return;
    }
    if (acquire != 0U) {
        ch_expect("deadline_hold_only_slot", rtos_channel_acquire(&channel, &holder, 0U)
            == RTOS_OK);
    }
    if (create_worker(0U) != 0U) {
        if (early_wake != 0U) {
            make_deadline_resource(acquire, &holder);
            ch_expect("deadline_early_wake_ready", workers[0].state == RTOS_TASK_READY);
        }
        /* Controller priority 9 prevents priority 11 worker from running, but
         * SysTick remains enabled so its original fifty-tick budget expires. */
        rtos_tick_t start = rtos_sched_get_tick_count();
        while (rtos_sched_get_tick_count() - start < 60U) {
            __NOP();
        }
        if (early_wake == 0U) {
            make_deadline_resource(acquire, &holder);
        }
        ch_expect("deadline_waiter_returned", rtos_sem_take(&done, 100U) == RTOS_OK);
        ch_expect("deadline_resource_not_granted_late", results[0].finished != 0U
            && results[0].status == RTOS_ERR_TIMEOUT && results[0].elapsed >= 50U);
        rtos_channel_stats_t stats = {0};
        ch_expect("deadline_ledger_read", rtos_channel_get_stats(&channel, &stats) == RTOS_OK);
        ch_expect("deadline_available_resource_preserved", acquire != 0U
            ? stats.free == 1U && stats.queued == 0U
            : stats.free == 0U && stats.queued == 1U);
        if (acquire == 0U) {
            rtos_channel_reader_t reader = {0};
            ch_expect("deadline_drain_retained_message", rtos_channel_receive(&channel,
                &reader, 0U) == RTOS_OK);
            ch_expect("deadline_release_retained_message", rtos_channel_release(&channel,
                &reader) == RTOS_OK);
        }
        (void)printf("CH|DEADLINE|operation=%s early_wake=%lu elapsed=%lu result=%ld\r\n",
            acquire != 0U ? "acquire" : "receive", (unsigned long)early_wake,
            (unsigned long)results[0].elapsed, (long)results[0].status);
    }
    cleanup_workers();
    inspect_free("deadline_all_blocks_returned", 1U);
    close_empty();
}

static void test_ready_waiter(uint32_t acquire, uint32_t remove)
{
    rtos_channel_writer_t holder = {0};

    if (initialize_main(1U) == 0U || prepare_workers(acquire, 100U) == 0U) {
        return;
    }
    if (acquire != 0U) {
        ch_expect("ready_holder", rtos_channel_acquire(&channel, &holder, 0U) == RTOS_OK);
    }
    if (create_worker(0U) != 0U && create_worker(1U) != 0U) {
        if (acquire != 0U) {
            ch_expect("ready_release_resource", rtos_channel_cancel(&channel, &holder) == RTOS_OK);
        } else {
            ch_expect("ready_writer", rtos_channel_acquire(&channel, &holder, 0U) == RTOS_OK);
            ch_fill(holder.data, CH_BLOCK_SIZE, 88U);
            ch_expect("ready_publish", rtos_channel_publish(&channel, &holder,
                CH_BLOCK_SIZE, 88U) == RTOS_OK);
        }
        ch_expect("first_waiter_is_ready", workers[0].state == RTOS_TASK_READY);
        if (remove != 0U) {
            ch_expect("delete_ready_waiter", rtos_task_delete(&workers[0]) == RTOS_OK);
            worker_mask &= ~1UL;
        } else {
            ch_expect("suspend_ready_waiter", rtos_task_suspend(&workers[0]) == RTOS_OK);
        }
        ch_expect("other_waiter_was_not_stranded", rtos_sem_take(&done, 100U) == RTOS_OK
            && results[1].finished != 0U && results[1].status == RTOS_OK
            && results[1].valid != 0U);
    }
    cleanup_workers();
    inspect_free("ready_case_all_free", 1U);
    close_empty();
}

static void test_close_waiters(uint32_t acquire, uint32_t recreate)
{
    rtos_channel_writer_t holder = {0};

    if (initialize_main(1U) == 0U || prepare_workers(acquire, RTOS_WAIT_FOREVER) == 0U) {
        return;
    }
    if (acquire != 0U) {
        ch_expect("close_wait_holder", rtos_channel_acquire(&channel, &holder, 0U) == RTOS_OK);
    }
    for (uint32_t index = 0U; index < CH_WORKERS; index++) {
        if (create_worker(index) == 0U) {
            break;
        }
    }
    ch_expect("close_wakes_all", rtos_channel_close(&channel) == RTOS_OK);
    if (acquire != 0U) {
        ch_expect("close_retains_writer", rtos_channel_deinit(&channel) == RTOS_ERR_BUSY);
        ch_expect("close_final_cancel", rtos_channel_cancel(&channel, &holder) == RTOS_OK);
    }
    if (recreate != 0U) {
        ch_expect("deinit_before_old_waiter_return", rtos_channel_deinit(&channel) == RTOS_OK);
        (void)initialize_main(1U);
    }
    for (uint32_t index = 0U; index < CH_WORKERS; index++) {
        ch_expect("closed_waiter_returned", rtos_sem_take(&done, 100U) == RTOS_OK
            && results[index].finished != 0U
            && results[index].status == (recreate != 0U ? RTOS_ERR_DELETED : RTOS_ERR_CLOSED));
    }
    cleanup_workers();
    inspect_free("close_wait_all_free", 1U);
    if (recreate != 0U) {
        ch_expect("close_recreated", rtos_channel_close(&channel) == RTOS_OK);
    }
    ch_expect("close_wait_deinit", rtos_channel_deinit(&channel) == RTOS_OK);
}

void TIM7_IRQHandler(void)
{
    rtos_channel_config_t config = main_config(CH_BLOCK_COUNT);
    rtos_channel_stats_t stats;

    if (irq_legal != 0U) {
        irq_status[0] = rtos_channel_acquire(&channel, &irq_writer, 0U);
        if (irq_status[0] == RTOS_OK) {
            ch_fill(irq_writer.data, CH_BLOCK_SIZE, 99U);
            irq_status[1] = rtos_channel_publish(&channel, &irq_writer, CH_BLOCK_SIZE, 99U);
            irq_status[2] = rtos_channel_receive(&channel, &irq_reader, 0U);
            if (irq_status[2] == RTOS_OK) {
                irq_status[3] = rtos_channel_release(&channel, &irq_reader);
            }
        }
        irq_status[4] = rtos_channel_acquire(&channel, &irq_writer, 1U);
    } else {
        irq_status[0] = rtos_channel_acquire(&channel, &irq_writer, 0U);
        irq_status[1] = rtos_channel_publish(&channel, &irq_writer, CH_BLOCK_SIZE, 99U);
        irq_status[2] = rtos_channel_receive(&channel, &irq_reader, 0U);
        irq_status[3] = rtos_channel_release(&channel, &irq_reader);
        irq_status[4] = rtos_channel_cancel(&channel, &irq_writer);
    }
    irq_status[5] = rtos_channel_init(&channel, &config);
    irq_status[6] = rtos_channel_close(&channel);
    irq_status[7] = rtos_channel_deinit(&channel);
    irq_status[8] = rtos_channel_get_stats(&channel, &stats);
    irq_status[9] = rtos_channel_receive(&channel, &irq_reader, 1U);
    irq_done = 1U;
}

static void run_irq(uint32_t priority)
{
    irq_legal = priority >= 5U;
    irq_done = 0U;
    for (uint32_t index = 0U; index < 10U; index++) {
        irq_status[index] = RTOS_ERR_PARAM;
    }
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
    ch_expect("real_irq_executed", irq_done != 0U);
    for (uint32_t index = 0U; index < 10U; index++) {
        rtos_status_t expected = irq_legal != 0U && index < 4U ? RTOS_OK : RTOS_ERR_ISR;
        ch_expect("irq_return_contract", irq_status[index] == expected);
    }
    (void)printf("CH|IRQ|priority=%lu legal=%lu completed=%lu\r\n",
        (unsigned long)priority, (unsigned long)irq_legal, (unsigned long)irq_done);
}

static void test_irq_contract(void)
{
    rtos_channel_stats_t before = {0};
    rtos_channel_stats_t after = {0};

    if (initialize_main(CH_BLOCK_COUNT) == 0U) {
        return;
    }
    irq_priority = NVIC_GetPriority(TIM7_IRQn);
    irq_enabled = NVIC_GetEnableIRQ(TIM7_IRQn);
    irq_pending = NVIC_GetPendingIRQ(TIM7_IRQn);
    ch_expect("irq_line_initially_unused", irq_enabled == 0U && irq_pending == 0U);
    ch_expect("irq_guard_writer", rtos_channel_acquire(&channel, &irq_writer, 0U) == RTOS_OK);
    rtos_channel_writer_t seed = {0};
    ch_expect("irq_guard_reader_seed", rtos_channel_acquire(&channel, &seed, 0U) == RTOS_OK);
    ch_expect("irq_guard_reader_publish", rtos_channel_publish(&channel, &seed, 0U, 0U)
        == RTOS_OK);
    ch_expect("irq_guard_reader", rtos_channel_receive(&channel, &irq_reader, 0U) == RTOS_OK);
    ch_expect("irq_snapshot_before", rtos_channel_get_stats(&channel, &before) == RTOS_OK);
    run_irq(0U);
    run_irq(4U);
    ch_expect("irq_snapshot_after", rtos_channel_get_stats(&channel, &after) == RTOS_OK);
    ch_expect("forbidden_irqs_preserve_state", before.acquired == after.acquired
        && before.received == after.received && before.writing == after.writing
        && before.reading == after.reading && before.free == after.free);
    ch_expect("guard_writer_cancel", rtos_channel_cancel(&channel, &irq_writer) == RTOS_OK);
    ch_expect("guard_reader_release", rtos_channel_release(&channel, &irq_reader) == RTOS_OK);
    run_irq(5U);
    NVIC_SetPriority(TIM7_IRQn, irq_priority);
    if (irq_enabled != 0U) {
        NVIC_EnableIRQ(TIM7_IRQn);
    }
    if (irq_pending != 0U) {
        NVIC_SetPendingIRQ(TIM7_IRQn);
    }
    ch_expect("irq_state_restored", NVIC_GetPriority(TIM7_IRQn) == irq_priority
        && NVIC_GetEnableIRQ(TIM7_IRQn) == irq_enabled
        && NVIC_GetPendingIRQ(TIM7_IRQn) == irq_pending);
    inspect_free("irq_all_blocks_returned", CH_BLOCK_COUNT);
    close_empty();
}

void audit_pre_start(void)
{
    (void)printf("CH|META|mode=functional blocks=4 bytes=64 workers=4 irq=TIM7 "
        "priorities=0,4,5 assertions=%u mask=%u\r\n",
        (unsigned int)RTOS_CONFIG_ASSERT_ENABLED, (unsigned int)RTOS_CONFIG_USE_BASEPRI);
}

void audit_regressions_task(void *argument)
{
    (void)argument;
    test_parameters();
    test_tickets_and_data();
    test_close_and_recreate();
    test_ticket_exhaustion();
    test_wait_contexts();
    test_total_budget();
    test_late_grant(0U, 0U);
    test_late_grant(0U, 1U);
    test_late_grant(1U, 0U);
    test_late_grant(1U, 1U);
    test_ready_waiter(0U, 0U);
    test_ready_waiter(0U, 1U);
    test_ready_waiter(1U, 0U);
    test_ready_waiter(1U, 1U);
    test_close_waiters(0U, 0U);
    test_close_waiters(1U, 0U);
    test_close_waiters(0U, 1U);
    test_irq_contract();
    ch_finish("functional");
}

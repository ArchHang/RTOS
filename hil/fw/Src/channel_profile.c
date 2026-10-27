/**
 * @file channel_profile.c
 * @brief Observe IRQ masking latency inside copy IPC and loan channel critical sections.
 *
 * The test-only ENTER_MASKED hook pends a real priority-5 TIM7 IRQ exactly once
 * in the selected task's first outer critical section. The IRQ records only a
 * DWT delta and an entry count. Results include hook/exception overhead and any
 * higher-priority interference; they are observed samples, never a WCET bound.
 * No timer is started and no SysTick configuration is changed.
 */
#include "audit_regressions.h"
#include "channel_fixture.h"

#if !defined(RTOS_HIL_CRITICAL_TEST)
#error "channel_profile requires RTOS_HIL_CRITICAL_TEST in every kernel translation unit"
#endif

#define CH_PROFILE_BYTES (16384U)
#define CH_PROFILE_REPETITIONS (8U)
#define CH_PROFILE_WAITERS (4U)
#define CH_PROFILE_STACK_WORDS (256U)

typedef enum ch_profile_operation {
    CH_PROFILE_CRITICAL = 0,
    CH_PROFILE_COPY_SEND,
    CH_PROFILE_COPY_RECEIVE,
    CH_PROFILE_LOAN_ACQUIRE,
    CH_PROFILE_LOAN_PUBLISH,
    CH_PROFILE_LOAN_RECEIVE,
    CH_PROFILE_LOAN_RELEASE
} ch_profile_operation_t;

static uint8_t queue_storage[CH_PROFILE_BYTES] __attribute__((aligned(8)));
static uint8_t arena[CH_PROFILE_BYTES] __attribute__((aligned(8)));
static uint8_t scratch[CH_PROFILE_BYTES] __attribute__((aligned(8)));
static rtos_queue_t queue;
static rtos_channel_t channel;
static rtos_channel_slot_t slots[1];
static rtos_channel_writer_t writer;
static rtos_channel_reader_t reader;
static rtos_tcb_t workers[CH_PROFILE_WAITERS];
static rtos_stack_t stacks[CH_PROFILE_WAITERS][CH_PROFILE_STACK_WORDS]
    __attribute__((aligned(8)));
static uint32_t worker_mask;
static uint32_t worker_acquire;
static uint32_t worker_returns;
static uint32_t worker_errors;
static uint32_t saved_priority;
static uint32_t saved_enabled;
static uint32_t saved_pending;
/* These fields communicate with a real asynchronous hardware exception. */
static volatile uint32_t window_armed;
static volatile uint32_t hook_hits;
static volatile uint32_t hook_start_cycle;
static volatile uint32_t irq_count;
static volatile uint32_t irq_cycles;

/**
 * @brief Pend the observed IRQ once at the selected outer task critical entry.
 * @param stage [in] Port critical transition; handler-mode entries are excluded.
 */
void rtos_port_critical_test_hook(uint32_t stage)
{
    if (stage == RTOS_PORT_TEST_ENTER_MASKED && window_armed != 0U
        && rtos_port_crit_nest == 0U && __get_IPSR() == 0U) {
        window_armed = 0U;
        hook_hits++;
        hook_start_cycle = DWT->CYCCNT;
        NVIC_SetPendingIRQ(TIM7_IRQn);
        __DSB();
    }
}

/* Fixed CMSIS vector ABI; no kernel or output operation is permitted here. */
void TIM7_IRQHandler(void)
{
    irq_cycles = DWT->CYCCNT - hook_start_cycle;
    irq_count++;
}

static uint32_t claim_irq(void)
{
    saved_priority = NVIC_GetPriority(TIM7_IRQn);
    saved_enabled = NVIC_GetEnableIRQ(TIM7_IRQn);
    saved_pending = NVIC_GetPendingIRQ(TIM7_IRQn);
    uint32_t peripheral_idle = (RCC->APB1ENR & RCC_APB1ENR_TIM7EN) == 0U
        || ((TIM7->CR1 & TIM_CR1_CEN) == 0U && TIM7->DIER == 0U && TIM7->SR == 0U);

    if (saved_enabled != 0U || saved_pending != 0U || peripheral_idle == 0U) {
        return 0U;
    }
    NVIC_SetPriority(TIM7_IRQn, 5U);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    NVIC_EnableIRQ(TIM7_IRQn);
    return 1U;
}

static void restore_irq(void)
{
    window_armed = 0U;
    NVIC_DisableIRQ(TIM7_IRQn);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    NVIC_SetPriority(TIM7_IRQn, saved_priority);
    ch_expect("profile_irq_restored", NVIC_GetPriority(TIM7_IRQn) == saved_priority
        && NVIC_GetEnableIRQ(TIM7_IRQn) == saved_enabled
        && NVIC_GetPendingIRQ(TIM7_IRQn) == saved_pending);
}

static const char *operation_name(ch_profile_operation_t operation)
{
    switch (operation) {
        case CH_PROFILE_CRITICAL:
            return "critical";
        case CH_PROFILE_COPY_SEND:
            return "copy_send";
        case CH_PROFILE_COPY_RECEIVE:
            return "copy_receive";
        case CH_PROFILE_LOAN_ACQUIRE:
            return "loan_acquire";
        case CH_PROFILE_LOAN_PUBLISH:
            return "loan_publish";
        case CH_PROFILE_LOAN_RECEIVE:
            return "loan_receive";
        case CH_PROFILE_LOAN_RELEASE:
            return "loan_release";
        default:
            return "invalid";
    }
}

static rtos_status_t execute_operation(ch_profile_operation_t operation, uint32_t bytes)
{
    switch (operation) {
        case CH_PROFILE_CRITICAL:
            RTOS_PORT_ENTER_CRITICAL();
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_OK;
        case CH_PROFILE_COPY_SEND:
            return rtos_queue_send(&queue, scratch, 0U, RTOS_FALSE);
        case CH_PROFILE_COPY_RECEIVE:
            return rtos_queue_recv(&queue, scratch, 0U);
        case CH_PROFILE_LOAN_ACQUIRE:
            return rtos_channel_acquire(&channel, &writer, 0U);
        case CH_PROFILE_LOAN_PUBLISH:
            return rtos_channel_publish(&channel, &writer, bytes, 71U);
        case CH_PROFILE_LOAN_RECEIVE:
            return rtos_channel_receive(&channel, &reader, 0U);
        case CH_PROFILE_LOAN_RELEASE:
            return rtos_channel_release(&channel, &reader);
        default:
            return RTOS_ERR_PARAM;
    }
}

static rtos_status_t measure_operation(ch_profile_operation_t operation, uint32_t bytes,
    uint32_t waiters, uint32_t repetition, const char *scenario)
{
    window_armed = 0U;
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    hook_hits = 0U;
    hook_start_cycle = 0U;
    irq_count = 0U;
    irq_cycles = 0U;
    __DMB();
    window_armed = 1U;
    __DMB();
    rtos_status_t status = execute_operation(operation, bytes);
    __DSB();
    __ISB();
    for (uint32_t poll = 0U; poll < 1000000U && irq_count == 0U; poll++) {
        __NOP();
    }
    window_armed = 0U;
    uint32_t valid = status == RTOS_OK && hook_hits == 1U && irq_count == 1U
        && irq_cycles != 0U && rtos_port_crit_nest == 0U
        && __get_BASEPRI() == 0U && __get_PRIMASK() == 0U;
    ch_expect("profile_observed_once", valid);
    (void)printf("CH|PROFILE|case=%s operation=%s bytes=%lu waiters=%lu repetition=%lu "
        "cycles=%lu hook_hits=%lu irq_count=%lu status=%ld valid=%lu\r\n",
        scenario, operation_name(operation), (unsigned long)bytes, (unsigned long)waiters,
        (unsigned long)repetition, (unsigned long)irq_cycles, (unsigned long)hook_hits,
        (unsigned long)irq_count, (long)status, (unsigned long)valid);
    return status;
}

static uint32_t initialize_channel(uint32_t bytes)
{
    rtos_channel_config_t config = {arena, sizeof(arena), slots, sizeof(slots), bytes, 1U};
    rtos_status_t status = rtos_channel_init(&channel, &config);

    ch_expect("profile_channel_init", status == RTOS_OK);
    return status == RTOS_OK;
}

static void finish_channel(void)
{
    if (writer.data != NULL) {
        ch_expect("profile_cleanup_writer", rtos_channel_cancel(&channel, &writer) == RTOS_OK);
    }
    if (reader.data != NULL) {
        ch_expect("profile_cleanup_reader", rtos_channel_release(&channel, &reader) == RTOS_OK);
    }
    rtos_channel_stats_t stats = {0};
    ch_expect("profile_ledger_read", rtos_channel_get_stats(&channel, &stats) == RTOS_OK);
    if (stats.queued != 0U) {
        rtos_status_t status = rtos_channel_receive(&channel, &reader, 0U);
        ch_expect("profile_cleanup_queue", status == RTOS_OK);
        if (status == RTOS_OK) {
            ch_expect("profile_cleanup_queue_release", rtos_channel_release(&channel,
                &reader) == RTOS_OK);
        }
        ch_expect("profile_final_ledger", rtos_channel_get_stats(&channel, &stats) == RTOS_OK);
    }
    ch_expect("profile_all_loans_returned", stats.free == 1U && stats.writing == 0U
        && stats.queued == 0U && stats.reading == 0U && ch_conserved(&stats));
    ch_expect("profile_channel_close", rtos_channel_close(&channel) == RTOS_OK);
    ch_expect("profile_channel_deinit", rtos_channel_deinit(&channel) == RTOS_OK);
}

static uint32_t run_ipc_round(uint32_t bytes, uint32_t repetition)
{
    ch_fill(scratch, bytes, 71U);
    if (measure_operation(CH_PROFILE_COPY_SEND, bytes, 0U, repetition, "ipc") != RTOS_OK
        || measure_operation(CH_PROFILE_COPY_RECEIVE, bytes, 0U, repetition, "ipc")
            != RTOS_OK) {
        return 0U;
    }
    ch_expect("profile_copy_payload", ch_verify(scratch, bytes, 71U));
    if (measure_operation(CH_PROFILE_LOAN_ACQUIRE, bytes, 0U, repetition, "ipc") != RTOS_OK) {
        return 0U;
    }
    uint32_t identity = writer.data == arena && writer.capacity == bytes;
    ch_expect("profile_writer_identity", identity);
    if (identity == 0U) {
        return 0U;
    }
    ch_fill(writer.data, bytes, 71U);
    if (measure_operation(CH_PROFILE_LOAN_PUBLISH, bytes, 0U, repetition, "ipc") != RTOS_OK
        || measure_operation(CH_PROFILE_LOAN_RECEIVE, bytes, 0U, repetition, "ipc")
            != RTOS_OK) {
        return 0U;
    }
    identity = reader.data == arena && reader.length == bytes && reader.tag == 71U;
    ch_expect("profile_reader_identity", identity);
    if (identity == 0U) {
        return 0U;
    }
    ch_expect("profile_loan_payload", ch_verify(reader.data, bytes, 71U));
    return measure_operation(CH_PROFILE_LOAN_RELEASE, bytes, 0U, repetition, "ipc") == RTOS_OK;
}

static void profile_ipc_size(uint32_t bytes)
{
    rtos_status_t status = rtos_queue_init(&queue, queue_storage, bytes, 1U);

    ch_expect("profile_copy_queue_init", status == RTOS_OK);
    if (status != RTOS_OK) {
        return;
    }
    if (initialize_channel(bytes) != 0U) {
        for (uint32_t repetition = 0U; repetition < CH_PROFILE_REPETITIONS; repetition++) {
            if (run_ipc_round(bytes, repetition) == 0U) {
                break;
            }
        }
        finish_channel();
    }
    ch_expect("profile_copy_queue_deinit", rtos_queue_deinit(&queue) == RTOS_OK);
}

static void waiter_task(void *argument)
{
    (void)argument;
    rtos_channel_writer_t local_writer = {0};
    rtos_channel_reader_t local_reader = {0};
    rtos_status_t status;
    uint32_t valid = 1U;

    if (worker_acquire != 0U) {
        status = rtos_channel_acquire(&channel, &local_writer, RTOS_WAIT_FOREVER);
        if (status == RTOS_OK) {
            valid = rtos_channel_cancel(&channel, &local_writer) == RTOS_OK;
        }
    } else {
        status = rtos_channel_receive(&channel, &local_reader, RTOS_WAIT_FOREVER);
        if (status == RTOS_OK) {
            valid = rtos_channel_release(&channel, &local_reader) == RTOS_OK;
        }
    }
    RTOS_PORT_ENTER_CRITICAL();
    worker_returns++;
    worker_errors += status != RTOS_OK || valid == 0U;
    RTOS_PORT_EXIT_CRITICAL();
    if (rtos_task_suspend(NULL) != RTOS_OK) {
        RTOS_PORT_ENTER_CRITICAL();
        worker_errors++;
        RTOS_PORT_EXIT_CRITICAL();
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static uint32_t create_waiters(uint32_t acquire)
{
    worker_mask = 0U;
    worker_acquire = acquire;
    worker_returns = 0U;
    worker_errors = 0U;
    for (uint32_t index = 0U; index < CH_PROFILE_WAITERS; index++) {
        rtos_status_t status = rtos_task_create_static(&workers[index], stacks[index],
            CH_PROFILE_STACK_WORDS, waiter_task, NULL, (rtos_prio_t)(11U + index),
            "ch_profile_wait");
        ch_expect("profile_waiter_created", status == RTOS_OK);
        if (status != RTOS_OK) {
            return 0U;
        }
        worker_mask |= 1UL << index;
        uint32_t blocked = ch_wait_blocked(&workers[index], &channel);
        ch_expect("profile_waiter_actually_blocked", blocked);
        if (blocked == 0U) {
            return 0U;
        }
    }
    return 1U;
}

static void delete_waiters(void)
{
    for (uint32_t index = 0U; index < CH_PROFILE_WAITERS; index++) {
        if ((worker_mask & (1UL << index)) != 0U) {
            ch_expect("profile_waiter_deleted", rtos_task_delete(&workers[index]) == RTOS_OK);
        }
    }
    worker_mask = 0U;
}

static uint32_t prepare_broadcast(uint32_t acquire)
{
    rtos_status_t status = rtos_channel_acquire(&channel, &writer, 0U);
    ch_expect("profile_broadcast_writer", status == RTOS_OK);
    if (status != RTOS_OK) {
        return 0U;
    }
    if (acquire != 0U) {
        status = rtos_channel_publish(&channel, &writer, 256U, 71U);
        ch_expect("profile_broadcast_seed_publish", status == RTOS_OK);
        if (status != RTOS_OK) {
            return 0U;
        }
        status = rtos_channel_receive(&channel, &reader, 0U);
        ch_expect("profile_broadcast_seed_receive", status == RTOS_OK);
    }
    return status == RTOS_OK;
}

static void profile_broadcast(uint32_t acquire, uint32_t count, uint32_t repetition)
{
    if (initialize_channel(256U) == 0U) {
        return;
    }
    uint32_t ready = prepare_broadcast(acquire);
    if (ready != 0U && count != 0U) {
        ready = create_waiters(acquire);
    }
    if (ready != 0U) {
        rtos_channel_stats_t before = {0};
        ch_expect("profile_broadcast_ledger_before", rtos_channel_get_stats(&channel,
            &before) == RTOS_OK);
        ch_expect("profile_broadcast_waiter_count", acquire != 0U
            ? before.waiting_writers == count : before.waiting_readers == count);
        ch_profile_operation_t operation = acquire != 0U ? CH_PROFILE_LOAN_RELEASE
            : CH_PROFILE_LOAN_PUBLISH;
        (void)measure_operation(operation, 256U, count, repetition, "broadcast");
        uint32_t all_ready = 1U;
        RTOS_PORT_ENTER_CRITICAL();
        for (uint32_t index = 0U; index < count; index++) {
            all_ready &= workers[index].state == RTOS_TASK_READY;
        }
        uint32_t premature = worker_returns + worker_errors;
        RTOS_PORT_EXIT_CRITICAL();
        ch_expect("profile_broadcast_woke_every_waiter", all_ready && premature == 0U);
    }
    delete_waiters();
    finish_channel();
}

/** @brief Describe sampling instrumentation and the independent IRQ target. */
void audit_pre_start(void)
{
    (void)printf("CH|META|mode=profile sizes=16,256,1024,4096,16384 repetitions=%u "
        "irq=TIM7 priority=5 trigger=software_pending timer=off rows=280 "
        "instrumentation=critical_hooks_included baseline=empty_critical "
        "metric=observed_irq_latency not_wcet=1 core_hz=%lu\r\n",
        (unsigned int)CH_PROFILE_REPETITIONS, (unsigned long)SystemCoreClock);
}

/**
 * @brief Observe first outer critical windows and zero/four-waiter broadcast costs.
 * @param argument [in] Unused audit task argument; controller priority must be 9.
 */
void audit_regressions_task(void *argument)
{
    (void)argument;
    static const uint32_t sizes[] = {16U, 256U, 1024U, 4096U, 16384U};

    rtos_perf_init_cycles();
    ch_expect("profile_dwt_enabled", (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
    ch_expect("profile_controller_unmasked", rtos_port_crit_nest == 0U
        && __get_BASEPRI() == 0U && __get_PRIMASK() == 0U);
    rtos_tcb_t *controller = rtos_task_get_current();
    uint32_t priority_valid = controller != NULL && controller->priority == 9U;
    ch_expect("profile_controller_priority", priority_valid);
    if (priority_valid == 0U) {
        ch_finish("profile");
    }
    uint32_t claimed = claim_irq();
    ch_expect("profile_unused_irq_claimed", claimed);
    if (claimed == 0U) {
        ch_finish("profile");
    }
    for (uint32_t repetition = 0U; repetition < CH_PROFILE_REPETITIONS; repetition++) {
        (void)measure_operation(CH_PROFILE_CRITICAL, 0U, 0U, repetition, "baseline");
    }
    for (uint32_t index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        profile_ipc_size(sizes[index]);
    }
    for (uint32_t repetition = 0U; repetition < CH_PROFILE_REPETITIONS; repetition++) {
        profile_broadcast(0U, 0U, repetition);
        profile_broadcast(0U, CH_PROFILE_WAITERS, repetition);
        profile_broadcast(1U, 0U, repetition);
        profile_broadcast(1U, CH_PROFILE_WAITERS, repetition);
    }
    restore_irq();
    ch_finish("profile");
}

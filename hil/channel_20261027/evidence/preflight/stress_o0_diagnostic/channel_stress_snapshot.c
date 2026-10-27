/**
 * @file channel_stress.c
 * @brief Four producers, four consumers and a real periodic interrupt share loans.
 *
 * Producers fill channel storage directly; consumers verify that same address.
 * The per-source bitmap detects duplicate, missing and corrupt publications.
 * All task and IRQ accounting uses kernel critical sections. No UART runs in IRQ.
 */
#include "audit_regressions.h"
#include "channel_fixture.h"

#define CH_TASK_SOURCES (4U)
#define CH_SOURCES (5U)
#define CH_CONSUMERS (4U)
#define CH_MESSAGES (1024U)
#define CH_IRQ_MESSAGES (512U)
#define CH_BLOCKS (8U)
#define CH_BYTES (256U)
#define CH_STACK_WORDS (256U)
#define CH_EXPECTED (CH_TASK_SOURCES * CH_MESSAGES + CH_IRQ_MESSAGES)

#ifndef CHANNEL_STRESS_INJECT_CORRUPTION
#define CHANNEL_STRESS_INJECT_CORRUPTION (0)
#endif

typedef struct ch_timer_snapshot {
    uint32_t cr1;
    uint32_t cr2;
    uint32_t dier;
    uint32_t psc;
    uint32_t arr;
    uint32_t cnt;
    uint32_t clock_enabled;
    uint32_t priority;
    uint32_t enabled;
    uint32_t pending;
} ch_timer_snapshot_t;

typedef enum ch_error_stage {
    CH_ERROR_NONE = 0,
    CH_ERROR_ACQUIRE,
    CH_ERROR_PUBLISH,
    CH_ERROR_CANCEL,
    CH_ERROR_RECEIVE,
    CH_ERROR_RELEASE,
    CH_ERROR_NOTIFY,
    CH_ERROR_PARK
} ch_error_stage_t;

typedef struct ch_task_diagnostic {
    uint32_t errors;
    ch_error_stage_t first_stage;
    rtos_status_t first_status;
    rtos_tick_t first_elapsed;
    ch_error_stage_t last_stage;
    rtos_status_t last_status;
    rtos_tick_t last_elapsed;
    rtos_status_t last_wait_status;
    rtos_tick_t last_wait_elapsed;
    rtos_tick_t max_wait_elapsed;
    uint32_t timeouts;
    uint32_t closed;
} ch_task_diagnostic_t;

#define CH_IRQ_DIAGNOSTIC (CH_TASK_SOURCES + CH_CONSUMERS)

static rtos_channel_t channel;
static uint8_t storage[CH_BLOCKS * CH_BYTES] __attribute__((aligned(8)));
static rtos_channel_slot_t slots[CH_BLOCKS];
static rtos_tcb_t workers[CH_TASK_SOURCES + CH_CONSUMERS];
static rtos_stack_t stacks[CH_TASK_SOURCES + CH_CONSUMERS][CH_STACK_WORDS]
    __attribute__((aligned(8)));
static rtos_sem_t producer_done;
static rtos_sem_t consumer_done;
static uint8_t seen[CH_SOURCES][CH_MESSAGES];
static uint32_t produced[CH_SOURCES];
static uint32_t consumed[CH_SOURCES];
static uint32_t per_consumer[CH_CONSUMERS];
static uint32_t errors;
static uint32_t checksum_errors;
static uint32_t injections;
static uint32_t irq_retries;
static uint32_t timer_irqs;
static uint32_t active_workers;
static ch_timer_snapshot_t saved_timer;
static ch_task_diagnostic_t diagnostics[CH_IRQ_DIAGNOSTIC + 1U];

static void record_error(uint32_t worker, ch_error_stage_t stage, rtos_status_t status,
    rtos_tick_t elapsed)
{
    RTOS_PORT_ENTER_CRITICAL();
    errors++;
    ch_task_diagnostic_t *diagnostic = &diagnostics[worker];
    if (diagnostic->errors == 0U) {
        diagnostic->first_stage = stage;
        diagnostic->first_status = status;
        diagnostic->first_elapsed = elapsed;
    }
    diagnostic->errors++;
    diagnostic->last_stage = stage;
    diagnostic->last_status = status;
    diagnostic->last_elapsed = elapsed;
    RTOS_PORT_EXIT_CRITICAL();
}

/* Each row has exactly one task writer. The controller reads only after every
 * task has joined and been deleted; no runtime reader observes partial fields. */
static void record_wait(uint32_t worker, rtos_status_t status, rtos_tick_t elapsed)
{
    ch_task_diagnostic_t *diagnostic = &diagnostics[worker];

    diagnostic->last_wait_status = status;
    diagnostic->last_wait_elapsed = elapsed;
    if (elapsed > diagnostic->max_wait_elapsed) {
        diagnostic->max_wait_elapsed = elapsed;
    }
    diagnostic->timeouts += status == RTOS_ERR_TIMEOUT;
    diagnostic->closed += status == RTOS_ERR_CLOSED;
}

static uint32_t message_seed(uint32_t source, uint32_t sequence)
{
    return source * 65537U + sequence;
}

static void park_worker(uint32_t worker)
{
    rtos_status_t status = rtos_task_suspend(NULL);
    if (status != RTOS_OK) {
        record_error(worker, CH_ERROR_PARK, status, 0U);
    }
    for (;;) {
        rtos_task_delay(1000U);
    }
}

static void producer_task(void *argument)
{
    uint32_t source = (uint32_t)(uintptr_t)argument;

    for (uint32_t sequence = 0U; sequence < CH_MESSAGES; sequence++) {
        rtos_channel_writer_t writer = {0};
        rtos_tick_t start = rtos_sched_get_tick_count();
        rtos_status_t status = rtos_channel_acquire(&channel, &writer, 1000U);
        rtos_tick_t elapsed = rtos_sched_get_tick_count() - start;
        record_wait(source, status, elapsed);
        if (status != RTOS_OK) {
            record_error(source, CH_ERROR_ACQUIRE, status, elapsed);
            break;
        }
        ch_fill(writer.data, CH_BYTES, message_seed(source, sequence));
#if CHANNEL_STRESS_INJECT_CORRUPTION
        if (source == 0U && sequence == 7U) {
            /* Deliberately damage only caller-owned WRITING payload. The negative
             * oracle must notice it and still release every delivered loan. */
            ((uint8_t *)writer.data)[17U] ^= 1U;
            RTOS_PORT_ENTER_CRITICAL();
            injections++;
            RTOS_PORT_EXIT_CRITICAL();
        }
#endif
        status = rtos_channel_publish(&channel, &writer, CH_BYTES,
            (source << 16U) | sequence);
        if (status != RTOS_OK) {
            record_error(source, CH_ERROR_PUBLISH, status, 0U);
            status = rtos_channel_cancel(&channel, &writer);
            if (status != RTOS_OK) {
                record_error(source, CH_ERROR_CANCEL, status, 0U);
            }
            break;
        }
        RTOS_PORT_ENTER_CRITICAL();
        produced[source]++;
        RTOS_PORT_EXIT_CRITICAL();
        if ((sequence & 15U) == 15U) {
            rtos_task_delay(1U);
        }
    }
    rtos_status_t status = rtos_sem_give(&producer_done);
    if (status != RTOS_OK) {
        record_error(source, CH_ERROR_NOTIFY, status, 0U);
    }
    park_worker(source);
}

static void validate_reader(const rtos_channel_reader_t *reader, uint32_t consumer)
{
    uint32_t source = reader->tag >> 16U;
    uint32_t sequence = reader->tag & 0xFFFFU;
    uint32_t valid = source < CH_SOURCES && sequence < CH_MESSAGES
        && (source != CH_TASK_SOURCES || sequence < CH_IRQ_MESSAGES)
        && reader->length == CH_BYTES && reader->ticket.slot < CH_BLOCKS
        && (reader->ticket.generation & 1U) == 0U;
    uint32_t checksum_bad = 0U;

    if (valid != 0U) {
        valid = reader->data == &storage[reader->ticket.slot * CH_BYTES];
        if (valid != 0U) {
            checksum_bad = ch_verify(reader->data, CH_BYTES, message_seed(source, sequence))
                == 0U;
            valid = checksum_bad == 0U;
        }
    }
    RTOS_PORT_ENTER_CRITICAL();
    checksum_errors += checksum_bad;
    if (valid != 0U && seen[source][sequence] == 0U) {
        seen[source][sequence] = 1U;
        consumed[source]++;
        per_consumer[consumer]++;
    } else {
        errors++;
    }
    RTOS_PORT_EXIT_CRITICAL();
}

static void consumer_task(void *argument)
{
    uint32_t consumer = (uint32_t)(uintptr_t)argument;
    uint32_t worker = CH_TASK_SOURCES + consumer;
    uint32_t delivered = 0U;

    for (;;) {
        rtos_channel_reader_t reader = {0};
        rtos_tick_t start = rtos_sched_get_tick_count();
        rtos_status_t status = rtos_channel_receive(&channel, &reader, 1000U);
        rtos_tick_t elapsed = rtos_sched_get_tick_count() - start;
        record_wait(worker, status, elapsed);
        if (status == RTOS_ERR_CLOSED) {
            break;
        }
        if (status != RTOS_OK) {
            record_error(worker, CH_ERROR_RECEIVE, status, elapsed);
            break;
        }
        validate_reader(&reader, consumer);
        status = rtos_channel_release(&channel, &reader);
        if (status != RTOS_OK) {
            record_error(worker, CH_ERROR_RELEASE, status, 0U);
            break;
        }
        delivered++;
        /* A strictly higher-priority consumer otherwise can consume every item.
         * Open a real scheduling window for all four business consumers. */
        if ((delivered & 15U) == 0U) {
            rtos_task_delay(1U);
        }
    }
    rtos_status_t status = rtos_sem_give(&consumer_done);
    if (status != RTOS_OK) {
        record_error(worker, CH_ERROR_NOTIFY, status, 0U);
    }
    park_worker(worker);
}

/* Fixed CMSIS vector ABI requires this exact symbol spelling. */
void TIM7_IRQHandler(void)
{
    if ((TIM7->SR & TIM_SR_UIF) == 0U) {
        return;
    }
    TIM7->SR = 0U;
    timer_irqs++;
    if (produced[CH_TASK_SOURCES] < CH_IRQ_MESSAGES) {
        uint32_t sequence = produced[CH_TASK_SOURCES];
        rtos_channel_writer_t writer = {0};
        rtos_status_t status = rtos_channel_acquire(&channel, &writer, 0U);
        if (status == RTOS_ERR_TIMEOUT) {
            irq_retries++;
        } else if (status != RTOS_OK) {
            record_error(CH_IRQ_DIAGNOSTIC, CH_ERROR_ACQUIRE, status, 0U);
        } else {
            ch_fill(writer.data, CH_BYTES, message_seed(CH_TASK_SOURCES, sequence));
            status = rtos_channel_publish(&channel, &writer, CH_BYTES,
                (CH_TASK_SOURCES << 16U) | sequence);
            if (status == RTOS_OK) {
                produced[CH_TASK_SOURCES]++;
            } else {
                record_error(CH_IRQ_DIAGNOSTIC, CH_ERROR_PUBLISH, status, 0U);
                status = rtos_channel_cancel(&channel, &writer);
                if (status != RTOS_OK) {
                    record_error(CH_IRQ_DIAGNOSTIC, CH_ERROR_CANCEL, status, 0U);
                }
            }
        }
    }
    if (produced[CH_TASK_SOURCES] == CH_IRQ_MESSAGES) {
        TIM7->CR1 &= ~TIM_CR1_CEN;
        TIM7->DIER = 0U;
    }
}

static uint32_t start_timer(void)
{
    saved_timer.clock_enabled = RCC->APB1ENR & RCC_APB1ENR_TIM7EN;
    saved_timer.priority = NVIC_GetPriority(TIM7_IRQn);
    saved_timer.enabled = NVIC_GetEnableIRQ(TIM7_IRQn);
    saved_timer.pending = NVIC_GetPendingIRQ(TIM7_IRQn);
    RCC->APB1ENR |= RCC_APB1ENR_TIM7EN;
    __DSB();
    saved_timer.cr1 = TIM7->CR1;
    saved_timer.cr2 = TIM7->CR2;
    saved_timer.dier = TIM7->DIER;
    saved_timer.psc = TIM7->PSC;
    saved_timer.arr = TIM7->ARR;
    saved_timer.cnt = TIM7->CNT;
    if (saved_timer.enabled != 0U || saved_timer.pending != 0U
        || (saved_timer.cr1 & TIM_CR1_CEN) != 0U || saved_timer.dier != 0U
        || TIM7->SR != 0U) {
        RCC->APB1ENR = (RCC->APB1ENR & ~RCC_APB1ENR_TIM7EN)
            | saved_timer.clock_enabled;
        return 0U;
    }
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U) {
        timer_clock *= 2U;
    }
    TIM7->CR1 = 0U;
    TIM7->CR2 = 0U;
    TIM7->PSC = timer_clock / 1000000U - 1U;
    TIM7->ARR = 999U;
    TIM7->CNT = 0U;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0U;
    TIM7->DIER = TIM_DIER_UIE;
    NVIC_SetPriority(TIM7_IRQn, 5U);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    NVIC_EnableIRQ(TIM7_IRQn);
    TIM7->CR1 = TIM_CR1_CEN;
    return 1U;
}

static void stop_timer(void)
{
    NVIC_DisableIRQ(TIM7_IRQn);
    TIM7->CR1 = 0U;
    TIM7->DIER = 0U;
    TIM7->SR = 0U;
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    TIM7->PSC = saved_timer.psc;
    TIM7->ARR = saved_timer.arr;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0U;
    TIM7->CNT = saved_timer.cnt;
    TIM7->CR2 = saved_timer.cr2;
    TIM7->DIER = saved_timer.dier;
    TIM7->CR1 = saved_timer.cr1;
    NVIC_SetPriority(TIM7_IRQn, saved_timer.priority);
    RCC->APB1ENR = (RCC->APB1ENR & ~RCC_APB1ENR_TIM7EN) | saved_timer.clock_enabled;
    ch_expect("timer_irq_restored", NVIC_GetPriority(TIM7_IRQn) == saved_timer.priority
        && NVIC_GetEnableIRQ(TIM7_IRQn) == saved_timer.enabled
        && NVIC_GetPendingIRQ(TIM7_IRQn) == saved_timer.pending);
}

static uint32_t create_workers(void)
{
    for (uint32_t index = 0U; index < CH_TASK_SOURCES + CH_CONSUMERS; index++) {
        uint32_t producer = index < CH_TASK_SOURCES;
        uint32_t number = producer != 0U ? index : index - CH_TASK_SOURCES;
        rtos_prio_t priority = (rtos_prio_t)(10U + number * 2U + (producer == 0U));
        rtos_status_t status = rtos_task_create_static(&workers[index], stacks[index],
            CH_STACK_WORDS, producer != 0U ? producer_task : consumer_task,
            (void *)(uintptr_t)number, priority, producer != 0U ? "ch_tx" : "ch_rx");
        ch_expect("stress_worker_created", status == RTOS_OK);
        if (status != RTOS_OK) {
            return 0U;
        }
        active_workers |= 1UL << index;
    }
    return 1U;
}

static const char *error_stage_name(ch_error_stage_t stage)
{
    switch (stage) {
        case CH_ERROR_NONE:
            return "none";
        case CH_ERROR_ACQUIRE:
            return "acquire";
        case CH_ERROR_PUBLISH:
            return "publish";
        case CH_ERROR_CANCEL:
            return "cancel";
        case CH_ERROR_RECEIVE:
            return "receive";
        case CH_ERROR_RELEASE:
            return "release";
        case CH_ERROR_NOTIFY:
            return "notify";
        case CH_ERROR_PARK:
            return "park";
        default:
            return "unknown";
    }
}

static void summarize_diagnostics(void)
{
    for (uint32_t index = 0U; index <= CH_IRQ_DIAGNOSTIC; index++) {
        const ch_task_diagnostic_t *diagnostic = &diagnostics[index];
        const char *role = index < CH_TASK_SOURCES ? "producer"
            : (index < CH_IRQ_DIAGNOSTIC ? "consumer" : "irq");
        uint32_t number = index < CH_TASK_SOURCES ? index
            : (index < CH_IRQ_DIAGNOSTIC ? index - CH_TASK_SOURCES : 0U);
        (void)printf("CH|TASK|role=%s index=%lu errors=%lu "
            "first_stage=%s first_status=%ld first_elapsed=%lu "
            "last_stage=%s last_status=%ld last_elapsed=%lu "
            "last_wait_status=%ld last_wait_elapsed=%lu max_wait_elapsed=%lu "
            "timeouts=%lu closed=%lu\r\n", role, (unsigned long)number,
            (unsigned long)diagnostic->errors, error_stage_name(diagnostic->first_stage),
            (long)diagnostic->first_status, (unsigned long)diagnostic->first_elapsed,
            error_stage_name(diagnostic->last_stage), (long)diagnostic->last_status,
            (unsigned long)diagnostic->last_elapsed, (long)diagnostic->last_wait_status,
            (unsigned long)diagnostic->last_wait_elapsed,
            (unsigned long)diagnostic->max_wait_elapsed, (unsigned long)diagnostic->timeouts,
            (unsigned long)diagnostic->closed);
    }
}

static void summarize_stress(void)
{
    uint32_t total_produced = 0U;
    uint32_t total_consumed = 0U;
    uint32_t unique = 0U;
    rtos_channel_stats_t stats = {0};

    for (uint32_t source = 0U; source < CH_SOURCES; source++) {
        uint32_t expected = source < CH_TASK_SOURCES ? CH_MESSAGES : CH_IRQ_MESSAGES;
        ch_expect("stress_source_exact_once", produced[source] == expected
            && consumed[source] == expected);
        total_produced += produced[source];
        total_consumed += consumed[source];
        for (uint32_t sequence = 0U; sequence < expected; sequence++) {
            unique += seen[source][sequence] == 1U;
        }
    }
    for (uint32_t consumer = 0U; consumer < CH_CONSUMERS; consumer++) {
        ch_expect("stress_each_consumer_delivered", per_consumer[consumer] != 0U);
        (void)printf("CH|CONSUMER|index=%lu consumed=%lu\r\n",
            (unsigned long)consumer, (unsigned long)per_consumer[consumer]);
    }
    ch_expect("stress_no_worker_or_irq_error", errors == 0U);
    ch_expect("stress_unique_message_bitmap", unique == CH_EXPECTED);
    ch_expect("stress_ledger_read", rtos_channel_get_stats(&channel, &stats) == RTOS_OK);
    ch_expect("stress_every_block_returned", stats.free == CH_BLOCKS
        && stats.writing == 0U && stats.queued == 0U && stats.reading == 0U
        && ch_conserved(&stats));
    ch_expect("stress_transfer_counters", stats.acquired == CH_EXPECTED
        && stats.published == CH_EXPECTED && stats.received == CH_EXPECTED
        && stats.released == CH_EXPECTED && stats.cancelled == 0U);
    if (injections != 0U) {
        (void)printf("CH|INJECT|boundary=payload_checksum source=0 sequence=7 "
            "byte=17 count=%lu\r\n", (unsigned long)injections);
    }
    (void)printf("CH|STRESS|produced=%lu consumed=%lu unique=%lu errors=%lu "
        "checksum_errors=%lu "
        "irq_accepted=%lu irq_retries=%lu interrupts=%lu returned=%lu capacity=%u\r\n",
        (unsigned long)total_produced, (unsigned long)total_consumed, (unsigned long)unique,
        (unsigned long)errors, (unsigned long)checksum_errors,
        (unsigned long)produced[CH_TASK_SOURCES],
        (unsigned long)irq_retries, (unsigned long)timer_irqs, (unsigned long)stats.free,
        (unsigned int)CH_BLOCKS);
}

/** @brief Describe the separate stress firmware before the scheduler starts. */
void audit_pre_start(void)
{
    (void)printf("CH|META|mode=stress producers=4 consumers=4 irq=TIM7 irq_priority=5 "
        "task_priorities=10..17 messages=%u bytes=%u blocks=%u corruption=%u "
        "diagnostics=1 consumer_wait=1000\r\n",
        (unsigned int)CH_EXPECTED, (unsigned int)CH_BYTES, (unsigned int)CH_BLOCKS,
        (unsigned int)CHANNEL_STRESS_INJECT_CORRUPTION);
}

/** @brief Run bounded real-scheduler stress, reclaim workers and close the channel. */
void audit_regressions_task(void *argument)
{
    (void)argument;
    rtos_channel_config_t config = {storage, sizeof(storage), slots, sizeof(slots),
        CH_BYTES, CH_BLOCKS};
    rtos_status_t status = rtos_channel_init(&channel, &config);
    ch_expect("stress_channel_init", status == RTOS_OK);
    if (status != RTOS_OK) {
        ch_finish("stress");
    }
    ch_expect("stress_producer_sem", rtos_sem_init(&producer_done, 0U, CH_TASK_SOURCES)
        == RTOS_OK);
    ch_expect("stress_consumer_sem", rtos_sem_init(&consumer_done, 0U, CH_CONSUMERS)
        == RTOS_OK);
    uint32_t ready = create_workers();
    uint32_t timer_started = ready != 0U ? start_timer() : 0U;
    ch_expect("stress_timer_started", timer_started != 0U);
    if (ready != 0U && timer_started != 0U) {
        for (uint32_t index = 0U; index < CH_TASK_SOURCES; index++) {
            ch_expect("stress_producer_completed", rtos_sem_take(&producer_done, 5000U)
                == RTOS_OK);
        }
        rtos_tick_t start = rtos_sched_get_tick_count();
        uint32_t complete = 0U;
        while (rtos_sched_get_tick_count() - start < 5000U) {
            RTOS_PORT_ENTER_CRITICAL();
            complete = produced[CH_TASK_SOURCES] == CH_IRQ_MESSAGES;
            RTOS_PORT_EXIT_CRITICAL();
            if (complete != 0U) {
                break;
            }
            rtos_task_delay(1U);
        }
        ch_expect("stress_real_irq_quota_completed", complete != 0U);
    }
    if (timer_started != 0U) {
        stop_timer();
    }
    ch_expect("stress_close", rtos_channel_close(&channel) == RTOS_OK);
    if (ready != 0U) {
        for (uint32_t index = 0U; index < CH_CONSUMERS; index++) {
            ch_expect("stress_consumer_drained", rtos_sem_take(&consumer_done, 2000U)
                == RTOS_OK);
        }
    }
    for (uint32_t index = 0U; index < CH_TASK_SOURCES + CH_CONSUMERS; index++) {
        if ((active_workers & (1UL << index)) != 0U) {
            ch_expect("stress_worker_deleted", rtos_task_delete(&workers[index]) == RTOS_OK);
        }
    }
    summarize_diagnostics();
    summarize_stress();
    ch_expect("stress_sem_producer_deinit", rtos_sem_deinit(&producer_done) == RTOS_OK);
    ch_expect("stress_sem_consumer_deinit", rtos_sem_deinit(&consumer_done) == RTOS_OK);
    ch_expect("stress_channel_deinit", rtos_channel_deinit(&channel) == RTOS_OK);
    ch_finish("stress");
}

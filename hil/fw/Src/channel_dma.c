/**
 * @file channel_dma.c
 * @brief Real DMA2 memory-to-memory transfers exercise channel loan ownership.
 *
 * DMA writes directly into an acquired channel block. Only the transfer-complete
 * IRQ publishes it. Closing a channel neither stops DMA nor reclaims the writer;
 * the completion driver cancels that loan after a closed publish is rejected.
 * F446 has no data cache; this fixture makes no M7 cache-coherency claim.
 */
#include "audit_regressions.h"
#include "channel_fixture.h"

#define CH_DMA_BYTES (16384U)
#define CH_DMA_WORDS (CH_DMA_BYTES / 4U)
#define CH_DMA_FLAGS (DMA_LISR_FEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_TEIF0 \
    | DMA_LISR_HTIF0 | DMA_LISR_TCIF0)
#define CH_DMA_ERRORS (DMA_LISR_FEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_TEIF0)

typedef struct ch_dma_snapshot {
    uint32_t cr;
    uint32_t ndtr;
    uint32_t par;
    uint32_t m0ar;
    uint32_t m1ar;
    uint32_t fcr;
    uint32_t clock_enabled;
    uint32_t priority;
    uint32_t enabled;
    uint32_t pending;
} ch_dma_snapshot_t;

typedef struct ch_dma_result {
    uint32_t flags;
    uint32_t remaining;
    uint32_t enabled;
    uint32_t completed;
    uint32_t errors;
    rtos_status_t publish;
    rtos_status_t cancel;
    rtos_status_t notify;
} ch_dma_result_t;

static rtos_channel_t channel;
static uint8_t storage[CH_DMA_BYTES] __attribute__((aligned(8)));
static uint8_t source[CH_DMA_BYTES] __attribute__((aligned(8)));
static rtos_channel_slot_t slots[1];
static rtos_channel_writer_t dma_writer;
static rtos_sem_t dma_done;
static ch_dma_snapshot_t saved_dma;
static ch_dma_result_t dma_result;
static uint32_t closing_transfer;

static uint32_t stop_dma_stream(void)
{
    DMA2_Stream0->CR &= ~DMA_SxCR_EN;
    for (uint32_t attempt = 0U; attempt < 100000U; attempt++) {
        if ((DMA2_Stream0->CR & DMA_SxCR_EN) == 0U) {
            __DSB();
            return 1U;
        }
    }
    return 0U;
}

/* Fixed CMSIS vector ABI requires this exact symbol spelling. */
void DMA2_Stream0_IRQHandler(void)
{
    uint32_t flags = DMA2->LISR & CH_DMA_FLAGS;

    if (flags == 0U) {
        return;
    }
    DMA2->LIFCR = flags;
    dma_result.flags = flags;
    dma_result.remaining = DMA2_Stream0->NDTR;
    dma_result.enabled = DMA2_Stream0->CR & DMA_SxCR_EN;
    dma_result.completed++;
    if ((flags & CH_DMA_ERRORS) != 0U || (flags & DMA_LISR_TCIF0) == 0U
        || dma_result.remaining != 0U || dma_result.enabled != 0U) {
        dma_result.errors++;
        if (stop_dma_stream() != 0U) {
            dma_result.cancel = rtos_channel_cancel(&channel, &dma_writer);
        }
    } else {
        /* Ownership transfers only after real TC, NDTR=0 and EN=0 evidence. */
        __DMB();
        dma_result.publish = rtos_channel_publish(&channel, &dma_writer,
            CH_DMA_BYTES, 0xD00U + closing_transfer);
        if (dma_result.publish != RTOS_OK) {
            dma_result.cancel = rtos_channel_cancel(&channel, &dma_writer);
        }
    }
    dma_result.notify = rtos_sem_give(&dma_done);
}

static uint32_t claim_dma(void)
{
    saved_dma.clock_enabled = RCC->AHB1ENR & RCC_AHB1ENR_DMA2EN;
    saved_dma.priority = NVIC_GetPriority(DMA2_Stream0_IRQn);
    saved_dma.enabled = NVIC_GetEnableIRQ(DMA2_Stream0_IRQn);
    saved_dma.pending = NVIC_GetPendingIRQ(DMA2_Stream0_IRQn);
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    __DSB();
    saved_dma.cr = DMA2_Stream0->CR;
    saved_dma.ndtr = DMA2_Stream0->NDTR;
    saved_dma.par = DMA2_Stream0->PAR;
    saved_dma.m0ar = DMA2_Stream0->M0AR;
    saved_dma.m1ar = DMA2_Stream0->M1AR;
    saved_dma.fcr = DMA2_Stream0->FCR;
    if ((saved_dma.cr & DMA_SxCR_EN) != 0U || saved_dma.enabled != 0U
        || saved_dma.pending != 0U || (DMA2->LISR & CH_DMA_FLAGS) != 0U) {
        RCC->AHB1ENR = (RCC->AHB1ENR & ~RCC_AHB1ENR_DMA2EN)
            | saved_dma.clock_enabled;
        return 0U;
    }
    NVIC_SetPriority(DMA2_Stream0_IRQn, 5U);
    NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
    NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    return 1U;
}

static void restore_dma(void)
{
    NVIC_DisableIRQ(DMA2_Stream0_IRQn);
    uint32_t stopped = stop_dma_stream();
    ch_expect("dma_stream_stopped", stopped != 0U);
    if (stopped == 0U) {
        return;
    }
    DMA2->LIFCR = CH_DMA_FLAGS;
    NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
    DMA2_Stream0->CR = saved_dma.cr;
    DMA2_Stream0->NDTR = saved_dma.ndtr;
    DMA2_Stream0->PAR = saved_dma.par;
    DMA2_Stream0->M0AR = saved_dma.m0ar;
    DMA2_Stream0->M1AR = saved_dma.m1ar;
    DMA2_Stream0->FCR = saved_dma.fcr;
    NVIC_SetPriority(DMA2_Stream0_IRQn, saved_dma.priority);
    ch_expect("dma_registration_restored", DMA2_Stream0->CR == saved_dma.cr
        && DMA2_Stream0->NDTR == saved_dma.ndtr && DMA2_Stream0->PAR == saved_dma.par
        && DMA2_Stream0->M0AR == saved_dma.m0ar && DMA2_Stream0->M1AR == saved_dma.m1ar
        && DMA2_Stream0->FCR == saved_dma.fcr
        && NVIC_GetPriority(DMA2_Stream0_IRQn) == saved_dma.priority
        && NVIC_GetEnableIRQ(DMA2_Stream0_IRQn) == saved_dma.enabled
        && NVIC_GetPendingIRQ(DMA2_Stream0_IRQn) == saved_dma.pending);
    RCC->AHB1ENR = (RCC->AHB1ENR & ~RCC_AHB1ENR_DMA2EN) | saved_dma.clock_enabled;
    ch_expect("dma_clock_restored", (RCC->AHB1ENR & RCC_AHB1ENR_DMA2EN)
        == saved_dma.clock_enabled);
}

static uint32_t prepare_transfer(uint32_t closing)
{
    rtos_channel_config_t config = {storage, sizeof(storage), slots, sizeof(slots),
        CH_DMA_BYTES, 1U};

    closing_transfer = closing;
    memset(&dma_result, 0, sizeof(dma_result));
    dma_result.publish = RTOS_ERR_SCHED;
    dma_result.cancel = RTOS_ERR_SCHED;
    dma_result.notify = RTOS_ERR_SCHED;
    rtos_status_t status = rtos_channel_init(&channel, &config);
    ch_expect("dma_channel_init", status == RTOS_OK);
    if (status != RTOS_OK) {
        return 0U;
    }
    status = rtos_channel_acquire(&channel, &dma_writer, 0U);
    ch_expect("dma_writer_acquired", status == RTOS_OK);
    if (status != RTOS_OK) {
        ch_expect("dma_failed_setup_close", rtos_channel_close(&channel) == RTOS_OK);
        ch_expect("dma_failed_setup_deinit", rtos_channel_deinit(&channel) == RTOS_OK);
        return 0U;
    }
    ch_expect("dma_destination_is_loan", dma_writer.data == storage);
    ch_fill(source, CH_DMA_BYTES, 123U + closing);
    memset(dma_writer.data, 0xA5, CH_DMA_BYTES);
    DMA2->LIFCR = CH_DMA_FLAGS;
    DMA2_Stream0->CR = DMA_SxCR_DIR_1 | DMA_SxCR_PINC | DMA_SxCR_MINC
        | DMA_SxCR_PSIZE_1 | DMA_SxCR_MSIZE_1 | DMA_SxCR_TCIE | DMA_SxCR_TEIE
        | DMA_SxCR_DMEIE;
    DMA2_Stream0->FCR = DMA_SxFCR_DMDIS | DMA_SxFCR_FTH | DMA_SxFCR_FEIE;
    DMA2_Stream0->PAR = (uint32_t)(uintptr_t)source;
    DMA2_Stream0->M0AR = (uint32_t)(uintptr_t)dma_writer.data;
    DMA2_Stream0->NDTR = CH_DMA_WORDS;
    return 1U;
}

static uint32_t wait_for_transfer(void)
{
    rtos_status_t status = rtos_sem_take(&dma_done, 1000U);

    ch_expect("dma_real_irq_received", status == RTOS_OK);
    if (status != RTOS_OK) {
        NVIC_DisableIRQ(DMA2_Stream0_IRQn);
        uint32_t stopped = stop_dma_stream();
        ch_expect("dma_timeout_driver_stopped", stopped != 0U);
        if (stopped != 0U && dma_writer.data != NULL) {
            ch_expect("dma_timeout_cancel_after_stop", rtos_channel_cancel(&channel,
                &dma_writer) == RTOS_OK);
        }
        DMA2->LIFCR = CH_DMA_FLAGS;
        NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
        NVIC_EnableIRQ(DMA2_Stream0_IRQn);
        return 0U;
    }
    ch_expect("dma_one_real_tc_no_error", dma_result.completed == 1U
        && dma_result.errors == 0U && (dma_result.flags & DMA_LISR_TCIF0) != 0U
        && dma_result.remaining == 0U && dma_result.enabled == 0U
        && dma_result.notify == RTOS_OK);
    return 1U;
}

static void finish_channel(uint32_t already_closed)
{
    rtos_channel_stats_t stats = {0};

    ch_expect("dma_ledger_read", rtos_channel_get_stats(&channel, &stats) == RTOS_OK);
    ch_expect("dma_all_storage_returned", stats.free == 1U && stats.writing == 0U
        && stats.queued == 0U && stats.reading == 0U && ch_conserved(&stats));
    if (already_closed == 0U) {
        ch_expect("dma_channel_close", rtos_channel_close(&channel) == RTOS_OK);
    }
    ch_expect("dma_channel_deinit", rtos_channel_deinit(&channel) == RTOS_OK);
}

static void test_normal_transfer(void)
{
    if (prepare_transfer(0U) == 0U) {
        return;
    }
    __DMB();
    DMA2_Stream0->CR |= DMA_SxCR_EN;
    if (wait_for_transfer() != 0U) {
        rtos_channel_reader_t reader = {0};
        ch_expect("dma_tc_published", dma_result.publish == RTOS_OK);
        rtos_status_t status = rtos_channel_receive(&channel, &reader, 0U);
        ch_expect("dma_receive", status == RTOS_OK);
        if (status == RTOS_OK) {
            ch_expect("dma_reader_exact_destination", reader.data == storage
                && reader.data == (void *)(uintptr_t)DMA2_Stream0->M0AR);
            ch_expect("dma_payload_verified_in_place", reader.length == CH_DMA_BYTES
                && reader.tag == 0xD00U && ch_verify(reader.data, CH_DMA_BYTES, 123U));
            ch_expect("dma_reader_released", rtos_channel_release(&channel, &reader)
                == RTOS_OK);
        }
    }
    (void)printf("CH|DMA|case=normal bytes=%u tc=%lu remaining=%lu errors=%lu "
        "publish=%ld source=%08lx destination=%08lx\r\n", (unsigned int)CH_DMA_BYTES,
        (unsigned long)((dma_result.flags & DMA_LISR_TCIF0) != 0U),
        (unsigned long)dma_result.remaining, (unsigned long)dma_result.errors,
        (long)dma_result.publish, (unsigned long)(uintptr_t)source,
        (unsigned long)(uintptr_t)storage);
    finish_channel(0U);
}

static void test_close_in_flight(void)
{
    if (prepare_transfer(1U) == 0U) {
        return;
    }
    rtos_channel_stats_t held = {0};
    RTOS_PORT_ENTER_CRITICAL();
    __DMB();
    DMA2_Stream0->CR |= DMA_SxCR_EN;
    uint32_t before = DMA2_Stream0->NDTR;
    uint32_t tc_before = DMA2->LISR & DMA_LISR_TCIF0;
    rtos_status_t closed = rtos_channel_close(&channel);
    uint32_t after = DMA2_Stream0->NDTR;
    uint32_t tc_after = DMA2->LISR & DMA_LISR_TCIF0;
    rtos_status_t inspected = rtos_channel_get_stats(&channel, &held);
    rtos_status_t deinit = rtos_channel_deinit(&channel);
    RTOS_PORT_EXIT_CRITICAL();
    ch_expect("dma_physically_running_across_close", before != 0U && after != 0U
        && tc_before == 0U && tc_after == 0U);
    ch_expect("dma_close_succeeded", closed == RTOS_OK);
    ch_expect("dma_close_did_not_reclaim_loan", inspected == RTOS_OK && held.writing == 1U
        && held.free == 0U && held.queued == 0U && held.reading == 0U);
    ch_expect("dma_deinit_refused_live_loan", deinit == RTOS_ERR_BUSY);
    if (wait_for_transfer() != 0U) {
        ch_expect("dma_closed_completion_refused_publish", dma_result.publish
            == RTOS_ERR_CLOSED);
        ch_expect("dma_cancel_only_after_tc", dma_result.cancel == RTOS_OK
            && dma_result.remaining == 0U && dma_result.enabled == 0U);
        ch_expect("dma_closed_transfer_payload_intact", ch_verify(storage,
            CH_DMA_BYTES, 124U));
        rtos_channel_reader_t reader = {0};
        ch_expect("dma_closed_no_message_leaked", rtos_channel_receive(&channel,
            &reader, 0U) == RTOS_ERR_CLOSED);
    }
    (void)printf("CH|DMA|case=close_in_flight bytes=%u before=%lu after=%lu "
        "tc_before=%lu tc_after=%lu tc=%lu remaining=%lu publish=%ld cancel=%ld\r\n",
        (unsigned int)CH_DMA_BYTES, (unsigned long)before, (unsigned long)after,
        (unsigned long)(tc_before != 0U), (unsigned long)(tc_after != 0U),
        (unsigned long)((dma_result.flags & DMA_LISR_TCIF0) != 0U),
        (unsigned long)dma_result.remaining, (long)dma_result.publish,
        (long)dma_result.cancel);
    finish_channel(1U);
}

/** @brief Describe the real DMA fixture; no simulated completion is used. */
void audit_pre_start(void)
{
    (void)printf("CH|META|mode=dma peripheral=DMA2_Stream0 irq_priority=5 bytes=%u "
        "source=memory destination=loan cache=none\r\n", (unsigned int)CH_DMA_BYTES);
}

/** @brief Run DMA completion and close-while-active ownership checks. */
void audit_regressions_task(void *argument)
{
    (void)argument;
    uint32_t claimed = claim_dma();
    ch_expect("dma_idle_stream_claimed", claimed != 0U);
    if (claimed == 0U) {
        ch_finish("dma");
    }
    rtos_status_t status = rtos_sem_init(&dma_done, 0U, 1U);
    ch_expect("dma_completion_sem_init", status == RTOS_OK);
    if (status == RTOS_OK) {
        test_normal_transfer();
        test_close_in_flight();
        ch_expect("dma_completion_sem_deinit", rtos_sem_deinit(&dma_done) == RTOS_OK);
    }
    restore_dma();
    ch_finish("dma");
}

/** @file h743_dma_cache.c
 *  @brief Verify the H743 AXI SRAM DMA ownership boundary with real DMA2 interrupts.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32h743xx.h"

#include "rtos.h"

enum {
    H743_DMA_LINE_SIZE = 32U,
    H743_DMA_GUARD_WORDS = H743_DMA_LINE_SIZE / sizeof(uint32_t),
    H743_DMA_WORDS = 256U,
    H743_DMA_ROUNDS = 32U,
    H743_DMA_WORKER_PRIORITY = 10U,
    H743_DMA_WORKER_STACK_WORDS = 192U,
    H743_DMA_TIMEOUT_TICKS = 20U,
    H743_DMA_DISABLE_POLLS = 10000U
};

#define H743_DMA_AXI_START UINT32_C(0x24000000)
#define H743_DMA_AXI_END UINT32_C(0x24080000)
#define H743_DMA_ERRORS (DMA_LISR_TEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_FEIF0)
#define H743_DMA_TERMINAL_FLAGS (DMA_LISR_TCIF0 | H743_DMA_ERRORS)
#define H743_DMA_CLEAR_FLAGS (DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 | \
                              DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0)

typedef struct h743_dma_buffers {
    uint32_t source[H743_DMA_WORDS];
    uint32_t guard_before[H743_DMA_GUARD_WORDS];
    uint32_t destination[H743_DMA_WORDS];
    uint32_t guard_after[H743_DMA_GUARD_WORDS];
} h743_dma_buffers_t;

_Static_assert(offsetof(h743_dma_buffers_t, destination) % H743_DMA_LINE_SIZE == 0U,
               "DMA destination must start on a private line");
_Static_assert(sizeof(((h743_dma_buffers_t *)0)->destination) % H743_DMA_LINE_SIZE == 0U,
               "DMA destination must end on a private line boundary");

static _Alignas(H743_DMA_LINE_SIZE) h743_dma_buffers_t s_buffers
    __attribute__((section(".h743_axi_bench.dma")));
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t s_worker_stack[H743_DMA_WORKER_STACK_WORDS];
static rtos_tcb_t s_worker;
static rtos_tcb_t *s_coordinator;
static rtos_sem_t s_start;
static rtos_sem_t s_done;
static _Atomic uint32_t s_irq_count;
static _Atomic uint32_t s_irq_flags;
static _Atomic rtos_status_t s_irq_post_status;
static _Atomic bool s_was_coordinator_blocked;
static _Atomic bool s_is_launch_failed;
static uint32_t s_checks;
static uint32_t s_failures;

void h743_dma_cache_run(void *argument);
void DMA2_Stream0_IRQHandler(void);

static void record_check(const char *name, bool passed)
{
    ++s_checks;
    if (!passed) {
        ++s_failures;
    }
    (void)printf("H7DMA|CHECK|%s|%s\r\n", name, passed ? "PASS" : "FAIL");
}

void DMA2_Stream0_IRQHandler(void)
{
    const uint32_t flags = DMA2->LISR & H743_DMA_TERMINAL_FLAGS;
    DMA2->LIFCR = H743_DMA_CLEAR_FLAGS;
    __DSB();
    if (flags != 0U) {
        s_irq_flags = flags;
        ++s_irq_count;
        s_irq_post_status = rtos_sem_give(&s_done);
    }
}

static bool wait_disabled(void)
{
    for (uint32_t attempt = 0U; attempt < H743_DMA_DISABLE_POLLS; ++attempt) {
        if ((DMA2_Stream0->CR & DMA_SxCR_EN) == 0U) {
            return true;
        }
    }
    return false;
}

static void run_dma_worker(void *argument)
{
    (void)argument;
    for (;;) {
        if (rtos_sem_take(&s_start, RTOS_WAIT_FOREVER) != RTOS_OK) {
            rtos_task_delay(1U);
            continue;
        }
        RTOS_PORT_ENTER_CRITICAL();
        s_was_coordinator_blocked = (s_coordinator->blocked_on == &s_done) &&
            (s_coordinator->state == RTOS_TASK_DELAYED);
        RTOS_PORT_EXIT_CRITICAL();
        if (!wait_disabled()) {
            s_is_launch_failed = true;
            /* A bounded coordinator wait will record the launch failure. */
            continue;
        }

        /* RM0433 DMA memory-to-memory mode uses FIFO, PAR as the source, and
         * M0AR as the destination. DMA2 Stream0 maps to DMAMUX1 channel 8. */
        DMA2->LIFCR = H743_DMA_CLEAR_FLAGS;
        DMA2_Stream0->PAR = (uint32_t)(uintptr_t)s_buffers.source;
        DMA2_Stream0->M0AR = (uint32_t)(uintptr_t)s_buffers.destination;
        DMA2_Stream0->NDTR = H743_DMA_WORDS;
        DMA2_Stream0->FCR = DMA_SxFCR_DMDIS | DMA_SxFCR_FTH | DMA_SxFCR_FEIE;
        const uint32_t control = DMA_SxCR_DIR_1 | DMA_SxCR_PINC | DMA_SxCR_MINC |
            DMA_SxCR_PSIZE_1 | DMA_SxCR_MSIZE_1 | DMA_SxCR_TCIE | DMA_SxCR_TEIE;
        __DSB();
        DMA2_Stream0->CR = control | DMA_SxCR_EN;
    }
}

static uint32_t source_value(uint32_t round, uint32_t index)
{
    return UINT32_C(0xA53C0000) ^ (round << 16U) ^ (index * UINT32_C(0x9E3779B9));
}

static void prepare_buffers(uint32_t round, bool has_dcache)
{
    for (uint32_t index = 0U; index < H743_DMA_WORDS; ++index) {
        s_buffers.source[index] = source_value(round, index);
        s_buffers.destination[index] = ~s_buffers.source[index];
    }
    for (uint32_t index = 0U; index < H743_DMA_GUARD_WORDS; ++index) {
        s_buffers.guard_before[index] = UINT32_C(0x11223344) ^ round ^ index;
        s_buffers.guard_after[index] = UINT32_C(0x55667788) ^ round ^ index;
    }
    if (has_dcache) {
        SCB_CleanDCache_by_Addr(s_buffers.source, (int32_t)sizeof(s_buffers.source));
        /* DMA overwrites the entire private destination, including dirty lines. */
        SCB_InvalidateDCache_by_Addr(s_buffers.destination,
                                    (int32_t)sizeof(s_buffers.destination));
    }
    __DSB();
}

static bool buffers_match(uint32_t round)
{
    for (uint32_t index = 0U; index < H743_DMA_WORDS; ++index) {
        const uint32_t expected = source_value(round, index);
        if ((s_buffers.source[index] != expected) || (s_buffers.destination[index] != expected)) {
            return false;
        }
    }
    return true;
}

static bool guards_match(uint32_t round)
{
    for (uint32_t index = 0U; index < H743_DMA_GUARD_WORDS; ++index) {
        if ((s_buffers.guard_before[index] != (UINT32_C(0x11223344) ^ round ^ index)) ||
            (s_buffers.guard_after[index] != (UINT32_C(0x55667788) ^ round ^ index))) {
            return false;
        }
    }
    return true;
}

void h743_dma_cache_run(void *argument)
{
    (void)argument;
    const uintptr_t base = (uintptr_t)&s_buffers;
    const bool is_axi_private = (base >= H743_DMA_AXI_START) &&
        (base <= H743_DMA_AXI_END - sizeof(s_buffers)) && (base % H743_DMA_LINE_SIZE == 0U);
    const bool has_dcache = (SCB->CCR & SCB_CCR_DC_Msk) != 0U;
    record_check("aligned_private_axi_lines", is_axi_private);
    (void)printf("H7DMA|LAYOUT|dcache=%u|src=%08lX|dst=%08lX|bytes=%lu\r\n",
        (unsigned int)has_dcache, (unsigned long)(uintptr_t)s_buffers.source,
        (unsigned long)(uintptr_t)s_buffers.destination, (unsigned long)sizeof(s_buffers.source));

    const bool is_start_ready = is_axi_private && (rtos_sem_init(&s_start, 0U, 1U) == RTOS_OK);
    const bool is_done_ready = is_start_ready && (rtos_sem_init(&s_done, 0U, 1U) == RTOS_OK);
    s_coordinator = rtos_task_get_current();
    const bool is_worker_ready = is_done_ready && (s_coordinator != NULL) &&
        (rtos_task_create_static(&s_worker, s_worker_stack, H743_DMA_WORKER_STACK_WORDS,
                                 run_dma_worker, NULL, H743_DMA_WORKER_PRIORITY, "h7-dma") ==
         RTOS_OK);
    record_check("dma_fixture_setup", is_worker_ready);

    uint32_t completed = 0U;
    bool has_exact_payload = is_worker_ready;
    bool has_intact_guards = is_worker_ready;
    bool is_dma_stopped = true;
    if (is_worker_ready) {
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
        (void)RCC->AHB1ENR; /* Read back after enabling the peripheral clock. */
        DMAMUX1_Channel8->CCR = 0U; /* HAL DMA_REQUEST_MEM2MEM. */
        DMA2->LIFCR = H743_DMA_CLEAR_FLAGS;
        NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
        const uint32_t irq_priority = RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >>
            (8U - __NVIC_PRIO_BITS);
        NVIC_SetPriority(DMA2_Stream0_IRQn, irq_priority);
        NVIC_EnableIRQ(DMA2_Stream0_IRQn);

        for (uint32_t round = 0U; round < H743_DMA_ROUNDS; ++round) {
            prepare_buffers(round, has_dcache);
            if ((rtos_sem_give(&s_start) != RTOS_OK) ||
                (rtos_sem_take(&s_done, H743_DMA_TIMEOUT_TICKS) != RTOS_OK) ||
                s_is_launch_failed || !s_was_coordinator_blocked ||
                (s_irq_count != round + 1U) || (s_irq_flags != DMA_LISR_TCIF0) ||
                (s_irq_post_status != RTOS_OK)) {
                break;
            }
            if (has_dcache) {
                SCB_InvalidateDCache_by_Addr(s_buffers.destination,
                                            (int32_t)sizeof(s_buffers.destination));
            }
            __DSB();
            has_exact_payload = buffers_match(round) && has_exact_payload;
            has_intact_guards = guards_match(round) && has_intact_guards;
            ++completed;
        }

        NVIC_DisableIRQ(DMA2_Stream0_IRQn);
        DMA2_Stream0->CR &= ~DMA_SxCR_EN;
        is_dma_stopped = wait_disabled();
        DMA2->LIFCR = H743_DMA_CLEAR_FLAGS;
        __DSB();
        NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
    }
    record_check("real_dma_irq_releases_waiter", completed == H743_DMA_ROUNDS);
    record_check("fresh_payload_after_invalidate", has_exact_payload && (completed != 0U));
    record_check("neighbor_lines_preserved", has_intact_guards && (completed != 0U));
    const bool worker_deleted = !is_worker_ready || (rtos_task_delete(&s_worker) == RTOS_OK);
    const bool done_released = !is_done_ready || (rtos_sem_deinit(&s_done) == RTOS_OK);
    const bool start_released = !is_start_ready || (rtos_sem_deinit(&s_start) == RTOS_OK);
    record_check("dma_and_rtos_cleanup", is_dma_stopped && worker_deleted && done_released &&
                 start_released && (__get_BASEPRI() == 0U) && (__get_PRIMASK() == 0U));
    (void)printf("H7DMA|RESULT|rounds=%lu|irq=%lu|flags=%08lX|launch_failed=%u\r\n",
        (unsigned long)completed, (unsigned long)s_irq_count, (unsigned long)s_irq_flags,
        (unsigned int)s_is_launch_failed);
    (void)printf("H7DMA|DONE|pass=%lu|fail=%lu\r\n",
        (unsigned long)(s_checks - s_failures), (unsigned long)s_failures);
    for (;;) {
        rtos_task_delay(1000U);
    }
}

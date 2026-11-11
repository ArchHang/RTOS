/**
 * @file project_adc_capture.c
 * @brief F446 board health acquisition using TIM2, ADC1 and DMA2.
 */
#include "audit_regressions.h"
#include "main.h"
#include "rtos.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ADC_CAPTURE_BLOCKS      (24U)
#define ADC_CAPTURE_SAMPLES     (16U)
#define ADC_CAPTURE_POOL_SIZE   (4U)
#define ADC_CAPTURE_RATE_HZ     (1000U)
#define ADC_CAPTURE_WAIT_TICKS  (60U)
#define ADC_VREFINT_CHANNEL     (17U)
#define ADC_VREFINT_CODE_MIN    (300U)
#define ADC_VREFINT_CODE_MAX    (3000U)
#define ADC_DMA_FLAGS           (DMA_LISR_FEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_TEIF0 | \
                                 DMA_LISR_HTIF0 | DMA_LISR_TCIF0)
#define ADC_DMA_ERRORS          (DMA_LISR_FEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_TEIF0)

typedef struct adc_capture {
    uint32_t sequence;
    uint16_t samples[ADC_CAPTURE_SAMPLES];
} adc_capture_t;

typedef struct adc_capture_stats {
    uint32_t completed;
    uint32_t invalid;
    uint16_t minimum;
    uint16_t maximum;
} adc_capture_stats_t;

typedef struct adc_pool_alias {
    uint8_t prefix[32];
    rtos_mempool_t control;
    uint8_t suffix[64];
} adc_pool_alias_t;

static adc_pool_alias_t g_alias;
static struct adc_capture_pool {
    rtos_mempool_t control;
    adc_capture_t blocks[ADC_CAPTURE_POOL_SIZE];
} g_pool;
static adc_capture_t *g_queue_slots[ADC_CAPTURE_POOL_SIZE];
static rtos_queue_t g_completed_queue;
static rtos_tcb_t g_consumer;
static _Alignas(RTOS_CONFIG_STACK_ALIGNMENT) rtos_stack_t g_consumer_stack[192];
static rtos_tcb_t *g_capture_task;
static volatile uint32_t g_dma_irqs;
static volatile uint32_t g_dma_notify_errors;

void DMA2_Stream0_IRQHandler(void);

/* Only the board test owns these peripherals. Stop the trigger before
 * stopping conversion; never release a buffer until the stream is disabled. */
static void stop_trigger(void)
{
    TIM2->CR1 = 0U;
    ADC1->CR2 = 0U;
}

void DMA2_Stream0_IRQHandler(void)
{
    uint32_t flags = DMA2->LISR & ADC_DMA_FLAGS;

    DMA2->LIFCR = flags;
    if ((flags & (ADC_DMA_ERRORS | DMA_LISR_TCIF0)) != 0U) {
        stop_trigger();
        ++g_dma_irqs;
        __DMB();
        if (rtos_task_notify(g_capture_task, flags, RTOS_NOTIFY_VALUE) != RTOS_OK) {
            ++g_dma_notify_errors;
        }
    }
}

static bool init_capture_hardware(void)
{
    uint32_t apb_divider = RCC->CFGR & RCC_CFGR_PPRE1;
    uint32_t timer_hz = HAL_RCC_GetPCLK1Freq();

    /* The normal Cube configuration uses APB1 /4 and TIMPRE=0. */
    if ((RCC->DCKCFGR & RCC_DCKCFGR_TIMPRE) != 0U) {
        return false;
    }
    if (apb_divider >= RCC_CFGR_PPRE1_DIV2) {
        timer_hz *= 2U;
    }
    if ((timer_hz < ADC_CAPTURE_RATE_HZ) || ((timer_hz % ADC_CAPTURE_RATE_HZ) != 0U)) {
        return false;
    }

    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();
    ADC->CCR = ADC_CCR_TSVREFE | ADC_CCR_ADCPRE_0;
    ADC1->SMPR1 = ADC_SMPR1_SMP17;
    ADC1->SQR3 = ADC_VREFINT_CHANNEL;
    TIM2->PSC = 0U;
    TIM2->ARR = (timer_hz / ADC_CAPTURE_RATE_HZ) - 1U;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CR2 = TIM_CR2_MMS_1; /* Update is TRGO. */

    DMA2->LIFCR = ADC_DMA_FLAGS;
    NVIC_ClearPendingIRQ(DMA2_Stream0_IRQn);
    NVIC_SetPriority(DMA2_Stream0_IRQn,
                     RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY >> (8U - __NVIC_PRIO_BITS));
    NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    rtos_task_delay(2U);
    return true;
}

static bool capture_block(adc_capture_t *block, bool *dma_quiesced)
{
    uint32_t flags = 0U;
    uint32_t stop_start;
    rtos_status_t status;

    memset(block->samples, 0xff, sizeof(block->samples));
    DMA2->LIFCR = ADC_DMA_FLAGS;
    ADC1->SR = 0U;
    DMA2_Stream0->PAR = (uint32_t)(uintptr_t)&ADC1->DR;
    DMA2_Stream0->M0AR = (uint32_t)(uintptr_t)block->samples;
    DMA2_Stream0->NDTR = ADC_CAPTURE_SAMPLES;
    DMA2_Stream0->FCR = 0U;
    __DMB();
    DMA2_Stream0->CR = DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC |
                      DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE | DMA_SxCR_EN;
    /* ADC1 regular trigger EXTSEL 0b0110 = TIM2 TRGO, rising edge. */
    ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_DMA | ADC_CR2_DDS |
                ADC_CR2_EXTSEL_2 | ADC_CR2_EXTSEL_1 | ADC_CR2_EXTEN_0;
    TIM2->CNT = 0U;
    TIM2->CR1 = TIM_CR1_CEN;
    status = rtos_task_notify_wait(ADC_CAPTURE_WAIT_TICKS, &flags);

    stop_trigger();
    DMA2_Stream0->CR &= ~DMA_SxCR_EN;
    stop_start = rtos_sched_get_tick_count();
    while ((DMA2_Stream0->CR & DMA_SxCR_EN) != 0U) {
        if ((rtos_sched_get_tick_count() - stop_start) >= 2U) {
            *dma_quiesced = false;
            return false;
        }
    }
    __DMB();
    *dma_quiesced = true;
    return (status == RTOS_OK) && ((flags & DMA_LISR_TCIF0) != 0U) &&
           ((flags & ADC_DMA_ERRORS) == 0U) && (DMA2_Stream0->NDTR == 0U);
}

static void process_capture(void *argument)
{
    adc_capture_stats_t stats = {.minimum = UINT16_MAX};
    rtos_tcb_t *producer = argument;

    for (;;) {
        adc_capture_t *block;
        if ((rtos_queue_recv(&g_completed_queue, &block, RTOS_WAIT_FOREVER) != RTOS_OK) ||
            (block == NULL)) {
            break;
        }
        if (block->sequence != stats.completed) {
            ++stats.invalid;
        }
        ++stats.completed;
        for (uint32_t sample = 0U; sample < ADC_CAPTURE_SAMPLES; ++sample) {
            uint16_t code = block->samples[sample];
            if ((code < ADC_VREFINT_CODE_MIN) || (code > ADC_VREFINT_CODE_MAX)) {
                ++stats.invalid;
            }
            if (code < stats.minimum) {
                stats.minimum = code;
            }
            if (code > stats.maximum) {
                stats.maximum = code;
            }
        }
        if (rtos_mempool_free(&g_pool.control, block) != RTOS_OK) {
            ++stats.invalid;
        }
        if (stats.completed == ADC_CAPTURE_BLOCKS) {
            (void)printf("ADC|samples|blocks=%lu|samples=%lu|min=%u|max=%u|invalid=%lu\r\n",
                         (unsigned long)stats.completed,
                         (unsigned long)(stats.completed * ADC_CAPTURE_SAMPLES),
                         (unsigned int)stats.minimum, (unsigned int)stats.maximum,
                         (unsigned long)stats.invalid);
            (void)rtos_task_notify(producer, stats.invalid, RTOS_NOTIFY_VALUE);
            break;
        }
    }
    (void)rtos_task_delete(NULL);
}

static bool check_alias_rejection(void)
{
    uint8_t before[sizeof(g_alias)];
    uint8_t *storage = (uint8_t *)(void *)&g_alias;
    const size_t offset = offsetof(adc_pool_alias_t, control);
    const size_t offsets[] = {0U, offset, offset + sizeof(void *)};
    bool all_rejected = true;

    for (uint32_t index = 0U; index < (sizeof(offsets) / sizeof(offsets[0])); ++index) {
        rtos_status_t status;
        bool unchanged;

        memset(&g_alias, 0, sizeof(g_alias));
        memcpy(before, &g_alias, sizeof(before));
        status = rtos_mempool_init(&g_alias.control, storage + offsets[index], 16U, 3U);
        unchanged = memcmp(before, &g_alias, sizeof(before)) == 0;
        (void)printf("ADC|alias_case|offset=%lu|status=%d|unchanged=%u\r\n",
                     (unsigned long)offsets[index], (int)status, (unsigned int)unchanged);
        all_rejected = (status == RTOS_ERR_PARAM) && unchanged && all_rejected;
    }
    return all_rejected;
}

static bool run_capture_pipeline(void)
{
    bool passed = false;
    uint32_t invalid = UINT32_MAX;

    g_capture_task = rtos_task_get_current();
    if ((rtos_mempool_init(&g_pool.control, g_pool.blocks, sizeof(g_pool.blocks[0]),
                           ADC_CAPTURE_POOL_SIZE) != RTOS_OK) ||
        (rtos_queue_init(&g_completed_queue, g_queue_slots, sizeof(g_queue_slots[0]),
                         ADC_CAPTURE_POOL_SIZE) != RTOS_OK) ||
        (rtos_task_create_static(&g_consumer, g_consumer_stack,
                                 sizeof(g_consumer_stack) / sizeof(g_consumer_stack[0]),
                                 process_capture, g_capture_task, 8U, "adc_consumer") != RTOS_OK) ||
        !init_capture_hardware()) {
        return false;
    }

    for (uint32_t index = 0U; index < ADC_CAPTURE_BLOCKS; ++index) {
        adc_capture_t *block = rtos_mempool_alloc(&g_pool.control);
        bool dma_quiesced = true;

        if (block == NULL) {
            break;
        }
        block->sequence = index;
        if (!capture_block(block, &dma_quiesced) ||
            (rtos_queue_send(&g_completed_queue, &block, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK)) {
            if (dma_quiesced) {
                (void)rtos_mempool_free(&g_pool.control, block);
            }
            break;
        }
        if (index == (ADC_CAPTURE_BLOCKS - 1U)) {
            passed = (rtos_task_notify_wait(ADC_CAPTURE_WAIT_TICKS, &invalid) == RTOS_OK) &&
                     (invalid == 0U);
        }
    }
    NVIC_DisableIRQ(DMA2_Stream0_IRQn);
    DMA2->LIFCR = ADC_DMA_FLAGS;
    return passed && (g_dma_irqs == ADC_CAPTURE_BLOCKS) && (g_dma_notify_errors == 0U) &&
           (rtos_mempool_get_free(&g_pool.control) == ADC_CAPTURE_POOL_SIZE);
}

void audit_pre_start(void)
{
}

void audit_regressions_task(void *argument)
{
    bool alias_ok;
    bool pipeline_ok;
    uint32_t failures;

    (void)argument;
    alias_ok = check_alias_rejection();
    (void)printf("ADC|pool_alias|%s\r\n", alias_ok ? "PASS" : "FAIL");
    pipeline_ok = run_capture_pipeline();
    (void)printf("ADC|dma_pipeline|%s|irqs=%lu|notify_errors=%lu|pool_free=%lu\r\n",
                 pipeline_ok ? "PASS" : "FAIL", (unsigned long)g_dma_irqs,
                 (unsigned long)g_dma_notify_errors,
                 (unsigned long)rtos_mempool_get_free(&g_pool.control));
    failures = (alias_ok ? 0U : 1U) + (pipeline_ok ? 0U : 1U);
    (void)printf("ADC|DONE|pass=%lu|fail=%lu\r\n",
                 (unsigned long)(2U - failures), (unsigned long)failures);
    for (;;) {
        (void)rtos_task_delay(1000U);
    }
}

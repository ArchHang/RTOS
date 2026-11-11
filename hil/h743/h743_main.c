/** @file h743_main.c
 * @brief Isolated Cortex-M7 hardware-in-loop entry for the H743 board.
 */
#include "h743_board.h"

#include <stddef.h>
#include <stdio.h>

#include "stm32h7xx_hal.h"

#include "rtos.h"
#include "usart.h"

#define H743_DWT_UNLOCK (UINT32_C(0xC5ACCE55))
#define H743_DWT_LSR_PRESENT_LOCKED (UINT32_C(0x03))

enum {
    H743_UART_SPIN_LIMIT = 1000000U,
    H743_ENTRY_STACK_WORDS = 2048U,
    H743_CPU_CLOCK_HZ = 480000000U,
    H743_HCLK_HZ = 240000000U,
    H743_PCLK_HZ = 120000000U,
    H743_LOWEST_PRIORITY = 15U
};

typedef enum h743_stage {
    H743_STAGE_UART = 1U,
    H743_STAGE_STDIO,
    H743_STAGE_CACHE_DISABLE,
    H743_STAGE_MPU,
    H743_STAGE_ICACHE_ENABLE,
    H743_STAGE_DCACHE_ENABLE,
    H743_STAGE_NVIC,
    H743_STAGE_DWT,
    H743_STAGE_BOOT_REPORT,
    H743_STAGE_RTOS_INIT,
    H743_STAGE_SCHED_START,
    H743_STAGE_ENTRY
} h743_stage_t;

#define H743_EXPAND_LABEL(symbol) H743_STRINGIFY(symbol)
#define H743_STRINGIFY(symbol) #symbol

void SystemClock_Config(void);
void Error_Handler(void);
void SysTick_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
int _write(int file, char *data, int size);
void H743_TEST_ENTRY(void *argument);
#if defined(H743_TEST_BEFORE_START)
void H743_TEST_BEFORE_START(void);
#endif
extern const uint32_t g_pfnVectors[];

static rtos_tcb_t entry_tcb;
static _Alignas(32) rtos_stack_t entry_stack[H743_ENTRY_STACK_WORDS];
static bool is_uart_ready;
static volatile h743_stage_t boot_stage;

static void set_boot_stage(h743_stage_t stage)
{
    /* Resolve a buffered fault before attributing a later stage to it. */
    __DSB();
    boot_stage = stage;
}

static bool wait_uart_flag(uint32_t flag)
{
    for (uint32_t remaining = H743_UART_SPIN_LIMIT; remaining != 0U; --remaining) {
        if ((USART1->ISR & flag) != 0U) {
            return true;
        }
    }
    return false;
}

int _write(int file, char *data, int size)
{
    (void)file;
    if (!is_uart_ready || (size < 0)) {
        return -1;
    }

    int written = 0;
    for (; written < size; ++written) {
        if (!wait_uart_flag(USART_ISR_TXE_TXFNF)) {
            return written == 0 ? -1 : written;
        }
        USART1->TDR = (uint32_t)(uint8_t)data[written];
    }
    return written;
}

bool h743_uart_try_read(uint8_t *value)
{
    if ((value == NULL) || ((USART1->ISR & USART_ISR_RXNE_RXFNE) == 0U)) {
        return false;
    }
    *value = (uint8_t)USART1->RDR;
    return true;
}

static _Noreturn void stop_execution(void)
{
    __disable_irq();
    if (is_uart_ready) {
        /* Best effort after a fault. Do not wait for a HAL/SysTick timeout. */
        (void)wait_uart_flag(USART_ISR_TC);
    }
    for (;;) {
        __WFI();
    }
}

void Error_Handler(void)
{
    if (is_uart_ready) {
        (void)printf("H743|FAIL|bsp_initialization|stage=%u\r\n", (unsigned int)boot_stage);
    }
    stop_execution();
}

static _Noreturn void report_fault(const char *name)
{
    __disable_irq();
    if (is_uart_ready) {
        (void)printf("H743|FAULT|kind=%s|stage=%u|cfsr=%08lx|hfsr=%08lx|bfar=%08lx"
                     "|mmfar=%08lx|ccr=%08lx\r\n",
                     name, (unsigned int)boot_stage, (unsigned long)SCB->CFSR,
                     (unsigned long)SCB->HFSR,
                     (unsigned long)SCB->BFAR, (unsigned long)SCB->MMFAR,
                     (unsigned long)SCB->CCR);
    }
    stop_execution();
}

void HardFault_Handler(void)
{
    report_fault("hard");
}

void MemManage_Handler(void)
{
    report_fault("memory");
}

void BusFault_Handler(void)
{
    report_fault("bus");
}

void UsageFault_Handler(void)
{
    report_fault("usage");
}

void rtos_assert_fail(const char *condition, const char *file, int line)
{
    __disable_irq();
    (void)printf("H743|FAIL|assert=%s|file=%s|line=%d\r\n", condition, file, line);
    stop_execution();
}

void SysTick_Handler(void)
{
    rtos_port_sys_tick_handler();
    HAL_IncTick();
}

static void configure_cache(void)
{
    /* AXI SRAM is CPU-only in this harness. TEX=1/C=1/B=1 is Normal WBWA. */
    MPU_Region_InitTypeDef region = {
        .Enable = MPU_REGION_ENABLE,
        .Number = MPU_REGION_NUMBER0,
        .BaseAddress = 0x24000000U,
        .Size = MPU_REGION_SIZE_512KB,
        .SubRegionDisable = 0U,
        .TypeExtField = MPU_TEX_LEVEL1,
        .AccessPermission = MPU_REGION_FULL_ACCESS,
        .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE,
        .IsShareable = MPU_ACCESS_NOT_SHAREABLE,
        .IsCacheable = MPU_ACCESS_CACHEABLE,
        .IsBufferable = MPU_ACCESS_BUFFERABLE,
    };

    set_boot_stage(H743_STAGE_CACHE_DISABLE);
    if ((SCB->CCR & SCB_CCR_IC_Msk) != 0U) {
        SCB_DisableICache();
    }
    /* The bundled CMSIS disable helper cleans all ways even when already off.
     * At a cold reset, invalidate via EnableDCache before using cache tags. */
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
        SCB_DisableDCache();
    }
    set_boot_stage(H743_STAGE_MPU);
    HAL_MPU_Disable();
    HAL_MPU_ConfigRegion(&region);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

#if H743_TEST_ICACHE
    set_boot_stage(H743_STAGE_ICACHE_ENABLE);
    SCB_EnableICache();
#endif
#if H743_TEST_DCACHE
    set_boot_stage(H743_STAGE_DCACHE_ENABLE);
    SCB_EnableDCache();
#endif
}

static void report_boot(void)
{
    const uint32_t cpu_hz = SystemCoreClock;
    const uint32_t hclk_hz = HAL_RCC_GetHCLKFreq();
    const uint32_t pclk1_hz = HAL_RCC_GetPCLK1Freq();
    const uint32_t pclk2_hz = HAL_RCC_GetPCLK2Freq();
    const bool is_timpre = (RCC->CFGR & RCC_CFGR_TIMPRE) != 0U;

    (void)printf("H743|BOOT|cpu_hz=%lu|hclk_hz=%lu|pclk1_hz=%lu|pclk2_hz=%lu|timpre=%u"
                 "|baud=%lu|cpuid=%08lx|idcode=%08lx|flash_kib=%u\r\n",
                 (unsigned long)cpu_hz, (unsigned long)hclk_hz, (unsigned long)pclk1_hz,
                 (unsigned long)pclk2_hz, (unsigned int)is_timpre,
                 (unsigned long)H743_HIL_UART_BAUD, (unsigned long)SCB->CPUID,
                 (unsigned long)DBGMCU->IDCODE,
                 (unsigned int)(*(const volatile uint16_t *)FLASHSIZE_BASE));
    if ((cpu_hz != H743_CPU_CLOCK_HZ) || (hclk_hz != H743_HCLK_HZ) ||
        (pclk1_hz != H743_PCLK_HZ) || (pclk2_hz != H743_PCLK_HZ) || is_timpre) {
        Error_Handler();
    }

    MPU->RNR = MPU_REGION_NUMBER0;
    (void)printf("H743|LAYOUT|kernel=%08lx|task_stack=%08lx|msp=%08lx|ccr=%08lx|actlr=%08lx"
                 "|icache=%u|dcache=%u|mpu_rbar=%08lx|mpu_rasr=%08lx|grouping=%lu\r\n",
                 (unsigned long)(uintptr_t)&rtos_kernel,
                 (unsigned long)(uintptr_t)entry_stack, (unsigned long)__get_MSP(),
                 (unsigned long)SCB->CCR, (unsigned long)SCnSCB->ACTLR,
                 (unsigned int)((SCB->CCR & SCB_CCR_IC_Msk) != 0U),
                 (unsigned int)((SCB->CCR & SCB_CCR_DC_Msk) != 0U),
                 (unsigned long)MPU->RBAR, (unsigned long)MPU->RASR,
                 (unsigned long)NVIC_GetPriorityGrouping());
}

static void run_entry(void *argument)
{
    set_boot_stage(H743_STAGE_ENTRY);
    (void)printf("H743|READY|entry=%s|priority=%u|tick_hz=%lu|basepri=%u|assert=%u"
                 "|systick_load=%lu|vtor=%08lx\r\n",
                 H743_EXPAND_LABEL(H743_TEST_ENTRY), (unsigned int)H743_TEST_ENTRY_PRIORITY,
                 (unsigned long)RTOS_CONFIG_TICK_RATE_HZ,
                 (unsigned int)RTOS_CONFIG_USE_BASEPRI, (unsigned int)RTOS_CONFIG_ASSERT_ENABLED,
                 (unsigned long)SysTick->LOAD, (unsigned long)SCB->VTOR);
    H743_TEST_ENTRY(argument);
    for (;;) {
        rtos_task_delay(RTOS_WAIT_FOREVER);
    }
}

int main(void)
{
    SCB->VTOR = (uint32_t)(uintptr_t)g_pfnVectors;
    __DSB();
    __ISB();
    if (HAL_Init() != HAL_OK) {
        Error_Handler();
    }
    SystemClock_Config();
    set_boot_stage(H743_STAGE_UART);
    MX_USART1_UART_Init();
    NVIC_DisableIRQ(USART1_IRQn);
    is_uart_ready = true;
    set_boot_stage(H743_STAGE_STDIO);
    /* HIL output is line-oriented and must reach the host before a reset. */
    if (setvbuf(stdout, NULL, _IONBF, 0U) != 0) {
        Error_Handler();
    }

    configure_cache();
    set_boot_stage(H743_STAGE_NVIC);
    NVIC_SetPriorityGrouping(3U);
    NVIC_SetPriority(SVCall_IRQn, 0U);
    NVIC_SetPriority(PendSV_IRQn, H743_LOWEST_PRIORITY);
    NVIC_SetPriority(SysTick_IRQn, H743_LOWEST_PRIORITY);
    set_boot_stage(H743_STAGE_DWT);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    __DSB();
    if ((DWT->LSR & H743_DWT_LSR_PRESENT_LOCKED) == H743_DWT_LSR_PRESENT_LOCKED) {
        DWT->LAR = H743_DWT_UNLOCK;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    set_boot_stage(H743_STAGE_BOOT_REPORT);
    report_boot();

    set_boot_stage(H743_STAGE_RTOS_INIT);
    if (rtos_init() != RTOS_OK) {
        Error_Handler();
    }
#if defined(H743_TEST_BEFORE_START)
    H743_TEST_BEFORE_START();
#endif
    if (rtos_task_create_static(&entry_tcb, entry_stack, H743_ENTRY_STACK_WORDS, run_entry,
                                NULL, H743_TEST_ENTRY_PRIORITY, "h743-hil") != RTOS_OK) {
        Error_Handler();
    }
    set_boot_stage(H743_STAGE_SCHED_START);
    const rtos_status_t start_status = rtos_start();
    (void)printf("H743|FAIL|start=%ld\r\n", (long)start_status);
    stop_execution();
}

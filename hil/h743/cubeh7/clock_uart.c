/** @file clock_uart.c
 * @brief Board glue for the STM32H743 HIL with public STM32CubeH7 drivers.
 */
#include "usart.h"

#include "stm32h7xx_hal.h"

#include "h743_board.h"

void Error_Handler(void);
void SystemClock_Config(void);

enum { VOS_TIMEOUT_MS = 100U };

static void wait_for_voltage_scale(void)
{
    const uint32_t start = HAL_GetTick();
    while (__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY) == RESET) {
        if ((HAL_GetTick() - start) >= VOS_TIMEOUT_MS) {
            Error_Handler();
        }
    }
}

void SystemClock_Config(void)
{
    if (HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY) != HAL_OK) {
        Error_Handler();
    }
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    wait_for_voltage_scale();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
    wait_for_voltage_scale();

    /* A 25 MHz HSE divided by 5 gives the 5 MHz PLL1 input. */
    RCC_OscInitTypeDef oscillators = {
        .OscillatorType = RCC_OSCILLATORTYPE_HSE,
        .HSEState = RCC_HSE_ON,
        .PLL = {
            .PLLState = RCC_PLL_ON,
            .PLLSource = RCC_PLLSOURCE_HSE,
            .PLLM = 5U,
            .PLLN = 192U,
            .PLLP = 2U,
            .PLLQ = 2U,
            .PLLR = 2U,
            .PLLRGE = RCC_PLL1VCIRANGE_2,
            .PLLVCOSEL = RCC_PLL1VCOWIDE,
        },
    };
    if (HAL_RCC_OscConfig(&oscillators) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitTypeDef clocks = {
        .ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1
            | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_D3PCLK1,
        .SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK,
        .SYSCLKDivider = RCC_SYSCLK_DIV1,
        .AHBCLKDivider = RCC_HCLK_DIV2,
        .APB1CLKDivider = RCC_APB1_DIV2,
        .APB2CLKDivider = RCC_APB2_DIV2,
        .APB3CLKDivider = RCC_APB3_DIV2,
        .APB4CLKDivider = RCC_APB4_DIV2,
    };
    if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_4) != HAL_OK) {
        Error_Handler();
    }
}

void MX_USART1_UART_Init(void)
{
    RCC_PeriphCLKInitTypeDef clock = {
        .PeriphClockSelection = RCC_PERIPHCLK_USART1,
        .Usart16ClockSelection = RCC_USART16CLKSOURCE_D2PCLK2,
    };
    if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK) {
        Error_Handler();
    }
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitTypeDef pins = {
        .Pin = GPIO_PIN_9 | GPIO_PIN_10,
        .Mode = GPIO_MODE_AF_PP,
        .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_LOW,
        .Alternate = GPIO_AF7_USART1,
    };
    HAL_GPIO_Init(GPIOA, &pins);

    static UART_HandleTypeDef uart = {
        .Instance = USART1,
        .Init = {
            .BaudRate = H743_HIL_UART_BAUD,
            .WordLength = UART_WORDLENGTH_8B,
            .StopBits = UART_STOPBITS_1,
            .Parity = UART_PARITY_NONE,
            .Mode = UART_MODE_TX_RX,
            .HwFlowCtl = UART_HWCONTROL_NONE,
            .OverSampling = UART_OVERSAMPLING_16,
            .OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE,
            .ClockPrescaler = UART_PRESCALER_DIV1,
        },
    };
    if ((HAL_UART_Init(&uart) != HAL_OK) || (HAL_UARTEx_DisableFifoMode(&uart) != HAL_OK)) {
        Error_Handler();
    }
}

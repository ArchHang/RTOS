#ifndef RTOS_HIL_H743_CUBEH7_HAL_CONF_H_
#define RTOS_HIL_H743_CUBEH7_HAL_CONF_H_

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

#define HSE_VALUE (25000000UL)
#define HSE_STARTUP_TIMEOUT (100UL)
#define HSI_VALUE (64000000UL)
#define CSI_VALUE (4000000UL)
#define LSE_VALUE (32768UL)
#define LSE_STARTUP_TIMEOUT (5000UL)
#define LSI_VALUE (32000UL)
#define EXTERNAL_CLOCK_VALUE (12288000UL)
#define VDD_VALUE (3300UL)
#define TICK_INT_PRIORITY (15UL)
#define USE_RTOS (0U)
#define USE_HAL_UART_REGISTER_CALLBACKS (0U)
#define assert_param(condition) ((void)0)

#include "stm32h7xx_hal_cortex.h"
#include "stm32h7xx_hal_dma.h"
#include "stm32h7xx_hal_flash.h"
#include "stm32h7xx_hal_gpio.h"
#include "stm32h7xx_hal_pwr.h"
#include "stm32h7xx_hal_rcc.h"
#include "stm32h7xx_hal_uart.h"

#endif /* RTOS_HIL_H743_CUBEH7_HAL_CONF_H_ */

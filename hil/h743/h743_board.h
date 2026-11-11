#ifndef RTOS_HIL_H743_BOARD_H_
#define RTOS_HIL_H743_BOARD_H_

#include <stdbool.h>
#include <stdint.h>

#define H743_HIL_UART_BAUD (921600U)

/** @brief Read one USART1 byte if available, without waiting. */
bool h743_uart_try_read(uint8_t *value);

#endif /* RTOS_HIL_H743_BOARD_H_ */

#ifndef F446_RTOS_HIL_FW_INC_HIL_TARGET_H_
#define F446_RTOS_HIL_FW_INC_HIL_TARGET_H_

#if defined(STM32H743xx)
#include "stm32h743xx.h"
#elif defined(STM32F446xx)
#include "stm32f4xx.h"
#else
#error "The board fixture requires STM32F446xx or STM32H743xx"
#endif

#endif /* F446_RTOS_HIL_FW_INC_HIL_TARGET_H_ */

################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (11.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../RTOS/lib/third/CherryUSB/core/usbd_core.c 

OBJS += \
./RTOS/lib/third/CherryUSB/core/usbd_core.o 

C_DEPS += \
./RTOS/lib/third/CherryUSB/core/usbd_core.d 


# Each subdirectory must supply rules for building sources it contributes
RTOS/lib/third/CherryUSB/core/%.o RTOS/lib/third/CherryUSB/core/%.su RTOS/lib/third/CherryUSB/core/%.cyclo: ../RTOS/lib/third/CherryUSB/core/%.c RTOS/lib/third/CherryUSB/core/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I"E:/STM32_Project/CubeIDE/f446_rtos/AI/output/int8" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/core" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/common" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/cdc" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/port/dwc2" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Inc" -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/msc" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/hid" -O2 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-core

clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-core:
	-$(RM) ./RTOS/lib/third/CherryUSB/core/usbd_core.cyclo ./RTOS/lib/third/CherryUSB/core/usbd_core.d ./RTOS/lib/third/CherryUSB/core/usbd_core.o ./RTOS/lib/third/CherryUSB/core/usbd_core.su

.PHONY: clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-core


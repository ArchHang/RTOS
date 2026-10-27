################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../RTOS/Memory/rtos_heap.c \
../RTOS/Memory/rtos_mempool.c 

OBJS += \
./RTOS/Memory/rtos_heap.o \
./RTOS/Memory/rtos_mempool.o 

C_DEPS += \
./RTOS/Memory/rtos_heap.d \
./RTOS/Memory/rtos_mempool.d 


# Each subdirectory must supply rules for building sources it contributes
RTOS/Memory/%.o RTOS/Memory/%.su RTOS/Memory/%.cyclo: ../RTOS/Memory/%.c RTOS/Memory/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I../AI/output/int8 -I../RTOS/Core/Inc -I../RTOS/Core/Private -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I../RTOS/lib/third/CherryUSB/core -I../RTOS/lib/third/CherryUSB/common -I../RTOS/lib/third/CherryUSB/class/cdc -I../RTOS/lib/third/CherryUSB/class/hid -I../RTOS/lib/third/CherryUSB/class/msc -I../RTOS/lib/third/CherryUSB/port/dwc2 -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-Memory

clean-RTOS-2f-Memory:
	-$(RM) ./RTOS/Memory/rtos_heap.cyclo ./RTOS/Memory/rtos_heap.d ./RTOS/Memory/rtos_heap.o ./RTOS/Memory/rtos_heap.su ./RTOS/Memory/rtos_mempool.cyclo ./RTOS/Memory/rtos_mempool.d ./RTOS/Memory/rtos_mempool.o ./RTOS/Memory/rtos_mempool.su

.PHONY: clean-RTOS-2f-Memory


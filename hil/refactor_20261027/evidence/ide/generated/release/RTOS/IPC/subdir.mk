################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../RTOS/IPC/rtos_event.c \
../RTOS/IPC/rtos_mutex.c \
../RTOS/IPC/rtos_queue.c \
../RTOS/IPC/rtos_sem.c 

OBJS += \
./RTOS/IPC/rtos_event.o \
./RTOS/IPC/rtos_mutex.o \
./RTOS/IPC/rtos_queue.o \
./RTOS/IPC/rtos_sem.o 

C_DEPS += \
./RTOS/IPC/rtos_event.d \
./RTOS/IPC/rtos_mutex.d \
./RTOS/IPC/rtos_queue.d \
./RTOS/IPC/rtos_sem.d 


# Each subdirectory must supply rules for building sources it contributes
RTOS/IPC/%.o RTOS/IPC/%.su RTOS/IPC/%.cyclo: ../RTOS/IPC/%.c RTOS/IPC/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I../AI/output/int8 -I../RTOS/Core/Inc -I../RTOS/Core/Private -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I../RTOS/lib/third/CherryUSB/core -I../RTOS/lib/third/CherryUSB/common -I../RTOS/lib/third/CherryUSB/class/cdc -I../RTOS/lib/third/CherryUSB/class/hid -I../RTOS/lib/third/CherryUSB/class/msc -I../RTOS/lib/third/CherryUSB/port/dwc2 -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-IPC

clean-RTOS-2f-IPC:
	-$(RM) ./RTOS/IPC/rtos_event.cyclo ./RTOS/IPC/rtos_event.d ./RTOS/IPC/rtos_event.o ./RTOS/IPC/rtos_event.su ./RTOS/IPC/rtos_mutex.cyclo ./RTOS/IPC/rtos_mutex.d ./RTOS/IPC/rtos_mutex.o ./RTOS/IPC/rtos_mutex.su ./RTOS/IPC/rtos_queue.cyclo ./RTOS/IPC/rtos_queue.d ./RTOS/IPC/rtos_queue.o ./RTOS/IPC/rtos_queue.su ./RTOS/IPC/rtos_sem.cyclo ./RTOS/IPC/rtos_sem.d ./RTOS/IPC/rtos_sem.o ./RTOS/IPC/rtos_sem.su

.PHONY: clean-RTOS-2f-IPC


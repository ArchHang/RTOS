################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (11.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../RTOS/Core/Src/rtos_event.c \
../RTOS/Core/Src/rtos_heap.c \
../RTOS/Core/Src/rtos_mem.c \
../RTOS/Core/Src/rtos_mutex.c \
../RTOS/Core/Src/rtos_perf.c \
../RTOS/Core/Src/rtos_queue.c \
../RTOS/Core/Src/rtos_sched.c \
../RTOS/Core/Src/rtos_sem.c \
../RTOS/Core/Src/rtos_task.c \
../RTOS/Core/Src/rtos_timer.c \
../RTOS/Core/Src/rtos_usb.c \
../RTOS/Core/Src/rtos_usb_osal.c 

OBJS += \
./RTOS/Core/Src/rtos_event.o \
./RTOS/Core/Src/rtos_heap.o \
./RTOS/Core/Src/rtos_mem.o \
./RTOS/Core/Src/rtos_mutex.o \
./RTOS/Core/Src/rtos_perf.o \
./RTOS/Core/Src/rtos_queue.o \
./RTOS/Core/Src/rtos_sched.o \
./RTOS/Core/Src/rtos_sem.o \
./RTOS/Core/Src/rtos_task.o \
./RTOS/Core/Src/rtos_timer.o \
./RTOS/Core/Src/rtos_usb.o \
./RTOS/Core/Src/rtos_usb_osal.o 

C_DEPS += \
./RTOS/Core/Src/rtos_event.d \
./RTOS/Core/Src/rtos_heap.d \
./RTOS/Core/Src/rtos_mem.d \
./RTOS/Core/Src/rtos_mutex.d \
./RTOS/Core/Src/rtos_perf.d \
./RTOS/Core/Src/rtos_queue.d \
./RTOS/Core/Src/rtos_sched.d \
./RTOS/Core/Src/rtos_sem.d \
./RTOS/Core/Src/rtos_task.d \
./RTOS/Core/Src/rtos_timer.d \
./RTOS/Core/Src/rtos_usb.d \
./RTOS/Core/Src/rtos_usb_osal.d 


# Each subdirectory must supply rules for building sources it contributes
RTOS/Core/Src/%.o RTOS/Core/Src/%.su RTOS/Core/Src/%.cyclo: ../RTOS/Core/Src/%.c RTOS/Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I"E:/STM32_Project/CubeIDE/f446_rtos/AI/output/int8" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/core" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/common" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/cdc" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/port/dwc2" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Inc" -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/msc" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/hid" -O2 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-Core-2f-Src

clean-RTOS-2f-Core-2f-Src:
	-$(RM) ./RTOS/Core/Src/rtos_event.cyclo ./RTOS/Core/Src/rtos_event.d ./RTOS/Core/Src/rtos_event.o ./RTOS/Core/Src/rtos_event.su ./RTOS/Core/Src/rtos_heap.cyclo ./RTOS/Core/Src/rtos_heap.d ./RTOS/Core/Src/rtos_heap.o ./RTOS/Core/Src/rtos_heap.su ./RTOS/Core/Src/rtos_mem.cyclo ./RTOS/Core/Src/rtos_mem.d ./RTOS/Core/Src/rtos_mem.o ./RTOS/Core/Src/rtos_mem.su ./RTOS/Core/Src/rtos_mutex.cyclo ./RTOS/Core/Src/rtos_mutex.d ./RTOS/Core/Src/rtos_mutex.o ./RTOS/Core/Src/rtos_mutex.su ./RTOS/Core/Src/rtos_perf.cyclo ./RTOS/Core/Src/rtos_perf.d ./RTOS/Core/Src/rtos_perf.o ./RTOS/Core/Src/rtos_perf.su ./RTOS/Core/Src/rtos_queue.cyclo ./RTOS/Core/Src/rtos_queue.d ./RTOS/Core/Src/rtos_queue.o ./RTOS/Core/Src/rtos_queue.su ./RTOS/Core/Src/rtos_sched.cyclo ./RTOS/Core/Src/rtos_sched.d ./RTOS/Core/Src/rtos_sched.o ./RTOS/Core/Src/rtos_sched.su ./RTOS/Core/Src/rtos_sem.cyclo ./RTOS/Core/Src/rtos_sem.d ./RTOS/Core/Src/rtos_sem.o ./RTOS/Core/Src/rtos_sem.su ./RTOS/Core/Src/rtos_task.cyclo ./RTOS/Core/Src/rtos_task.d ./RTOS/Core/Src/rtos_task.o ./RTOS/Core/Src/rtos_task.su ./RTOS/Core/Src/rtos_timer.cyclo ./RTOS/Core/Src/rtos_timer.d ./RTOS/Core/Src/rtos_timer.o ./RTOS/Core/Src/rtos_timer.su ./RTOS/Core/Src/rtos_usb.cyclo ./RTOS/Core/Src/rtos_usb.d ./RTOS/Core/Src/rtos_usb.o ./RTOS/Core/Src/rtos_usb.su ./RTOS/Core/Src/rtos_usb_osal.cyclo ./RTOS/Core/Src/rtos_usb_osal.d ./RTOS/Core/Src/rtos_usb_osal.o ./RTOS/Core/Src/rtos_usb_osal.su

.PHONY: clean-RTOS-2f-Core-2f-Src


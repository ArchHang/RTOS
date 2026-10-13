################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (11.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../RTOS/Port/GCC/ARM_CM7/port.c 

OBJS += \
./RTOS/Port/GCC/ARM_CM7/port.o 

C_DEPS += \
./RTOS/Port/GCC/ARM_CM7/port.d 


# Each subdirectory must supply rules for building sources it contributes
RTOS/Port/GCC/ARM_CM7/%.o RTOS/Port/GCC/ARM_CM7/%.su RTOS/Port/GCC/ARM_CM7/%.cyclo: ../RTOS/Port/GCC/ARM_CM7/%.c RTOS/Port/GCC/ARM_CM7/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I"E:/STM32_Project/CubeIDE/f446_rtos/AI/output/int8" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/core" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/common" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/cdc" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/port/dwc2" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Inc" -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/msc" -I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/lib/third/CherryUSB/class/hid" -O2 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-Port-2f-GCC-2f-ARM_CM7

clean-RTOS-2f-Port-2f-GCC-2f-ARM_CM7:
	-$(RM) ./RTOS/Port/GCC/ARM_CM7/port.cyclo ./RTOS/Port/GCC/ARM_CM7/port.d ./RTOS/Port/GCC/ARM_CM7/port.o ./RTOS/Port/GCC/ARM_CM7/port.su

.PHONY: clean-RTOS-2f-Port-2f-GCC-2f-ARM_CM7


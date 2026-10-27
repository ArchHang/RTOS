################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../RTOS/Port/GCC/ARM_CM4F_CM7F/port.c 

OBJS += \
./RTOS/Port/GCC/ARM_CM4F_CM7F/port.o 

C_DEPS += \
./RTOS/Port/GCC/ARM_CM4F_CM7F/port.d 


# Each subdirectory must supply rules for building sources it contributes
RTOS/Port/GCC/ARM_CM4F_CM7F/%.o RTOS/Port/GCC/ARM_CM4F_CM7F/%.su RTOS/Port/GCC/ARM_CM4F_CM7F/%.cyclo: ../RTOS/Port/GCC/ARM_CM4F_CM7F/%.c RTOS/Port/GCC/ARM_CM4F_CM7F/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I../AI/output/int8 -I../RTOS/Core/Inc -I../RTOS/Core/Private -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I../RTOS/lib/third/CherryUSB/core -I../RTOS/lib/third/CherryUSB/common -I../RTOS/lib/third/CherryUSB/class/cdc -I../RTOS/lib/third/CherryUSB/class/hid -I../RTOS/lib/third/CherryUSB/class/msc -I../RTOS/lib/third/CherryUSB/port/dwc2 -O3 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-Port-2f-GCC-2f-ARM_CM4F_CM7F

clean-RTOS-2f-Port-2f-GCC-2f-ARM_CM4F_CM7F:
	-$(RM) ./RTOS/Port/GCC/ARM_CM4F_CM7F/port.cyclo ./RTOS/Port/GCC/ARM_CM4F_CM7F/port.d ./RTOS/Port/GCC/ARM_CM4F_CM7F/port.o ./RTOS/Port/GCC/ARM_CM4F_CM7F/port.su

.PHONY: clean-RTOS-2f-Port-2f-GCC-2f-ARM_CM4F_CM7F


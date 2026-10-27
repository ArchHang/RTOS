################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../RTOS/lib/third/CherryUSB/class/cdc/usbd_cdc_acm.c 

OBJS += \
./RTOS/lib/third/CherryUSB/class/cdc/usbd_cdc_acm.o 

C_DEPS += \
./RTOS/lib/third/CherryUSB/class/cdc/usbd_cdc_acm.d 


# Each subdirectory must supply rules for building sources it contributes
RTOS/lib/third/CherryUSB/class/cdc/%.o RTOS/lib/third/CherryUSB/class/cdc/%.su RTOS/lib/third/CherryUSB/class/cdc/%.cyclo: ../RTOS/lib/third/CherryUSB/class/cdc/%.c RTOS/lib/third/CherryUSB/class/cdc/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I../AI/output/int8 -I../RTOS/Core/Inc -I../RTOS/Core/Private -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I../RTOS/lib/third/CherryUSB/core -I../RTOS/lib/third/CherryUSB/common -I../RTOS/lib/third/CherryUSB/class/cdc -I../RTOS/lib/third/CherryUSB/class/hid -I../RTOS/lib/third/CherryUSB/class/msc -I../RTOS/lib/third/CherryUSB/port/dwc2 -O3 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-class-2f-cdc

clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-class-2f-cdc:
	-$(RM) ./RTOS/lib/third/CherryUSB/class/cdc/usbd_cdc_acm.cyclo ./RTOS/lib/third/CherryUSB/class/cdc/usbd_cdc_acm.d ./RTOS/lib/third/CherryUSB/class/cdc/usbd_cdc_acm.o ./RTOS/lib/third/CherryUSB/class/cdc/usbd_cdc_acm.su

.PHONY: clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-class-2f-cdc


################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../AI/output/int8/ai_app.c \
../AI/output/int8/ai_param.c \
../AI/output/int8/conv.c 

OBJS += \
./AI/output/int8/ai_app.o \
./AI/output/int8/ai_param.o \
./AI/output/int8/conv.o 

C_DEPS += \
./AI/output/int8/ai_app.d \
./AI/output/int8/ai_param.d \
./AI/output/int8/conv.d 


# Each subdirectory must supply rules for building sources it contributes
AI/output/int8/%.o AI/output/int8/%.su AI/output/int8/%.cyclo: ../AI/output/int8/%.c AI/output/int8/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I../AI/output/int8 -I../RTOS/Core/Inc -I../RTOS/Core/Private -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I../RTOS/lib/third/CherryUSB/core -I../RTOS/lib/third/CherryUSB/common -I../RTOS/lib/third/CherryUSB/class/cdc -I../RTOS/lib/third/CherryUSB/class/hid -I../RTOS/lib/third/CherryUSB/class/msc -I../RTOS/lib/third/CherryUSB/port/dwc2 -O3 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-AI-2f-output-2f-int8

clean-AI-2f-output-2f-int8:
	-$(RM) ./AI/output/int8/ai_app.cyclo ./AI/output/int8/ai_app.d ./AI/output/int8/ai_app.o ./AI/output/int8/ai_app.su ./AI/output/int8/ai_param.cyclo ./AI/output/int8/ai_param.d ./AI/output/int8/ai_param.o ./AI/output/int8/ai_param.su ./AI/output/int8/conv.cyclo ./AI/output/int8/conv.d ./AI/output/int8/conv.o ./AI/output/int8/conv.su

.PHONY: clean-AI-2f-output-2f-int8


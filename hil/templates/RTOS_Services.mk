# Canonical HIL build template; also usable after CubeIDE regenerates Debug.
# Build directory: project-root/Debug; GNU Tools for STM32 11.3.rel1.

C_SRCS += \
../RTOS/Services/rtos_perf.c \
../RTOS/Services/rtos_timer.c

OBJS += \
./RTOS/Services/rtos_perf.o \
./RTOS/Services/rtos_timer.o

C_DEPS += \
./RTOS/Services/rtos_perf.d \
./RTOS/Services/rtos_timer.d

RTOS/Services/%.o RTOS/Services/%.su RTOS/Services/%.cyclo: ../RTOS/Services/%.c RTOS/Services/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I"../Core/Inc" -I"../AI/output/int8" -I"../RTOS/Core/Inc" -I"../RTOS/Core/Private" -I"../Drivers/STM32F4xx_HAL_Driver/Inc" -I"../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy" -I"../Drivers/CMSIS/Device/ST/STM32F4xx/Include" -I"../Drivers/CMSIS/Include" -I"../RTOS/lib/third/CherryUSB/core" -I"../RTOS/lib/third/CherryUSB/common" -I"../RTOS/lib/third/CherryUSB/class/cdc" -I"../RTOS/lib/third/CherryUSB/class/hid" -I"../RTOS/lib/third/CherryUSB/class/msc" -I"../RTOS/lib/third/CherryUSB/port/dwc2" -O2 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-Services

clean-RTOS-2f-Services:
	-$(RM) ./RTOS/Services/rtos_perf.cyclo ./RTOS/Services/rtos_perf.d ./RTOS/Services/rtos_perf.o ./RTOS/Services/rtos_perf.su ./RTOS/Services/rtos_timer.cyclo ./RTOS/Services/rtos_timer.d ./RTOS/Services/rtos_timer.o ./RTOS/Services/rtos_timer.su

.PHONY: clean-RTOS-2f-Services

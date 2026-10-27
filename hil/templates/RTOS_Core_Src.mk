# Canonical HIL build template; also usable after CubeIDE regenerates Debug.
# Build directory: project-root/Debug; GNU Tools for STM32 11.3.rel1.

C_SRCS += \
../RTOS/Core/Src/rtos_kernel.c \
../RTOS/Core/Src/rtos_task.c \
../RTOS/Core/Src/rtos_sched.c \
../RTOS/Core/Src/rtos_wait.c

OBJS += \
./RTOS/Core/Src/rtos_kernel.o \
./RTOS/Core/Src/rtos_task.o \
./RTOS/Core/Src/rtos_sched.o \
./RTOS/Core/Src/rtos_wait.o

C_DEPS += \
./RTOS/Core/Src/rtos_kernel.d \
./RTOS/Core/Src/rtos_task.d \
./RTOS/Core/Src/rtos_sched.d \
./RTOS/Core/Src/rtos_wait.d

RTOS/Core/Src/%.o RTOS/Core/Src/%.su RTOS/Core/Src/%.cyclo: ../RTOS/Core/Src/%.c RTOS/Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I"../Core/Inc" -I"../AI/output/int8" -I"../RTOS/Core/Inc" -I"../RTOS/Core/Private" -I"../Drivers/STM32F4xx_HAL_Driver/Inc" -I"../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy" -I"../Drivers/CMSIS/Device/ST/STM32F4xx/Include" -I"../Drivers/CMSIS/Include" -I"../RTOS/lib/third/CherryUSB/core" -I"../RTOS/lib/third/CherryUSB/common" -I"../RTOS/lib/third/CherryUSB/class/cdc" -I"../RTOS/lib/third/CherryUSB/class/hid" -I"../RTOS/lib/third/CherryUSB/class/msc" -I"../RTOS/lib/third/CherryUSB/port/dwc2" -O2 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-Core-2f-Src

clean-RTOS-2f-Core-2f-Src:
	-$(RM) ./RTOS/Core/Src/rtos_kernel.cyclo ./RTOS/Core/Src/rtos_kernel.d ./RTOS/Core/Src/rtos_kernel.o ./RTOS/Core/Src/rtos_kernel.su ./RTOS/Core/Src/rtos_task.cyclo ./RTOS/Core/Src/rtos_task.d ./RTOS/Core/Src/rtos_task.o ./RTOS/Core/Src/rtos_task.su ./RTOS/Core/Src/rtos_sched.cyclo ./RTOS/Core/Src/rtos_sched.d ./RTOS/Core/Src/rtos_sched.o ./RTOS/Core/Src/rtos_sched.su ./RTOS/Core/Src/rtos_wait.cyclo ./RTOS/Core/Src/rtos_wait.d ./RTOS/Core/Src/rtos_wait.o ./RTOS/Core/Src/rtos_wait.su

.PHONY: clean-RTOS-2f-Core-2f-Src

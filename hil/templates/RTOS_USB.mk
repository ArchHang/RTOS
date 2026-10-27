# Canonical HIL build template; also usable after CubeIDE regenerates Debug.
# Build directory: project-root/Debug; GNU Tools for STM32 11.3.rel1.

C_SRCS += \
../RTOS/USB/rtos_usb_osal.c \
../RTOS/USB/rtos_usb_device.c \
../RTOS/USB/rtos_usb_msc.c

OBJS += \
./RTOS/USB/rtos_usb_osal.o \
./RTOS/USB/rtos_usb_device.o \
./RTOS/USB/rtos_usb_msc.o

C_DEPS += \
./RTOS/USB/rtos_usb_osal.d \
./RTOS/USB/rtos_usb_device.d \
./RTOS/USB/rtos_usb_msc.d

RTOS/USB/%.o RTOS/USB/%.su RTOS/USB/%.cyclo: ../RTOS/USB/%.c RTOS/USB/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I"../Core/Inc" -I"../AI/output/int8" -I"../RTOS/Core/Inc" -I"../RTOS/Core/Private" -I"../Drivers/STM32F4xx_HAL_Driver/Inc" -I"../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy" -I"../Drivers/CMSIS/Device/ST/STM32F4xx/Include" -I"../Drivers/CMSIS/Include" -I"../RTOS/lib/third/CherryUSB/core" -I"../RTOS/lib/third/CherryUSB/common" -I"../RTOS/lib/third/CherryUSB/class/cdc" -I"../RTOS/lib/third/CherryUSB/class/hid" -I"../RTOS/lib/third/CherryUSB/class/msc" -I"../RTOS/lib/third/CherryUSB/port/dwc2" -O2 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-USB

clean-RTOS-2f-USB:
	-$(RM) ./RTOS/USB/rtos_usb_osal.cyclo ./RTOS/USB/rtos_usb_osal.d ./RTOS/USB/rtos_usb_osal.o ./RTOS/USB/rtos_usb_osal.su ./RTOS/USB/rtos_usb_device.cyclo ./RTOS/USB/rtos_usb_device.d ./RTOS/USB/rtos_usb_device.o ./RTOS/USB/rtos_usb_device.su ./RTOS/USB/rtos_usb_msc.cyclo ./RTOS/USB/rtos_usb_msc.d ./RTOS/USB/rtos_usb_msc.o ./RTOS/USB/rtos_usb_msc.su

.PHONY: clean-RTOS-2f-USB

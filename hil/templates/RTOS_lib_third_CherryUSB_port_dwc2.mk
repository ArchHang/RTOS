# Canonical HIL build template; also usable after CubeIDE regenerates Debug.
# Build directory: project-root/Debug; GNU Tools for STM32 11.3.rel1.

C_SRCS += \
../RTOS/lib/third/CherryUSB/port/dwc2/usb_dc_dwc2.c \
../RTOS/lib/third/CherryUSB/port/dwc2/usb_glue_st.c

OBJS += \
./RTOS/lib/third/CherryUSB/port/dwc2/usb_dc_dwc2.o \
./RTOS/lib/third/CherryUSB/port/dwc2/usb_glue_st.o

C_DEPS += \
./RTOS/lib/third/CherryUSB/port/dwc2/usb_dc_dwc2.d \
./RTOS/lib/third/CherryUSB/port/dwc2/usb_glue_st.d

RTOS/lib/third/CherryUSB/port/dwc2/%.o RTOS/lib/third/CherryUSB/port/dwc2/%.su RTOS/lib/third/CherryUSB/port/dwc2/%.cyclo: ../RTOS/lib/third/CherryUSB/port/dwc2/%.c RTOS/lib/third/CherryUSB/port/dwc2/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx -c -I"../Core/Inc" -I"../AI/output/int8" -I"../RTOS/Core/Inc" -I"../RTOS/Core/Private" -I"../Drivers/STM32F4xx_HAL_Driver/Inc" -I"../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy" -I"../Drivers/CMSIS/Device/ST/STM32F4xx/Include" -I"../Drivers/CMSIS/Include" -I"../RTOS/lib/third/CherryUSB/core" -I"../RTOS/lib/third/CherryUSB/common" -I"../RTOS/lib/third/CherryUSB/class/cdc" -I"../RTOS/lib/third/CherryUSB/class/hid" -I"../RTOS/lib/third/CherryUSB/class/msc" -I"../RTOS/lib/third/CherryUSB/port/dwc2" -O2 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-port-2f-dwc2

clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-port-2f-dwc2:
	-$(RM) ./RTOS/lib/third/CherryUSB/port/dwc2/usb_dc_dwc2.cyclo ./RTOS/lib/third/CherryUSB/port/dwc2/usb_dc_dwc2.d ./RTOS/lib/third/CherryUSB/port/dwc2/usb_dc_dwc2.o ./RTOS/lib/third/CherryUSB/port/dwc2/usb_dc_dwc2.su ./RTOS/lib/third/CherryUSB/port/dwc2/usb_glue_st.cyclo ./RTOS/lib/third/CherryUSB/port/dwc2/usb_glue_st.d ./RTOS/lib/third/CherryUSB/port/dwc2/usb_glue_st.o ./RTOS/lib/third/CherryUSB/port/dwc2/usb_glue_st.su

.PHONY: clean-RTOS-2f-lib-2f-third-2f-CherryUSB-2f-port-2f-dwc2

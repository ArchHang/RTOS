# Canonical HIL build template; also usable after CubeIDE regenerates Debug.
# Build directory: project-root/Debug; GNU Tools for STM32 11.3.rel1.

S_SRCS += \
../Core/Startup/startup_stm32f446retx.s

OBJS += \
./Core/Startup/startup_stm32f446retx.o

S_DEPS += \
./Core/Startup/startup_stm32f446retx.d

Core/Startup/%.o: ../Core/Startup/%.s Core/Startup/subdir.mk
	arm-none-eabi-gcc -x assembler-with-cpp "$<" -mcpu=cortex-m4 -g3 -DDEBUG -c -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Startup

clean-Core-2f-Startup:
	-$(RM) ./Core/Startup/startup_stm32f446retx.d ./Core/Startup/startup_stm32f446retx.o

.PHONY: clean-Core-2f-Startup

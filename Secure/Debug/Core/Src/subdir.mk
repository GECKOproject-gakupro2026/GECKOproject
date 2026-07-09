################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../Core/Src/app_main.cpp \
../Core/Src/console.cpp \
../Core/Src/telemetry.cpp \
../Core/Src/test.cpp \
../Core/Src/tests_audio.cpp \
../Core/Src/tests_board.cpp \
../Core/Src/tests_memory.cpp \
../Core/Src/tests_security.cpp \
../Core/Src/tests_sensors.cpp \
../Core/Src/tests_wireless.cpp 

C_SRCS += \
../Core/Src/frame_codec.c \
../Core/Src/main.c \
../Core/Src/secure_nsc.c \
../Core/Src/stm32u5xx_hal_msp.c \
../Core/Src/stm32u5xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32u5xx_s.c 

C_DEPS += \
./Core/Src/frame_codec.d \
./Core/Src/main.d \
./Core/Src/secure_nsc.d \
./Core/Src/stm32u5xx_hal_msp.d \
./Core/Src/stm32u5xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32u5xx_s.d 

OBJS += \
./Core/Src/app_main.o \
./Core/Src/console.o \
./Core/Src/frame_codec.o \
./Core/Src/main.o \
./Core/Src/secure_nsc.o \
./Core/Src/stm32u5xx_hal_msp.o \
./Core/Src/stm32u5xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32u5xx_s.o \
./Core/Src/telemetry.o \
./Core/Src/test.o \
./Core/Src/tests_audio.o \
./Core/Src/tests_board.o \
./Core/Src/tests_memory.o \
./Core/Src/tests_security.o \
./Core/Src/tests_sensors.o \
./Core/Src/tests_wireless.o 

CPP_DEPS += \
./Core/Src/app_main.d \
./Core/Src/console.d \
./Core/Src/telemetry.d \
./Core/Src/test.d \
./Core/Src/tests_audio.d \
./Core/Src/tests_board.d \
./Core/Src/tests_memory.d \
./Core/Src/tests_security.d \
./Core/Src/tests_sensors.d \
./Core/Src/tests_wireless.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.cpp Core/Src/subdir.mk
	arm-none-eabi-g++ "$<" -mcpu=cortex-m33 -std=gnu++14 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/app_main.cyclo ./Core/Src/app_main.d ./Core/Src/app_main.o ./Core/Src/app_main.su ./Core/Src/console.cyclo ./Core/Src/console.d ./Core/Src/console.o ./Core/Src/console.su ./Core/Src/frame_codec.cyclo ./Core/Src/frame_codec.d ./Core/Src/frame_codec.o ./Core/Src/frame_codec.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/secure_nsc.cyclo ./Core/Src/secure_nsc.d ./Core/Src/secure_nsc.o ./Core/Src/secure_nsc.su ./Core/Src/stm32u5xx_hal_msp.cyclo ./Core/Src/stm32u5xx_hal_msp.d ./Core/Src/stm32u5xx_hal_msp.o ./Core/Src/stm32u5xx_hal_msp.su ./Core/Src/stm32u5xx_it.cyclo ./Core/Src/stm32u5xx_it.d ./Core/Src/stm32u5xx_it.o ./Core/Src/stm32u5xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32u5xx_s.cyclo ./Core/Src/system_stm32u5xx_s.d ./Core/Src/system_stm32u5xx_s.o ./Core/Src/system_stm32u5xx_s.su ./Core/Src/telemetry.cyclo ./Core/Src/telemetry.d ./Core/Src/telemetry.o ./Core/Src/telemetry.su ./Core/Src/test.cyclo ./Core/Src/test.d ./Core/Src/test.o ./Core/Src/test.su ./Core/Src/tests_audio.cyclo ./Core/Src/tests_audio.d ./Core/Src/tests_audio.o ./Core/Src/tests_audio.su ./Core/Src/tests_board.cyclo ./Core/Src/tests_board.d ./Core/Src/tests_board.o ./Core/Src/tests_board.su ./Core/Src/tests_memory.cyclo ./Core/Src/tests_memory.d ./Core/Src/tests_memory.o ./Core/Src/tests_memory.su ./Core/Src/tests_security.cyclo ./Core/Src/tests_security.d ./Core/Src/tests_security.o ./Core/Src/tests_security.su ./Core/Src/tests_sensors.cyclo ./Core/Src/tests_sensors.d ./Core/Src/tests_sensors.o ./Core/Src/tests_sensors.su ./Core/Src/tests_wireless.cyclo ./Core/Src/tests_wireless.d ./Core/Src/tests_wireless.o ./Core/Src/tests_wireless.su

.PHONY: clean-Core-2f-Src


################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/checksumutils.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_address.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.c 

C_DEPS += \
./Drivers/BSP/Components/mx_wifi/core/checksumutils.d \
./Drivers/BSP/Components/mx_wifi/core/mx_address.d \
./Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.d \
./Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.d \
./Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.d \
./Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.d 

OBJS += \
./Drivers/BSP/Components/mx_wifi/core/checksumutils.o \
./Drivers/BSP/Components/mx_wifi/core/mx_address.o \
./Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.o \
./Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.o \
./Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.o \
./Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.o 


# Each subdirectory must supply rules for building sources it contributes
Drivers/BSP/Components/mx_wifi/core/checksumutils.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/checksumutils.c Drivers/BSP/Components/mx_wifi/core/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Drivers/BSP/Components/mx_wifi/core/mx_address.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_address.c Drivers/BSP/Components/mx_wifi/core/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.c Drivers/BSP/Components/mx_wifi/core/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.c Drivers/BSP/Components/mx_wifi/core/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.c Drivers/BSP/Components/mx_wifi/core/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.c Drivers/BSP/Components/mx_wifi/core/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-BSP-2f-Components-2f-mx_wifi-2f-core

clean-Drivers-2f-BSP-2f-Components-2f-mx_wifi-2f-core:
	-$(RM) ./Drivers/BSP/Components/mx_wifi/core/checksumutils.cyclo ./Drivers/BSP/Components/mx_wifi/core/checksumutils.d ./Drivers/BSP/Components/mx_wifi/core/checksumutils.o ./Drivers/BSP/Components/mx_wifi/core/checksumutils.su ./Drivers/BSP/Components/mx_wifi/core/mx_address.cyclo ./Drivers/BSP/Components/mx_wifi/core/mx_address.d ./Drivers/BSP/Components/mx_wifi/core/mx_address.o ./Drivers/BSP/Components/mx_wifi/core/mx_address.su ./Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.cyclo ./Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.d ./Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.o ./Drivers/BSP/Components/mx_wifi/core/mx_rtos_abs.su ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.cyclo ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.d ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.o ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_hci.su ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.cyclo ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.d ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.o ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_ipc.su ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.cyclo ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.d ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.o ./Drivers/BSP/Components/mx_wifi/core/mx_wifi_slip.su

.PHONY: clean-Drivers-2f-BSP-2f-Components-2f-mx_wifi-2f-core


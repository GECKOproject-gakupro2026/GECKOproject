################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.c 

C_DEPS += \
./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.d \
./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.d \
./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.d \
./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.d 

OBJS += \
./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.o \
./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.o \
./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.o \
./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.o 


# Each subdirectory must supply rules for building sources it contributes
Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.c Drivers/BSP/Components/vl53l5cx/modules/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.c Drivers/BSP/Components/vl53l5cx/modules/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.c Drivers/BSP/Components/vl53l5cx/modules/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.c Drivers/BSP/Components/vl53l5cx/modules/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_FULL_LL_DRIVER -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I../../Drivers/BSP/B-U585I-IOT02A -I../../Drivers/BSP/Components/Common -I../../Drivers/BSP/Components/vl53l5cx -I../../Drivers/BSP/Components/vl53l5cx/modules -I../../Drivers/BSP/Components/vl53l5cx/porting -I../../Drivers/BSP/Components/stm32wb_at -I../../Drivers/BSP/Components/mx_wifi -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-BSP-2f-Components-2f-vl53l5cx-2f-modules

clean-Drivers-2f-BSP-2f-Components-2f-vl53l5cx-2f-modules:
	-$(RM) ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.cyclo ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.d ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.o ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_api.su ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.cyclo ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.d ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.o ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_detection_thresholds.su ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.cyclo ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.d ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.o ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_motion_indicator.su ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.cyclo ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.d ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.o ./Drivers/BSP/Components/vl53l5cx/modules/vl53l5cx_plugin_xtalk.su

.PHONY: clean-Drivers-2f-BSP-2f-Components-2f-vl53l5cx-2f-modules


################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Utilities/lpbam/stm32_adv_lpbam_common.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Utilities/lpbam/STM32U5/stm32_ll_lpbam.c \
D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Utilities/lpbam/stm32_lpbam_common.c 

OBJS += \
./Utilities/stm32_adv_lpbam_common.o \
./Utilities/stm32_ll_lpbam.o \
./Utilities/stm32_lpbam_common.o 

C_DEPS += \
./Utilities/stm32_adv_lpbam_common.d \
./Utilities/stm32_ll_lpbam.d \
./Utilities/stm32_lpbam_common.d 


# Each subdirectory must supply rules for building sources it contributes
Utilities/stm32_adv_lpbam_common.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Utilities/lpbam/stm32_adv_lpbam_common.c Utilities/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -c -I../LPBAM/LpbamAp1 -I../../Secure_nsclib -I../Core/Inc -I../../Utilities/lpbam -I../../Utilities/lpbam/STM32U5 -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Utilities/stm32_ll_lpbam.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Utilities/lpbam/STM32U5/stm32_ll_lpbam.c Utilities/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -c -I../LPBAM/LpbamAp1 -I../../Secure_nsclib -I../Core/Inc -I../../Utilities/lpbam -I../../Utilities/lpbam/STM32U5 -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Utilities/stm32_lpbam_common.o: D:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A/Utilities/lpbam/stm32_lpbam_common.c Utilities/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -c -I../LPBAM/LpbamAp1 -I../../Secure_nsclib -I../Core/Inc -I../../Utilities/lpbam -I../../Utilities/lpbam/STM32U5 -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Utilities

clean-Utilities:
	-$(RM) ./Utilities/stm32_adv_lpbam_common.cyclo ./Utilities/stm32_adv_lpbam_common.d ./Utilities/stm32_adv_lpbam_common.o ./Utilities/stm32_adv_lpbam_common.su ./Utilities/stm32_ll_lpbam.cyclo ./Utilities/stm32_ll_lpbam.d ./Utilities/stm32_ll_lpbam.o ./Utilities/stm32_ll_lpbam.su ./Utilities/stm32_lpbam_common.cyclo ./Utilities/stm32_lpbam_common.d ./Utilities/stm32_lpbam_common.o ./Utilities/stm32_lpbam_common.su

.PHONY: clean-Utilities


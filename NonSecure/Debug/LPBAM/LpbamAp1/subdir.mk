################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../LPBAM/LpbamAp1/lpbam_lpbamap1_config.c \
../LPBAM/LpbamAp1/lpbam_lpbamap1_scenario_build.c 

OBJS += \
./LPBAM/LpbamAp1/lpbam_lpbamap1_config.o \
./LPBAM/LpbamAp1/lpbam_lpbamap1_scenario_build.o 

C_DEPS += \
./LPBAM/LpbamAp1/lpbam_lpbamap1_config.d \
./LPBAM/LpbamAp1/lpbam_lpbamap1_scenario_build.d 


# Each subdirectory must supply rules for building sources it contributes
LPBAM/LpbamAp1/%.o LPBAM/LpbamAp1/%.su LPBAM/LpbamAp1/%.cyclo: ../LPBAM/LpbamAp1/%.c LPBAM/LpbamAp1/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -c -I../LPBAM/LpbamAp1 -I../../Secure_nsclib -I../Core/Inc -I../../Utilities/lpbam -I../../Utilities/lpbam/STM32U5 -I../../Drivers/STM32U5xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-LPBAM-2f-LpbamAp1

clean-LPBAM-2f-LpbamAp1:
	-$(RM) ./LPBAM/LpbamAp1/lpbam_lpbamap1_config.cyclo ./LPBAM/LpbamAp1/lpbam_lpbamap1_config.d ./LPBAM/LpbamAp1/lpbam_lpbamap1_config.o ./LPBAM/LpbamAp1/lpbam_lpbamap1_config.su ./LPBAM/LpbamAp1/lpbam_lpbamap1_scenario_build.cyclo ./LPBAM/LpbamAp1/lpbam_lpbamap1_scenario_build.d ./LPBAM/LpbamAp1/lpbam_lpbamap1_scenario_build.o ./LPBAM/LpbamAp1/lpbam_lpbamap1_scenario_build.su

.PHONY: clean-LPBAM-2f-LpbamAp1


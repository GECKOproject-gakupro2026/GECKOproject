################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include Drivers/STM32U5xx_HAL_Driver/subdir.mk
-include Drivers/BSP/Components/vl53l5cx/porting/subdir.mk
-include Drivers/BSP/Components/vl53l5cx/modules/subdir.mk
-include Drivers/BSP/Components/vl53l5cx/subdir.mk
-include Drivers/BSP/Components/veml6030/subdir.mk
-include Drivers/BSP/Components/veml3235/subdir.mk
-include Drivers/BSP/Components/stm32wb_at/subdir.mk
-include Drivers/BSP/Components/mx_wifi/io_pattern/subdir.mk
-include Drivers/BSP/Components/mx_wifi/core/subdir.mk
-include Drivers/BSP/Components/mx_wifi/subdir.mk
-include Drivers/BSP/Components/mx25lm51245g/subdir.mk
-include Drivers/BSP/Components/m24256/subdir.mk
-include Drivers/BSP/Components/lps22hh/subdir.mk
-include Drivers/BSP/Components/ism330dhcx/subdir.mk
-include Drivers/BSP/Components/iis2mdc/subdir.mk
-include Drivers/BSP/Components/hts221/subdir.mk
-include Drivers/BSP/Components/aps6408/subdir.mk
-include Drivers/BSP/B-U585I-IOT02A/subdir.mk
-include Core/Startup/subdir.mk
-include Core/Src/subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(CC_DEPS)),)
-include $(CC_DEPS)
endif
ifneq ($(strip $(C++_DEPS)),)
-include $(C++_DEPS)
endif
ifneq ($(strip $(CCM_DEPS)),)
-include $(CCM_DEPS)
endif
ifneq ($(strip $(C_UPPER_DEPS)),)
-include $(C_UPPER_DEPS)
endif
ifneq ($(strip $(CXX_DEPS)),)
-include $(CXX_DEPS)
endif
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(CXXM_DEPS)),)
-include $(CXXM_DEPS)
endif
ifneq ($(strip $(C++M_DEPS)),)
-include $(C++M_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
ifneq ($(strip $(CPP_DEPS)),)
-include $(CPP_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := B-U585I-IOT02A_Secure
BUILD_ARTIFACT_EXTENSION := elf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 
EXECUTABLES += \
B-U585I-IOT02A_Secure.elf \

MAP_FILES += \
B-U585I-IOT02A_Secure.map \

SIZE_OUTPUT += \
default.size.stdout \

OBJDUMP_LIST += \
B-U585I-IOT02A_Secure.list \


# All Target
all: main-build

# Main-build Target
main-build: B-U585I-IOT02A_Secure.elf secondary-outputs

# Tool invocations
B-U585I-IOT02A_Secure.elf B-U585I-IOT02A_Secure.map: $(OBJS) $(USER_OBJS) D:\App\STM32CubeIDE\workspace_2.1.1\B-U585I-IOT02A\Secure\STM32U585AIIXQ_FLASH.ld makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-g++ -o "B-U585I-IOT02A_Secure.elf" @"objects.list" $(USER_OBJS) $(LIBS) -mcpu=cortex-m33 -T"D:\App\STM32CubeIDE\workspace_2.1.1\B-U585I-IOT02A\Secure\STM32U585AIIXQ_FLASH.ld" --specs=nosys.specs -Wl,-Map="B-U585I-IOT02A_Secure.map" -Wl,--gc-sections -static -Wl,--cmse-implib -Wl,--out-implib=./secure_nsclib.o --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -Wl,--start-group -lc -lm -lstdc++ -lsupc++ -Wl,--end-group
	@echo 'Finished building target: $@'
	@echo ' '

default.size.stdout: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-size  $(EXECUTABLES)
	@echo 'Finished building: $@'
	@echo ' '

B-U585I-IOT02A_Secure.list: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objdump -h -S $(EXECUTABLES) > "B-U585I-IOT02A_Secure.list"
	@echo 'Finished building: $@'
	@echo ' '

# Other Targets
clean:
	-$(RM) B-U585I-IOT02A_Secure.elf B-U585I-IOT02A_Secure.list B-U585I-IOT02A_Secure.map default.size.stdout
	-@echo ' '

secondary-outputs: $(SIZE_OUTPUT) $(OBJDUMP_LIST)

fail-specified-linker-script-missing:
	@echo 'Error: Cannot find the specified linker script. Check the linker settings in the build configuration.'
	@exit 2

warn-no-linker-script-specified:
	@echo 'Warning: No linker script specified. Check the linker settings in the build configuration.'

.PHONY: all clean dependents main-build fail-specified-linker-script-missing warn-no-linker-script-specified

-include ../makefile.targets

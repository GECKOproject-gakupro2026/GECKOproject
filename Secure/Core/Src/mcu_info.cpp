/**
  ******************************************************************************
  * @file    mcu_info.cpp
  * @brief   mcu_info.hpp の STM32U5 向け実装。
  ******************************************************************************
  */
#include "mcu_info.hpp"

#include "main.h"

#include <cstdio>
#include <malloc.h>

/* Linker symbols for memory statistics */
extern "C" uint8_t _end;    /* end of .bss (start of heap)   */
extern "C" uint8_t _sdata;  /* start of .data                */
extern "C" uint8_t _edata;  /* end of .data                  */
extern "C" uint8_t _etext;  /* end of .text (flash)          */

namespace mcu_info
{
namespace
{
/* Internal ADC for die temperature and VDDA (via VREFINT) */
ADC_HandleTypeDef hadcMcu = {};
bool adcOk = false;

uint32_t adcReadChannel(uint32_t channel)
{
  ADC_ChannelConfTypeDef cfg = {};
  cfg.Channel = channel;
  cfg.Rank = ADC_REGULAR_RANK_1;
  cfg.SamplingTime = ADC_SAMPLETIME_814CYCLES;
  cfg.SingleDiff = ADC_SINGLE_ENDED;
  if (HAL_ADC_ConfigChannel(&hadcMcu, &cfg) != HAL_OK ||
      HAL_ADC_Start(&hadcMcu) != HAL_OK ||
      HAL_ADC_PollForConversion(&hadcMcu, 10) != HAL_OK)
  {
    return 0;
  }
  uint32_t v = HAL_ADC_GetValue(&hadcMcu);
  (void)HAL_ADC_Stop(&hadcMcu);
  return v;
}

} // namespace

void Init(telemetry::FullStatus &st)
{
  st.reset_cause = static_cast<uint8_t>(RCC->CSR >> 24);
  __HAL_RCC_CLEAR_RESET_FLAGS();
  st.sysclk_hz = HAL_RCC_GetSysClockFreq();
  st.hclk_hz = HAL_RCC_GetHCLKFreq();
  st.flash_kb = static_cast<uint16_t>(*reinterpret_cast<const uint16_t *>(FLASHSIZE_BASE));
  st.uid[0] = HAL_GetUIDw0();
  st.uid[1] = HAL_GetUIDw1();
  st.uid[2] = HAL_GetUIDw2();
  st.idcode = DBGMCU->IDCODE;

  adcOk = false;
  HAL_PWREx_EnableVddA(); /* release the analog supply isolation */
  __HAL_RCC_ADC12_CLK_ENABLE();
  RCC_PeriphCLKInitTypeDef clk = {};
  clk.PeriphClockSelection = RCC_PERIPHCLK_ADCDAC;
  clk.AdcDacClockSelection = RCC_ADCDACCLKSOURCE_HSI;
  if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
  {
    printf("[TLM] ADC kernel clock config failed\r\n");
  }

  hadcMcu.Instance = ADC1;
  hadcMcu.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV4;
  hadcMcu.Init.Resolution = ADC_RESOLUTION_14B;
  hadcMcu.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadcMcu.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadcMcu.Init.ContinuousConvMode = DISABLE;
  hadcMcu.Init.NbrOfConversion = 1;
  hadcMcu.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadcMcu.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  hadcMcu.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadcMcu.Init.OversamplingMode = DISABLE;
  HAL_StatusTypeDef initRet = HAL_ADC_Init(&hadcMcu);
  HAL_StatusTypeDef calRet = HAL_ERROR;
  if (initRet == HAL_OK)
  {
    calRet = HAL_ADCEx_Calibration_Start(&hadcMcu, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED);
  }
  adcOk = (initRet == HAL_OK && calRet == HAL_OK);
  if (!adcOk)
  {
    printf("[TLM] internal ADC init failed (init=%d cal=%d state=0x%lX err=0x%lX)\r\n",
           initRet, calRet, hadcMcu.State, hadcMcu.ErrorCode);
  }
}

void Refresh(telemetry::FullStatus &st)
{
  if (adcOk)
  {
    uint32_t vrefRaw = adcReadChannel(ADC_CHANNEL_VREFINT);
    uint32_t tempRaw = adcReadChannel(ADC_CHANNEL_TEMPSENSOR);
    if (vrefRaw != 0U)
    {
      uint32_t vdda = __HAL_ADC_CALC_VREFANALOG_VOLTAGE(ADC1, vrefRaw, ADC_RESOLUTION_14B);
      st.vdda_mv = static_cast<uint16_t>(vdda);
      int32_t tc = __HAL_ADC_CALC_TEMPERATURE(ADC1, vdda, tempRaw, ADC_RESOLUTION_14B);
      st.die_temp_x100 = static_cast<int16_t>(tc * 100);
    }
  }

  struct mallinfo mi = mallinfo();
  uint32_t staticRam = reinterpret_cast<uint32_t>(&_end) - 0x30000000UL;
  st.heap_used = static_cast<uint32_t>(mi.uordblks);
  st.heap_free = static_cast<uint32_t>(mi.fordblks);
  st.ram_used = staticRam + st.heap_used;
  st.ram_total = 256U * 1024U;
  st.flash_used = (reinterpret_cast<uint32_t>(&_etext) - 0x0C000000UL) +
                  (reinterpret_cast<uint32_t>(&_edata) - reinterpret_cast<uint32_t>(&_sdata));
  st.flash_total = 1024U * 1024U;
}

} // namespace mcu_info

/**
  ******************************************************************************
  * @file    tests_security.cpp
  * @brief   TrustZone / GTZC memory-protection verification:
  *          TZEN option bit, SAU state, MPCBB SRAM security map,
  *          TZSC peripheral security attributes.
  ******************************************************************************
  */
#include "test.hpp"
#include "app_config.h"
#include "main.h"

#include <cstdio>

namespace apptest
{

Result testTrustZone()
{
  bool ok = true;

  /* 1. Global TrustZone enable (option byte, mirrored in FLASH_OPTR) */
  bool tzen = (FLASH->OPTR & FLASH_OPTR_TZEN) != 0U;
  printf("  TZEN option bit: %s\r\n", tzen ? "enabled" : "DISABLED");
  ok &= tzen;

  /* 2. SAU active (configured by partition_stm32u585xx.h at SystemInit) */
  bool sauOn = (SAU->CTRL & SAU_CTRL_ENABLE_Msk) != 0U;
  printf("  SAU: %s, %lu regions\r\n", sauOn ? "enabled" : "DISABLED", SAU->TYPE);
  ok &= sauOn;

  /* 3. MPCBB block security: SRAM1/2 expected secure (app RAM),
   *    SRAM3 expected non-secure (released to the NS app by MX_GTZC_S_Init) */
  uint32_t sram1Cfg = GTZC_MPCBB1_S->SECCFGR[0];
  uint32_t sram2Cfg = GTZC_MPCBB2_S->SECCFGR[0];
  uint32_t sram3Cfg = GTZC_MPCBB3_S->SECCFGR[0];
  printf("  MPCBB SECCFGR[0]: SRAM1=0x%08lX SRAM2=0x%08lX SRAM3=0x%08lX\r\n",
         sram1Cfg, sram2Cfg, sram3Cfg);
  if (sram1Cfg != 0xFFFFFFFFUL || sram2Cfg != 0xFFFFFFFFUL)
  {
    printf("  secure app SRAM1/2 blocks are not fully secure\r\n");
    ok = false;
  }
  if (sram3Cfg != 0x00000000UL)
  {
    printf("  SRAM3 first blocks not released to non-secure\r\n");
    ok = false;
  }

  /* 4. TZSC peripheral attribute: USART1 (console) was set secure in CubeMX */
  uint32_t attr = 0;
  if (HAL_GTZC_TZSC_GetConfigPeriphAttributes(GTZC_PERIPH_USART1, &attr) == HAL_OK)
  {
    bool sec = (attr & GTZC_TZSC_PERIPH_SEC) != 0U;
    printf("  TZSC USART1 attribute: %s\r\n", sec ? "secure" : "non-secure");
    ok &= sec;
  }
  else
  {
    printf("  TZSC attribute read failed\r\n");
    ok = false;
  }

  /* 5. Secure world can read the NS alias of SRAM3 (one-way isolation) */
  volatile uint32_t *nsRam = reinterpret_cast<volatile uint32_t *>(0x20040000UL);
  uint32_t probe = *nsRam;
  (void)probe;
  printf("  secure -> non-secure SRAM3 read OK (isolation is NS->S only)\r\n");

  return ok ? Result::Pass : Result::Fail;
}

} // namespace apptest

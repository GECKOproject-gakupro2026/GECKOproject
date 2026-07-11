/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    Secure/Src/secure_nsc.c
  * @author  MCD Application Team
  * @brief   This file contains the non-secure callable APIs (secure world)
  ******************************************************************************
    * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* USER CODE BEGIN Non_Secure_CallLib */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "secure_nsc.h"

/* Implemented in boot_guard.cpp (C++); confirmBoot() itself is C++-only
 * (namespaced), so this thin extern "C" wrapper is what secure_nsc.c calls. */
extern void BootGuard_ConfirmBoot(void);
/** @addtogroup STM32U5xx_HAL_Examples

  * @{
  */

/** @addtogroup Templates
  * @{
  */

/* Global variables ----------------------------------------------------------*/
void *pSecureFaultCallback = NULL;   /* Pointer to secure fault callback in Non-secure */
void *pSecureErrorCallback = NULL;   /* Pointer to secure error callback in Non-secure */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Secure registration of non-secure callback.
  * @param  CallbackId  callback identifier
  * @param  func        pointer to non-secure function
  * @retval None
  */
    CMSE_NS_ENTRY void SECURE_RegisterCallback(SECURE_CallbackIDTypeDef CallbackId, void *func)
    {
      if(func != NULL)
      {
        switch(CallbackId)
        {
          case SECURE_FAULT_CB_ID:           /* SecureFault Interrupt occurred */
          pSecureFaultCallback = func;
          break;
          case GTZC_ERROR_CB_ID:             /* GTZC Interrupt occurred */
          pSecureErrorCallback = func;
          break;
          default:
          /* unknown */
          break;
        }
      }
    }

/**
  * @brief  Called by NonSecure once its own startup checks pass, to clear
  *         the Stage-0 boot-attempt counter (OTA Phase 2 rollback guard).
  * @retval None
  */
CMSE_NS_ENTRY void Secure_ConfirmBoot(void)
{
  BootGuard_ConfirmBoot();
}

/**
  * @}
  */

/**
  * @}
  */
/* USER CODE END Non_Secure_CallLib */


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

#include "comm_backend.h"

#include <arm_cmse.h>
#include <string.h>

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

/* ---- Comm_* gateways (NonSecure app layer -> Secure comm stack) ---------- */

/**
  * @brief  One scheduler pass of the Secure comm service (TCP/OTA/BLE/audio).
  *         The NonSecure main loop must call this every iteration.
  */
CMSE_NS_ENTRY void Comm_Poll(void)
{
  CommBridge_Poll();
}

/**
  * @brief  Submits a NonSecure-produced telemetry snapshot for transmission.
  * @retval 0 accepted, -1 bad pointer, -2 comm service not ready
  */
CMSE_NS_ENTRY int Comm_SendTelemetry(const FullStatus_t *st)
{
  /* The pointer comes from the NonSecure world: verify it really points at
   * NonSecure-readable memory of the right size, then copy to Secure stack
   * before use, so NonSecure can neither alias Secure RAM nor mutate the
   * buffer mid-transmission. */
  if (cmse_check_address_range((void *)st, sizeof(FullStatus_t),
                               CMSE_NONSECURE | CMSE_MPU_READ) == NULL)
  {
    return -1;
  }
  FullStatus_t local;
  memcpy(&local, st, sizeof(local));
  return CommBridge_SendTelemetry(&local);
}

/**
  * @brief  Drains one plain host-command byte / reports inbound activity.
  * @retval COMM_POLL_BYTE (byte written to *out), COMM_POLL_ACTIVITY,
  *         COMM_POLL_NONE, or -1 on bad pointer
  */
CMSE_NS_ENTRY int Comm_PollHostCommand(uint8_t *out)
{
  if (cmse_check_address_range(out, sizeof(uint8_t),
                               CMSE_NONSECURE | CMSE_MPU_READWRITE) == NULL)
  {
    return -1;
  }
  uint8_t byte = 0U;
  int ret = CommBridge_PollHostCommand(&byte);
  if (ret == COMM_POLL_BYTE)
  {
    *out = byte;
  }
  return ret;
}

/**
  * @brief  Enables/disables the Secure telemetry push (low-power support).
  */
CMSE_NS_ENTRY void Comm_SetTelemetryEnabled(uint32_t on)
{
  CommBridge_SetTelemetryEnabled(on);
}

/**
  * @brief  Additive gateway (not part of the frozen 7): forwards the
  *         NonSecure device state machine's current state so Secure can
  *         surface it over BLE (MiniStatus.flags) and the state-transition
  *         log. A plain uint32_t needs no pointer validation.
  */
CMSE_NS_ENTRY void Comm_SetDeviceState(uint32_t state)
{
  CommBridge_SetDeviceState(state);
}

/**
  * @brief  Link status bits: 0=BLE alive, 1=WiFi up, 2=BLE conn, 3=TCP client.
  */
CMSE_NS_ENTRY uint32_t Comm_GetLinkStatus(void)
{
  return CommBridge_GetLinkStatus();
}

/**
  * @brief  Copies the Secure mic capture window into a NonSecure buffer.
  * @retval sample count copied, or 0 on bad pointer / audio not running
  */
CMSE_NS_ENTRY uint32_t Comm_GetAudioBuffer(int16_t *dst, uint32_t maxSamples)
{
  if (maxSamples == 0U ||
      cmse_check_address_range(dst, maxSamples * sizeof(int16_t),
                               CMSE_NONSECURE | CMSE_MPU_READWRITE) == NULL)
  {
    return 0U;
  }
  return CommBridge_GetAudioBuffer(dst, maxSamples);
}

/**
  * @brief  Fills the MCU-info fields (die temp, VDDA, clocks, flash size,
  *         UID, reset cause, CPU load, RAM/flash usage) and ble_alive into
  *         a NonSecure-owned FullStatus_t, leaving every other field
  *         untouched. See CommBridge_GetMcuInfo()'s doc comment.
  */
CMSE_NS_ENTRY void Comm_GetMcuInfo(FullStatus_t *dst)
{
  if (cmse_check_address_range(dst, sizeof(FullStatus_t),
                               CMSE_NONSECURE | CMSE_MPU_READWRITE) == NULL)
  {
    return;
  }
  FullStatus_t local;
  memcpy(&local, dst, sizeof(local));
  CommBridge_GetMcuInfo(&local);
  memcpy(dst, &local, sizeof(local));
}

/**
  * @}
  */

/**
  * @}
  */
/* USER CODE END Non_Secure_CallLib */


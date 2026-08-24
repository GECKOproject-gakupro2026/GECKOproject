/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    Secure_nsclib/secure_nsc.h
  * @author  MCD Application Team
  * @brief   Header for secure non-secure callable APIs list
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

/* USER CODE BEGIN Non_Secure_CallLib_h */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef SECURE_NSC_H
#define SECURE_NSC_H

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>

#include "comm_dto.h"

/* Exported types ------------------------------------------------------------*/
/**
  * @brief  non-secure callback ID enumeration definition
  */
typedef enum
{
SECURE_FAULT_CB_ID     = 0x00U, /*!< System secure fault callback ID */
  GTZC_ERROR_CB_ID       = 0x01U  /*!< GTZC secure error callback ID */
} SECURE_CallbackIDTypeDef;
/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */
void SECURE_RegisterCallback(SECURE_CallbackIDTypeDef CallbackId, void *func);

/* OTA Phase 2: called by the NonSecure app once it has finished its own
 * startup checks, to clear the Stage-0 boot-attempt counter (BootGuard). */
void Secure_ConfirmBoot(void);

/* ---- Comm_* gateways: the Secure-owned communication stack -----------------
 * The NonSecure application is the main loop; it must call Comm_Poll() every
 * iteration to pump the Secure comm service (TCP accept/recv, OTA frame
 * handling, BLE events, audio streaming). Secure IRQs (UART RX DMA, BLE UART,
 * Wi-Fi SPI) keep firing in the secure state regardless. */

/* One scheduler pass of the Secure comm service. Call as fast as possible. */
void Comm_Poll(void);

/* Submits a telemetry snapshot for transmission over UART/TCP (+BLE at its
 * own pace). Returns 0 when accepted, negative on error (bad pointer / comm
 * service not ready). The first accepted call switches the Secure side to
 * "NonSecure produces telemetry" mode (Secure stops generating its own). */
int Comm_SendTelemetry(const FullStatus_t *st);

/* Drains one plain host-command byte (console/TCP single-char commands).
 * Returns COMM_POLL_BYTE and writes *out when a byte was dequeued,
 * COMM_POLL_ACTIVITY when inbound traffic was seen but nothing is queued
 * (e.g. OTA frame bytes), COMM_POLL_NONE otherwise. Any nonzero return
 * counts as host activity for the NonSecure idle timer. */
int Comm_PollHostCommand(uint8_t *out);

/* Enables/disables the Secure-side telemetry push (BLE/TCP/UART). The
 * NonSecure app uses this to quiet the links in low-power mode. */
void Comm_SetTelemetryEnabled(uint32_t on);

/* bit0 = BLE module alive, bit1 = Wi-Fi joined, bit2 = BLE central
 * connected, bit3 = TCP client connected */
uint32_t Comm_GetLinkStatus(void);

/* Copies the live microphone capture window (16 kHz mono int16, owned by the
 * Secure audio DMA) into a NonSecure buffer. Returns the number of samples
 * copied (<= maxSamples), or 0 if audio isn't running / the pointer is bad.
 * NonSecure uses this for both the RMS/peak/waveform telemetry fields and
 * the AI inference input - audio capture itself stays Secure (Phase D). */
uint32_t Comm_GetAudioBuffer(int16_t *dst, uint32_t maxSamples);

/* Runs one 975 ms log-mel + YamNet inference in Secure world. */
int AI_RunOnce(void);

#endif /* SECURE_NSC_H */
/* USER CODE END Non_Secure_CallLib_h */


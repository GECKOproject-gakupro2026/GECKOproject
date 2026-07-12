/**
  ******************************************************************************
  * @file    Secure_nsclib/comm_dto.h
  * @brief   Data types crossing the Secure <-> NonSecure (CMSE) boundary.
  *
  *          FullStatus_t is the C POD twin of the C++ telemetry::FullStatus
  *          (Secure/Core/Inc/telemetry.hpp). CMSE gateways may only pass
  *          scalars and pointers to plain C PODs, so the NonSecure app fills
  *          this struct and hands it to Comm_SendTelemetry(); the Secure comm
  *          stack serializes and transmits it unchanged. The two definitions
  *          MUST stay byte-identical (both sides static-assert sizeof == 165,
  *          matching the PC parser "<BBIhHI3h3h3hIHBhh32hBB6I + hHIIBBH3II").
  ******************************************************************************
  */
#ifndef COMM_DTO_H
#define COMM_DTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed))
{
  uint8_t ver; /* = 2 */
  uint8_t button;
  uint32_t uptime_ms;
  int16_t temp_x100;
  uint16_t hum_x100;
  uint32_t press_x100;
  int16_t acc_mg[3];
  int16_t gyro_dps10[3];
  int16_t mag_mgauss[3];
  uint32_t light_raw;
  uint16_t tof_mm;
  uint8_t tof_ok;
  int16_t audio_rms;
  int16_t audio_peak;
  int16_t wave[32];
  uint8_t ble_alive;
  uint8_t wifi_alive;
  uint32_t ram_used;
  uint32_t ram_total;
  uint32_t heap_used;
  uint32_t heap_free;
  uint32_t flash_used;
  uint32_t flash_total;
  /* --- v2: MCU details --- */
  int16_t die_temp_x100;
  uint16_t vdda_mv;
  uint32_t sysclk_hz;
  uint32_t hclk_hz;
  uint8_t reset_cause;
  uint8_t cpu_load_pct;
  uint16_t flash_kb;
  uint32_t uid[3];
  uint32_t idcode;
} FullStatus_t;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(FullStatus_t) == 165,
               "FullStatus_t layout must match telemetry::FullStatus / PC parser");
#endif

/* Comm_PollHostCommand() return codes */
#define COMM_POLL_NONE      0  /* no host activity since the last poll        */
#define COMM_POLL_ACTIVITY  1  /* inbound bytes seen (frames), none dequeued  */
#define COMM_POLL_BYTE      2  /* a plain command byte was written to *out    */

#ifdef __cplusplus
}
#endif

#endif /* COMM_DTO_H */

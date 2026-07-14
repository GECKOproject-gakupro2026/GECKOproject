/**
  ******************************************************************************
  * @file    app_loop.c
  * @brief   ACTIVE/IDLEステートマシン【コア層・基板非依存】。
  *
  *          このファイルはHALもGPIOもBSPも知らない。すべて契約ヘッダ経由:
  *            board_io.h  ... LED・ボタン
  *            sensors.h   ... センサー
  *            ns_audio.h  ... 音声
  *            comm_api.h  ... 通信(TrustZoneの有無に非依存)
  *          だから基板を変えてもこのファイルは無改造で移植できる。
  ******************************************************************************
  */
#include "app_loop.h"

#include "app_config.h"
#include "board_io.h"
#include "comm_api.h"
#include "ns_audio.h"
#include "sensors.h"

#include "main.h"   /* HAL_GetTick */

/* NonSecure is the application layer's main loop. It pumps the Secure comm
 * service via Comm_Poll() and submits a telemetry snapshot through the
 * Comm_SendTelemetry() NSC gateway. Phase C: env/motion/light/ToF sensors
 * (I2C1/I2C2) are now read here too; audio/AI/MCU-info fields stay zeroed
 * until Phase D/F. */
static void build_status(FullStatus_t *st)
{
  Sensors_Refresh(st);
  Audio_Refresh(st);
  st->ver = 2U;
  st->uptime_ms = HAL_GetTick();
  st->button = Board_ButtonRead();
}

/* TrustZone Phase E: ACTIVE/IDLE low-power state machine. In ACTIVE the app
 * reads sensors, streams 50 Hz telemetry and blinks the green heartbeat.
 * After CFG_IDLE_TIMEOUT_MS with no host command it drops to IDLE: sensors
 * and telemetry stop (Secure comm push is quieted too), ToF stops ranging,
 * and the red LED slow-blinks. Any inbound host activity - which the Secure
 * comm stack keeps listening for even in IDLE - wakes it back to ACTIVE. */
void App_Run(void)
{
  enum { MODE_ACTIVE = 0, MODE_IDLE = 1 } mode = MODE_ACTIVE;
  uint32_t lastActivityMs = HAL_GetTick(); /* start ACTIVE, not instantly idle */
  uint32_t nextTelemetryTick = 0U;
  uint32_t nextLedTick = 0U;

  while (1)
  {
    Comm_Poll(); /* pumps the Secure comm service (TCP/OTA/BLE/audio) */

    uint32_t now = HAL_GetTick();

    /* Poll host input every loop (even in IDLE) - this is the wake source. */
    uint8_t cmdByte = 0U;
    if (Comm_PollHostCommand(&cmdByte) != COMM_POLL_NONE)
    {
      lastActivityMs = now; /* any inbound host traffic counts as activity */
    }

    if (mode == MODE_ACTIVE)
    {
      if ((int32_t)(now - lastActivityMs) >= (int32_t)CFG_IDLE_TIMEOUT_MS)
      {
        /* Enter IDLE: stop sensors/telemetry, quiet the Secure links. */
        mode = MODE_IDLE;
        Sensors_Stop();
        Audio_Stop();
        Comm_SetTelemetryEnabled(0U);
        Board_LedGreenOff();
        Board_LedRedOff();
        nextLedTick = now;
      }
      else
      {
        if ((int32_t)(now - nextTelemetryTick) >= 0)
        {
          nextTelemetryTick = now + CFG_ACTIVE_TELEMETRY_MS;
          static FullStatus_t st;
          build_status(&st);
          (void)Comm_SendTelemetry(&st);
        }
        if ((int32_t)(now - nextLedTick) >= 0)
        {
          nextLedTick = now + CFG_ACTIVE_HB_MS;
          Board_LedRedOff();
          Board_LedGreenToggle(); /* green heartbeat */
        }
      }
    }
    else /* MODE_IDLE */
    {
      if ((int32_t)(now - lastActivityMs) < (int32_t)CFG_IDLE_TIMEOUT_MS)
      {
        /* Woke on host activity: resume ACTIVE. */
        mode = MODE_ACTIVE;
        Sensors_Resume();
        Audio_Resume();
        Comm_SetTelemetryEnabled(1U);
        Board_LedRedOff();
        nextTelemetryTick = now;
        nextLedTick = now;
      }
      else if ((int32_t)(now - nextLedTick) >= 0)
      {
        nextLedTick = now + CFG_IDLE_LED_BLINK_MS;
        Board_LedGreenOff();
        Board_LedRedToggle(); /* red slow blink = low-power indicator */
      }
    }
  }
}

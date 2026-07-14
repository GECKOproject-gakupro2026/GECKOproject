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
 * Comm_SendTelemetry() NSC gateway. env/motion/light/ToF sensors (I2C1/I2C2)
 * and audio (RMS/waveform, read from the Secure capture buffer) are read
 * here on every call, in both ACTIVE and IDLE. */
static void build_status(FullStatus_t *st)
{
  Sensors_Refresh(st);
  Audio_Refresh(st);
  st->ver = 2U;
  st->uptime_ms = HAL_GetTick();
  st->button = Board_ButtonRead();
}

/* ACTIVE/IDLE state machine, redefined around "IDLE = waiting for a
 * trigger" rather than "IDLE = shut everything down". Only ToF (the one
 * sensor with meaningful power draw) is put to sleep in IDLE; telemetry,
 * BLE and TCP stay live at a slower cadence so the comm stack never goes
 * quiet. This matters: quieting the comm stack via
 * Comm_SetTelemetryEnabled(0) used to also skip Service::pollTcp() on the
 * Secure side, and pollTcp()'s blocking MX_WIFI_Socket_accept() (hundreds
 * of ms to ~10s with no client) then starved Console_GetChar() of CPU time
 * to drain the wake byte - the board could enter IDLE but never leave it
 * (see docs/refactoring/既知の問題_IDLE復帰の間欠的不安定性.md). Never calling
 * Comm_SetTelemetryEnabled(0) removes that failure mode entirely. */
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
        /* Enter IDLE: only ToF (the power-hungry sensor) sleeps. Telemetry,
         * BLE and TCP keep running at the slower IDLE cadence below. */
        mode = MODE_IDLE;
        Sensors_Stop();
        Board_LedGreenOff();
        Board_LedRedOff();
        nextLedTick = now;
        nextTelemetryTick = now;
      }
      else if ((int32_t)(now - nextLedTick) >= 0)
      {
        nextLedTick = now + CFG_ACTIVE_HB_MS;
        Board_LedRedOff();
        Board_LedGreenToggle(); /* green heartbeat */
      }
    }
    else /* MODE_IDLE */
    {
      if ((int32_t)(now - lastActivityMs) < (int32_t)CFG_IDLE_TIMEOUT_MS)
      {
        /* Woke on host activity: resume ToF ranging. */
        mode = MODE_ACTIVE;
        Sensors_Resume();
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

    /* Telemetry runs in both modes now, just at different cadences
     * (CFG_ACTIVE_TELEMETRY_MS vs CFG_IDLE_TELEMETRY_MS). */
    uint32_t telemetryPeriodMs =
        (mode == MODE_ACTIVE) ? CFG_ACTIVE_TELEMETRY_MS : CFG_IDLE_TELEMETRY_MS;
    if ((int32_t)(now - nextTelemetryTick) >= 0)
    {
      nextTelemetryTick = now + telemetryPeriodMs;
      static FullStatus_t st;
      build_status(&st);
      (void)Comm_SendTelemetry(&st);
    }
  }
}

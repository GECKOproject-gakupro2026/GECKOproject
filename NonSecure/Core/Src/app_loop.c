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
#include "app_state.h"
#include "comm_api.h"
#include "triggers.h"

#include "main.h"   /* HAL_GetTick */

/* NonSecure is the application layer's main loop. The device-level state
 * machine (IDLE / ACTIVE with its ACQUIRE/COMM sub-cycle) now lives in
 * app_state.c; App_Run is a thin driver that pumps the Secure comm service
 * via Comm_Poll() and steps the state machine each iteration.
 *
 * IDLE stays "waiting for a trigger" rather than "shut everything down": only
 * ToF (the one power-hungry sensor) sleeps; telemetry/BLE/TCP keep running at
 * a slower cadence so the comm stack never goes quiet (never call
 * Comm_SetTelemetryEnabled(0) - see the IDLE-lockup note in app_state.c's
 * history / docs). The idle-entry guard also checks the live link status so
 * the board won't idle while a BLE/TCP link is up. */
void App_Run(void)
{
  static AppStateCtx_t ctx; /* static: large-ish, and lives for the whole run */
  AppState_Init(&ctx, HAL_GetTick());

  Trigger_SetLightThreshold(CFG_TRIGGER_LIGHT_THRESHOLD);
  Trigger_SetAudioThreshold(CFG_TRIGGER_AUDIO_THRESHOLD);

  while (1)
  {
    Comm_Poll(); /* pumps the Secure comm service (TCP/OTA/BLE/audio) */
    AppState_Tick(&ctx, HAL_GetTick(), Comm_GetLinkStatus());
  }
}

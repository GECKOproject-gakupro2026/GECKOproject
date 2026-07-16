/**
  ******************************************************************************
  * @file    app_state.c
  * @brief   app_state.h の実装。app_loop.c から抽出したデバイス状態機械。
  *
  *          この Step(リファクタ抽出)では挙動は従来の App_Run と同一
  *          (テレメトリ周期・LED・ToF・IDLE判定を据え置き)。ACQUIRE/COMM の
  *          明示的サブ状態と default→IDLE フォールバックを導入し、以降の
  *          Step(周期最適化・ログ連携)の土台とする。
  ******************************************************************************
  */
#include "app_state.h"

#include "app_config.h"
#include "board_io.h"
#include "comm_api.h"
#include "ns_audio.h"
#include "sensors.h"
#include "triggers.h"

#include "main.h" /* HAL_GetTick */

/* Comm_GetLinkStatus のビット(comm_api.h と一致): bit2=BLE接続中, bit3=TCP client */
#define LINK_BIT_BLE_CONNECTED  (1U << 2)
#define LINK_BIT_TCP_CLIENT     (1U << 3)

/* 最新スナップショットを埋める(旧 app_loop.c の build_status と同一)。 */
static void build_status(FullStatus_t *st)
{
  Sensors_Refresh(st);
  Audio_Refresh(st);
  Comm_GetMcuInfo(st);
  st->ver = 2U;
  st->uptime_ms = HAL_GetTick();
  st->button = Board_ButtonRead();
}

void AppState_Init(AppStateCtx_t *ctx, uint32_t now_ms)
{
  ctx->state = STATE_ACTIVE_COMM; /* 起動直後はACTIVE(即IDLEにしない) */
  ctx->lastActivityMs = now_ms;
  ctx->nextTelemetryMs = now_ms;
  ctx->nextLedMs = now_ms;
  ctx->commReturnMs = now_ms;
}

/* IDLEに入ってよいか: 無通信タイムアウト経過 かつ どのリンクもアクティブでない。
 * リンクが張られている間はIDLEに落とさない(要求2/3の調停)。 */
static int idle_allowed(const AppStateCtx_t *ctx, uint32_t now_ms, uint32_t linkStatus)
{
  int timedOut = (int32_t)(now_ms - ctx->lastActivityMs) >= (int32_t)CFG_IDLE_TIMEOUT_MS;
  int linkActive = (linkStatus & (LINK_BIT_BLE_CONNECTED | LINK_BIT_TCP_CLIENT)) != 0U;
  return timedOut && !linkActive;
}

void AppState_Tick(AppStateCtx_t *ctx, uint32_t now_ms, uint32_t linkStatus)
{
  /* トリガー(ホスト通信/照度/音圧)を毎周ポーリング。発火で活動時刻を更新。 */
  if (Trigger_Poll(&ctx->st) != TRIG_NONE)
  {
    ctx->lastActivityMs = now_ms;
  }

  switch (ctx->state)
  {
    case STATE_IDLE:
      if (!idle_allowed(ctx, now_ms, linkStatus))
      {
        /* 活動あり or リンクアクティブ: ACTIVEへ復帰し ToF ranging 再開。 */
        ctx->state = STATE_ACTIVE_COMM;
        Sensors_Resume();
        Board_LedRedOff();
        ctx->nextTelemetryMs = now_ms;
        ctx->nextLedMs = now_ms;
      }
      else if ((int32_t)(now_ms - ctx->nextLedMs) >= 0)
      {
        ctx->nextLedMs = now_ms + CFG_IDLE_LED_BLINK_MS;
        Board_LedGreenOff();
        Board_LedRedToggle(); /* 赤 遅点滅 = 低電力インジケータ */
      }
      break;

    case STATE_ACTIVE_ACQUIRE:
    case STATE_ACTIVE_COMM:
      if (idle_allowed(ctx, now_ms, linkStatus))
      {
        /* IDLEへ: 電力を食う ToF だけ SLEEP。通信は低頻度で継続。 */
        ctx->state = STATE_IDLE;
        Sensors_Stop();
        Board_LedGreenOff();
        Board_LedRedOff();
        ctx->nextLedMs = now_ms;
        ctx->nextTelemetryMs = now_ms;
      }
      else if ((int32_t)(now_ms - ctx->nextLedMs) >= 0)
      {
        ctx->nextLedMs = now_ms + CFG_ACTIVE_HB_MS;
        Board_LedRedOff();
        Board_LedGreenToggle(); /* 緑 ハートビート */
      }
      break;

    default:
      /* 不明/エラー状態: 待機へフォールバック(要求1)。 */
      ctx->state = STATE_IDLE;
      Sensors_Stop();
      Board_LedGreenOff();
      Board_LedRedOff();
      ctx->nextLedMs = now_ms;
      ctx->nextTelemetryMs = now_ms;
      break;
  }

  /* テレメトリは両モードで送る(周期だけ違う)。旧 App_Run と同一。 */
  uint32_t telemetryPeriodMs =
      (ctx->state == STATE_IDLE) ? CFG_IDLE_TELEMETRY_MS : CFG_ACTIVE_TELEMETRY_MS;
  if ((int32_t)(now_ms - ctx->nextTelemetryMs) >= 0)
  {
    ctx->nextTelemetryMs = now_ms + telemetryPeriodMs;
    build_status(&ctx->st);
    (void)Comm_SendTelemetry(&ctx->st);
    ctx->commReturnMs = HAL_GetTick(); /* COMM戻り時刻(Step7で使用) */
  }
}

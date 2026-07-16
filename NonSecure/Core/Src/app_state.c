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
  ctx->nextCommMs = now_ms;
  ctx->haveAcquired = 0U;
  /* 起点をACTIVE以外の値にしておき、初回のAppState_Tick()で必ず一度
   * Comm_SetDeviceState(STATE_ACTIVE_COMM) が呼ばれるようにする。 */
  ctx->lastReportedState = STATE_IDLE;
  Comm_SetDeviceState((uint32_t)ctx->state);
  ctx->lastReportedState = ctx->state;
}

/* IDLEに入ってよいか(=ACTIVEからIDLEへの遷移条件): 無通信タイムアウト経過
 * かつ どのリンクもアクティブでない。リンクが張られている間はIDLEに落とさない
 * (要求2/3の調停)。 */
static int idle_entry_allowed(const AppStateCtx_t *ctx, uint32_t now_ms, uint32_t linkStatus)
{
  int timedOut = (int32_t)(now_ms - ctx->lastActivityMs) >= (int32_t)CFG_IDLE_TIMEOUT_MS;
  int linkActive = (linkStatus & (LINK_BIT_BLE_CONNECTED | LINK_BIT_TCP_CLIENT)) != 0U;
  return timedOut && !linkActive;
}

/* IDLEから抜けてよいか(=IDLEからACTIVEへの遷移条件、Step C2で厳密化)。
 * 通常のトラフィック(nsActivity/lastActivityMs)はIDLEに居続ける根拠にはならない
 * - 一度IDLEに入ったら、UARTの明示コマンド(FRAME_CMD_ENTER_COMM)か、ファーム
 * ウェア自身が張ったリンク(BLE central接続やTCPクライアント接続)がない限り、
 * IDLEを維持し続ける(ユーザー要求「一度待機状態に入れば...維持し続ける」)。
 * Comm_TakeExplicitWake() は一回消費フラグなので、呼ぶのはこの判定の中だけ。 */
static int wake_requested(const AppStateCtx_t *ctx, uint32_t now_ms, uint32_t linkStatus)
{
  (void)ctx;
  (void)now_ms;
  int linkActive = (linkStatus & (LINK_BIT_BLE_CONNECTED | LINK_BIT_TCP_CLIENT)) != 0U;
  int explicitWake = (Comm_TakeExplicitWake() != 0U);
  return explicitWake || linkActive;
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
      if (wake_requested(ctx, now_ms, linkStatus))
      {
        /* 活動あり or リンクアクティブ: ACTIVEへ復帰し ToF ranging 再開。
         * COMMサイクルを now から仕切り直す(過去の nextCommMs でバーストしない)。 */
        ctx->state = STATE_ACTIVE_COMM;
        Sensors_Resume();
        Board_LedRedOff();
        ctx->nextTelemetryMs = now_ms;
        ctx->nextLedMs = now_ms;
        ctx->nextCommMs = now_ms;
        ctx->haveAcquired = 0U;
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
    {
      /* 両方を必ず評価してフラグを消費する(短絡評価でTakeStopRequestedが
       * 呼ばれ損ねないように、先に変数へ受けてからORする)。 */
      int timedOutIdle = idle_entry_allowed(ctx, now_ms, linkStatus);
      int stopRequested = (Comm_TakeStopRequested() != 0U);
      if (timedOutIdle || stopRequested)
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
    }

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

  if (ctx->state != ctx->lastReportedState)
  {
    /* Secure側へ状態変化を伝える(MiniStatus.flagsとstate_logに反映される)。
     * ACQUIRE<->COMMのサブ状態切り替えも含め、遷移のたびに呼ぶ(comm_service.cpp
     * 側でIDLE<->ACTIVEの跨ぎだけをログに記録するので、ここでは間引かない)。 */
    Comm_SetDeviceState((uint32_t)ctx->state);
    ctx->lastReportedState = ctx->state;
  }

  if (ctx->state == STATE_IDLE)
  {
    /* IDLE: 低頻度で送るだけ(取得と送信を分けるほどの精度は不要)。 */
    if ((int32_t)(now_ms - ctx->nextTelemetryMs) >= 0)
    {
      ctx->nextTelemetryMs = now_ms + CFG_IDLE_TELEMETRY_MS;
      build_status(&ctx->st);
      (void)Comm_SendTelemetry(&ctx->st);
      ctx->commReturnMs = HAL_GetTick();
    }
    /* IDLE中は次回ACTIVE突入時に取得し直す。 */
    ctx->haveAcquired = 0U;
    return;
  }

  /* ACTIVE: COMM -> ACQUIRE -> WAIT サイクル(要求4)。
   *  - COMM   : nextCommMs に達したら送信し、戻り時刻を記録。次回送信予定を
   *             「今回の戻り + 周期」に張る(前回の戻り基準なので、取得や送信に
   *             かかった時間の揺らぎを吸収し、PC到達が一定周期になる)。
   *  - ACQUIRE: 送信直後に次フレーム用センサーを読む。読取に時間がかかっても
   *             次のCOMM時刻は既に確定しているので到達周期はぶれない。
   *  - WAIT   : 何もしない期間(nextCommMs まで)。ループは回り続ける。 */
  if ((int32_t)(now_ms - ctx->nextCommMs) >= 0)
  {
    /* --- COMM --- */
    if (!ctx->haveAcquired)
    {
      /* 初回や IDLE 復帰直後: 送信前に一度取得しておく。 */
      build_status(&ctx->st);
    }
    (void)Comm_SendTelemetry(&ctx->st);
    ctx->commReturnMs = HAL_GetTick();
    ctx->state = STATE_ACTIVE_COMM;

    /* 次回COMMは「前回の戻り + 周期」。大きく遅延したらバースト回避で resync。 */
    ctx->nextCommMs += CFG_COMM_PERIOD_MS;
    if ((int32_t)(ctx->commReturnMs - ctx->nextCommMs) > (int32_t)CFG_COMM_PERIOD_MS)
    {
      ctx->nextCommMs = ctx->commReturnMs + CFG_COMM_PERIOD_MS;
    }

    /* --- ACQUIRE --- 次フレーム用スナップショットを先読み。 */
    build_status(&ctx->st);
    ctx->haveAcquired = 1U;
    ctx->state = STATE_ACTIVE_ACQUIRE;
  }
}

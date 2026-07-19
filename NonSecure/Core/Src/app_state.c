/**
  ******************************************************************************
  * @file    app_state.c
  * @brief   app_state.h の実装。app_loop.c から抽出したデバイス状態機械。
  *
  *          IDLEはトリガー待機状態: センサーの取得も送信も行わない
  *          (例外: 照度・音圧はトリガー判定用に取得)。低頻度のIDLE_BEACON
  *          だけを送りPCに生存を伝える。ACTIVEはACQUIRE/COMMのサブサイクルで
  *          センサー取得とテレメトリ送信を分離し、一定周期でPCへ届ける。
  ******************************************************************************
  */
#include "app_state.h"

#include "app_config.h"
#include "board_io.h"
#include "comm_api.h"
#include "sensor_store.h"
#include "sensors.h"
#include "triggers.h"

#include "main.h" /* HAL_GetTick */

/* Comm_GetLinkStatus のビット(comm_api.h と一致):
 * bit2=BLE接続中, bit3=TCP client, bit4=BLE録音送信中 */
#define LINK_BIT_BLE_CONNECTED  (1U << 2)
#define LINK_BIT_TCP_CLIENT     (1U << 3)
#define LINK_BIT_BLE_REC_TX     (1U << 4)

void AppState_Init(AppStateCtx_t *ctx, uint32_t now_ms)
{
  ctx->state = STATE_ACTIVE_COMM; /* 起動直後はACTIVE(即IDLEにしない) */
  ctx->lastActivityMs = now_ms;
  ctx->nextBeaconMs = now_ms;
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
  int linkActive = (linkStatus &
                    (LINK_BIT_BLE_CONNECTED | LINK_BIT_TCP_CLIENT | LINK_BIT_BLE_REC_TX)) != 0U;
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
  int linkActive = (linkStatus &
                    (LINK_BIT_BLE_CONNECTED | LINK_BIT_TCP_CLIENT | LINK_BIT_BLE_REC_TX)) != 0U;
  int explicitWake = (Comm_TakeExplicitWake() != 0U);
  return explicitWake || linkActive;
}

void AppState_Tick(AppStateCtx_t *ctx, uint32_t now_ms, uint32_t linkStatus)
{
  /* PC側からのセンサー周期変更コマンド(FRAME_CMD_SET_SENSOR_RATE)を消費する。
   * 一回消費フラグなので、呼ぶのはここだけ。 */
  uint8_t rateSensorId;
  uint16_t ratePeriodMs;
  if (Comm_TakeSensorRateCmd(&rateSensorId, &ratePeriodMs) != 0U)
  {
    Sensors_SetPeriod(rateSensorId, ratePeriodMs);
  }

  /* トリガー(ホスト通信/照度/音圧)を毎周ポーリング。発火で活動時刻を更新。 */
  if (Trigger_Poll(SensorStore_Peek()) != TRIG_NONE)
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
    case STATE_ACTIVE_BLE_REC:
    {
      /* 両方を必ず評価してフラグを消費する(短絡評価でTakeStopRequestedが
       * 呼ばれ損ねないように、先に変数へ受けてからORする)。 */
      int timedOutIdle = idle_entry_allowed(ctx, now_ms, linkStatus);
      int stopRequested = (Comm_TakeStopRequested() != 0U);
      int bleRecActive = (linkStatus & LINK_BIT_BLE_REC_TX) != 0U;
      if (timedOutIdle || stopRequested)
      {
        /* IDLEへ: 電力を食う ToF だけ SLEEP。センサー取得/送信は停止し、
         * 低頻度のIDLE_BEACONだけに切り替わる。 */
        ctx->state = STATE_IDLE;
        Sensors_Stop();
        Board_LedGreenOff();
        Board_LedRedOff();
        ctx->nextLedMs = now_ms;
        ctx->nextBeaconMs = now_ms;
      }
      else if (bleRecActive)
      {
        /* BLE録音の連続ストリーミング転送中: Secure側(comm_ble.cpp)が緑LED
         * (GPIOH PIN_7、board_io.cのLED_GREEN_PINと同一物理ピン)を専有点灯
         * させている間、NonSecure側のACTIVEハートビートトグルは行わない
         * (行うと同じピンを取り合って点滅して見えてしまう)。センサー取得/
         * UART・TCPテレメトリ送信サイクル自体は下のACTIVEブロックで通常通り
         * 続く - 止めるのはLEDだけ。 */
        ctx->state = STATE_ACTIVE_BLE_REC;
        ctx->nextLedMs = now_ms + CFG_ACTIVE_HB_MS;
      }
      else
      {
        if (ctx->state == STATE_ACTIVE_BLE_REC)
        {
          /* 録音送信が終わった: 通常のCOMMサイクルへ戻り、LEDハートビートを
           * ここから仕切り直す(録音中に溜まった経過時間でいきなり飛ばない)。 */
          ctx->state = STATE_ACTIVE_COMM;
          ctx->nextLedMs = now_ms;
        }
        if ((int32_t)(now_ms - ctx->nextLedMs) >= 0)
        {
          ctx->nextLedMs = now_ms + CFG_ACTIVE_HB_MS;
          Board_LedRedOff();
          Board_LedGreenToggle(); /* 緑 ハートビート */
        }
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
      ctx->nextBeaconMs = now_ms;
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
    /* IDLE: 要求「uartのdma/フラグ/BLE・WiFiコマンドを待機する以外の動作は
     * しない」を実装する中核部分。センサーの取得も送信も行わない。
     * 例外として照度・音圧だけは取得する - Trigger_Poll() の TRIG_LIGHT/
     * TRIG_AUDIO(=ファームウェア自身がフラグを立てて状態遷移する経路、
     * memo.md「書き込まれたファームによってトリガーを動作させることも可能」)
     * がIDLE中も生きるように。取得はするが送信はしない(トリガー判定専用)。 */
    SensorStore_AcquireTriggerInputs();

    /* データではなく「IDLEで生存中」だけを低頻度で知らせる。 */
    if ((int32_t)(now_ms - ctx->nextBeaconMs) >= 0)
    {
      ctx->nextBeaconMs = now_ms + CFG_IDLE_BEACON_MS;
      Comm_SendIdleBeacon((uint32_t)ctx->state);
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
      SensorStore_AcquireAll();
    }
    (void)Comm_SendTelemetry(SensorStore_GetForSend());
    ctx->commReturnMs = HAL_GetTick();
    /* BLE録音送信中(STATE_ACTIVE_BLE_REC)はCOMM/ACQUIREサイクル自体(センサー
     * 取得・UART/TCPテレメトリ送信)は続けるが、状態値はBLE_RECのまま維持する
     * (ここでCOMM/ACQUIREに上書きすると、上のswitch文が次周でLEDハートビート
     * を再開してしまい緑LEDの専有点灯が壊れる)。 */
    if (ctx->state != STATE_ACTIVE_BLE_REC)
    {
      ctx->state = STATE_ACTIVE_COMM;
    }

    /* 次回COMMは「前回の戻り + 周期」。大きく遅延したらバースト回避で resync。 */
    ctx->nextCommMs += CFG_COMM_PERIOD_MS;
    if ((int32_t)(ctx->commReturnMs - ctx->nextCommMs) > (int32_t)CFG_COMM_PERIOD_MS)
    {
      ctx->nextCommMs = ctx->commReturnMs + CFG_COMM_PERIOD_MS;
    }

    /* --- ACQUIRE --- 次フレーム用スナップショットを先読み。 */
    SensorStore_AcquireAll();
    ctx->haveAcquired = 1U;
    if (ctx->state != STATE_ACTIVE_BLE_REC)
    {
      ctx->state = STATE_ACTIVE_ACQUIRE;
    }
  }
}

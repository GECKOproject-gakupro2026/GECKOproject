/**
  ******************************************************************************
  * @file    app_state.h
  * @brief   デバイス状態機械(IDLE / ACTIVE)【コア層・基板非依存】。
  *
  *          app_loop.c から抽出したデバイス級のステートマシン。IDLEは
  *          「トリガー待機」状態で、通信は沈黙させず ToF だけ SLEEP させる
  *          (既存 App_Run の設計を踏襲)。ACTIVE は取得(ACQUIRE)と通信
  *          (COMM)のサブサイクルを持つ。
  *
  *          このヘッダは HAL も BSP も知らない。時刻(now_ms)と、リンク状態
  *          ビット(Comm_GetLinkStatus 相当)を引数で受け取り、LED/センサー/
  *          通信は契約ヘッダ(board_io.h / sensors.h / comm_api.h)経由で叩く。
  ******************************************************************************
  */
#ifndef APP_STATE_H
#define APP_STATE_H

#include "comm_dto.h" /* FullStatus_t */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* デバイス状態。ACTIVEはACQUIRE/COMMのサブ状態を持つが、外から見える主状態は
 * IDLE / ACTIVE の2つ。不明値は AppState_Tick 内の default で IDLE に落ちる。 */
typedef enum
{
  STATE_IDLE          = 0, /* 待機: 赤LED遅点滅・ToF SLEEP・通信は低頻度で継続 */
  STATE_ACTIVE_ACQUIRE = 1, /* 稼働(取得): 次フレーム用にセンサーを読む */
  STATE_ACTIVE_COMM    = 2, /* 稼働(通信): テレメトリを送る */
} AppState_t;

/* 状態機械のコンテキスト(App_Run が1つ保持し、毎周 AppState_Tick に渡す)。
 * 呼び出し側は AppState_Init で初期化するだけでよく、中身は触らない。 */
typedef struct
{
  AppState_t state;
  uint32_t   lastActivityMs;   /* 最後にトリガーが立った時刻 */
  uint32_t   nextTelemetryMs;  /* 次にテレメトリを送る時刻 */
  uint32_t   nextLedMs;        /* 次にLEDをトグルする時刻 */
  uint32_t   commReturnMs;     /* 直近のCOMM(送信)が戻った時刻(Step7で使用) */
  FullStatus_t st;             /* 最新スナップショット(Trigger_Poll にも渡す) */
} AppStateCtx_t;

/* コンテキストを初期化する(起動直後はACTIVE、now を基準に各タイマーを張る)。 */
void AppState_Init(AppStateCtx_t *ctx, uint32_t now_ms);

/* 状態機械を1周進める。now_ms は現在時刻、linkStatus は Comm_GetLinkStatus()
 * のビット(bit2=BLE接続中, bit3=TCPクライアント)。内部で Trigger_Poll /
 * build_status / Comm_SendTelemetry / LED / ToF を叩く。 */
void AppState_Tick(AppStateCtx_t *ctx, uint32_t now_ms, uint32_t linkStatus);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_H */

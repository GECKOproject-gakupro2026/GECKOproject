/**
  ******************************************************************************
  * @file    comm_backend.h
  * @brief   通信サービスの移植境界【契約層】。
  *
  *          ここに宣言された関数が、アプリ層と通信サービスの間の唯一の接点。
  *          実装は comm_service.cpp にある。
  *
  *          アプリ層(NonSecure)からは、これらの関数を直接は呼ばない。必ず
  *          アダプタ層を経由する:
  *
  *            TrustZoneあり基板:
  *              app --[comm_api.h]--> secure_nsc.c --[この境界]--> comm_service.cpp
  *                                     (CMSE検証 + ローカルコピー)
  *
  *            TrustZoneなし基板:
  *              app --[comm_api.h]--> comm_api_direct.c --[この境界]--> comm_service.cpp
  *                                     (単に転送するだけ)
  *
  *          【アダプタ層の責務】この境界に渡すポインタは、呼び出し前に検証済み
  *          であること。comm_service.cpp 側は、渡されたポインタを信頼して使う。
  *          TrustZone基板では secure_nsc.c が cmse_check_address_range で検証し、
  *          Secureローカルへコピーしてから渡している。この保証を弱めてはならない
  *          (OTAの信頼ルートがこの境界の内側で完結していることが安全性の根拠)。
  *
  *          【凍結】既存関数(CommBridge_Poll〜CommBridge_GetAudioBuffer、
  *          7個)のシグネチャと戻り値の意味は変更禁止。新規関数の追加は可。
  ******************************************************************************
  */
#ifndef COMM_BACKEND_H
#define COMM_BACKEND_H

#include "comm_dto.h"   /* FullStatus_t, COMM_POLL_* */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通信サービスを1回分駆動する */
void CommBridge_Poll(void);

/* テレメトリを送信する。stは検証済み・コピー済みのポインタであること。
 * 戻り値: 0=受理、負=エラー */
int CommBridge_SendTelemetry(const FullStatus_t *st);

/* ホスト受信バイトを1つ取り出す。
 * 戻り値: COMM_POLL_BYTE / COMM_POLL_ACTIVITY / COMM_POLL_NONE */
int CommBridge_PollHostCommand(uint8_t *out);

/* テレメトリ送信を止める/再開する（0=止める, 1=再開） */
void CommBridge_SetTelemetryEnabled(uint32_t on);

/* 接続状態ビット: bit0=BLE生存, bit1=Wi-Fi接続, bit2=BLE接続中, bit3=TCP接続中 */
uint32_t CommBridge_GetLinkStatus(void);

/* 音声窓をコピーする。戻り値: コピーしたサンプル数 */
uint32_t CommBridge_GetAudioBuffer(int16_t *dst, uint32_t maxSamples);

/* dstのうちMCU情報フィールド(die_temp, vdda, sysclk, hclk, reset_cause,
 * cpu_load, flash_kb, uid, idcode, ram_used, ram_total, heap_used,
 * heap_free, flash_used, flash_total)とble_alive, wifi_aliveだけを
 * 上書きする。
 * 他フィールドは呼び出し元の値のまま。NonSecure側にこれらの値のソースが
 * 無いための専用ゲートウェイ。 */
void CommBridge_GetMcuInfo(FullStatus_t *dst);

/* 起動成功を起動監視(BootGuard)に伝える。実装は boot_guard.cpp */
void BootGuard_ConfirmBoot(void);

#ifdef __cplusplus
}
#endif

#endif /* COMM_BACKEND_H */

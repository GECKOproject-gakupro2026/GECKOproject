/**
  ******************************************************************************
  * @file    comm_api.h
  * @brief   アプリ層(NonSecure)から通信サービスを呼ぶための唯一の窓口【契約層】。
  *
  *          アプリ側のコードは、この関数群の実体がどう実装されているかを
  *          知らなくてよい:
  *            - TrustZoneあり基板: Secure/Core/Src/secure_nsc.c のCMSEゲートウェイ
  *            - TrustZoneなし基板: portability/comm_api_direct.c の直接呼び出し
  *          どちらの場合もこのヘッダは変更しない。だからアプリ層(app_loop.c,
  *          sensors.c, ns_audio.c)は無改造で移植できる。
  *
  *          【重要】既存関数(Comm_Poll〜Comm_GetAudioBuffer、7個)のシグネチャと
  *          戻り値の意味は凍結。変更禁止。新規関数の追加は可。
  ******************************************************************************
  */
#ifndef COMM_API_H
#define COMM_API_H

#include "comm_dto.h"   /* FullStatus_t(165バイト固定) と COMM_POLL_* */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通信サービスを1回分駆動する。アプリのメインループから毎周回必ず呼ぶこと。
 * これを呼ばないと受信もOTAも進まない。 */
void Comm_Poll(void);

/* テレメトリ1件を通信サービスに渡して送信させる。
 * 戻り値: 0=受理、負=エラー(不正ポインタ等) */
int Comm_SendTelemetry(const FullStatus_t *st);

/* ホストからの受信を1バイト取り出す。低消費電力モードからの復帰検知源。
 * 戻り値: COMM_POLL_BYTE(*outにバイトあり) / COMM_POLL_ACTIVITY(バイトは無いが
 *         受信はあった) / COMM_POLL_NONE(何もなし) / 負=不正ポインタ */
int Comm_PollHostCommand(uint8_t *out);

/* 通信サービスのテレメトリ送信を止める/再開する（低消費電力用）。
 * 0=止める、1=再開する。止めても受信は生きているのでウェイクできる。 */
void Comm_SetTelemetryEnabled(uint32_t on);

/* 接続状態のビット: bit0=BLE生存, bit1=Wi-Fi接続済, bit2=BLE接続中, bit3=TCPクライアント接続中 */
uint32_t Comm_GetLinkStatus(void);

/* 通信サービス側が録っている音声窓をコピーして受け取る。
 * 戻り値: 実際にコピーされたサンプル数 */
uint32_t Comm_GetAudioBuffer(int16_t *dst, uint32_t maxSamples);

/* MCU情報(ダイ温度・電圧・クロック・Flashサイズ・UID・リセット要因・CPU負荷・
 * RAM/Flash使用量)とble_alive, wifi_aliveを*dstに埋める(他フィールドは
 * 変更しない)。
 * これらの値はSecure専用リソース(内蔵ADC・Secureリンカシンボル・DBGMCU)
 * 由来でNonSecure側に情報源が無いため、Comm_SendTelemetry()の前にこれを
 * 呼んで埋めること。 */
void Comm_GetMcuInfo(FullStatus_t *dst);

/* 起動が成功したことを通信サービス側の起動監視に伝える。
 * これを呼ばないまま数回リセットが続くと、前のファームへ自動ロールバックされる。 */
void Secure_ConfirmBoot(void);

#ifdef __cplusplus
}
#endif

#endif /* COMM_API_H */

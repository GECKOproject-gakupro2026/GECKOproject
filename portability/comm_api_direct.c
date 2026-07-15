/**
  ******************************************************************************
  * @file    comm_api_direct.c
  * @brief   【TrustZoneなし基板用】comm_api.h の実装テンプレート。
  *
  *          ★このファイルは現在のプロジェクトではビルドされない。
  *            TrustZoneなしの基板へ移植するとき、secure_nsc.c の代わりに
  *            新プロジェクトへコピーして使う。
  *
  *          TrustZoneがあると、アプリ(NonSecure)と通信サービス(Secure)は
  *          別のメモリ空間にいるため、境界を越えるポインタを検証し、
  *          Secure側へコピーする必要があった(secure_nsc.c がそれをしている)。
  *
  *          TrustZoneが無ければ両者は同じメモリ空間にいるので、検証もコピーも
  *          要らない。単に転送するだけでよい。それがこのファイル。
  *
  *          【重要】TrustZoneが無くなっても、Secure/NonSecureの「設計上の区分」は
  *          維持すること:
  *            - 基礎領域(通信・OTA・起動監視) = 更新しない、壊すと復旧不能
  *            - アプリ領域(センサー・アプリロジック) = OTAで更新する
  *          ハードウェアによる隔離が無くなるだけで、フラッシュ配置・OTAの更新単位・
  *          起動時のイメージ検証(Stage-0)・ロールバック(BootGuard)はそのまま残す。
  *          詳しくは PORTING.md を読むこと。
  ******************************************************************************
  */
#include "comm_api.h"
#include "comm_backend.h"

#include <stddef.h>

void Comm_Poll(void)
{
  CommBridge_Poll();
}

int Comm_SendTelemetry(const FullStatus_t *st)
{
  if (st == NULL)
  {
    return -1;
  }
  /* TrustZoneなし: 同一メモリ空間なので、検証もローカルコピーも不要。
   * そのまま渡す。 */
  return CommBridge_SendTelemetry(st);
}

int Comm_PollHostCommand(uint8_t *out)
{
  if (out == NULL)
  {
    return -1;
  }
  return CommBridge_PollHostCommand(out);
}

void Comm_SetTelemetryEnabled(uint32_t on)
{
  CommBridge_SetTelemetryEnabled(on);
}

uint32_t Comm_GetLinkStatus(void)
{
  return CommBridge_GetLinkStatus();
}

uint32_t Comm_GetAudioBuffer(int16_t *dst, uint32_t maxSamples)
{
  if (dst == NULL || maxSamples == 0U)
  {
    return 0U;
  }
  return CommBridge_GetAudioBuffer(dst, maxSamples);
}

void Comm_GetMcuInfo(FullStatus_t *dst)
{
  if (dst == NULL)
  {
    return;
  }
  /* TrustZoneなし: 同一メモリ空間なので、検証もローカルコピーも不要。 */
  CommBridge_GetMcuInfo(dst);
}

void Secure_ConfirmBoot(void)
{
  BootGuard_ConfirmBoot();
}

/**
  ******************************************************************************
  * @file    triggers.h
  * @brief   ACTIVE復帰トリガーの抽象化【契約層】。
  *
  *          app_loop.c のIDLEステートマシンは「何かトリガーが起きたか」しか
  *          知らなくてよい。トリガー源の追加(照度・音圧センサーなど)は、この
  *          ヘッダのシグネチャを変えずに triggers.c 側だけを拡張して行う。
  *
  *          現状のトリガー源:
  *            - TRIG_COMM  : ホストからの通信(UART/BLE/TCP経由のOTAも含む)
  *            - TRIG_LIGHT : 照度センサーがしきい値を超えた(既定は無効)
  *            - TRIG_AUDIO : 音圧(RMS)がしきい値を超えた(既定は無効)
  *
  *          しきい値・トリガー状態は FullStatus_t には入れない(165バイト固定
  *          レイアウトを変更しないため)。triggers.c 内部の static 変数で持つ。
  ******************************************************************************
  */
#ifndef TRIGGERS_H
#define TRIGGERS_H

#include "comm_dto.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  TRIG_NONE  = 0,
  TRIG_COMM  = 1,
  TRIG_LIGHT = 2,
  TRIG_AUDIO = 3,
} TriggerSource_t;

/* 直近の呼び出し以降に発火したトリガー源を1つ返す(複数同時発火時はTRIG_COMM
 * を優先)。発火が無ければ TRIG_NONE。呼ぶたびに内部状態はクリアされる。
 * st には Sensors_Refresh()/Audio_Refresh() 済みの最新スナップショットを渡す
 * こと(照度・音圧トリガーの判定に使う)。 */
TriggerSource_t Trigger_Poll(const FullStatus_t *st);

/* 照度トリガーのしきい値を設定する。raw は st->light_raw と同じ単位。
 * 0 を渡すと無効化(既定は無効)。 */
void Trigger_SetLightThreshold(uint32_t raw);

/* 音圧トリガーのしきい値を設定する。rms は st->audio_rms と同じ単位。
 * 0 を渡すと無効化(既定は無効)。 */
void Trigger_SetAudioThreshold(int16_t rms);

#ifdef __cplusplus
}
#endif

#endif /* TRIGGERS_H */

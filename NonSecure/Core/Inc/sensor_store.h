/**
  ******************************************************************************
  * @file    sensor_store.h
  * @brief   センサーデータの単一保管庫【コア層・基板非依存】。
  *
  *          プロジェクト全体でこの1インスタンスの FullStatus_t のみを共有する。
  *          「データ取得」状態が Acquire* で上書きし、「データ送信」状態が
  *          GetForSend() で読むだけ、という役割分担をコード上に固定するための
  *          薄いモジュール。FullStatus_t 自体のレイアウト(165B固定)は変えない
  *          - 変えるのは所有権の置き場所だけ。
  ******************************************************************************
  */
#ifndef SENSOR_STORE_H
#define SENSOR_STORE_H

#include "comm_dto.h" /* FullStatus_t */

#ifdef __cplusplus
extern "C" {
#endif

void SensorStore_Init(void);

/* データ取得状態: 全センサー(env/motion/tof/light/audio/MCU情報)を読んで
 * 保管庫を上書きする。旧 app_state.c の build_status() の実体。 */
void SensorStore_AcquireAll(void);

/* データ送信状態: 保管庫の現在値をそのまま返す(取得はしない)。 */
const FullStatus_t *SensorStore_GetForSend(void);

/* Trigger_Poll() 等、読み取り専用アクセスに使う(GetForSend と同じ実体)。 */
const FullStatus_t *SensorStore_Peek(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_STORE_H */

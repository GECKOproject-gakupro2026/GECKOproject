/**
  ******************************************************************************
  * @file    sensors.h
  * @brief   NonSecure app-layer sensor scheduling (env/motion/light/ToF).
  ******************************************************************************
  */
#ifndef SENSORS_H
#define SENSORS_H

#include "comm_dto.h"

#ifdef __cplusplus
extern "C" {
#endif

void Sensors_Init(void);
void Sensors_Refresh(FullStatus_t *st);

/* IDLE用の例外取得: 照度だけを自身の周期で更新する(env/motion/tofには触れない)。
 * Trigger_Poll() の TRIG_LIGHT 判定に使う。 */
void Sensors_RefreshLightOnly(FullStatus_t *st);

/* 実行時にセンサーの取得周期を変更する(FRAME_CMD_SET_SENSOR_RATE経由)。
 * sensor_id: 0=env, 1=light, 2=tof, 3=motion(0=毎回)。
 * ハード制約の下限を割る値は自動的にクランプされる。 */
void Sensors_SetPeriod(uint8_t sensor_id, uint16_t period_ms);

/* Low-power mode support (Phase E): stop/resume the ToF ranging cycle. */
void Sensors_Stop(void);
void Sensors_Resume(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */

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

/* Low-power mode support (Phase E): stop/resume the ToF ranging cycle. */
void Sensors_Stop(void);
void Sensors_Resume(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */

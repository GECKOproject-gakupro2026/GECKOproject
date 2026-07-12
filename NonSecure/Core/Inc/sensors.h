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

/* Low-power mode support (Phase E): stop/resume the ToF ranging cycle. */
void Sensors_Stop(void);
void Sensors_Resume(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */

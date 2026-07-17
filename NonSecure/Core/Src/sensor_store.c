/**
  ******************************************************************************
  * @file    sensor_store.c
  * @brief   sensor_store.h の実装。See sensor_store.h.
  ******************************************************************************
  */
#include "sensor_store.h"

#include "board_io.h"
#include "comm_api.h"
#include "ns_audio.h"
#include "sensors.h"

#include "main.h" /* HAL_GetTick */

static FullStatus_t s_status;

void SensorStore_Init(void)
{
  s_status = (FullStatus_t){0};
}

void SensorStore_AcquireAll(void)
{
  Sensors_Refresh(&s_status);
  Audio_Refresh(&s_status);
  Comm_GetMcuInfo(&s_status);
  s_status.ver = 2U;
  s_status.uptime_ms = HAL_GetTick();
  s_status.button = Board_ButtonRead();
}

void SensorStore_AcquireTriggerInputs(void)
{
  Sensors_RefreshLightOnly(&s_status);
  Audio_Refresh(&s_status);
}

const FullStatus_t *SensorStore_GetForSend(void)
{
  return &s_status;
}

const FullStatus_t *SensorStore_Peek(void)
{
  return &s_status;
}

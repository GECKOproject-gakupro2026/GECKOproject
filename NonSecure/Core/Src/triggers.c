/**
  ******************************************************************************
  * @file    triggers.c
  * @brief   triggers.h の実装【コア層・基板非依存】。
  *
  *          HAL/BSPを直接叩かず、comm_api.h経由の通信トリガーとFullStatus_t
  *          の値を見るだけ。基板を変えても無改造で移植できる。
  ******************************************************************************
  */
#include "triggers.h"

#include "comm_api.h"

#include <stddef.h>

static uint32_t s_lightThreshold = 0U; /* 0=無効(既定) */
static int16_t s_audioThreshold = 0;   /* 0=無効(既定) */

TriggerSource_t Trigger_Poll(const FullStatus_t *st)
{
  /* 通信トリガー: 任意のホスト受信バイト/アクティビティ。最優先。 */
  uint8_t cmdByte = 0U;
  if (Comm_PollHostCommand(&cmdByte) != COMM_POLL_NONE)
  {
    return TRIG_COMM;
  }

  if (st != NULL)
  {
    if (s_lightThreshold != 0U && st->light_raw >= s_lightThreshold)
    {
      return TRIG_LIGHT;
    }
    if (s_audioThreshold != 0 && st->audio_rms >= s_audioThreshold)
    {
      return TRIG_AUDIO;
    }
  }

  return TRIG_NONE;
}

void Trigger_SetLightThreshold(uint32_t raw)
{
  s_lightThreshold = raw;
}

void Trigger_SetAudioThreshold(int16_t rms)
{
  s_audioThreshold = rms;
}

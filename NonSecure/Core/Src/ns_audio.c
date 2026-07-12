/**
  ******************************************************************************
  * @file    ns_audio.c
  * @brief   TrustZone app-layer refactor Phase D (revised): audio CAPTURE
  *          (MIC2/MDF1 + PLL3 + DMA) stays Secure - moving its DMA/clock
  *          chain to NonSecure proved fragile (DMA never transferred). This
  *          NonSecure module pulls the live mic window from Secure via the
  *          Comm_GetAudioBuffer NSC gateway, computes the RMS/peak/waveform
  *          telemetry fields, and exposes the buffer to the AI inference path.
  ******************************************************************************
  */
#include "main.h"
#include "ns_audio.h"
#include "secure_nsc.h"

#include <math.h>

#define AUDIO_SAMPLES  2048U  /* matches the Secure capture buffer size */

static int16_t s_audioBuf[AUDIO_SAMPLES];
static uint32_t s_audioCount;

void Audio_Init(void)
{
  /* Nothing to init on the NonSecure side - capture is Secure-owned. */
  s_audioCount = 0U;
}

void Audio_Stop(void)
{
  /* No-op: capture is Secure-owned. Low-power gating (Phase E) just stops
   * pulling/using the buffer. */
}

void Audio_Resume(void)
{
}

void Audio_Refresh(FullStatus_t *st)
{
  s_audioCount = Comm_GetAudioBuffer(s_audioBuf, AUDIO_SAMPLES);
  if (s_audioCount == 0U)
  {
    return;
  }

  int32_t sum = 0;
  for (uint32_t i = 0; i < s_audioCount; i++)
  {
    sum += s_audioBuf[i];
  }
  int16_t mean = (int16_t)(sum / (int32_t)s_audioCount);

  int64_t sqSum = 0;
  int32_t peak = 0;
  for (uint32_t i = 0; i < s_audioCount; i++)
  {
    int32_t v = s_audioBuf[i] - mean;
    sqSum += (int64_t)v * v;
    if (v > peak) peak = v;
    if (-v > peak) peak = -v;
  }
  st->audio_rms = (int16_t)sqrtf((float)sqSum / (float)s_audioCount);
  st->audio_peak = (int16_t)peak;

  const uint32_t stride = s_audioCount / 32U;
  if (stride > 0U)
  {
    for (uint32_t i = 0; i < 32U; i++)
    {
      st->wave[i] = (int16_t)(s_audioBuf[i * stride] - mean);
    }
  }
}

const int16_t *Audio_GetBuffer(uint32_t *count)
{
  *count = s_audioCount;
  return s_audioBuf;
}

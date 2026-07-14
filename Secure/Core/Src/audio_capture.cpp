/**
  ******************************************************************************
  * @file    audio_capture.cpp
  * @brief   audio_capture.hpp の MIC2(MDF1 + GPDMA + PLL3) 向け実装【port層】。
  ******************************************************************************
  */
#include "audio_capture.hpp"

#include "app_config.h"   /* CFG_AUDIO_SAMPLE_RATE */
#include "main.h"

#include "b_u585i_iot02a_audio.h"

#include <cstdio>

/* CubeMX-generated ADF1 handle (main.c), released before the BSP takes over */
extern "C" MDF_HandleTypeDef AdfHandle0;

/* Shared BSP audio DMA event flags */
extern "C" volatile uint32_t g_AudioEvents;
extern "C" volatile uint32_t g_AudioErrors;

namespace audio_capture
{
namespace
{
constexpr size_t kAudioSamples = 2048;   /* circular capture buffer */
int16_t audioBuf[kAudioSamples];

/* ADF1 kernel clock: CubeMX MspInit forces HCLK; restore the BSP's PLL3 */
void reselectAudioPll3()
{
  RCC_PeriphCLKInitTypeDef cfg = {};
  cfg.PLL3.PLL3Source = RCC_PLLSOURCE_MSI;
  cfg.PLL3.PLL3M = 1;
  cfg.PLL3.PLL3N = 80;
  cfg.PLL3.PLL3P = 28;
  cfg.PLL3.PLL3Q = 28;
  cfg.PLL3.PLL3R = 2;
  cfg.PLL3.PLL3ClockOut = RCC_PLL3_DIVQ;
  cfg.PeriphClockSelection = RCC_PERIPHCLK_MDF1;
  cfg.Mdf1ClockSelection = RCC_MDF1CLKSOURCE_PLL3;
  (void)HAL_RCCEx_PeriphCLKConfig(&cfg);
}

} // namespace

bool Init()
{
  /* Phase D revised: audio capture (MIC2/MDF1, DMA via PLL3) stays Secure.
   * Only AI inference moved to NonSecure - it reads audioBuf through the
   * Comm_GetAudioBuffer NSC gateway. */
  bool audioOk = false;

  static bool mxAdfReleased = false;
  if (!mxAdfReleased)
  {
    HAL_MDF_DeInit(&AdfHandle0);
    mxAdfReleased = true;
  }

  BSP_AUDIO_Init_t init = {};
  init.Device = AUDIO_IN_DEVICE_DIGITAL_MIC2;
  init.SampleRate = CFG_AUDIO_SAMPLE_RATE;
  init.BitsPerSample = AUDIO_RESOLUTION_16B;
  init.ChannelsNbr = 1;
  init.Volume = 100;
  if (BSP_AUDIO_IN_Init(0, &init) != BSP_ERROR_NONE)
  {
    printf("[TLM] audio init failed\r\n");
    return audioOk;
  }
  reselectAudioPll3();

  /* TrustZone: secure+privileged DMA channel, secure source/destination */
  (void)HAL_DMA_ConfigChannelAttributes(
      &haudio_mdf[1],
      DMA_CHANNEL_SEC | DMA_CHANNEL_PRIV | DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC);

  if (BSP_AUDIO_IN_Record(0, reinterpret_cast<uint8_t *>(audioBuf),
                          sizeof(audioBuf)) != BSP_ERROR_NONE)
  {
    printf("[TLM] audio record start failed\r\n");
    BSP_AUDIO_IN_DeInit(0);
    return audioOk;
  }
  audioOk = true;
  return audioOk;
}

const int16_t *Buffer() { return audioBuf; }

uint32_t Samples() { return kAudioSamples; }

} // namespace audio_capture

/* AI推論パス用の音声窓（C linkage）。telemetry.cpp から移動。 */
extern "C" const int16_t *Telemetry_GetAudioBuffer(uint32_t *count)
{
  *count = audio_capture::Samples();
  return audio_capture::Buffer();
}

/* 音声DMAのイベントフラグ。telemetry.cpp から移動。
 * 【重要】volatile を落とさないこと。 */
extern "C" volatile uint32_t g_AudioEvents = 0; /* bit0 = half, bit1 = full */
extern "C" volatile uint32_t g_AudioErrors = 0;

/* BSPオーディオのDMAコールバック。telemetry.cpp から移動。
 * 【重要】プロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void BSP_AUDIO_IN_HalfTransfer_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioEvents |= 1U;
}

extern "C" void BSP_AUDIO_IN_TransferComplete_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioEvents |= 2U;
}

extern "C" void BSP_AUDIO_IN_Error_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioErrors = g_AudioErrors + 1U;
}

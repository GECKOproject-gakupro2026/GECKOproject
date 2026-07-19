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

/* Pitch correction ratio for the MIC2/MDF1 capture path, calibrated
 * empirically against on-target FFT sweeps
 * (pc_side/mic_freq_response/sweep.py, 300Hz-4000Hz).
 *
 * The naive approach - measuring the effective sample rate from the pitch
 * shift alone (~15682Hz vs nominal 16000Hz, ratio 0.9802) and using that
 * ratio directly as the resample step - overcorrected: the DMA
 * half/full-complete interrupt cadence (which is what actually paces
 * FeedForResample() calls) runs about 4.5% faster than that ratio predicts,
 * for reasons not fully root-caused (see
 * docs/refactoring/計画_IDLE再定義とトリガー抽象化と不要コード削除.md 付録).
 * kResampleStep = 1.0196 (> 1, i.e. output runs slightly slower than input -
 * a mild downsample) was tuned directly against sweep.py output until the
 * measured/input frequency ratio landed at 1.000 (0.9998-1.0000 across
 * 300Hz-3000Hz, std-dev 0.01%). If the mic or its clocking changes, re-tune
 * this constant the same way rather than trusting the naive derivation. */
constexpr float kResampleStep = 1.0196f;

/* Output queue for FeedForResample()/PopResampled(). Sized generously for
 * either up- or down-sampling. */
constexpr size_t kResampleQueueCap = 2048;
int16_t resampleQueue[kResampleQueueCap];
size_t resampleQueueHead = 0; /* next sample to pop */
size_t resampleQueueTail = 0; /* next free slot to push */
size_t resampleQueueCount = 0;

/* Phase carried across successive FeedForResample() calls so the resampler
 * treats the input as one continuous stream instead of restarting at
 * pos=0 for every isolated 512-sample frame - a per-frame restart only
 * consumes ~502 of each frame's 512 input samples, silently dropping ~10
 * samples at every frame boundary (audible clicks, and it corrupted the
 * FFT peak at anything above ~1kHz in on-target sweeps).
 *
 * Modelled as one virtual stream v[] = [prevLastSample, src[0], src[1], ...]
 * per call, i.e. the previous call's final sample prepended to this call's
 * src[]. resamplePhase is the fractional position into v[], always in
 * [0, 1) at entry (so v[0]/v[1] - prevLastSample/src[0] - is the first
 * interpolation pair). */
float resamplePhase = 0.0f;
int16_t resamplePrevLastSample = 0;
bool resampleHavePrevSample = false;

void queuePush(int16_t sample)
{
  if (resampleQueueCount >= kResampleQueueCap)
  {
    return; /* drop rather than overwrite unread data */
  }
  resampleQueue[resampleQueueTail] = sample;
  resampleQueueTail = (resampleQueueTail + 1U) % kResampleQueueCap;
  resampleQueueCount++;
}

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

void FeedForResample(const int16_t *src, uint32_t inCount)
{
  if (src == nullptr || inCount == 0U)
  {
    return;
  }
  /* Virtual stream v[] for this call: v[0] = previous call's last sample
   * (or src[0] itself on the very first call, giving frac=0 there), then
   * v[1..inCount] = src[0..inCount-1]. Sample count = inCount + 1. */
  const int16_t firstV = resampleHavePrevSample ? resamplePrevLastSample : src[0];

  auto vAt = [&](uint32_t vIdx) -> int16_t {
    return (vIdx == 0U) ? firstV : src[vIdx - 1U];
  };

  /* pos in [0,1) at entry; walk it across v[], producing one interpolated
   * output per kResampleStep advance, until the next output would need
   * v[inCount+1] (one past what this call's virtual stream provides).
   * The leftover phase (< kResampleStep, so < 1 sample either way) carries
   * into the next call regardless of whether kResampleStep is above or
   * below 1 (down- vs up-sampling). */
  float pos = resamplePhase;
  const uint32_t vCount = inCount + 1U;
  for (;;)
  {
    uint32_t idx = static_cast<uint32_t>(pos);
    if (idx + 1U >= vCount)
    {
      break; /* would need v[vCount]; stop and carry the phase forward */
    }
    float frac = pos - static_cast<float>(idx);
    float a = static_cast<float>(vAt(idx));
    float b = static_cast<float>(vAt(idx + 1U));
    queuePush(static_cast<int16_t>(a + (b - a) * frac));
    pos += kResampleStep;
  }
  /* Rebase the leftover phase onto the next call's virtual stream: pos
   * currently indexes v[] = [prevLast, src[0..inCount-1]] (inCount+1
   * samples); the next call's v[0] will be *this* call's src[inCount-1],
   * which is v[inCount] here. So subtract inCount to rebase. */
  resamplePhase = pos - static_cast<float>(inCount);
  resamplePrevLastSample = src[inCount - 1U];
  resampleHavePrevSample = true;
}

uint32_t ResampledAvailable()
{
  return static_cast<uint32_t>(resampleQueueCount);
}

void PopResampled(int16_t *dst, uint32_t outCount)
{
  if (dst == nullptr)
  {
    return;
  }
  for (uint32_t i = 0; i < outCount; i++)
  {
    if (resampleQueueCount == 0U)
    {
      dst[i] = 0; /* underrun guard; caller should check ResampledAvailable() first */
      continue;
    }
    dst[i] = resampleQueue[resampleQueueHead];
    resampleQueueHead = (resampleQueueHead + 1U) % kResampleQueueCap;
    resampleQueueCount--;
  }
}

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
/* DMA half/full 取りこぼし検出: コールバックが立てようとしたビットが既に
 * 立っていた(=前回のフラグをまだ poll() が処理していない)回数。0以外なら
 * poll() の呼び出しが DMA ペース(64ms/半)に追いつけておらず、その半バッファ
 * 分の音声が失われている(録音のズレ/末尾無音の直接原因)。 */
extern "C" volatile uint32_t g_AudioDmaOverruns = 0;

/* BSPオーディオのDMAコールバック。telemetry.cpp から移動。
 * 【重要】プロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void BSP_AUDIO_IN_HalfTransfer_CallBack(uint32_t Instance)
{
  (void)Instance;
  if ((g_AudioEvents & 1U) != 0U)
  {
    g_AudioDmaOverruns++; /* 前回の half をまだ処理していないのに次が来た */
  }
  g_AudioEvents |= 1U;
}

extern "C" void BSP_AUDIO_IN_TransferComplete_CallBack(uint32_t Instance)
{
  (void)Instance;
  if ((g_AudioEvents & 2U) != 0U)
  {
    g_AudioDmaOverruns++; /* 前回の full をまだ処理していないのに次が来た */
  }
  g_AudioEvents |= 2U;
}

extern "C" void BSP_AUDIO_IN_Error_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioErrors = g_AudioErrors + 1U;
}

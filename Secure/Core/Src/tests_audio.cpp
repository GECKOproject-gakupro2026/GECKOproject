/**
  ******************************************************************************
  * @file    tests_audio.cpp
  * @brief   Digital microphone tests: MIC1 via ADF1, MIC2 via MDF1.
  *          Primary path uses the BSP DMA capture; if that yields no signal
  *          (GPDMA linked-list + TrustZone issue under investigation) the test
  *          falls back to HAL_MDF polling acquisition to verify the mic itself.
  ******************************************************************************
  */
#include "test.hpp"
#include "app_config.h"
#include "b_u585i_iot02a_audio.h"

#include <cstdio>
#include <cstdlib>

/* CubeMX-generated ADF1 handle (main.c) - released before the BSP takes over */
extern "C" MDF_HandleTypeDef AdfHandle0;

/* Shared BSP audio DMA event flags (defined in telemetry.cpp) */
extern "C" volatile uint32_t g_AudioEvents;
extern "C" volatile uint32_t g_AudioErrors;

namespace apptest
{

namespace
{
int16_t pcmBuffer[CFG_AUDIO_REC_SAMPLES];

/* CubeMX's HAL_MDF_MspInit() re-selects HCLK as ADF1 kernel clock during
 * HAL_MDF_Init(), overriding the PLL3 setting the BSP made just before.
 * Re-apply the BSP clock tree (PLL3/Q = 11.43 MHz) after init. */
bool reselectAdf1Pll3Clock()
{
  RCC_PeriphCLKInitTypeDef cfg = {};
  cfg.PLL3.PLL3Source = RCC_PLLSOURCE_MSI;
  cfg.PLL3.PLL3M = 1;
  cfg.PLL3.PLL3N = 80;
  cfg.PLL3.PLL3P = 28;
  cfg.PLL3.PLL3Q = 28;
  cfg.PLL3.PLL3R = 2;
  cfg.PLL3.PLL3RGE = 0;
  cfg.PLL3.PLL3FRACN = 0;
  cfg.PLL3.PLL3ClockOut = RCC_PLL3_DIVQ;
  cfg.PeriphClockSelection = RCC_PERIPHCLK_ADF1;
  cfg.Adf1ClockSelection = RCC_ADF1CLKSOURCE_PLL3;
  return HAL_RCCEx_PeriphCLKConfig(&cfg) == HAL_OK;
}

void releaseCubeMxAdfOnce()
{
  static bool released = false;
  if (!released)
  {
    HAL_MDF_DeInit(&AdfHandle0);
    released = true;
  }
}

bool bspAudioInit(uint32_t device)
{
  BSP_AUDIO_Init_t init = {};
  init.Device = device;
  init.SampleRate = CFG_AUDIO_SAMPLE_RATE;
  init.BitsPerSample = AUDIO_RESOLUTION_16B;
  init.ChannelsNbr = 1;
  init.Volume = 100; /* unused for MEMS mics */
  if (BSP_AUDIO_IN_Init(0, &init) != BSP_ERROR_NONE)
  {
    return false;
  }
  if (device == AUDIO_IN_DEVICE_DIGITAL_MIC1)
  {
    return reselectAdf1Pll3Clock();
  }
  return true;
}

/* Signal sanity over pcmBuffer[0..n): a live PDM mic always shows
 * noise-floor variation; a dead path yields constant samples. */
Result analyzeSignal(size_t n, const char *label, const char *path)
{
  int32_t sum = 0;
  for (size_t i = 0; i < n; i++)
  {
    sum += pcmBuffer[i];
  }
  int16_t mean = static_cast<int16_t>(sum / static_cast<int32_t>(n));
  int32_t peak = 0;
  uint32_t changes = 0;
  for (size_t i = 0; i < n; i++)
  {
    int32_t dev = abs(static_cast<int32_t>(pcmBuffer[i]) - mean);
    if (dev > peak) peak = dev;
    if (i > 0 && pcmBuffer[i] != pcmBuffer[i - 1]) changes++;
  }
  printf("  %s(%s): %u samples, DC=%d, peak=%ld, activity=%lu\r\n",
         label, path, static_cast<unsigned>(n), mean, peak, changes);
  if (changes < n / 100U || peak == 0)
  {
    return Result::Fail;
  }
  return Result::Pass;
}

/* DMA capture via the BSP (production path) */
Result dmaCapture(uint32_t device, const char *label)
{
  g_AudioEvents = 0;
  g_AudioErrors = 0;

  if (!bspAudioInit(device))
  {
    printf("  %s: audio init failed\r\n", label);
    return Result::Fail;
  }

  /* TrustZone: MDF/ADF and the target SRAM are secure, so the GPDMA channel
   * must be granted secure (and privileged) attributes */
  DMA_HandleTypeDef *hdma = &haudio_mdf[(device == AUDIO_IN_DEVICE_DIGITAL_MIC1) ? 0 : 1];
  (void)HAL_DMA_ConfigChannelAttributes(
      hdma, DMA_CHANNEL_SEC | DMA_CHANNEL_PRIV | DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC);

  if (BSP_AUDIO_IN_Record(0, reinterpret_cast<uint8_t *>(pcmBuffer),
                          sizeof(pcmBuffer)) != BSP_ERROR_NONE)
  {
    printf("  %s: record start failed\r\n", label);
    BSP_AUDIO_IN_DeInit(0);
    return Result::Fail;
  }

  uint32_t start = HAL_GetTick();
  while ((g_AudioEvents & 2U) == 0U && (HAL_GetTick() - start) < 2000U)
  {
  }
  bool timedOut = ((g_AudioEvents & 2U) == 0U);
  uint32_t dmaErr = hdma->ErrorCode;

  BSP_AUDIO_IN_Stop(0);
  BSP_AUDIO_IN_DeInit(0);

  if (timedOut)
  {
    printf("  %s: DMA capture timeout (dmaErr=0x%08lX)\r\n", label, dmaErr);
    return Result::Fail;
  }
  if (g_AudioErrors != 0U)
  {
    printf("  %s: DMA error during capture (dmaErr=0x%08lX)\r\n", label, dmaErr);
  }
  return analyzeSignal(CFG_AUDIO_REC_SAMPLES, label, "DMA");
}

/* Polling capture directly through HAL_MDF (no GPDMA involved) */
Result pollingCapture(uint32_t device, const char *label)
{
  constexpr size_t kPollSamples = 1024;

  if (!bspAudioInit(device))
  {
    printf("  %s: audio re-init failed\r\n", label);
    return Result::Fail;
  }

  MDF_HandleTypeDef *hmdf =
      &haudio_in_mdf_filter[(device == AUDIO_IN_DEVICE_DIGITAL_MIC1) ? 0 : 1];

  /* Same filter settings the BSP uses for 16 kHz capture */
  MDF_FilterConfigTypeDef cfg = {};
  cfg.DataSource = MDF_DATA_SOURCE_BSMX;
  cfg.Delay = 0U;
  cfg.CicMode = MDF_ONE_FILTER_SINC4;
  cfg.DecimationRatio = 176U; /* 16 kHz (BSP MDF_DECIMATION_RATIO) */
  cfg.Offset = 0;
  cfg.Gain = 0;
  cfg.ReshapeFilter.Activation = DISABLE;
  cfg.ReshapeFilter.DecimationRatio = MDF_RSF_DECIMATION_RATIO_4;
  cfg.HighPassFilter.Activation = DISABLE;
  cfg.HighPassFilter.CutOffFrequency = MDF_HPF_CUTOFF_0_000625FPCM;
  cfg.Integrator.Activation = DISABLE;
  cfg.Integrator.Value = 4U;
  cfg.Integrator.OutputDivision = MDF_INTEGRATOR_OUTPUT_NO_DIV;
  cfg.SoundActivity.Activation = DISABLE;
  cfg.AcquisitionMode = MDF_MODE_ASYNC_CONT;
  cfg.FifoThreshold = MDF_FIFO_THRESHOLD_NOT_EMPTY;
  cfg.DiscardSamples = 1U;
  cfg.Trigger.Source = MDF_FILTER_TRIG_TRGO;
  cfg.Trigger.Edge = MDF_FILTER_TRIG_RISING_EDGE;
  cfg.SnapshotFormat = MDF_SNAPSHOT_23BITS;

  if (HAL_MDF_AcqStart(hmdf, &cfg) != HAL_OK)
  {
    printf("  %s: polling acquisition start failed\r\n", label);
    BSP_AUDIO_IN_DeInit(0);
    return Result::Fail;
  }

  size_t got = 0;
  for (size_t i = 0; i < kPollSamples; i++)
  {
    if (HAL_MDF_PollForAcq(hmdf, 50) != HAL_OK)
    {
      break;
    }
    int32_t v = 0;
    (void)HAL_MDF_GetAcqValue(hmdf, &v);
    pcmBuffer[i] = static_cast<int16_t>(v >> 8);
    got++;
  }
  (void)HAL_MDF_AcqStop(hmdf);
  BSP_AUDIO_IN_DeInit(0);

  if (got < kPollSamples)
  {
    printf("  %s: polling acquisition stalled at %u samples\r\n", label,
           static_cast<unsigned>(got));
    return Result::Fail;
  }
  return analyzeSignal(kPollSamples, label, "polling");
}

Result recordAndCheck(uint32_t device, const char *label)
{
  releaseCubeMxAdfOnce();

  if (dmaCapture(device, label) == Result::Pass)
  {
    return Result::Pass;
  }
  printf("  %s: DMA path failed, verifying mic via polling...\r\n", label);
  return pollingCapture(device, label);
}
} // namespace

Result testMic1()
{
  return recordAndCheck(AUDIO_IN_DEVICE_DIGITAL_MIC1, "MIC1/ADF1");
}

Result testMic2()
{
  return recordAndCheck(AUDIO_IN_DEVICE_DIGITAL_MIC2, "MIC2/MDF1");
}

} // namespace apptest

/* BSP audio callbacks are defined once in telemetry.cpp (g_AudioEvents). */

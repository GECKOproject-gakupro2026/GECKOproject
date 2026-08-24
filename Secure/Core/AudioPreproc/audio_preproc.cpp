#include "audio_preproc.hpp"

extern "C" {
#include "feature_extraction.h"
#include "user_mel_tables.h"
}

#include <cfloat>
#include <cstring>

namespace audio_preproc
{
namespace
{
constexpr uint32_t kFftLength = 512U;
constexpr float kInputScale = 0.057150375097990036F;
constexpr int8_t kInputZeroPoint = 33;

arm_rfft_fast_instance_f32 rfft;
float32_t scratch1[kFftLength];
float32_t scratch2[kFftLength];
SpectrogramTypeDef spectrogram;
MelFilterTypeDef melFilter;
MelSpectrogramTypeDef melSpectrogram;
LogMelSpectrogramTypeDef logMelSpectrogram;
bool initialized = false;
} // namespace

bool init()
{
  if (initialized)
  {
    return true;
  }
  if (arm_rfft_fast_init_f32(&rfft, kFftLength) != ARM_MATH_SUCCESS)
  {
    return false;
  }

  std::memset(&spectrogram, 0, sizeof(spectrogram));
  spectrogram.pRfft = &rfft;
  spectrogram.Type = SPECTRUM_TYPE_MAGNITUDE;
  spectrogram.pWindow = const_cast<float32_t *>(user_win);
  spectrogram.SampRate = 16000U;
  spectrogram.FrameLen = kWindowSamples;
  spectrogram.FFTLen = kFftLength;
  spectrogram.pad_left = (kFftLength - kWindowSamples) / 2U;
  spectrogram.pad_right = spectrogram.pad_left;
  spectrogram.pScratch1 = scratch1;
  spectrogram.pScratch2 = scratch2;

  std::memset(&melFilter, 0, sizeof(melFilter));
  melFilter.pStartIndices = const_cast<uint32_t *>(user_melFilterStartIndices);
  melFilter.pStopIndices = const_cast<uint32_t *>(user_melFilterStopIndices);
  melFilter.pCoefficients = const_cast<float32_t *>(user_melFilterLut);
  melFilter.CoefficientsLength = 462U;
  melFilter.NumMels = kMelBands;
  melFilter.FFTLen = kFftLength;
  melFilter.SampRate = 16000U;
  melFilter.Formula = MEL_HTK;
  melFilter.Normalize = 0U;
  melFilter.Mel2F = 1U;
  melFilter.FMin = 125.0F;
  melFilter.FMax = 7500.0F;

  melSpectrogram.SpectrogramConf = &spectrogram;
  melSpectrogram.MelFilter = &melFilter;
  logMelSpectrogram.MelSpectrogramConf = &melSpectrogram;
  logMelSpectrogram.LogFormula = LOGMELSPECTROGRAM_SCALE_LOG;
  logMelSpectrogram.Ref = 1.0F;
  logMelSpectrogram.TopdB = FLT_MAX;
  initialized = true;
  return true;
}

bool run(const int16_t *pcm, std::size_t sampleCount, int8_t *features)
{
  if (pcm == nullptr || features == nullptr || sampleCount < kPatchSamples ||
      !init())
  {
    return false;
  }

  int8_t column[kMelBands];
  constexpr float kInverseInputScale = 1.0F / kInputScale;
  for (std::size_t col = 0; col < kColumns; ++col)
  {
    LogMelSpectrogramColumn_q15_Q8(
        &logMelSpectrogram, pcm + col * kHopSamples, column,
        kInputZeroPoint, kInverseInputScale);
    for (std::size_t mel = 0; mel < kMelBands; ++mel)
    {
      features[mel * kColumns + col] = column[mel];
    }
  }
  return true;
}

} // namespace audio_preproc

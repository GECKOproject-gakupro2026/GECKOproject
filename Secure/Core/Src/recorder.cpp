/**
  ******************************************************************************
  * @file    recorder.cpp
  * @brief   Recorder implementation. See recorder.hpp.
  ******************************************************************************
  */
#include "recorder.hpp"

#include "adpcm.h"

#include <cstdio>
#include <cstring>

namespace recorder
{
namespace
{

uint8_t s_buf[kBufBytes];
uint32_t s_writePos = 0;
uint32_t s_totalSamples = 0;
adpcm_state_t s_state = {};
bool s_active = false;

} // namespace

void Start()
{
  s_writePos = 0;
  s_totalSamples = 0;
  s_state.predictor = 0;
  s_state.step_index = 0;
  s_active = true;
}

uint32_t Stop()
{
  s_active = false;
  return s_writePos;
}

bool Active() { return s_active; }

void FeedPcm(const int16_t *pcm, size_t count)
{
  if (!s_active || count == 0U)
  {
    return;
  }

  /* BLE転送量を半分にするため、録音は 16 kHz → 8 kHz に 2:1 デシメートしてから
   * ADPCM圧縮する(音声帯域には十分。エイリアシング低減に単純間引きではなく
   * 隣接2サンプルの平均を取る簡易ローパス)。count は常に 512(偶数)なので
   * 端数処理は不要。PC側は 8 kHz で WAV を書く(protocol.REC_SAMPLE_RATE)。 */
  static int16_t decim[256];
  size_t outCount = count / 2U;
  for (size_t i = 0; i < outCount; ++i)
  {
    int32_t avg = (static_cast<int32_t>(pcm[2U * i]) +
                   static_cast<int32_t>(pcm[2U * i + 1U])) / 2;
    decim[i] = static_cast<int16_t>(avg);
  }

  size_t need = ADPCM_BLOCK_HEADER_SIZE + (outCount + 1U) / 2U;
  if (s_writePos + need > kBufBytes)
  {
    s_active = false;
    printf("[REC] buffer full, auto-stopped: samples=%lu bytes=%lu\r\n",
           static_cast<unsigned long>(s_totalSamples),
           static_cast<unsigned long>(s_writePos));
    return;
  }

  size_t written = adpcm_encode(&s_state, decim, outCount, &s_buf[s_writePos]);
  s_writePos += static_cast<uint32_t>(written);
  s_totalSamples += static_cast<uint32_t>(outCount);
}

uint32_t UsedBytes() { return s_writePos; }

uint32_t TotalSamples() { return s_totalSamples; }

uint32_t Read(uint32_t off, uint8_t *dst, uint32_t len)
{
  if (off >= s_writePos)
  {
    return 0U;
  }
  uint32_t avail = s_writePos - off;
  uint32_t n = (len < avail) ? len : avail;
  memcpy(dst, &s_buf[off], n);
  return n;
}

} // namespace recorder

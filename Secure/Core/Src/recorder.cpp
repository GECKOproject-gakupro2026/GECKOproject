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
  s_totalSamples = 0;
  s_state.predictor = 0;
  s_state.step_index = 0;
  /* One 4-byte header for the WHOLE recording (was: one per 256-sample block,
   * which reset the predictor at every block boundary and produced audible
   * clicks). The rest of the stream is raw nibbles appended continuously by
   * FeedPcm, so the predictor/step_index never reset mid-stream. Layout:
   * [predictor int16 LE][step_index u8][pad u8][nibbles...]. */
  s_buf[0] = static_cast<uint8_t>(static_cast<uint16_t>(s_state.predictor) & 0xFFU);
  s_buf[1] = static_cast<uint8_t>((static_cast<uint16_t>(s_state.predictor) >> 8) & 0xFFU);
  s_buf[2] = static_cast<uint8_t>(s_state.step_index);
  s_buf[3] = 0U;
  s_writePos = ADPCM_BLOCK_HEADER_SIZE;
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

  /* Record at the full 16 kHz (no decimation). The enlarged 240-byte notify
   * payload (kRecChunkPayload) already cut the chunk count ~4x, so we can
   * afford the 2x data of 16 kHz and still transfer fewer chunks than the old
   * 8 kHz / 58-byte scheme. PC writes the WAV at 16 kHz (protocol.REC_SAMPLE_RATE).
   * Header is written once at Start(); here we only append nibbles (no
   * per-block header), so the predictor stays continuous across feeds. */
  size_t need = (count + 1U) / 2U;
  if (s_writePos + need > kBufBytes)
  {
    s_active = false;
    printf("[REC] buffer full, auto-stopped: samples=%lu bytes=%lu\r\n",
           static_cast<unsigned long>(s_totalSamples),
           static_cast<unsigned long>(s_writePos));
    return;
  }

  size_t written = adpcm_encode_nibbles(&s_state, pcm, count, &s_buf[s_writePos]);
  s_writePos += static_cast<uint32_t>(written);
  s_totalSamples += static_cast<uint32_t>(count);
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

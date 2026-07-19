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

  /* BLE録音は 16 kHz フルレート(デシメーションなし)で ADPCM 圧縮する。音質を
   * 下げずに録りたいという要求のため。240B notify ペイロードでチャンク数を大きく
   * 減らしたので、16 kHz でも 3 秒あたり約104チャンク(旧 8kHz/58B 時の219より
   * ずっと少ない)。PC側は 16 kHz で WAV を書く(protocol.REC_SAMPLE_RATE)。
   * count は常に 512(偶数)。
   *
   * 各 FeedPcm を1つの自己完結 ADPCM ブロック(先頭にヘッダ)として書く。連続
   * ストリーム(ヘッダ1回)方式は、ファームのエンコーダと PC のデコーダの微小な
   * 状態差が延々と蓄積して predictor が DC 発散する不具合があったため、各ブロックが
   * ヘッダから状態を復元し直す自己完結方式にしている(以前音声が正しく録れていた
   * 構成)。 */
  size_t need = ADPCM_BLOCK_HEADER_SIZE + (count + 1U) / 2U;
  if (s_writePos + need > kBufBytes)
  {
    s_active = false;
    printf("[REC] buffer full, auto-stopped: samples=%lu bytes=%lu\r\n",
           static_cast<unsigned long>(s_totalSamples),
           static_cast<unsigned long>(s_writePos));
    return;
  }

  size_t written = adpcm_encode(&s_state, pcm, count, &s_buf[s_writePos]);
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

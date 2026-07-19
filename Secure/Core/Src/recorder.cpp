/**
  ******************************************************************************
  * @file    recorder.cpp
  * @brief   Recorder implementation. See recorder.hpp.
  *
  *          連続ストリーミング方式: 録音しながら1ブロック=1チャンク(240B)ずつ
  *          BLEへ送出し、送信済みブロックをリングから解放する。RAM使用量に依らず
  *          無制限長の録音ができる。各ブロックは自己完結ADPCM(先頭ヘッダ)なので、
  *          チャンク欠落はそのブロック1個の局所的音飛びで済み発散しない。
  *          FeedPcm() は入力(16 kHz)を 8 kHz に 2:1 デシメートしてから
  *          ADPCMエンコードする(BLE転送レートが16kHz生成レートに追いつけない
  *          ため。詳細はFeedPcm()内コメント参照)。
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

/* ブロック単位のリング。各スロットは kBlockBytes の完結ADPCMブロック。 */
uint8_t  s_ring[kRingBlocks][kBlockBytes];
uint32_t s_ringLen[kRingBlocks];   /* 各スロットの有効バイト数(末尾ブロックは短いことがある) */
uint32_t s_ringSeq[kRingBlocks];   /* 各スロットのブロック通し番号 */
uint32_t s_head = 0;               /* 次に PopBlock で取り出すスロット */
uint32_t s_tail = 0;               /* 次に書き込むスロット */
uint32_t s_count = 0;              /* リング内の送信待ちブロック数 */

/* kBlockSamples 個たまるまで PCM を溜めるアキュムレータ。 */
int16_t  s_acc[kBlockSamples];
uint32_t s_accCount = 0;

adpcm_state_t s_state = {};
uint32_t s_totalBlocks = 0;        /* 次に振る seq */
uint32_t s_totalSamples = 0;       /* FeedPcm() へ供給された総サンプル数 */
bool     s_active = false;

/* アキュムレータ内の kBlockSamples 個(または末尾の端数)を1ブロックにエンコード
 * してリングへ積む。リング満杯なら最古を捨てる(drop-oldest)。 */
void flushBlock()
{
  if (s_accCount == 0U)
  {
    return;
  }
  if (s_count == kRingBlocks)
  {
    /* 送信が追いつかず満杯: 最古を捨てて上書き(音は飛ぶが録音は継続)。 */
    s_head = (s_head + 1U) % kRingBlocks;
    s_count--;
  }
  uint8_t *out = s_ring[s_tail];
  size_t written = adpcm_encode(&s_state, s_acc, s_accCount, out);
  s_ringLen[s_tail] = static_cast<uint32_t>(written);
  s_ringSeq[s_tail] = s_totalBlocks;
  s_tail = (s_tail + 1U) % kRingBlocks;
  s_count++;
  s_totalBlocks++;
  s_accCount = 0U;
}

} // namespace

void Start()
{
  s_head = s_tail = s_count = 0U;
  s_accCount = 0U;
  s_state.predictor = 0;
  s_state.step_index = 0;
  s_totalBlocks = 0U;
  s_totalSamples = 0U;
  s_active = true;
}

void Stop()
{
  if (!s_active)
  {
    return;
  }
  s_active = false;
  flushBlock(); /* 端数サンプルを最後のブロックとして送信待ちに積む */
}

bool Active() { return s_active; }

void FeedPcm(const int16_t *pcm, size_t count)
{
  if (!s_active || count == 0U)
  {
    return;
  }
  /* BLE転送レートが録音の生成レートに追いつけない(230400 baudのAT往復が
   * 律速。16 kHzでは実測26.6チャンク/秒 vs 生成33.9チャンク/秒でリングが
   * 溢れ約10%欠落した)ため、16 kHz → 8 kHz に 2:1 デシメートしてから
   * ADPCM圧縮する(音声帯域には十分。エイリアシング低減に単純間引きではなく
   * 隣接2サンプルの平均を取る簡易ローパス)。count は常に偶数(512)で来るので
   * 端数処理は不要。PC側は 8 kHz で WAV を書く(protocol.REC_SAMPLE_RATE)。 */
  size_t outCount = count / 2U;
  for (size_t i = 0; i < outCount; ++i)
  {
    int32_t avg = (static_cast<int32_t>(pcm[2U * i]) +
                   static_cast<int32_t>(pcm[2U * i + 1U])) / 2;
    s_acc[s_accCount++] = static_cast<int16_t>(avg);
    if (s_accCount == kBlockSamples)
    {
      flushBlock();
    }
  }
  s_totalSamples += static_cast<uint32_t>(outCount);
}

bool PopBlock(uint8_t *dst, uint32_t *outLen, uint32_t *outSeq)
{
  if (s_count == 0U)
  {
    return false;
  }
  memcpy(dst, s_ring[s_head], s_ringLen[s_head]);
  *outLen = s_ringLen[s_head];
  *outSeq = s_ringSeq[s_head];
  s_head = (s_head + 1U) % kRingBlocks;
  s_count--;
  return true;
}

bool HasPending() { return s_count != 0U; }

uint32_t TotalBlocks() { return s_totalBlocks; }

uint32_t TotalSamples() { return s_totalSamples; }

} // namespace recorder

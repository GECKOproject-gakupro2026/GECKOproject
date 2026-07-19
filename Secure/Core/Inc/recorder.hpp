/**
  ******************************************************************************
  * @file    recorder.hpp
  * @brief   One-shot BLE voice recording: captures the resample-corrected
  *          mic stream into a fixed SRAM ring, ADPCM-compressed, until
  *          stopped or the buffer fills. Recording lives in Secure because
  *          the resampled audio pipeline (audio_capture::FeedForResample /
  *          PopResampled, driven by the Secure-only g_AudioEvents DMA flags)
  *          has no NonSecure-side equivalent - see comm_service.cpp's
  *          poll() audio block.
  ******************************************************************************
  */
#ifndef RECORDER_HPP
#define RECORDER_HPP

#include <cstddef>
#include <cstdint>

namespace recorder
{

/* 連続ストリーミング録音: 録音しながら BLE で送信し、送信済みブロックを
 * リングから解放するので、RAM 使用量に依らず(数十KBの小さいリングで)無制限
 * 長の録音ができる。1 ブロック = 1 REC_CHUNK = 240 バイトの自己完結 ADPCM
 * ブロック(4B ヘッダ + 236B nibbles = 472 サンプル分、8 kHz なので 59ms/ブロック)。
 * チャンクとブロックの境界が一致するので、チャンク欠落はそのブロック1個の
 * 局所的な音飛びで済み、predictor は各ブロック先頭でヘッダから復元されるため
 * 発散しない。FeedPcm() が入力(16 kHz)を内部で 8 kHz に 2:1 デシメートする
 * (230400 baud の BLE notify 転送レートが 16 kHz の生成レートに追いつけず
 * リングが溢れていたため; 8 kHz なら実測送信レートを十分下回り欠落がほぼ無い)。 */
constexpr uint32_t kBlockSamples = 472U;                       /* 1ブロックのPCM(8kHz)サンプル数(偶数) */
constexpr uint32_t kBlockBytes = 4U + kBlockSamples / 2U;      /* = 240 (ヘッダ4 + nibbles236) */
constexpr uint32_t kRingBlocks = 256U;                         /* 送信待ちリングのブロック数(=~60KB, 送信が詰まっても数秒ぶん吸収) */

/* 録音を開始する。リングとADPCM状態をリセットする。 */
void Start();

/* 録音を停止する。まだリングに残っている送信待ちブロックはそのまま送信され続ける。 */
void Stop();

bool Active();

/* リサンプル済みPCM(16 kHz)を供給する。内部で 8 kHz にデシメートしつつ
 * kBlockSamples ごとに1ブロックへ ADPCM エンコードし、リングへ積む。
 * Service::poll() から呼ぶ。録音中でない、または録音停止後(残ブロック送信中)
 * は溜めない。リングが満杯(送信が追いつかない)なら最古のブロックを捨てて
 * 上書きする(drop-oldest)。 */
void FeedPcm(const int16_t *pcm, size_t count);

/* 送信すべき次のブロックがあれば取り出して dst(>= kBlockBytes)にコピーし、
 * その通し番号(seq)を *outSeq に入れて true を返す。取り出したブロックはリング
 * から消費される。無ければ false。録音中も呼べる(逐次ストリーミング送信)。 */
bool PopBlock(uint8_t *dst, uint32_t *outLen, uint32_t *outSeq);

/* リングに送信待ちブロックが残っているか(録音停止後、全部送り切ったかの判定用)。 */
bool HasPending();

/* これまでにエンコードした総ブロック数(= 次に振られる seq)。 */
uint32_t TotalBlocks();

/* これまでに FeedPcm() へ供給された総サンプル数(録音全体の長さ)。 */
uint32_t TotalSamples();

} // namespace recorder

#endif /* RECORDER_HPP */

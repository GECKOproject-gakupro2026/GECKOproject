/**
  ******************************************************************************
  * @file    audio_capture.hpp
  * @brief   マイク音声のDMAキャプチャ(MIC2 / MDF1 / PLL3)【port層】。
  *
  *          マイクやADCを変えるときは audio_capture.cpp だけを書き直す。
  *          呼び出し側は「16bit PCMの循環バッファがある」ことだけを知る。
  ******************************************************************************
  */
#ifndef AUDIO_CAPTURE_HPP
#define AUDIO_CAPTURE_HPP

#include <cstdint>

namespace audio_capture
{

/* マイクを初期化して連続キャプチャを開始する。
 * 戻り値: キャプチャが動き始めたら true */
bool Init();

/* 循環キャプチャバッファの先頭。サンプル数は Samples() で得る。 */
const int16_t *Buffer();

/* バッファのサンプル数（固定値） */
uint32_t Samples();

/* 実測ピッチ補正: MIC2/MDF1の実際の出力ペースは、名目16000Hzからずれる
 * (原因はMSIクロック個体差が疑われるが未確定)。線形補間で16000Hz相当に
 * 補正する。補正係数はPCでのFFTスイープ測定(pc_side/mic_freq_response/
 * sweep.py、300Hz〜4000Hz)で実機較正した固定値(audio_capture.cppの
 * kResampleStep)を使う — 音程ズレだけから単純に逆算した理論値では、DMAの
 * half/full-completeが呼ばれる実際の頻度と噛み合わず補正が合わなかった
 * ため、sweep.pyの結果が比1.000になるまで直接チューニングしている。
 *
 * 512サンプル固定の送信フレームと補正比率のミスマッチを吸収するため、
 * キュー方式にしている:
 *   FeedForResample() : 生の512サンプルを1回分投入し、補正後サンプルを
 *                        内部キューに積む(投入した512個は全部消費する)
 *   PopResampled()     : キューから送信用にoutCount個取り出す
 * 呼び出し側(comm_service.cpp)は「生の半バッファが1つ来るたびFeed、
 * その後キューにoutCount分溜まっていればPop→送信」という順で使う。
 * こうしないと、512出力を毎回pos=0から作り直す設計では、フレーム境界の
 * たびに生サンプルの端数が間引かれ、音がおかしくなる(実測でFFTピークが
 * 乱れることを確認済み)。 */
void FeedForResample(const int16_t *src, uint32_t inCount);

/* PopResampled()が今すぐ返せる補正済みサンプル数(キューの残量)。 */
uint32_t ResampledAvailable();

/* キューからoutCount個取り出してdstに書く。ResampledAvailable() >= outCount
 * であることを呼び出し側で確認してから呼ぶこと。 */
void PopResampled(int16_t *dst, uint32_t outCount);

} // namespace audio_capture

#endif /* AUDIO_CAPTURE_HPP */

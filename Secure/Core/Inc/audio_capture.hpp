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

} // namespace audio_capture

#endif /* AUDIO_CAPTURE_HPP */

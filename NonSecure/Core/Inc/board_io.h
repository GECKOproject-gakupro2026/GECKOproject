/**
  ******************************************************************************
  * @file    board_io.h
  * @brief   基板依存のI/O契約（LED・ユーザーボタン・バージョン公開）。
  *
  *          【port層】この宣言は基板が変わっても変えない。実装(board_io.c)だけを
  *          新しい基板用に書き直すこと。呼び出し側(app_loop.c / main.c)は
  *          GPIOポート番号もHALも知らない。
  ******************************************************************************
  */
#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LED・ボタンのGPIOを初期化する。両LEDは消灯状態で始まる。 */
void Board_Init(void);

/* ユーザーボタンの状態。1=押されている、0=離されている。 */
uint8_t Board_ButtonRead(void);

/* LED制御。この基板では緑=ハートビート(ACTIVE)、赤=低消費電力(IDLE)を示す。 */
void Board_LedGreenOff(void);
void Board_LedRedOff(void);
void Board_LedGreenToggle(void);
void Board_LedRedToggle(void);

/* 動作中のNonSecureバージョンを、Secure側やホストツールが読める固定番地へ公開する。 */
void Board_PublishVersion(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_IO_H */

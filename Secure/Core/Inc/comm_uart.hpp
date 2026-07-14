/**
  ******************************************************************************
  * @file    comm_uart.hpp
  * @brief   VCP(USART1)への非同期送信【port層】。
  *
  *          受信(RX)は console.cpp が循環DMAで担当している。ここは送信のみ。
  ******************************************************************************
  */
#ifndef COMM_UART_HPP
#define COMM_UART_HPP

#include <cstddef>
#include <cstdint>

namespace comm_uart
{

/* 割り込み駆動で送る。前の送信が終わるのを最大 waitMs だけ待つ。
 * 戻り値: 送信を開始できたら true、待っても前の送信が終わらなければ false */
bool SendAsync(const uint8_t *data, size_t len, uint32_t waitMs);

} // namespace comm_uart

#endif /* COMM_UART_HPP */

/**
  ******************************************************************************
  * @file    comm_uart.cpp
  * @brief   comm_uart.hpp の USART1(VCP) 向け実装【port層】。
  ******************************************************************************
  */
#include "comm_uart.hpp"

#include "frame_codec.h"   /* FRAME_OVERHEAD */
#include "main.h"

#include <cstring>

extern UART_HandleTypeDef huart1; /* VCP console / telemetry stream */

namespace comm_uart
{
namespace
{
/* --- Non-blocking VCP transmit (interrupt driven, single in-flight buffer).
 * Status frames are droppable (next one comes in 20 ms); audio frames spin
 * briefly for the previous transfer instead. --- */
volatile bool uartTxBusy = false;
uint8_t uartTxBuf[1024 + FRAME_OVERHEAD];
} // namespace

bool SendAsync(const uint8_t *data, size_t len, uint32_t waitMs)
{
  uint32_t t0 = HAL_GetTick();
  while (uartTxBusy)
  {
    if (HAL_GetTick() - t0 >= waitMs)
    {
      return false;
    }
  }
  if (len > sizeof(uartTxBuf))
  {
    return false;
  }
  memcpy(uartTxBuf, data, len);
  uartTxBusy = true;
  if (HAL_UART_Transmit_IT(&huart1, uartTxBuf, static_cast<uint16_t>(len)) != HAL_OK)
  {
    uartTxBusy = false;
    return false;
  }
  return true;
}

} // namespace comm_uart

/* USART1の送信完了割り込み。
 * 【重要】プロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    comm_uart::uartTxBusy = false;
  }
}

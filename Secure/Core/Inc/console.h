/**
  ******************************************************************************
  * @file    console.h
  * @brief   printf retarget and simple character input over the ST-LINK VCP
  *          (USART1). C linkage so it can be used from both C and C++.
  ******************************************************************************
  */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Blocks up to timeout_ms; returns the received character or -1 on timeout. */
int Console_GetChar(uint32_t timeout_ms);

/* Drains up to maxLen already-received bytes into dst without blocking.
 * Returns the number of bytes copied (0 if none are pending). Unlike
 * Console_GetChar(0) (which returns at most one byte per call), this drains
 * everything the circular RX DMA has produced since the last read - callers
 * that only ever pull one byte per superloop iteration can starve behind a
 * momentarily slow loop and, worse, let the DMA lap the ring under them (see
 * Console_GetRxOverrunCount()). */
size_t Console_ReadBlock(uint8_t *dst, size_t maxLen);

/* Bytes currently sitting in the RX ring, not yet consumed by either
 * Console_GetChar() or Console_ReadBlock(). Used to repair the dead
 * UART_FLAG_RXNE-based "let a pending byte win" guard in comm_wifi.cpp -
 * that flag never sets because RX is DMA-driven, this is the real signal. */
size_t Console_RxPending(void);

/* Cumulative count of detected DMA overruns (the circular RX DMA laps the
 * ring faster than the app drains it). Each overrun forces a resync (the
 * torn window since the last read is dropped) instead of silently aliasing
 * to "buffer empty" forever. */
uint32_t Console_GetRxOverrunCount(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_H */

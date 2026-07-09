/**
  ******************************************************************************
  * @file    console.h
  * @brief   printf retarget and simple character input over the ST-LINK VCP
  *          (USART1). C linkage so it can be used from both C and C++.
  ******************************************************************************
  */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Blocks up to timeout_ms; returns the received character or -1 on timeout. */
int Console_GetChar(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_H */

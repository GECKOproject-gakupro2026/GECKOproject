/**
  ******************************************************************************
  * @file    console.cpp
  * @brief   printf retarget and character input over USART1 (ST-LINK VCP).
  ******************************************************************************
  */
#include "console.h"
#include "main.h"

extern UART_HandleTypeDef huart1;

/* newlib-nano: pull in the floating-point printf implementation (%f) */
asm(".global _printf_float");

extern "C" int __io_putchar(int ch)
{
  HAL_UART_Transmit(&huart1, reinterpret_cast<uint8_t *>(&ch), 1, 100);
  return ch;
}

extern "C" int _write(int file, char *ptr, int len)
{
  (void)file;
  for (int i = 0; i < len; i++)
  {
    __io_putchar(ptr[i]);
  }
  return len;
}

extern "C" int Console_GetChar(uint32_t timeout_ms)
{
  uint8_t ch = 0;
  if (HAL_UART_Receive(&huart1, &ch, 1, timeout_ms) == HAL_OK)
  {
    return ch;
  }
  return -1;
}

/**
  ******************************************************************************
  * @file    tests_board.cpp
  * @brief   LED and user-button tests (official BSP).
  ******************************************************************************
  */
#include "test.hpp"
#include "app_config.h"
#include "b_u585i_iot02a.h"

#include <cstdio>

extern "C" volatile uint32_t g_LedBlinkEnable;

namespace apptest
{

Result testLed()
{
  /* Pause the SysTick run indicator - this test owns the LEDs */
  g_LedBlinkEnable = 0;
  if (BSP_LED_Init(LED_RED) != BSP_ERROR_NONE || BSP_LED_Init(LED_GREEN) != BSP_ERROR_NONE)
  {
    printf("  LED init failed\r\n");
    return Result::Fail;
  }
  for (int i = 0; i < 3; i++)
  {
    BSP_LED_On(LED_RED);
    BSP_LED_Off(LED_GREEN);
    HAL_Delay(100);
    BSP_LED_Off(LED_RED);
    BSP_LED_On(LED_GREEN);
    HAL_Delay(100);
  }
  BSP_LED_Off(LED_GREEN);
  /* GetState after Off must read back 0 for both LEDs */
  if (BSP_LED_GetState(LED_RED) != 0 || BSP_LED_GetState(LED_GREEN) != 0)
  {
    printf("  LED state readback mismatch\r\n");
    return Result::Fail;
  }
  printf("  LD6/LD7 blinked, state readback OK\r\n");
  /* Resume the alternating run indicator */
  BSP_LED_On(LED_RED);
  BSP_LED_Off(LED_GREEN);
  g_LedBlinkEnable = 1;
  return Result::Pass;
}

Result testButton()
{
  if (BSP_PB_Init(BUTTON_USER, BUTTON_MODE_GPIO) != BSP_ERROR_NONE)
  {
    printf("  button init failed\r\n");
    return Result::Fail;
  }
  int32_t state = BSP_PB_GetState(BUTTON_USER);
  if (state != 0 && state != 1)
  {
    printf("  unexpected button state %ld\r\n", state);
    return Result::Fail;
  }
  printf("  USER button readable (state=%ld, not pressed=0)\r\n", state);
  return Result::Pass;
}

} // namespace apptest

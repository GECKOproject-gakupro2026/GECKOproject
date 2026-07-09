/**
  ******************************************************************************
  * @file    app_main.cpp
  * @brief   Application entry: continuous status telemetry with an optional
  *          on-demand hardware test run.
  *          Console keys: 't' = run the full test suite once, then resume.
  ******************************************************************************
  */
#include "app_config.h"
#include "console.h"
#include "main.h"
#include "telemetry.hpp"

#include "b_u585i_iot02a.h"

#include <cstdio>

extern "C" volatile uint32_t g_LedBlinkEnable;
extern "C" void App_RunTestsOnce(void);

extern UART_HandleTypeDef huart1;

extern "C" void App_Main(void)
{
  /* Speed up the VCP link (USB-bridged by the ST-LINK) beyond the 115200
   * CubeMX default */
  huart1.Init.BaudRate = CFG_CONSOLE_BAUDRATE;
  (void)HAL_UART_Init(&huart1);

  printf("\r\n\r\n===== B-U585I-IOT02A status telemetry firmware =====\r\n");
  printf("SYSCLK=%lu Hz, build " __DATE__ " " __TIME__ "\r\n",
         HAL_RCC_GetSysClockFreq());
  printf("console keys: 't' = run test suite, 'a' = audio stream ON, 's' = OFF\r\n");

  /* Run indicator: LD6/LD7 blink alternately while the firmware is running */
  BSP_LED_Init(LED_RED);
  BSP_LED_Init(LED_GREEN);
  BSP_LED_On(LED_RED);
  BSP_LED_Off(LED_GREEN);
  g_LedBlinkEnable = 1;

  static telemetry::Service service;
  service.init();

  for (;;)
  {
    service.poll();

    int key = Console_GetChar(0);
    if (key == 't' || key == 'T')
    {
      printf("\r\n[APP] pausing telemetry, running the hardware test suite...\r\n");
      App_RunTestsOnce();
      printf("[APP] test suite done, re-initializing telemetry...\r\n");
      g_LedBlinkEnable = 1;
      service.init();
    }
    else if (key == 'a' || key == 'A')
    {
      service.setAudioStream(true);
    }
    else if (key == 's' || key == 'S')
    {
      service.setAudioStream(false);
    }
  }
}

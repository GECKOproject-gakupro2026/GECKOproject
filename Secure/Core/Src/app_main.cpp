/**
  ******************************************************************************
  * @file    app_main.cpp
  * @brief   Application entry: continuous status telemetry.
  ******************************************************************************
  */
#include "ai_app.hpp"
#include "app_config.h"
#include "console.h"
#include "main.h"
#include "telemetry.hpp"

#include "b_u585i_iot02a.h"

#include <cstdio>

/* Run indicator toggle for SysTick_Handler's LD6/LD7 alternate blink
 * (stm32u5xx_it.c). Was previously defined in test.cpp (removed); the
 * definition belongs here since app_main.cpp is what actually drives it. */
extern "C" volatile uint32_t g_LedBlinkEnable = 0;

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart4;

/* Transparent VCP<->UART4 bridge for probing the BLE module's AT dialect
 * from the PC. Telemetry is paused while the bridge runs; ESC exits. */
static void bleBridge(void)
{
  static const uint32_t bauds[] = {9600U, 19200U, 38400U, 57600U, 115200U};
  int sel = Console_GetChar(3000);
  uint32_t baud = (sel >= '1' && sel <= '5') ? bauds[sel - '1'] : 9600U;

  HAL_NVIC_DisableIRQ(UART4_IRQn); /* suspend the AT client RX interrupt */
  (void)HAL_UART_Abort(&huart4);
  huart4.Init.BaudRate = baud;
  (void)HAL_UART_Init(&huart4);

  printf("[BRIDGE] UART4 @%lu baud, ESC exits\r\n", baud);
  for (;;)
  {
    /* Forwarding a byte to a 9600-baud UART takes ~1 ms; PC bytes arriving
     * meanwhile overrun the 1-byte RDR. Clear ORE so RX keeps working (the
     * PC-side probe paces its bytes to avoid the loss). */
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE))
    {
      __HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_OREF);
    }
    int c = Console_GetChar(0);
    if (c == 0x1B)
    {
      break;
    }
    if (c >= 0)
    {
      uint8_t b = static_cast<uint8_t>(c);
      (void)HAL_UART_Transmit(&huart4, &b, 1, 100);
    }
    uint8_t r;
    while (HAL_UART_Receive(&huart4, &r, 1, 0) == HAL_OK)
    {
      (void)HAL_UART_Transmit(&huart1, &r, 1, 100);
    }
  }
  printf("\r\n[BRIDGE] exit\r\n");
}

extern "C" void App_Main(void)
{
  printf("\r\n\r\n===== B-U585I-IOT02A status telemetry firmware =====\r\n");
  printf("SYSCLK=%lu Hz, build " __DATE__ " " __TIME__ "\r\n",
         HAL_RCC_GetSysClockFreq());
  printf("console keys: 'a'/'s'=audio stream  'i'=infer once  'I'=auto infer\r\n");

  /* Run indicator: LD6/LD7 blink alternately while the firmware is running */
  BSP_LED_Init(LED_RED);
  BSP_LED_Init(LED_GREEN);
  BSP_LED_On(LED_RED);
  BSP_LED_Off(LED_GREEN);
  g_LedBlinkEnable = 1;

  /* Comm_Init() (called from Secure main() before the Stage-0 jump) already
   * constructed and initialized the single Service instance in
   * NonSecure-driven mode. Reaching App_Main() means Stage-0 decided to stay
   * resident (button held / no valid NonSecure image) - reuse that same
   * instance as the interactive Secure-side loader instead of Comm_Poll(). */
  telemetry::Service &service = telemetry::CommInit();
  service.setNsDriven(false);

  auto inferAndPrint = []() {
    aiapp::Result r;
    if (!aiapp::runOnce(r))
    {
      return;
    }
    printf("[AI] %s", aiapp::label(r.topClass));
    for (uint8_t i = 0; i < r.nClasses; i++)
    {
      printf("  %s=%d%%", aiapp::label(i), (int)(r.scores[i] * 100.0f + 0.5f));
    }
    printf("  (%lums)\r\n", r.inferenceUs / 1000U);
  };
  bool autoInfer = false;
  uint32_t nextInferTick = 0;

  for (;;)
  {
    service.poll();

    if (autoInfer && static_cast<int32_t>(HAL_GetTick() - nextInferTick) >= 0)
    {
      nextInferTick = HAL_GetTick() + 2000U;
      inferAndPrint();
    }

    int key = Console_GetChar(0);
    if (key >= 0)
    {
      /* frame bytes (OTA & co.) are consumed inside; plain chars come back */
      key = service.processRxByte(static_cast<uint8_t>(key), false);
    }
    if (key == 'a' || key == 'A')
    {
      service.setAudioStream(true);
    }
    else if (key == 's' || key == 'S')
    {
      service.setAudioStream(false);
    }
    else if (key == 'b' || key == 'B')
    {
      bleBridge();
    }
    else if (key == 'i')
    {
      inferAndPrint();
    }
    else if (key == 'I')
    {
      autoInfer = !autoInfer;
      printf("[AI] auto inference %s\r\n", autoInfer ? "ON (2s)" : "OFF");
    }
  }
}

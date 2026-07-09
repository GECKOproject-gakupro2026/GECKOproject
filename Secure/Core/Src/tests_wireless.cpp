/**
  ******************************************************************************
  * @file    tests_wireless.cpp
  * @brief   Wireless module aliveness tests:
  *          - STM32WB5MMG BLE module: AT probe over UART4 (9600 baud)
  *          - EMW3080 Wi-Fi module: reset and watch NOTIFY/FLOW handshake pins
  ******************************************************************************
  */
#include "test.hpp"
#include "app_config.h"
#include "main.h"

#include <cstdio>
#include <cstring>

extern UART_HandleTypeDef huart4;

namespace apptest
{

namespace
{
/* EMW3080 control pins (board schematic, same as ST NetXDuo examples) */
constexpr uint16_t kWifiResetPin = GPIO_PIN_15;  /* PF15 */
constexpr uint16_t kWifiNotifyPin = GPIO_PIN_14; /* PD14 */
constexpr uint16_t kWifiFlowPin = GPIO_PIN_15;   /* PG15 */

bool bleProbe(const char *cmd, uint8_t *resp, size_t respSize, uint16_t *received)
{
  *received = 0;
  memset(resp, 0, respSize);
  HAL_UART_Transmit(&huart4, reinterpret_cast<const uint8_t *>(cmd),
                    static_cast<uint16_t>(strlen(cmd)), 500);
  uint32_t start = HAL_GetTick();
  while ((HAL_GetTick() - start) < CFG_BLE_REPLY_TIMEOUT_MS && *received < respSize - 1)
  {
    uint8_t ch;
    if (HAL_UART_Receive(&huart4, &ch, 1, 50) == HAL_OK)
    {
      resp[(*received)++] = ch;
    }
  }
  return *received > 0;
}
} // namespace

Result testBleModule()
{
  /* The factory AT-server firmware of the module runs at 9600 baud */
  huart4.Init.BaudRate = CFG_BLE_BAUDRATE;
  if (HAL_UART_Init(&huart4) != HAL_OK)
  {
    printf("  UART4 reconfiguration failed\r\n");
    return Result::Fail;
  }

  uint8_t resp[64];
  uint16_t n = 0;
  bool alive = bleProbe("AT\r\n", resp, sizeof(resp), &n);
  if (!alive)
  {
    alive = bleProbe("AT+BLE_TEST?\r\n", resp, sizeof(resp), &n);
  }

  if (!alive)
  {
    printf("  no reply from STM32WB5MMG (AT server FW may be missing)\r\n");
    return Result::Fail;
  }
  printf("  BLE module replied %u bytes: \"%.*s\"\r\n", n, n, resp);
  return Result::Pass;
}

Result testWifiModule()
{
  /* PG[15:2] pins are powered from VDDIO2 */
  HAL_PWREx_EnableVddIO2();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {};

  /* SPI chip-select must idle high before the module leaves reset */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);
  gpio.Pin = GPIO_PIN_12;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &gpio);

  gpio.Pin = kWifiResetPin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOF, &gpio);

  gpio.Pin = kWifiNotifyPin;
  gpio.Mode = GPIO_MODE_INPUT;
  HAL_GPIO_Init(GPIOD, &gpio);
  gpio.Pin = kWifiFlowPin;
  HAL_GPIO_Init(GPIOG, &gpio);

  /* Hard reset per the mx_wifi driver: low 100 ms, high, boot wait 1200 ms */
  HAL_GPIO_WritePin(GPIOF, kWifiResetPin, GPIO_PIN_RESET);
  HAL_Delay(100);
  HAL_GPIO_WritePin(GPIOF, kWifiResetPin, GPIO_PIN_SET);
  HAL_Delay(1200);

  int notifyAfterBoot = HAL_GPIO_ReadPin(GPIOD, kWifiNotifyPin);

  /* SPI protocol handshake: the slave raises FLOW in response to the master
   * asserting CS - this needs no SPI data and proves the module is alive */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET); /* CS low */
  uint32_t start = HAL_GetTick();
  bool flowResponded = false;
  while ((HAL_GetTick() - start) < 500U)
  {
    if (HAL_GPIO_ReadPin(GPIOG, kWifiFlowPin) == GPIO_PIN_SET)
    {
      flowResponded = true;
      break;
    }
  }
  uint32_t flowDelay = HAL_GetTick() - start;
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET); /* CS high */

  printf("  EMW3080: NOTIFY(boot event)=%d, FLOW response to CS=%s (%lu ms)\r\n",
         notifyAfterBoot, flowResponded ? "yes" : "no", flowDelay);
  if (!flowResponded && notifyAfterBoot == 0)
  {
    printf("  module not responding (no boot event, no CS handshake)\r\n");
    return Result::Fail;
  }
  return Result::Pass;
}

} // namespace apptest

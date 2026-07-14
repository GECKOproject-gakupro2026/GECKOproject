/**
  ******************************************************************************
  * @file    board_io.c
  * @brief   board_io.h の B-U585I-IOT02A 向け実装【port層】。
  *
  *          基板を変えるときは、このファイルだけを新しい基板のピン配置で
  *          書き直す。board_io.h の関数名・シグネチャは変えないこと。
  ******************************************************************************
  */
#include "board_io.h"

#include "main.h"

/* User LEDs on this board: LD6 red = PH6, LD7 green = PH7 */
#define LED_RED_PIN      GPIO_PIN_6
#define LED_GREEN_PIN    GPIO_PIN_7
#define LED_PORT         GPIOH

/* USER button (B1): PC13, active-high, pulldown (matches the Secure BSP's
 * BSP_PB_Init/BUTTON_MODE_GPIO configuration it used before Phase B). */
#define BUTTON_USER_PIN  GPIO_PIN_13
#define BUTTON_USER_PORT GPIOC

/* Mirror the version into a fixed SRAM3 (non-secure RAM) word so the Secure
 * side can display which NonSecure version is actually running. */
#define NS_RUNNING_VERSION_ADDR  0x200BFFF0UL /* top of NS SRAM3 */

static void led_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOH_CLK_ENABLE();
  gpio.Pin = LED_RED_PIN | LED_GREEN_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_PORT, &gpio);
  /* both off (PH6/PH7 read off = SET on this board's LED wiring) */
  HAL_GPIO_WritePin(LED_PORT, LED_RED_PIN | LED_GREEN_PIN, GPIO_PIN_SET);
}

/* TrustZone app-layer refactor Phase B: first sensor input moved to
 * NonSecure. PC13 was released NSEC by Secure's MX_GPIO_Init(); this
 * mirrors the BSP's BUTTON_MODE_GPIO config (input, pulldown). */
static void button_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE();
  gpio.Pin = BUTTON_USER_PIN;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(BUTTON_USER_PORT, &gpio);
}

void Board_Init(void)
{
  led_init();
  button_init();
}

uint8_t Board_ButtonRead(void)
{
  return (HAL_GPIO_ReadPin(BUTTON_USER_PORT, BUTTON_USER_PIN) == GPIO_PIN_SET) ? 1U : 0U;
}

/* Both user LEDs use the same wiring: SET = off, RESET = on (matches the
 * long-standing NonSecure demo where green blinked via TogglePin and red
 * was held off with SET). */
void Board_LedGreenOff(void) { HAL_GPIO_WritePin(LED_PORT, LED_GREEN_PIN, GPIO_PIN_SET); }
void Board_LedRedOff(void)   { HAL_GPIO_WritePin(LED_PORT, LED_RED_PIN, GPIO_PIN_SET); }
void Board_LedGreenToggle(void) { HAL_GPIO_TogglePin(LED_PORT, LED_GREEN_PIN); }
void Board_LedRedToggle(void)   { HAL_GPIO_TogglePin(LED_PORT, LED_RED_PIN); }

void Board_PublishVersion(uint32_t version)
{
  *(volatile uint32_t *)NS_RUNNING_VERSION_ADDR = version;
}

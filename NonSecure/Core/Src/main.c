/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "secure_nsc.h"
#include "board_io.h"
#include "sensors.h"
#include "ns_audio.h"
#include "app_config.h"

#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* OTA-updatable NonSecure application. Bump NS_APP_VERSION and re-flash over
 * the air to see the LED pattern change - the running version is proven by
 * how the LEDs blink (see the app loop below). */
#define NS_APP_VERSION   19U

/* Version banner placed at a fixed offset so the Secure loader (and a host
 * tool) can read the staged/running NonSecure version without executing it.
 * Lives after the vector table area, in a dedicated .ns_appinfo
 * section pinned by the linker. */
typedef struct
{
  uint32_t magic;    /* 0x4E534150 = "NSAP" */
  uint32_t version;
  uint32_t reserved[2];
} ns_appinfo_t;
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MX_GTZC_NS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Version banner - pinned by the linker at the start of NS flash + 0x400 */
__attribute__((section(".ns_appinfo"), used))
const ns_appinfo_t g_ns_appinfo = {0x4E534150U, NS_APP_VERSION, {0U, 0U}};

/* NonSecure is the application layer's main loop. It pumps the Secure comm
 * service via Comm_Poll() and submits a telemetry snapshot through the
 * Comm_SendTelemetry() NSC gateway. Phase C: env/motion/light/ToF sensors
 * (I2C1/I2C2) are now read here too; audio/AI/MCU-info fields stay zeroed
 * until Phase D/F. */
static void build_status(FullStatus_t *st)
{
  Sensors_Refresh(st);
  Audio_Refresh(st);
  st->ver = 2U;
  st->uptime_ms = HAL_GetTick();
  st->button = Board_ButtonRead();
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* GTZC initialisation */
  MX_GTZC_NS_Init();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  /* USER CODE BEGIN 2 */
  Board_Init();
  Sensors_Init();
  Audio_Init();
  /* Publish the running version for the Secure side / host tools */
  Board_PublishVersion(g_ns_appinfo.version);
  /* Startup reached the main loop without a fault: tell the Secure Stage-0
   * loader this boot was good, clearing its rollback attempt counter (OTA
   * Phase 2, see boot_guard.hpp on the Secure side). */
  Secure_ConfirmBoot();
  /* USER CODE END 2 */

  /* Infinite loop
   * TrustZone Phase E: ACTIVE/IDLE low-power state machine. In ACTIVE the app
   * reads sensors, streams 50 Hz telemetry and blinks the green heartbeat.
   * After CFG_IDLE_TIMEOUT_MS with no host command it drops to IDLE: sensors
   * and telemetry stop (Secure comm push is quieted too), ToF stops ranging,
   * and the red LED slow-blinks. Any inbound host activity - which the Secure
   * comm stack keeps listening for even in IDLE - wakes it back to ACTIVE. */
  /* USER CODE BEGIN WHILE */
  enum { MODE_ACTIVE = 0, MODE_IDLE = 1 } mode = MODE_ACTIVE;
  uint32_t lastActivityMs = HAL_GetTick(); /* start ACTIVE, not instantly idle */
  uint32_t nextTelemetryTick = 0U;
  uint32_t nextLedTick = 0U;
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    Comm_Poll(); /* pumps the Secure comm service (TCP/OTA/BLE/audio) */

    uint32_t now = HAL_GetTick();

    /* Poll host input every loop (even in IDLE) - this is the wake source. */
    uint8_t cmdByte = 0U;
    if (Comm_PollHostCommand(&cmdByte) != COMM_POLL_NONE)
    {
      lastActivityMs = now; /* any inbound host traffic counts as activity */
    }

    if (mode == MODE_ACTIVE)
    {
      if ((int32_t)(now - lastActivityMs) >= (int32_t)CFG_IDLE_TIMEOUT_MS)
      {
        /* Enter IDLE: stop sensors/telemetry, quiet the Secure links. */
        mode = MODE_IDLE;
        Sensors_Stop();
        Audio_Stop();
        Comm_SetTelemetryEnabled(0U);
        Board_LedGreenOff();
        Board_LedRedOff();
        nextLedTick = now;
      }
      else
      {
        if ((int32_t)(now - nextTelemetryTick) >= 0)
        {
          nextTelemetryTick = now + CFG_ACTIVE_TELEMETRY_MS;
          static FullStatus_t st;
          build_status(&st);
          (void)Comm_SendTelemetry(&st);
        }
        if ((int32_t)(now - nextLedTick) >= 0)
        {
          nextLedTick = now + CFG_ACTIVE_HB_MS;
          Board_LedRedOff();
          Board_LedGreenToggle(); /* green heartbeat */
        }
      }
    }
    else /* MODE_IDLE */
    {
      if ((int32_t)(now - lastActivityMs) < (int32_t)CFG_IDLE_TIMEOUT_MS)
      {
        /* Woke on host activity: resume ACTIVE. */
        mode = MODE_ACTIVE;
        Sensors_Resume();
        Audio_Resume();
        Comm_SetTelemetryEnabled(1U);
        Board_LedRedOff();
        nextTelemetryTick = now;
        nextLedTick = now;
      }
      else if ((int32_t)(now - nextLedTick) >= 0)
      {
        nextLedTick = now + CFG_IDLE_LED_BLINK_MS;
        Board_LedGreenOff();
        Board_LedRedToggle(); /* red slow blink = low-power indicator */
      }
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief GTZC_NS Initialization Function
  * @param None
  * @retval None
  */
static void MX_GTZC_NS_Init(void)
{

  /* USER CODE BEGIN GTZC_NS_Init 0 */

  /* USER CODE END GTZC_NS_Init 0 */

  /* USER CODE BEGIN GTZC_NS_Init 1 */

  /* USER CODE END GTZC_NS_Init 1 */
  /* USER CODE BEGIN GTZC_NS_Init 2 */

  /* USER CODE END GTZC_NS_Init 2 */

}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

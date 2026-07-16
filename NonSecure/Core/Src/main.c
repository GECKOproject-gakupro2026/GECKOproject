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
#include "app_loop.h"
#include "board_io.h"
#include "comm_api.h"
#include "sensors.h"
#include "ns_audio.h"

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
#define NS_APP_VERSION   29U

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

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  App_Run(); /* does not return */
  /* USER CODE END WHILE */

  /* USER CODE BEGIN 3 */
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

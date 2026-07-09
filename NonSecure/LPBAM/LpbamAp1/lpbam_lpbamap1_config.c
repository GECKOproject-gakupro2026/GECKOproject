/* USER CODE BEGIN Header */
/**
  **********************************************************************************************************************
  * @file   lpbam_lpbamap1_config.c
  * @author MCD Application Team
  * @brief  Provides LPBAM LpbamAp1 application configuration services
  **********************************************************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  **********************************************************************************************************************
  */
/* USER CODE END Header */
/* Includes ----------------------------------------------------------------------------------------------------------*/
#include "lpbam_lpbamap1.h"

/* Private variables -------------------------------------------------------------------------------------------------*/
static RAMCFG_HandleTypeDef hramcfg;

/* Private function prototypes ---------------------------------------------------------------------------------------*/
static void MX_SystemClock_Config(void);
static void MX_SystemPower_Config(void);
static void MX_SRAM_WaitState_Config(void);

/* Exported functions ------------------------------------------------------------------------------------------------*/
/**
  * @brief  LpbamAp1 application initialization
  * @param  None
  * @retval None
  */
void MX_LpbamAp1_Init(void)
{
  /* USER CODE BEGIN LpbamAp1_Init 0 */

  /* USER CODE END LpbamAp1_Init 0 */

  /* Configure SRAM wait states */
  MX_SRAM_WaitState_Config();

  /* Configure system clock */
  MX_SystemClock_Config();

  /* Configure system power */
  MX_SystemPower_Config();

  /* USER CODE BEGIN LpbamAp1_Init 1 */

  /* USER CODE END LpbamAp1_Init 1 */
}

/* Private functions -------------------------------------------------------------------------------------------------*/
/**
  * @brief  System Clock Configuration
  * @param  None
  * @retval None
  */
static void MX_SystemClock_Config(void)
{
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};

  /** Initializes the CPU, AHB and APB buses clocks
  */
  if ((__HAL_RCC_GET_PLL_OSCSOURCE() == RCC_PLLSOURCE_MSI) &&
      (__HAL_RCC_GET_SYSCLK_SOURCE() == RCC_SYSCLKSOURCE_STATUS_PLLCLK))
  {
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_SYSCLK;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_4;
  RCC_OscInitStruct.LSIDiv = RCC_LSI_DIV1;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the wakeup clock
  */
  __HAL_RCC_WAKEUPSTOP_CLK_CONFIG(RCC_STOP_WAKEUPCLOCK_MSI);

  /** Disable the force MSIK in stop mode
  */
  __HAL_RCC_MSIKSTOP_DISABLE();

  /** Disable the force HSI in stop mode
  */
  __HAL_RCC_HSISTOP_DISABLE();
}

/**
  * @brief  System Power Configuration
  * @param  None
  * @retval None
  */
static void MX_SystemPower_Config(void)
{
  /* USER CODE BEGIN PWR_Config 0 */

  /* USER CODE END PWR_Config 0 */

  /* Enable PWR interface clock */
  __HAL_RCC_PWR_CLK_ENABLE();

  /* USER CODE BEGIN PWR_Config 1 */

  /* USER CODE END PWR_Config 1 */
}

/**
  * @brief  SRAM Wait State configuration
  * @param  None
  * @retval None
  */
static void MX_SRAM_WaitState_Config(void)
{
  /* RAMCFG clock enable */
  __HAL_RCC_RAMCFG_CLK_ENABLE();

  /*
   * RAMCFG initialization
   */
  hramcfg.Instance = RAMCFG_SRAM4;
  if (HAL_RAMCFG_Init(&hramcfg) != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * RAMCFG wait states configuration
   */
  if (HAL_RAMCFG_ConfigWaitState(&hramcfg, RAMCFG_WAITSTATE_0) != HAL_OK)
  {
    Error_Handler();
  }
}

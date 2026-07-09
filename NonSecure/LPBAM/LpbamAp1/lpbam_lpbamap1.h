/* USER CODE BEGIN Header */
/**
  **********************************************************************************************************************
  * @file    lpbam_lpbamap1.h
  * @author  MCD Application Team
  * @brief   Header for LPBAM LpbamAp1 application
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
/* Define to prevent recursive inclusion -----------------------------------------------------------------------------*/
#ifndef LPBAM_LPBAMAP1_H
#define LPBAM_LPBAMAP1_H

/* Includes ----------------------------------------------------------------------------------------------------------*/
#include "main.h"
#include "stm32_lpbam.h"

/* Exported functions ------------------------------------------------------------------------------------------------*/
/* LpbamAp1 application initialization */
void MX_LpbamAp1_Init(void);

/* LpbamAp1 application Scenario scenario initialization */
void MX_LpbamAp1_Scenario_Init(void);

/* LpbamAp1 application Scenario scenario de-initialization */
void MX_LpbamAp1_Scenario_DeInit(void);

/* LpbamAp1 application Scenario scenario build */
void MX_LpbamAp1_Scenario_Build(void);

/* LpbamAp1 application Scenario scenario link */
void MX_LpbamAp1_Scenario_Link(DMA_HandleTypeDef *hdma);

/* LpbamAp1 application Scenario scenario unlink */
void MX_LpbamAp1_Scenario_UnLink(DMA_HandleTypeDef *hdma);

/* LpbamAp1 application Scenario scenario start */
void MX_LpbamAp1_Scenario_Start(DMA_HandleTypeDef *hdma);

/* LpbamAp1 application Scenario scenario stop */
void MX_LpbamAp1_Scenario_Stop(DMA_HandleTypeDef *hdma);

#endif /* LPBAM_LPBAMAP1_H */

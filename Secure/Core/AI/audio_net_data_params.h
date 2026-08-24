/**
  ******************************************************************************
  * @file    audio_net_data_params.h
  * @author  AST Embedded Analytics Research Platform
  * @date    2026-08-24T14:51:47+0900
  * @brief   AI Tool Automatic Code Generator for Embedded NN computing
  ******************************************************************************
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */

#ifndef AUDIO_NET_DATA_PARAMS_H
#define AUDIO_NET_DATA_PARAMS_H

#include "ai_platform.h"

/*
#define AI_AUDIO_NET_DATA_WEIGHTS_PARAMS \
  (AI_HANDLE_PTR(&ai_audio_net_data_weights_params[1]))
*/

#define AI_AUDIO_NET_DATA_CONFIG               (NULL)


#define AI_AUDIO_NET_DATA_ACTIVATIONS_SIZES \
  { 112196, }
#define AI_AUDIO_NET_DATA_ACTIVATIONS_SIZE     (112196)
#define AI_AUDIO_NET_DATA_ACTIVATIONS_COUNT    (1)
#define AI_AUDIO_NET_DATA_ACTIVATION_1_SIZE    (112196)



#define AI_AUDIO_NET_DATA_WEIGHTS_SIZES \
  { 138136, }
#define AI_AUDIO_NET_DATA_WEIGHTS_SIZE         (138136)
#define AI_AUDIO_NET_DATA_WEIGHTS_COUNT        (1)
#define AI_AUDIO_NET_DATA_WEIGHT_1_SIZE        (138136)



#define AI_AUDIO_NET_DATA_ACTIVATIONS_TABLE_GET() \
  (&g_audio_net_activations_table[1])

extern ai_handle g_audio_net_activations_table[1 + 2];



#define AI_AUDIO_NET_DATA_WEIGHTS_TABLE_GET() \
  (&g_audio_net_weights_table[1])

extern ai_handle g_audio_net_weights_table[1 + 2];


#endif    /* AUDIO_NET_DATA_PARAMS_H */

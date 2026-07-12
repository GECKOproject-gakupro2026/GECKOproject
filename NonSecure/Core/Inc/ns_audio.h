/**
  ******************************************************************************
  * @file    ns_audio.h
  * @brief   NonSecure app-layer audio capture (MIC2/MDF1). Named ns_audio.h
  *          (not audio.h) to avoid colliding with the BSP's
  *          Drivers/BSP/Components/Common/audio.h on the include path.
  ******************************************************************************
  */
#ifndef NS_AUDIO_H
#define NS_AUDIO_H

#include "comm_dto.h"

#ifdef __cplusplus
extern "C" {
#endif

void Audio_Init(void);
void Audio_Refresh(FullStatus_t *st);

/* Low-power mode support (Phase E). */
void Audio_Stop(void);
void Audio_Resume(void);

/* Live PCM window for AI inference (Phase D-2). */
const int16_t *Audio_GetBuffer(uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif /* NS_AUDIO_H */

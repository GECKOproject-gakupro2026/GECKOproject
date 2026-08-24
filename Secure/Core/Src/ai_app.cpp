/**
  ******************************************************************************
  * @file    ai_app.cpp
  * @brief   On-device audio inference wrapper around the stedgeai-generated
  *          network (X-CUBE-AI runtime, Core/AI/audio_net*).
  ******************************************************************************
  */
#include "ai_app.hpp"

#include "main.h"

#include "audio_net.h"
#include "audio_net_data.h"
#include "ai_labels.h"
#include "audio_preproc.hpp"

#include <cstdio>
#include <cstring>

extern "C" uint32_t CommBridge_GetAudioBuffer(int16_t *dst, uint32_t maxSamples);

namespace aiapp
{

namespace
{
ai_handle network = AI_HANDLE_NULL;
AI_ALIGNED(32)
ai_u8 activations[AI_AUDIO_NET_DATA_ACTIVATIONS_SIZE];
bool initDone = false;
bool initOk = false;
int16_t pcmSnapshot[audio_preproc::kPatchSamples];
} // namespace

bool init()
{
  if (initDone)
  {
    return initOk;
  }
  initDone = true;

  const ai_handle actAddr[] = {activations};
  ai_error err = ai_audio_net_create_and_init(&network, actAddr, nullptr);
  if (err.type != AI_ERROR_NONE)
  {
    printf("[AI] init failed (type=%d code=%d)\r\n", err.type, err.code);
    return false;
  }

  ai_network_report report;
  if (ai_audio_net_get_report(network, &report))
  {
    printf("[AI] model \"%s\" ready: %lu MACC, in=%ld, out=%ld classes\r\n",
           report.model_name, (unsigned long)report.n_macc,
           (long)AI_AUDIO_NET_IN_1_SIZE, (long)AI_AUDIO_NET_OUT_1_SIZE);
  }
  initOk = audio_preproc::init();
  if (!initOk)
  {
    printf("[AI] audio preprocessing init failed\r\n");
  }
  return initOk;
}

bool runOnce(Result &out)
{
  if (!init())
  {
    return false;
  }

  ai_buffer *in = ai_audio_net_inputs_get(network, nullptr);
  ai_buffer *outBuf = ai_audio_net_outputs_get(network, nullptr);
  if (in == nullptr || outBuf == nullptr)
  {
    return false;
  }

  uint32_t count = CommBridge_GetAudioBuffer(
      pcmSnapshot, static_cast<uint32_t>(audio_preproc::kPatchSamples));
  if (count != audio_preproc::kPatchSamples)
  {
    printf("[AI] audio snapshot unavailable (%lu/%lu samples)\r\n",
           (unsigned long)count,
           (unsigned long)audio_preproc::kPatchSamples);
    return false;
  }

  uint32_t prepStart = HAL_GetTick();
  if (!audio_preproc::run(pcmSnapshot, count,
                          reinterpret_cast<int8_t *>(in[0].data)))
  {
    printf("[AI] preprocessing failed\r\n");
    return false;
  }
  out.preprocessUs = (HAL_GetTick() - prepStart) * 1000U;

  uint32_t t0 = HAL_GetTick();
  if (ai_audio_net_run(network, in, outBuf) != 1)
  {
    ai_error err = ai_audio_net_get_error(network);
    printf("[AI] run failed (type=%d code=%d)\r\n", err.type, err.code);
    return false;
  }
  uint32_t dtMs = HAL_GetTick() - t0;

  const ai_float *probs = reinterpret_cast<const ai_float *>(outBuf[0].data);
  out.nClasses = (AI_AUDIO_NET_OUT_1_SIZE < kMaxClasses)
                     ? AI_AUDIO_NET_OUT_1_SIZE : kMaxClasses;
  out.topClass = 0;
  for (uint8_t i = 0; i < out.nClasses; i++)
  {
    out.scores[i] = probs[i];
    if (probs[i] > out.scores[out.topClass])
    {
      out.topClass = i;
    }
  }
  out.inferenceUs = dtMs * 1000U;
  return true;
}

const char *label(uint8_t cls)
{
  return (cls < AI_LABELS_COUNT) ? AI_LABELS[cls] : "?";
}

} // namespace aiapp

extern "C" int AiBridge_RunOnce(void)
{
  aiapp::Result result = {};
  if (!aiapp::runOnce(result))
  {
    return -1;
  }
  printf("[AI] %s", aiapp::label(result.topClass));
  for (uint8_t i = 0; i < result.nClasses; ++i)
  {
    printf("  %s=%d%%", aiapp::label(i),
           (int)(result.scores[i] * 100.0F + 0.5F));
  }
  printf("  pre=%lums infer=%lums\r\n",
         (unsigned long)(result.preprocessUs / 1000U),
         (unsigned long)(result.inferenceUs / 1000U));
  return 0;
}

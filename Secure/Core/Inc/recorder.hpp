/**
  ******************************************************************************
  * @file    recorder.hpp
  * @brief   One-shot BLE voice recording: captures the resample-corrected
  *          mic stream into a fixed SRAM ring, ADPCM-compressed, until
  *          stopped or the buffer fills. Recording lives in Secure because
  *          the resampled audio pipeline (audio_capture::FeedForResample /
  *          PopResampled, driven by the Secure-only g_AudioEvents DMA flags)
  *          has no NonSecure-side equivalent - see comm_service.cpp's
  *          poll() audio block.
  ******************************************************************************
  */
#ifndef RECORDER_HPP
#define RECORDER_HPP

#include <cstddef>
#include <cstdint>

namespace recorder
{

/* 96 KiB at ~8 KiB/s (IMA ADPCM 4-bit, 16 kHz mono) ~= 12 s. Secure RAM is
 * 256 KiB with ~42 KiB already used by .data+.bss (see B-U585I-IOT02A_Secure.map),
 * so this comfortably fits without touching NonSecure RAM or any NOR flash -
 * NOR stays reserved for OTA staging/backup (ota.hpp's 0x200000-0x5FFFFF). */
constexpr uint32_t kBufBytes = 96U * 1024U;

/* Starts a new recording: clears the ring, resets the ADPCM state. Safe to
 * call while already active (restarts). */
void Start();

/* Stops the current recording (no-op if not active). Returns the number of
 * ADPCM bytes captured (== ready to stream via Read()). */
uint32_t Stop();

bool Active();

/* Feeds resample-corrected PCM samples into the ADPCM encoder and appends to
 * the ring. Call from Service::poll() with whatever the caller already
 * popped via audio_capture::PopResampled(). No-op if not Active(). Stops
 * automatically (see Active()) once kBufBytes is reached - the ring is never
 * overwritten. */
void FeedPcm(const int16_t *pcm, size_t count);

uint32_t UsedBytes();
uint32_t TotalSamples();

/* Copies up to `len` recorded bytes starting at `off` into `dst`. Returns
 * the number of bytes copied (0 once off >= UsedBytes()). */
uint32_t Read(uint32_t off, uint8_t *dst, uint32_t len);

} // namespace recorder

#endif /* RECORDER_HPP */

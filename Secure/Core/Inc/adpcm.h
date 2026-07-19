/**
  ******************************************************************************
  * @file    adpcm.h
  * @brief   IMA ADPCM 4-bit block codec (16 kHz mono int16 PCM <-> 4 bytes
  *          overhead + packed nibbles). Pure functions, state passed by the
  *          caller, no globals - safe to use from both Secure and NonSecure
  *          builds and reentrant across independent streams.
  *
  *          Block layout (encode() output / decode() input):
  *            [predictor int16 LE][step_index u8][pad u8][nibbles...]
  *          Each block is independently decodable (the header seeds the
  *          predictor/step_index), giving packet-loss tolerance at a 4-byte
  *          cost per block.
  ******************************************************************************
  */
#ifndef ADPCM_H
#define ADPCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADPCM_BLOCK_HEADER_SIZE 4U

typedef struct
{
  int16_t predictor;
  int8_t step_index;
} adpcm_state_t;

/* Encodes `count` PCM samples into a single self-contained block: a 4-byte
 * header (current predictor/step_index) followed by ceil(count/2) bytes of
 * packed 4-bit nibbles. `state` is updated in place so the caller can chain
 * calls, but the header always reflects the state *before* this call, so
 * each block can be decoded independently of any other.
 * `out` must be at least ADPCM_BLOCK_HEADER_SIZE + (count+1)/2 bytes.
 * Returns the number of bytes written. */
size_t adpcm_encode(adpcm_state_t *state, const int16_t *pcm, size_t count,
                    uint8_t *out);

/* Decodes a block produced by adpcm_encode(). `state` is seeded from the
 * block header (the caller's prior state is ignored) and updated in place.
 * `nbytes` is the total block size (header + nibble bytes). `pcm` must be
 * at least 2*(nbytes-ADPCM_BLOCK_HEADER_SIZE) samples.
 * Returns the number of PCM samples written. */
size_t adpcm_decode(adpcm_state_t *state, const uint8_t *in, size_t nbytes,
                    int16_t *pcm);

/* Encodes `count` PCM samples as raw nibbles ONLY (no per-block header),
 * chaining from the caller's `state`. Use this to build one continuous ADPCM
 * stream across many calls: write a single header once (the recorder does this
 * at Start via adpcm_encode of 0 samples, or by storing the initial state) and
 * then append with this. Because the predictor/step_index are never reset
 * mid-stream, there are no per-block boundary discontinuities (clicks).
 * `count` must be even (the recorder feeds fixed even-sized frames), writing
 * count/2 bytes. Returns the number of bytes written. */
size_t adpcm_encode_nibbles(adpcm_state_t *state, const int16_t *pcm,
                            size_t count, uint8_t *out);

/* Decodes a continuous nibble stream (no header) produced by
 * adpcm_encode_nibbles(), chaining from the caller's `state` (which must be
 * seeded to the stream's initial predictor/step_index). Writes 2*nbytes
 * samples. Returns the number of PCM samples written. */
size_t adpcm_decode_nibbles(adpcm_state_t *state, const uint8_t *in,
                            size_t nbytes, int16_t *pcm);

#ifdef __cplusplus
}
#endif

#endif /* ADPCM_H */

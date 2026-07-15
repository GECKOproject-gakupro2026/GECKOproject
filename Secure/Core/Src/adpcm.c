/**
  ******************************************************************************
  * @file    adpcm.c
  * @brief   IMA ADPCM 4-bit block codec implementation. See adpcm.h.
  ******************************************************************************
  */
#include "adpcm.h"

/* Standard IMA ADPCM step size table (89 entries). */
static const int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

/* Standard IMA ADPCM step index adjustment table (16 entries, one per
 * 4-bit nibble value). */
static const int8_t kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8};

static int8_t clamp_index(int16_t idx)
{
  if (idx < 0) return 0;
  if (idx > 88) return 88;
  return (int8_t)idx;
}

static int16_t clamp_sample(int32_t v)
{
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

/* Encodes one PCM sample into a 4-bit nibble and advances *state. */
static uint8_t encode_sample(adpcm_state_t *state, int16_t sample)
{
  int32_t diff = (int32_t)sample - state->predictor;
  uint8_t sign = 0U;
  if (diff < 0)
  {
    sign = 8U;
    diff = -diff;
  }

  int32_t step = kStepTable[state->step_index];
  int32_t diffq = step >> 3;
  uint8_t nibble = 0U;

  if (diff >= step)
  {
    nibble |= 4U;
    diff -= step;
    diffq += step;
  }
  step >>= 1;
  if (diff >= step)
  {
    nibble |= 2U;
    diff -= step;
    diffq += step;
  }
  step >>= 1;
  if (diff >= step)
  {
    nibble |= 1U;
    diffq += step;
  }

  int32_t predicted = state->predictor + (sign != 0U ? -diffq : diffq);
  state->predictor = clamp_sample(predicted);

  nibble |= sign;
  state->step_index = clamp_index((int16_t)(state->step_index + kIndexTable[nibble]));

  return nibble;
}

static int16_t decode_nibble(adpcm_state_t *state, uint8_t nibble)
{
  int32_t step = kStepTable[state->step_index];
  int32_t diffq = step >> 3;

  if ((nibble & 4U) != 0U) diffq += step;
  step >>= 1;
  if ((nibble & 2U) != 0U) diffq += step;
  step >>= 1;
  if ((nibble & 1U) != 0U) diffq += step;

  int32_t predicted = state->predictor;
  predicted += ((nibble & 8U) != 0U) ? -diffq : diffq;
  state->predictor = clamp_sample(predicted);

  state->step_index = clamp_index((int16_t)(state->step_index + kIndexTable[nibble]));

  return state->predictor;
}

size_t adpcm_encode(adpcm_state_t *state, const int16_t *pcm, size_t count,
                    uint8_t *out)
{
  out[0] = (uint8_t)((uint16_t)state->predictor & 0xFFU);
  out[1] = (uint8_t)(((uint16_t)state->predictor >> 8) & 0xFFU);
  out[2] = (uint8_t)state->step_index;
  out[3] = 0U;

  size_t outPos = ADPCM_BLOCK_HEADER_SIZE;
  size_t i = 0U;
  while (i < count)
  {
    uint8_t lo = encode_sample(state, pcm[i]);
    i++;
    uint8_t hi = 0U;
    if (i < count)
    {
      hi = encode_sample(state, pcm[i]);
      i++;
    }
    out[outPos] = (uint8_t)(lo | (hi << 4));
    outPos++;
  }
  return outPos;
}

size_t adpcm_decode(adpcm_state_t *state, const uint8_t *in, size_t nbytes,
                    int16_t *pcm)
{
  if (nbytes < ADPCM_BLOCK_HEADER_SIZE)
  {
    return 0U;
  }

  uint16_t predictor = (uint16_t)in[0] | ((uint16_t)in[1] << 8);
  state->predictor = (int16_t)predictor;
  state->step_index = clamp_index((int8_t)in[2]);

  size_t outCount = 0U;
  for (size_t i = ADPCM_BLOCK_HEADER_SIZE; i < nbytes; i++)
  {
    uint8_t byte = in[i];
    pcm[outCount] = decode_nibble(state, (uint8_t)(byte & 0x0FU));
    outCount++;
    pcm[outCount] = decode_nibble(state, (uint8_t)((byte >> 4) & 0x0FU));
    outCount++;
  }
  return outCount;
}

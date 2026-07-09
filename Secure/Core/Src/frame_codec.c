/**
  ******************************************************************************
  * @file    frame_codec.c
  * @brief   Command frame codec (requirement spec section 11.2).
  ******************************************************************************
  */
#include "frame_codec.h"

#include <string.h>

uint16_t Frame_Crc16(const uint8_t *data, size_t len)
{
  /* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout */
  uint16_t crc = 0xFFFFU;
  for (size_t i = 0; i < len; i++)
  {
    crc ^= (uint16_t)((uint16_t)data[i] << 8);
    for (int b = 0; b < 8; b++)
    {
      crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

size_t Frame_Encode(uint8_t cmd, uint8_t seq, const uint8_t *payload,
                    size_t payload_len, uint8_t *out, size_t out_size)
{
  if (payload_len > FRAME_MAX_PAYLOAD || out_size < payload_len + FRAME_OVERHEAD)
  {
    return 0;
  }
  out[0] = FRAME_SOF;
  out[1] = cmd;
  out[2] = seq;
  out[3] = (uint8_t)(payload_len & 0xFFU);
  out[4] = (uint8_t)(payload_len >> 8);
  if (payload_len > 0U)
  {
    memcpy(&out[5], payload, payload_len);
  }
  uint16_t crc = Frame_Crc16(out, 5U + payload_len);
  out[5U + payload_len] = (uint8_t)(crc & 0xFFU);
  out[6U + payload_len] = (uint8_t)(crc >> 8);
  out[7U + payload_len] = FRAME_EOF;
  return payload_len + FRAME_OVERHEAD;
}

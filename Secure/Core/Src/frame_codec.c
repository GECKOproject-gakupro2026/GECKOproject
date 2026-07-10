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

void Frame_DecoderInit(Frame_Decoder *dec)
{
  dec->pos = 0;
}

Frame_FeedResult Frame_DecoderFeed(Frame_Decoder *dec, uint8_t byte,
                                   uint8_t *cmd, uint8_t *seq,
                                   const uint8_t **payload, uint16_t *len)
{
  if (dec->pos == 0U)
  {
    if (byte != FRAME_SOF)
    {
      return FRAME_FEED_PLAIN;
    }
    dec->buf[dec->pos++] = byte;
    return FRAME_FEED_CONSUMED;
  }

  dec->buf[dec->pos++] = byte;

  if (dec->pos >= 5U)
  {
    uint16_t plen = (uint16_t)(dec->buf[3] | ((uint16_t)dec->buf[4] << 8));
    if (plen > FRAME_MAX_PAYLOAD)
    {
      dec->pos = 0; /* corrupt header: resync */
      return FRAME_FEED_CONSUMED;
    }
    uint16_t total = (uint16_t)(plen + FRAME_OVERHEAD);
    if (dec->pos == total)
    {
      uint16_t rxCrc = (uint16_t)(dec->buf[5U + plen] |
                                  ((uint16_t)dec->buf[6U + plen] << 8));
      dec->pos = 0;
      if (dec->buf[total - 1U] == FRAME_EOF &&
          rxCrc == Frame_Crc16(dec->buf, 5U + plen))
      {
        *cmd = dec->buf[1];
        *seq = dec->buf[2];
        *payload = &dec->buf[5];
        *len = plen;
        return FRAME_FEED_COMPLETE;
      }
      return FRAME_FEED_CONSUMED; /* bad CRC/EOF: drop silently */
    }
  }
  return FRAME_FEED_CONSUMED;
}

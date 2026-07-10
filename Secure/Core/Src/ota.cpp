/**
  ******************************************************************************
  * @file    ota.cpp
  * @brief   OTA firmware staging to the external NOR flash (area B).
  ******************************************************************************
  */
#include "ota.hpp"

#include "frame_codec.h"
#include "main.h"

#include "b_u585i_iot02a_ospi.h"

#include <cstdio>
#include <cstring>

namespace ota
{

namespace
{
/* Incremental CRC-16/CCITT-FALSE (Frame_Crc16 == crc16Update(0xFFFF, ...)) */
uint16_t crc16Update(uint16_t crc, const uint8_t *data, size_t len)
{
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
} // namespace

bool Manager::ensureNorReady()
{
  if (norReady_)
  {
    return true;
  }
  BSP_OSPI_NOR_Init_t init = {};
  init.InterfaceMode = BSP_OSPI_NOR_OPI_MODE;
  init.TransferRate = BSP_OSPI_NOR_STR_TRANSFER;
  if (BSP_OSPI_NOR_Init(0, &init) != BSP_ERROR_NONE)
  {
    printf("[OTA] NOR init failed\r\n");
    return false;
  }
  norReady_ = true;
  return true;
}

uint8_t Manager::ensureErased(uint32_t endOffset)
{
  while (erasedUpTo_ < endOffset)
  {
    if (BSP_OSPI_NOR_Erase_Block(0, kStagingBase + erasedUpTo_,
                                 BSP_OSPI_NOR_ERASE_64K) != BSP_ERROR_NONE)
    {
      return FRAME_ERR_ERASE;
    }
    while (BSP_OSPI_NOR_GetStatus(0) == BSP_ERROR_BUSY)
    {
      HAL_Delay(5);
    }
    erasedUpTo_ += kEraseBlock;
  }
  return 0;
}

uint8_t Manager::writeChunk(const uint8_t *payload, uint16_t len)
{
  if (len <= 4U)
  {
    lastError_ = FRAME_ERR_BAD_OFFSET;
    return lastError_;
  }
  uint32_t offset;
  memcpy(&offset, payload, 4);
  const uint8_t *data = payload + 4;
  uint32_t dlen = (uint32_t)len - 4U;

  if (offset == 0U)
  {
    /* first chunk (re)starts a transfer */
    state_ = State::Receiving;
    received_ = 0;
    erasedUpTo_ = 0;
    expected_ = 0;
    imageCrc_ = 0;
    lastError_ = 0;
    if (!ensureNorReady())
    {
      state_ = State::Error;
      lastError_ = FRAME_ERR_WRITE;
      return lastError_;
    }
    printf("[OTA] receiving new image...\r\n");
  }

  if (state_ != State::Receiving)
  {
    lastError_ = FRAME_ERR_BAD_STATE;
    return lastError_;
  }
  if (offset + dlen == received_)
  {
    return 0; /* duplicate of the last chunk (our ACK was lost) - re-ACK */
  }
  if (offset != received_)
  {
    lastError_ = FRAME_ERR_BAD_OFFSET;
    return lastError_;
  }
  if (offset + dlen > kStagingSize)
  {
    state_ = State::Error;
    lastError_ = FRAME_ERR_TOO_LARGE;
    return lastError_;
  }

  uint8_t err = ensureErased(offset + dlen);
  if (err != 0U)
  {
    state_ = State::Error;
    lastError_ = err;
    return err;
  }
  if (BSP_OSPI_NOR_Write(0, data, kStagingBase + offset, dlen) != BSP_ERROR_NONE)
  {
    state_ = State::Error;
    lastError_ = FRAME_ERR_WRITE;
    return lastError_;
  }
  received_ = offset + dlen;
  return 0;
}

uint8_t Manager::complete(const uint8_t *payload, uint16_t len)
{
  if (len < 6U || state_ != State::Receiving)
  {
    lastError_ = FRAME_ERR_BAD_STATE;
    return lastError_;
  }
  uint32_t size;
  uint16_t crc;
  memcpy(&size, payload, 4);
  memcpy(&crc, payload + 4, 2);

  if (size != received_)
  {
    state_ = State::Error;
    lastError_ = FRAME_ERR_BAD_STATE;
    return lastError_;
  }

  /* Read the staged image back and verify the announced CRC */
  uint8_t buf[512];
  uint16_t calc = 0xFFFFU;
  for (uint32_t off = 0; off < size; off += sizeof(buf))
  {
    uint32_t n = (size - off) < sizeof(buf) ? (size - off) : sizeof(buf);
    if (BSP_OSPI_NOR_Read(0, buf, kStagingBase + off, n) != BSP_ERROR_NONE)
    {
      state_ = State::Error;
      lastError_ = FRAME_ERR_VERIFY;
      return lastError_;
    }
    calc = crc16Update(calc, buf, n);
  }

  if (calc != crc)
  {
    printf("[OTA] verify FAILED: crc calc=0x%04X announced=0x%04X\r\n", calc, crc);
    state_ = State::Error;
    lastError_ = FRAME_ERR_VERIFY;
    return lastError_;
  }

  expected_ = size;
  imageCrc_ = crc;
  state_ = State::Staged;
  printf("[OTA] image staged: %lu bytes, crc=0x%04X (NOR 0x%08lX)\r\n",
         (unsigned long)size, crc, (unsigned long)kStagingBase);
  return 0;
}

void Manager::fillReport(StatusReport &r) const
{
  r.state = static_cast<uint8_t>(state_);
  r.received = received_;
  r.expected = expected_;
  r.image_crc = imageCrc_;
  r.last_error = lastError_;
}

void Manager::reset()
{
  state_ = State::Idle;
  received_ = 0;
  expected_ = 0;
  erasedUpTo_ = 0;
  imageCrc_ = 0;
  lastError_ = 0;
}

} // namespace ota

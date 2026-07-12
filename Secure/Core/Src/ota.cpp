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
constexpr uint32_t kNsFlashBase = 0x08100000U;
constexpr uint32_t kNsFlashSize = 0x00100000U;
constexpr uint32_t kFlashPageSize = 0x2000U;
constexpr uint32_t kBackupMetaMagic = 0x42544D45U; /* "EMTB" */

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

uint16_t metaSelfCrc(const SlotMeta &m)
{
  uint16_t crc = 0xFFFFU;
  crc = crc16Update(crc, reinterpret_cast<const uint8_t *>(&m.magic), sizeof(m.magic));
  crc = crc16Update(crc, reinterpret_cast<const uint8_t *>(&m.size), sizeof(m.size));
  crc = crc16Update(crc, reinterpret_cast<const uint8_t *>(&m.crc16), sizeof(m.crc16));
  crc = crc16Update(crc, reinterpret_cast<const uint8_t *>(&m.version), sizeof(m.version));
  return crc;
}
} // namespace

bool Manager::validateNonSecureImage()
{
  if (state_ != State::Staged || expected_ < 8U || expected_ > kNsFlashSize)
  {
    return false;
  }
  uint32_t vectors[2] = {};
  if (BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(vectors),
                        kStagingBase, sizeof(vectors)) != BSP_ERROR_NONE)
  {
    return false;
  }
  const bool stackOk = vectors[0] >= 0x20040000U && vectors[0] <= 0x200C0000U;
  const bool resetOk = (vectors[1] & 1U) != 0U &&
                       (vectors[1] & ~1U) >= kNsFlashBase &&
                       (vectors[1] & ~1U) < (kNsFlashBase + expected_);
  return stackOk && resetOk;
}

bool Manager::backupCurrentBank2()
{
  /* Snapshot the currently-running Bank2 image (if any) into NOR slot B
   * before overwriting Bank2, so a bad update can be rolled back. If Bank2
   * doesn't hold a valid image yet (first-ever apply), skip the backup -
   * there's nothing good to preserve. */
  const uint32_t sp = *reinterpret_cast<const uint32_t *>(kNsFlashBase);
  const uint32_t reset = *reinterpret_cast<const uint32_t *>(kNsFlashBase + 4U);
  const uint32_t magic = *reinterpret_cast<const uint32_t *>(kNsFlashBase + 0x400U);
  const uint32_t version = *reinterpret_cast<const uint32_t *>(kNsFlashBase + 0x404U);
  const bool spOk = sp >= 0x20040000U && sp <= 0x200C0000U;
  const bool resetOk = (reset & 1U) != 0U &&
                       (reset & ~1U) >= kNsFlashBase &&
                       (reset & ~1U) < (kNsFlashBase + kNsFlashSize);
  if (!spOk || !resetOk || magic != 0x4E534150U /* "NSAP" */ ||
      version == 0U || version == 0xFFFFFFFFU)
  {
    printf("[OTA] Bank2 has no valid image yet, skipping backup\r\n");
    return true; /* not an error - just nothing to back up */
  }

  printf("[OTA] backing up current Bank2 (v%lu) to NOR slot B...\r\n",
         (unsigned long)version);
  /* Only the kNsFlashSize (1 MB / 16 blocks) actually holding the image
   * needs erasing - looping all the way to kBackupMetaOffset (1.9375 MB /
   * 31 blocks) wasted ~15 blocks erasing NOR that's never written here,
   * which was slow enough (each 64 KB erase can take on the order of a
   * second) to blow through the PC tool's 60 s FW_APPLY timeout on its
   * own, well before Bank2 itself was ever touched. */
  for (uint32_t off = 0; off < kNsFlashSize; off += kEraseBlock)
  {
    if (BSP_OSPI_NOR_Erase_Block(0, kBackupBase + off, BSP_OSPI_NOR_ERASE_64K) !=
        BSP_ERROR_NONE)
    {
      return false;
    }
    while (BSP_OSPI_NOR_GetStatus(0) == BSP_ERROR_BUSY)
    {
      HAL_Delay(5);
    }
  }
  if (BSP_OSPI_NOR_Write(0, reinterpret_cast<const uint8_t *>(kNsFlashBase),
                         kBackupBase, kNsFlashSize) != BSP_ERROR_NONE)
  {
    return false;
  }

  SlotMeta meta = {};
  meta.magic = kBackupMetaMagic;
  meta.size = kNsFlashSize;
  meta.crc16 = crc16Update(0xFFFFU,
      reinterpret_cast<const uint8_t *>(kNsFlashBase), kNsFlashSize);
  meta.version = version;
  meta.selfCrc = metaSelfCrc(meta);
  if (BSP_OSPI_NOR_Erase_Block(0, kBackupBase + kBackupMetaOffset,
                               BSP_OSPI_NOR_ERASE_64K) != BSP_ERROR_NONE)
  {
    return false;
  }
  while (BSP_OSPI_NOR_GetStatus(0) == BSP_ERROR_BUSY)
  {
    HAL_Delay(5);
  }
  /* write the meta twice (primary + mirror) within the same erased block */
  if (BSP_OSPI_NOR_Write(0, reinterpret_cast<const uint8_t *>(&meta),
                         kBackupBase + kBackupMetaOffset, sizeof(meta)) != BSP_ERROR_NONE ||
      BSP_OSPI_NOR_Write(0, reinterpret_cast<const uint8_t *>(&meta),
                         kBackupBase + kBackupMetaOffset + sizeof(meta), sizeof(meta)) !=
          BSP_ERROR_NONE)
  {
    return false;
  }
  printf("[OTA] Bank2 backup complete, crc=0x%04X\r\n", meta.crc16);
  return true;
}

bool Manager::readBackupMeta(SlotMeta &meta)
{
  SlotMeta primary = {};
  SlotMeta mirror = {};
  if (BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(&primary),
                        kBackupBase + kBackupMetaOffset, sizeof(primary)) != BSP_ERROR_NONE)
  {
    return false;
  }
  if (BSP_OSPI_NOR_Read(0, reinterpret_cast<uint8_t *>(&mirror),
                        kBackupBase + kBackupMetaOffset + sizeof(primary), sizeof(mirror)) !=
      BSP_ERROR_NONE)
  {
    return false;
  }
  const bool primaryOk = primary.magic == kBackupMetaMagic &&
                         primary.selfCrc == metaSelfCrc(primary);
  const bool mirrorOk = mirror.magic == kBackupMetaMagic &&
                        mirror.selfCrc == metaSelfCrc(mirror);
  if (primaryOk)
  {
    meta = primary;
    return true;
  }
  if (mirrorOk)
  {
    meta = mirror;
    return true;
  }
  return false;
}

bool Manager::hasValidBackup()
{
  if (!ensureNorReady())
  {
    return false;
  }
  SlotMeta meta;
  return readBackupMeta(meta) && meta.size > 0U && meta.size <= kNsFlashSize;
}

bool Manager::restoreFromBackup()
{
  if (!ensureNorReady())
  {
    return false;
  }
  SlotMeta meta;
  if (!readBackupMeta(meta) || meta.size == 0U || meta.size > kNsFlashSize)
  {
    printf("[OTA] no valid backup slot to restore from\r\n");
    return false;
  }

  printf("[OTA] restoring Bank2 from NOR backup slot B (v%lu, %lu bytes)...\r\n",
         (unsigned long)meta.version, (unsigned long)meta.size);
  HAL_FLASH_Unlock();
  FLASH_EraseInitTypeDef erase = {};
  erase.TypeErase = FLASH_TYPEERASE_PAGES_NS;
  erase.Banks = FLASH_BANK_2;
  erase.Page = 0U;
  erase.NbPages = (meta.size + kFlashPageSize - 1U) / kFlashPageSize;
  uint32_t pageError = 0xFFFFFFFFU;
  if (HAL_FLASHEx_Erase(&erase, &pageError) != HAL_OK)
  {
    HAL_FLASH_Lock();
    printf("[OTA] restore erase failed at page %lu\r\n", (unsigned long)pageError);
    return false;
  }

  alignas(16) uint8_t quad[16];
  for (uint32_t off = 0; off < meta.size; off += sizeof(quad))
  {
    memset(quad, 0xFF, sizeof(quad));
    const uint32_t n = (meta.size - off) < sizeof(quad) ? (meta.size - off) : sizeof(quad);
    if (BSP_OSPI_NOR_Read(0, quad, kBackupBase + off, n) != BSP_ERROR_NONE ||
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD_NS, kNsFlashBase + off,
                          reinterpret_cast<uint32_t>(quad)) != HAL_OK)
    {
      HAL_FLASH_Lock();
      printf("[OTA] restore program failed at +0x%08lX\r\n", (unsigned long)off);
      return false;
    }
  }
  HAL_FLASH_Lock();

  /* Bank2 was just erased+reprogrammed - ICACHE may still hold stale lines
   * from before the erase (or from Bank2 code that was executing pre-OTA).
   * Without this, the verify read below can see old/garbage data even
   * though the flash itself was written correctly (root cause of the
   * "no reply"/VERIFY-mismatch OTA-apply failures seen with larger
   * NonSecure images, which touch far more cache lines than the small
   * images this path was first tested with). */
  (void)HAL_ICACHE_Invalidate();

  const uint16_t crc = crc16Update(0xFFFFU,
      reinterpret_cast<const uint8_t *>(kNsFlashBase), meta.size);
  if (crc != meta.crc16)
  {
    printf("[OTA] restore verify failed: 0x%04X != 0x%04X\r\n", crc, meta.crc16);
    return false;
  }
  printf("[OTA] Bank2 restored from backup, crc=0x%04X\r\n", crc);
  return true;
}

uint8_t Manager::applyToNonSecure()
{
  if (!ensureNorReady() || !validateNonSecureImage())
  {
    lastError_ = FRAME_ERR_BAD_STATE;
    return lastError_;
  }

  if (!backupCurrentBank2())
  {
    printf("[OTA] Bank2 backup failed, aborting apply (current image left intact)\r\n");
    lastError_ = FRAME_ERR_WRITE;
    return lastError_;
  }

  printf("[OTA] applying %lu-byte NonSecure image to Bank2...\r\n",
         (unsigned long)expected_);

  /* Erasing Bank2 pulls the running NonSecure image out from under any
   * interrupt whose vector/handler lives there (SCB_NS->VTOR points into
   * Bank2 once the NS app is running). If a Secure or NonSecure IRQ fires
   * between the erase and the re-program, the CPU fetches from the now-blank
   * Bank2 and HardFaults - which is exactly what happened applying a large
   * image while the NS app was live (small images tested earlier happened to
   * finish before any such IRQ landed). Mask interrupts across the whole
   * erase+program window; the FW_APPLY handler jumps straight to the fresh
   * image afterwards (Secure_JumpToNonSecure re-enables), and the error
   * paths below re-enable before returning. */
  __disable_irq();

  HAL_FLASH_Unlock();
  FLASH_EraseInitTypeDef erase = {};
  erase.TypeErase = FLASH_TYPEERASE_PAGES_NS;
  erase.Banks = FLASH_BANK_2;
  erase.Page = 0U;
  erase.NbPages = (expected_ + kFlashPageSize - 1U) / kFlashPageSize;
  uint32_t pageError = 0xFFFFFFFFU;
  if (HAL_FLASHEx_Erase(&erase, &pageError) != HAL_OK)
  {
    HAL_FLASH_Lock();
    __enable_irq();
    printf("[OTA] Bank2 erase failed at page %lu, HAL=0x%08lX\r\n",
           (unsigned long)pageError, (unsigned long)HAL_FLASH_GetError());
    lastError_ = FRAME_ERR_ERASE;
    return lastError_;
  }

  alignas(16) uint8_t quad[16];
  for (uint32_t off = 0; off < expected_; off += sizeof(quad))
  {
    memset(quad, 0xFF, sizeof(quad));
    const uint32_t n = (expected_ - off) < sizeof(quad)
                           ? (expected_ - off) : sizeof(quad);
    if (BSP_OSPI_NOR_Read(0, quad, kStagingBase + off, n) != BSP_ERROR_NONE ||
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD_NS, kNsFlashBase + off,
                          reinterpret_cast<uint32_t>(quad)) != HAL_OK)
    {
      HAL_FLASH_Lock();
      __enable_irq();
      printf("[OTA] Bank2 program failed at +0x%08lX, HAL=0x%08lX\r\n",
             (unsigned long)off, (unsigned long)HAL_FLASH_GetError());
      lastError_ = FRAME_ERR_WRITE;
      return lastError_;
    }
  }
  HAL_FLASH_Lock();

  /* Bank2 now holds a complete image again, so it's safe to take interrupts:
   * the CRC verify + ACK send below need SysTick (HAL_GetTick timeouts) and
   * the UART TX interrupt. The final jump is protected separately by the
   * ICACHE invalidate in the FW_APPLY handler. */
  __enable_irq();

  /* See restoreFromBackup() for why this is required: without invalidating
   * ICACHE after erase+program, the verify read can return stale cached
   * data instead of what was just written to Bank2. */
  (void)HAL_ICACHE_Invalidate();

  const uint16_t crc = crc16Update(0xFFFFU,
      reinterpret_cast<const uint8_t *>(kNsFlashBase), expected_);
  if (crc != imageCrc_)
  {
    printf("[OTA] Bank2 verify failed: 0x%04X != 0x%04X\r\n", crc, imageCrc_);
    lastError_ = FRAME_ERR_VERIFY;
    return lastError_;
  }
  printf("[OTA] Bank2 apply verified, crc=0x%04X\r\n", crc);
  lastError_ = 0U;
  return 0U;
}

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

/* extern "C" bridge so main.c (Stage-0, plain C) can trigger a rollback
 * without needing the full C++ Manager type. Uses a scratch instance since
 * restoreFromBackup()/hasValidBackup() don't depend on transfer state. */
extern "C" int OTA_RestoreFromBackup(void)
{
  ota::Manager mgr;
  return mgr.restoreFromBackup() ? 1 : 0;
}

extern "C" int OTA_HasValidBackup(void)
{
  ota::Manager mgr;
  return mgr.hasValidBackup() ? 1 : 0;
}

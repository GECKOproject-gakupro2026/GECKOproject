/**
  ******************************************************************************
  * @file    ota.hpp
  * @brief   OTA firmware staging: receives an image over the command frame
  *          protocol (11.2) and stores it in the external NOR flash firmware
  *          area B (0x200000, memory map 11.3). applyToNonSecure() copies the
  *          staged candidate to internal Bank2, backing up the current good
  *          Bank2 image to NOR slot B (0x400000) first so a failed boot can
  *          be rolled back by the Stage-0 loader (see BootGuard in main.c).
  ******************************************************************************
  */
#ifndef OTA_HPP
#define OTA_HPP

#include <cstdint>

namespace ota
{

enum class State : uint8_t
{
  Idle = 0,
  Receiving = 1,
  Staged = 2,   /* image complete and CRC-verified in NOR */
  Error = 3
};

/* STATUS_RESP payload (little endian, packed).
 * Python: "<BIIHB" size 12 */
struct __attribute__((packed)) StatusReport
{
  uint8_t state;
  uint32_t received;
  uint32_t expected;
  uint16_t image_crc;
  uint8_t last_error;
};

/* Persisted in external NOR, two copies (primary + mirror) so a power loss
 * mid-write leaves at least one intact copy. 32 bytes, self-CRC'd. */
struct __attribute__((packed)) SlotMeta
{
  uint32_t magic;     /* 0x42544D45 = "EMTB" (backup slot marker) */
  uint32_t size;
  uint16_t crc16;
  uint16_t reserved;
  uint32_t version;
  uint32_t selfCrc;    /* CRC16 (in low 16 bits) over the fields above */
  uint8_t pad[16];
};

class Manager
{
public:
  static constexpr uint32_t kStagingBase = 0x200000U; /* NOR FW area A (candidate) */
  static constexpr uint32_t kStagingSize = 0x200000U; /* 2 MB                      */
  static constexpr uint32_t kBackupBase = 0x400000U;  /* NOR FW area B (last-good) */
  static constexpr uint32_t kBackupSize = 0x200000U;  /* 2 MB                      */
  static constexpr uint32_t kBackupMetaOffset = 0x1F0000U; /* meta lives at slot end */
  static constexpr uint32_t kEraseBlock = 0x10000U;   /* 64 KB blocks   */

  /* Handles one FW_CHUNK: payload = offset u32 LE + data.
   * Returns 0 on success or a FRAME_ERR_* code. */
  uint8_t writeChunk(const uint8_t *payload, uint16_t len);

  /* Handles FW_COMPLETE: payload = total size u32 + crc16 u16.
   * Reads the staged image back and verifies the CRC.
   * Returns 0 on success or a FRAME_ERR_* code. */
  uint8_t complete(const uint8_t *payload, uint16_t len);
  uint8_t applyToNonSecure();

  /* Restores NOR backup slot B into internal Bank2. Used by the Stage-0
   * BootGuard after repeated failed boots. Returns true on success. */
  bool restoreFromBackup();
  bool hasValidBackup();

  void fillReport(StatusReport &r) const;
  void reset();

private:
  bool ensureNorReady();
  bool validateNonSecureImage();
  uint8_t ensureErased(uint32_t endOffset);
  bool backupCurrentBank2();
  bool readBackupMeta(SlotMeta &meta);

  State state_ = State::Idle;
  uint32_t received_ = 0;
  uint32_t expected_ = 0;
  uint32_t erasedUpTo_ = 0;
  uint16_t imageCrc_ = 0;
  uint8_t lastError_ = 0;
  bool norReady_ = false;
};

} // namespace ota

#endif /* OTA_HPP */

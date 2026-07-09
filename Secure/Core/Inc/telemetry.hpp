/**
  ******************************************************************************
  * @file    telemetry.hpp
  * @brief   Continuous board-status telemetry: collects sensors / audio /
  *          radio / memory info and streams framed packets over the ST-LINK
  *          VCP (full status, 5 Hz) and the BLE module (compact, 1 Hz).
  ******************************************************************************
  */
#ifndef TELEMETRY_HPP
#define TELEMETRY_HPP

#include <cstdint>

namespace telemetry
{

/* Full status payload (FRAME_CMD_STATUS, little endian, packed).
 * Python: struct.unpack("<BBIhHI3h3h3hIHBhh32hBB6I", payload), size 133 */
struct __attribute__((packed)) FullStatus
{
  uint8_t ver;
  uint8_t button;
  uint32_t uptime_ms;
  int16_t temp_x100;
  uint16_t hum_x100;
  uint32_t press_x100;
  int16_t acc_mg[3];
  int16_t gyro_dps10[3];
  int16_t mag_mgauss[3];
  uint32_t light_raw;
  uint16_t tof_mm;
  uint8_t tof_ok;
  int16_t audio_rms;
  int16_t audio_peak;
  int16_t wave[32];
  uint8_t ble_alive;
  uint8_t wifi_alive;
  uint32_t ram_used;
  uint32_t ram_total;
  uint32_t heap_used;
  uint32_t heap_free;
  uint32_t flash_used;
  uint32_t flash_total;
};
static_assert(sizeof(FullStatus) == 133, "FullStatus layout must match PC parser");

/* Compact status payload (FRAME_CMD_STATUS_MINI, 12 bytes -> 20-byte frame,
 * fits a default-MTU BLE notification). Python: "<BhHHHHB" */
struct __attribute__((packed)) MiniStatus
{
  uint8_t button;
  int16_t temp_x100;
  uint16_t hum_x100;
  uint16_t press_x10;
  uint16_t light_raw16;
  uint16_t tof_mm;
  uint8_t audio_level;
};
static_assert(sizeof(MiniStatus) == 12, "MiniStatus layout must match PC parser");

class Service
{
public:
  /* Initializes every data source; safe to call again (e.g. after the test
   * suite has re-configured peripherals). */
  void init();

  /* One scheduler pass; call from the main loop as fast as possible. */
  void poll();

  /* Live PCM streaming over the VCP (16 kHz mono int16, CMD 0x03 frames) */
  void setAudioStream(bool enable);
  bool audioStreamEnabled() const { return audioStream_; }

private:
  void initSensors();
  void initAudio();
  void initRadio();
  void collect(FullStatus &st);
  void sendUart(const FullStatus &st);
  void sendBle(const FullStatus &st);
  void sendTcp(const FullStatus &st);
  void pollTcp();

  FullStatus status_ = {};
  uint32_t nextFullTick_ = 0;
  uint32_t nextTofTick_ = 0;
  uint32_t nextBleTick_ = 0;
  uint8_t uartSeq_ = 0;
  uint8_t bleSeq_ = 0;
  uint8_t audioSeq_ = 0;
  uint8_t tcpSeq_ = 0;
  bool sensorsOk_ = false;
  bool audioOk_ = false;
  bool tofOk_ = false;
  bool audioStream_ = false;
};

} // namespace telemetry

#endif /* TELEMETRY_HPP */

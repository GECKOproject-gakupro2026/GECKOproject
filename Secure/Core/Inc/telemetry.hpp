/**
  ******************************************************************************
  * @file    telemetry.hpp
  * @brief   Continuous board-status telemetry: collects sensors / audio /
  *          radio / memory / MCU info and streams framed packets over the
  *          VCP + Wi-Fi TCP (full status, 50 Hz) and BLE (compact, all
  *          sensors, 2 Hz).
  ******************************************************************************
  */
#ifndef TELEMETRY_HPP
#define TELEMETRY_HPP

#include <cstdint>

namespace telemetry
{

/* Full status payload v2 (FRAME_CMD_STATUS, little endian, packed).
 * Python: struct.unpack("<BBIhHI3h3h3hIHBhh32hBB6I" + "hHIIBBH3II", payload)
 * size 133 (v1 part) + 32 (MCU info) = 165 */
struct __attribute__((packed)) FullStatus
{
  uint8_t ver; /* = 2 */
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
  /* --- v2: MCU details --- */
  int16_t die_temp_x100;  /* internal temperature sensor via ADC1 */
  uint16_t vdda_mv;       /* analog supply derived from VREFINT   */
  uint32_t sysclk_hz;
  uint32_t hclk_hz;
  uint8_t reset_cause;    /* RCC_CSR[31:24]: LPWR|WWDG|IWDG|SFT|BOR|PIN|OBL|- */
  uint8_t cpu_load_pct;   /* main-loop headroom estimate                     */
  uint16_t flash_kb;      /* factory flash size register                     */
  uint32_t uid[3];        /* 96-bit unique device ID                         */
  uint32_t idcode;        /* DBGMCU IDCODE (device + revision)               */
};
static_assert(sizeof(FullStatus) == 165, "FullStatus layout must match PC parser");

/* Compact status payload v2 (FRAME_CMD_STATUS_MINI): every sensor in one
 * BLE notification (39 B payload -> 47 B frame, fits the AT server's 64 B
 * limit). Python: "<BhHHHH3h3h3hhhHhBB" */
struct __attribute__((packed)) MiniStatus
{
  uint8_t button;
  int16_t temp_x100;
  uint16_t hum_x100;
  uint16_t press_x10;
  uint16_t light_raw16;
  uint16_t tof_mm;
  int16_t acc_mg[3];
  int16_t gyro_dps10[3];
  int16_t mag_mgauss[3];
  int16_t audio_rms;
  int16_t audio_peak;
  uint16_t uptime_s;
  int16_t die_temp_x100;
  uint8_t flags;      /* bit0 ble, bit1 wifi, bit2 tof_ok */
  uint8_t cpu_load_pct;
};
static_assert(sizeof(MiniStatus) == 39, "MiniStatus layout must match PC parser");

class Service
{
public:
  /* Initializes every data source; safe to call again (e.g. after the test
   * suite has re-configured peripherals). */
  void init();

  /* One scheduler pass; call from the main loop as fast as possible. */
  void poll();

  /* Live PCM streaming over VCP + TCP (16 kHz mono int16, CMD 0x03 frames) */
  void setAudioStream(bool enable);
  bool audioStreamEnabled() const { return audioStream_; }

  /* Feeds one inbound byte (UART console or TCP). Frame bytes (OTA & co.)
   * are consumed internally and -1 is returned; otherwise the byte comes
   * back for the legacy single-character command handling. */
  int processRxByte(uint8_t byte, bool fromTcp);

  /* NonSecure-driven mode (the NS app is the main loop and pumps poll() via
   * the Comm_Poll NSC gateway): poll() pumps the console itself and plain
   * command bytes are queued for the NS app instead of App_Main. */
  void setNsDriven(bool on);

  /* Accepts a telemetry snapshot produced by the NonSecure app and sends it
   * over UART + TCP now (BLE keeps its own pace off the stored status).
   * The first call switches off the Secure-side status generation. */
  void submitExternalStatus(const FullStatus &st);

  /* bit1 = Wi-Fi joined, bit3 = TCP client connected (used by
   * Comm_GetLinkStatus; BLE bits are read directly from file-scope state). */
  uint32_t wifiTcpLinkBits() const;

  /* Copies the MCU-info fields (die temp, VDDA, clocks, flash size, UID,
   * reset cause, CPU load, RAM/flash usage) and ble_alive/wifi_alive out of
   * the Secure-side status_ snapshot, which poll() keeps refreshed independently
   * of NS-driven telemetry (see the nsTelemetryActive comment in poll()) -
   * unlike the rest of FullStatus, these fields have no NonSecure-side
   * source, so the NS app layer has to pull them via this getter instead of
   * filling them itself. Only the fields below are touched; caller supplies
   * the rest of *dst. */
  void copyMcuStatusInto(FullStatus &dst) const;

private:
  void handleFrame(uint8_t cmd, uint8_t seq, const uint8_t *payload,
                   uint16_t len, bool fromTcp);
  void sendResponse(bool fromTcp, uint8_t cmd, const uint8_t *payload,
                    uint16_t len);
  void initSensors();
  void initRadio();
  void collect(FullStatus &st);
  void refreshSlowSensors(FullStatus &st);
  void refreshMcuInfo(FullStatus &st);
  void sendUart(const FullStatus &st);
  void sendTcp(const FullStatus &st);
  void pollTcp();

  FullStatus status_ = {};
  uint32_t nextFullTick_ = 0;
  uint32_t nextEnvTick_ = 0;
  uint32_t nextLightTick_ = 0;
  uint32_t nextTofTick_ = 0;
  uint32_t nextMcuTick_ = 0;
  uint32_t nextBleTick_ = 0;
  uint32_t loopCount_ = 0;
  uint32_t loopWindowStart_ = 0;
  uint32_t loopMax_ = 0;
  uint8_t uartSeq_ = 0;
  uint8_t audioSeq_ = 0;
  uint8_t tcpSeq_ = 0;
  bool sensorsOk_ = false;
  bool audioOk_ = false;
  bool tofOk_ = false;
  bool audioStream_ = false;
};

/* Brings up the comm service (and, until the Phase C/D moves land, the
 * sensors/audio it still owns) and switches it into NonSecure-driven mode:
 * Comm_Poll() pumps it instead of App_Main()'s loop, and plain host command
 * bytes queue for Comm_PollHostCommand() instead of the legacy dispatch
 * table. Called once from Secure main() before the Stage-0 NonSecure jump.
 * Returns the Service instance so App_Main() can reuse it (and flip
 * nsDriven back off) if Stage-0 stays resident as the interactive loader. */
Service &CommInit();

} // namespace telemetry

#endif /* TELEMETRY_HPP */

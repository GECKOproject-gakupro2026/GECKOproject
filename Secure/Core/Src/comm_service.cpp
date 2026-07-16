/**
  ******************************************************************************
  * @file    comm_service.cpp
  * @brief   通信サービス本体【コア層・基板非依存】。
  *
  *          テレメトリの収集・組み立て、フレームプロトコルの処理(OTA含む)、
  *          NonSecureからのNSCゲートウェイの受け口(CommBridge_*)を担当する。
  *
  *          ハードウェアには直接触らない。すべてport層のAPI経由:
  *            comm_wifi.hpp     ... Wi-Fi/TCP
  *            comm_ble.hpp      ... BLE
  *            comm_uart.hpp     ... UART送信
  *            audio_capture.hpp ... マイク
  *            mcu_info.hpp      ... 内蔵ADC/メモリ統計
  *            console.h         ... UART受信
  *          だから基板を変えてもこのファイルは(ほぼ)無改造で移植できる。
  ******************************************************************************
  */
#include "telemetry.hpp"

#include "app_config.h"
#include "comm_backend.h"
#include "comm_dto.h"
#include "console.h"
#include "frame_codec.h"
#include "main.h"
#include "ota.hpp"

#include "b_u585i_iot02a.h"
#include "b_u585i_iot02a_env_sensors.h"
#include "b_u585i_iot02a_light_sensor.h"
#include "b_u585i_iot02a_motion_sensors.h"
#include "b_u585i_iot02a_ranging_sensor.h"

#include "audio_capture.hpp"
#include "comm_ble.hpp"
#include "state_log.hpp"
#include "comm_uart.hpp"
#include "comm_wifi.hpp"
#include "mcu_info.hpp"
#include "recorder.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

extern UART_HandleTypeDef huart1; /* VCP console / telemetry stream */
extern "C" void Secure_JumpToNonSecure(void);
extern "C" void BootGuard_ConfirmBoot(void); /* boot_guard.cpp, OTA Phase 2 */

/* Shared BSP audio DMA event flags (defined in audio_capture.cpp) */
extern "C" volatile uint32_t g_AudioEvents;
extern "C" volatile uint32_t g_AudioErrors;

namespace telemetry
{

namespace
{
constexpr uint32_t kFullPeriodMs = CFG_TLM_FULL_PERIOD_MS;
constexpr uint32_t kEnvPeriodMs = CFG_TLM_ENV_PERIOD_MS;
constexpr uint32_t kLightPeriodMs = CFG_TLM_LIGHT_PERIOD_MS;
constexpr uint32_t kTofPeriodMs = CFG_TLM_TOF_PERIOD_MS;
constexpr uint32_t kMcuPeriodMs = CFG_TLM_MCU_PERIOD_MS;
constexpr uint32_t kBlePeriodMs = CFG_TLM_BLE_PERIOD_MS;

uint8_t audioFrame[1024 + FRAME_OVERHEAD]; /* PCM streaming TX buffer */

/* OTA staging + per-link inbound frame decoders */
ota::Manager otaMgr;
Frame_Decoder uartDecoder = {};
Frame_Decoder tcpDecoder = {};
uint8_t respSeq = 0;

/* --- NonSecure-driven mode state (Comm_* NSC gateways, see secure_nsc.c).
 * The NS app is the main loop: it pumps poll() through Comm_Poll() and
 * drains plain host-command bytes from this ring via Comm_PollHostCommand().
 * Single-threaded (NS superloop), so no locking needed; nsActivity is also
 * set from the BLE UART IRQ, hence volatile. --- */
Service *g_service = nullptr;
bool nsDriven = false;          /* NS app pumps the loop                    */
bool nsTelemetryActive = false; /* NS has taken over status production      */
bool telemetryEnabled = true;   /* Comm_SetTelemetryEnabled (low-power)     */
volatile bool nsActivity = false; /* any inbound host traffic since poll    */
/* NonSecure device state machine's current state (AppState_t: 0=IDLE,
 * 1=ACTIVE_ACQUIRE, 2=ACTIVE_COMM), forwarded via Comm_SetDeviceState(). Only
 * meaningful once NS has called it at least once; defaults to "ACTIVE" (2) so
 * MiniStatus.flags reads something sane before the first update. */
uint32_t deviceState = 2U;
uint8_t nsCmdRing[16];
uint8_t nsCmdHead = 0;
uint8_t nsCmdTail = 0;

void nsCmdPush(uint8_t byte)
{
  uint8_t next = static_cast<uint8_t>((nsCmdHead + 1U) % sizeof(nsCmdRing));
  if (next != nsCmdTail) /* on overflow: drop newest, keep the queue sane */
  {
    nsCmdRing[nsCmdHead] = byte;
    nsCmdHead = next;
  }
}

} // namespace

void Service::initSensors()
{
  /* TrustZone Phase C: ToF/env/motion/light sensors and their I2C1/I2C2
   * buses moved to NonSecure (GTZC flip in Secure main.c). Secure can no
   * longer touch these peripherals at all, so this is now a no-op except
   * for the ok-flags, which stay false since Secure has nothing to report
   * for them (NonSecure fills these FullStatus fields directly). USER
   * button (PC13) init moved to NonSecure earlier, in Phase B. */
  sensorsOk_ = false;
  tofOk_ = false;
}

void Service::initRadio()
{
  status_.wifi_alive = comm_wifi::Init() ? 1U : 0U;
  status_.ble_alive = comm_ble::Init() ? 1U : 0U;

  printf("[TLM] radio: BLE=%s WiFi=%s\r\n",
         status_.ble_alive != 0U ? "OK" : "NG",
         status_.wifi_alive != 0U ? "OK" : "NG");
}

void Service::init()
{
  g_service = this; /* Comm_* NSC gateways dispatch through this instance */

  /* Interrupt-driven TX on the VCP link */
  HAL_NVIC_SetPriority(USART1_IRQn, 12, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);

  printf("[TLM] initializing telemetry sources...\r\n");
  initSensors();
  audioOk_ = audio_capture::Init();
  mcu_info::Init(status_);
  initRadio();
  uint32_t now = HAL_GetTick();
  nextFullTick_ = now;
  nextEnvTick_ = now;
  nextLightTick_ = now;
  nextTofTick_ = now;
  nextMcuTick_ = now;
  nextBleTick_ = now + 500U;
  loopWindowStart_ = now;
  loopCount_ = 0;
  printf("[TLM] streaming: UART/TCP %luHz (165B frames v2), BLE %luHz (all sensors)\r\n",
         1000UL / kFullPeriodMs, 1000UL / kBlePeriodMs);
}

/* Die temperature / VDDA / memory statistics / CPU load (every 500 ms) */
void Service::refreshMcuInfo(FullStatus &st)
{
  mcu_info::Refresh(st);

  /* CPU load: main-loop iterations in this window vs a slow-tracking peak
   * rate. loopMax_ used to be strictly monotonic (never decayed once set),
   * which meant a single blocking call early on (e.g. a slow Wi-Fi
   * accept/recv, or the peak still being low right after boot) could pin a
   * low ceiling that made pct spike towards 100 on perfectly normal loops
   * ever after - reproduced as a spurious 100% reading with no matching
   * slowdown. loopMax_ now still jumps up immediately on a new peak, but
   * decays 1/16th of the way towards the current rate every window when
   * rate is lower, so a transient stall or a low post-boot ceiling gets
   * corrected within a couple of seconds instead of persisting for the
   * life of the process. */
  uint32_t now = HAL_GetTick();
  uint32_t win = now - loopWindowStart_;
  if (win >= 500U)
  {
    uint32_t rate = (loopCount_ * 1000U) / win;
    if (rate > loopMax_)
    {
      loopMax_ = rate;
    }
    else
    {
      loopMax_ -= (loopMax_ - rate) / 16U;
    }
    st.cpu_load_pct = (loopMax_ > 0U)
        ? static_cast<uint8_t>(100U - (100U * rate) / loopMax_) : 0U;
    loopCount_ = 0;
    loopWindowStart_ = now;
  }
}

/* Slow sensors on their own schedules (ODR / integration-time limited).
 * TrustZone Phase C: env/light/ToF sensors moved to NonSecure along with
 * I2C1/I2C2 - Secure can no longer reach them, so only the MCU info refresh
 * (ADC-based, no I2C) remains here. */
void Service::refreshSlowSensors(FullStatus &st)
{
  uint32_t now = HAL_GetTick();

  if (static_cast<int32_t>(now - nextMcuTick_) >= 0)
  {
    nextMcuTick_ += kMcuPeriodMs;
    refreshMcuInfo(st);
  }
}

void Service::collect(FullStatus &st)
{
  st.ver = 2;
  st.uptime_ms = HAL_GetTick();
  /* TrustZone Phase B: the USER button (PC13) moved to NonSecure - it is no
   * longer readable from here (the pin is NSEC) and the NonSecure app fills
   * st.button itself before calling Comm_SendTelemetry(). This Secure-side
   * collect() path only still runs when Stage-0 stays resident as the
   * interactive OTA loader (button held / bad image), where BSP_PB_GetState
   * would just read back the same NSEC-released pin - leave it 0 there. */

  refreshSlowSensors(st);
  /* TrustZone Phase C: accelero/gyro/mag (ISM330DHCX/IIS2MDC, I2C1) moved
   * to NonSecure with the rest of the I2C sensors - Secure can no longer
   * read them, NonSecure overwrites these fields. Phase D: audio capture
   * stays Secure, so audio_rms/peak/wave are computed here (but NonSecure's
   * submitExternalStatus overwrites the whole struct - so these values only
   * apply on the Secure-resident OTA-loader path; NonSecure fills its own
   * audio fields via the Comm_GetAudioBuffer gateway). */
  if (audioOk_)
  {
    const int16_t *audioBuf = audio_capture::Buffer();
    const size_t kAudioSamples = audio_capture::Samples();
    int64_t sqSum = 0;
    int32_t peak = 0;
    int32_t sum = 0;
    for (size_t i = 0; i < kAudioSamples; i++)
    {
      sum += audioBuf[i];
    }
    int16_t mean = static_cast<int16_t>(sum / static_cast<int32_t>(kAudioSamples));
    for (size_t i = 0; i < kAudioSamples; i++)
    {
      int32_t v = audioBuf[i] - mean;
      sqSum += static_cast<int64_t>(v) * v;
      if (v > peak) peak = v;
      if (-v > peak) peak = -v;
    }
    st.audio_rms = static_cast<int16_t>(
        sqrtf(static_cast<float>(sqSum) / static_cast<float>(kAudioSamples)));
    st.audio_peak = static_cast<int16_t>(peak);
    const size_t stride = kAudioSamples / 32U;
    for (size_t i = 0; i < 32U; i++)
    {
      st.wave[i] = static_cast<int16_t>(audioBuf[i * stride] - mean);
    }
  }

  st.ble_alive = comm_ble::IsAlive() ? 1U : 0U;
}

void Service::sendUart(const FullStatus &st)
{
  uint8_t frame[sizeof(FullStatus) + FRAME_OVERHEAD];
  size_t len = Frame_Encode(FRAME_CMD_STATUS, uartSeq_++,
                            reinterpret_cast<const uint8_t *>(&st), sizeof(st),
                            frame, sizeof(frame));
  if (len > 0U)
  {
    /* drop the frame when the previous one is still in flight */
    (void)comm_uart::SendAsync(frame, len, 0);
  }
}

void Service::sendTcp(const FullStatus &st)
{
  uint8_t frame[sizeof(FullStatus) + FRAME_OVERHEAD];
  size_t len = Frame_Encode(FRAME_CMD_STATUS, tcpSeq_++,
                            reinterpret_cast<const uint8_t *>(&st), sizeof(st),
                            frame, sizeof(frame));
  if (len > 0U)
  {
    comm_wifi::SendFrame(frame, len);
  }
}

void Service::pollTcp()
{
  uint8_t buf[64];
  int32_t n = comm_wifi::PollRecv(buf, sizeof(buf));
  if (n > 0)
  {
    for (int32_t i = 0; i < n; i++)
    {
      int plain = processRxByte(buf[i], true);
      if (plain < 0)
      {
        /* consumed by the frame layer (OTA & co.), or - when nsDriven - by
         * processRxByte()'s own 'a'/'s' handling + nsCmdPush(). This loop's
         * 'p'/'l'/'a'/'s' below only fires in the legacy (non-NS-driven)
         * App_Main path where processRxByte() returns the byte instead. */
        continue;
      }
      char c = static_cast<char>(plain);
      if (c == 'p' || c == 'P')
      {
        char msg[48];
        int m = snprintf(msg, sizeof(msg), "[TCP] PONG uptime=%lu\r\n", HAL_GetTick());
        comm_wifi::SendRaw(reinterpret_cast<uint8_t *>(msg), m);
      }
      else if (c == 'l' || c == 'L')
      {
        BSP_LED_Toggle(LED_GREEN);
        const char *msg = "[TCP] LED toggled\r\n";
        comm_wifi::SendRaw(reinterpret_cast<const uint8_t *>(msg),
                           static_cast<int32_t>(strlen(msg)));
      }
      else if (c == 'a' || c == 'A')
      {
        setAudioStream(true);
      }
      else if (c == 's' || c == 'S')
      {
        setAudioStream(false);
      }
    }
  }
  /* Note: recv error codes are unreliable for disconnect detection (the
   * module returns generic errors on a mere receive timeout) - the send
   * path in sendTcp() is the disconnect authority. */
}

void Service::setAudioStream(bool enable)
{
  audioStream_ = enable && audioOk_;
  g_AudioEvents = 0;
  printf("[TLM] audio streaming %s\r\n", audioStream_ ? "ON (16kHz mono)" : "OFF");
}

void Service::sendResponse(bool fromTcp, uint8_t cmd, const uint8_t *payload,
                           uint16_t len)
{
  uint8_t frame[64];
  size_t n = Frame_Encode(cmd, respSeq++, payload, len, frame, sizeof(frame));
  if (n == 0U)
  {
    return;
  }
  if (fromTcp)
  {
    comm_wifi::SendRaw(frame, static_cast<int32_t>(n));
  }
  else
  {
    (void)comm_uart::SendAsync(frame, n, 20);
  }
}

void Service::handleFrame(uint8_t cmd, uint8_t seq, const uint8_t *payload,
                          uint16_t len, bool fromTcp)
{
  struct __attribute__((packed)) AckPayload
  {
    uint8_t orig_cmd;
    uint8_t orig_seq;
    uint32_t arg;
  };
  struct __attribute__((packed)) NackPayload
  {
    uint8_t orig_cmd;
    uint8_t orig_seq;
    uint8_t error;
  };

  switch (cmd)
  {
    case FRAME_CMD_FW_CHUNK:
    case FRAME_CMD_FW_COMPLETE:
    {
      uint8_t err = (cmd == FRAME_CMD_FW_CHUNK)
                        ? otaMgr.writeChunk(payload, len)
                        : otaMgr.complete(payload, len);
      if (err == 0U)
      {
        ota::StatusReport rep;
        otaMgr.fillReport(rep);
        AckPayload ack = {cmd, seq, rep.received};
        sendResponse(fromTcp, FRAME_CMD_ACK,
                     reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
      }
      else
      {
        NackPayload nack = {cmd, seq, err};
        sendResponse(fromTcp, FRAME_CMD_NACK,
                     reinterpret_cast<const uint8_t *>(&nack), sizeof(nack));
      }
      break;
    }
    case FRAME_CMD_STATUS_REQ:
    {
      ota::StatusReport rep;
      otaMgr.fillReport(rep);
      sendResponse(fromTcp, FRAME_CMD_STATUS_RESP,
                   reinterpret_cast<const uint8_t *>(&rep), sizeof(rep));
      break;
    }
    case FRAME_CMD_FW_APPLY:
    {
      uint8_t err = otaMgr.applyToNonSecure();
      if (err != 0U)
      {
        NackPayload nack = {cmd, seq, err};
        sendResponse(fromTcp, FRAME_CMD_NACK,
                     reinterpret_cast<const uint8_t *>(&nack), sizeof(nack));
        break;
      }
      AckPayload ack = {cmd, seq, 0U};
      sendResponse(fromTcp, FRAME_CMD_ACK,
                   reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
      HAL_Delay(300U); /* let the ACK reach the host before the link drops */
      /* Freshly-applied image: don't inherit the previous image's failed
       * boot count (OTA Phase 2 rollback guard, see boot_guard.hpp). The
       * reboot below re-enters Stage-0, which starts a fresh attempt count
       * for the new image. */
      BootGuard_ConfirmBoot();
      printf("[OTA] rebooting into the new NonSecure application...\r\n");
      HAL_Delay(50U);
      /* Reboot rather than jumping straight into the new image.
       *
       * Jumping (Secure_JumpToNonSecure) works from a clean loader state, but
       * not when the NonSecure app was already running when the update landed:
       * SCB_NS->VTOR, MSP_NS, the peripherals it had brought up and ICACHE all
       * still describe the *previous* image, and a second hot apply HardFaults
       * right after the jump. A system reset re-runs Stage-0 from scratch - it
       * re-validates Bank2, re-initialises everything and enters the new image
       * exactly as a cold boot would, from any prior state. */
      NVIC_SystemReset(); /* does not return */
      break;
    }
    case FRAME_CMD_LINK_STANDBY:
    {
      /* Host explicitly ends/idles the link it sent this on. Put that link's
       * FSM into Idle now; it re-activates on the next inbound traffic (the
       * per-link update at the top of poll()). BLE writes don't reach here
       * (they take comm_ble's own decoder path), so the origins that matter
       * are TCP (fromTcp) and UART. */
      if (fromTcp)
      {
        tcpLink_.state = LinkState::Idle;
      }
      else
      {
        uartLink_.state = LinkState::Idle;
      }
      state_log::Push(state_log::Event::LinkStandby, fromTcp ? 1U : 0U);
      AckPayload ack = {cmd, seq, 0U};
      sendResponse(fromTcp, FRAME_CMD_ACK,
                   reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
      break;
    }
    case FRAME_CMD_TIME_SYNC:
    {
      /* PC sends the current Unix epoch (u32 seconds, LE). There is no RTC on
       * the U585, so store an offset relative to the millisecond uptime:
       *   epochOffsetMs = pc_epoch*1000 - HAL_GetTick()
       *   wall_ms       = epochOffsetMs + HAL_GetTick()   (used by state_log)
       * The offset lives in the SRAM log module and is lost on reset (re-sync
       * needed). */
      if (len >= 4U)
      {
        uint32_t epochSec = static_cast<uint32_t>(payload[0]) |
                            (static_cast<uint32_t>(payload[1]) << 8) |
                            (static_cast<uint32_t>(payload[2]) << 16) |
                            (static_cast<uint32_t>(payload[3]) << 24);
        uint32_t offsetMs = epochSec * 1000U - HAL_GetTick();
        state_log::SetEpochOffset(offsetMs);
        state_log::Push(state_log::Event::None, epochSec); /* mark the sync */
        AckPayload ack = {cmd, seq, epochSec};
        sendResponse(fromTcp, FRAME_CMD_ACK,
                     reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
      }
      else
      {
        NackPayload nack = {cmd, seq, FRAME_ERR_BAD_STATE};
        sendResponse(fromTcp, FRAME_CMD_NACK,
                     reinterpret_cast<const uint8_t *>(&nack), sizeof(nack));
      }
      break;
    }
    default:
      break; /* unknown inbound command: ignore */
  }
}

int Service::processRxByte(uint8_t byte, bool fromTcp)
{
  Frame_Decoder &dec = fromTcp ? tcpDecoder : uartDecoder;
  uint8_t cmd = 0;
  uint8_t seq = 0;
  const uint8_t *payload = nullptr;
  uint16_t len = 0;
  Frame_FeedResult fed = Frame_DecoderFeed(&dec, byte, &cmd, &seq, &payload, &len);
  if (nsDriven)
  {
    /* Any inbound byte counts as host activity for the NS idle timer,
     * regardless of whether it turns into a frame or a plain command. */
    nsActivity = true;
  }
  switch (fed)
  {
    case FRAME_FEED_COMPLETE:
      handleFrame(cmd, seq, payload, len, fromTcp);
      return -1;
    case FRAME_FEED_PLAIN:
      if (nsDriven)
      {
        /* Audio streaming on/off must take effect here regardless of link
         * (UART console or TCP): this used to be handled only inside
         * pollTcp()'s own byte loop, which never runs for these bytes once
         * nsDriven routes everything through processRxByte() - UART 'a'/'s'
         * was a silent no-op and TCP's copy was dead code (pollTcp() feeds
         * bytes through this same function, so its post-processRxByte
         * 'a'/'s' check was never reached either). */
        if (byte == 'a' || byte == 'A')
        {
          setAudioStream(true);
        }
        else if (byte == 's' || byte == 'S')
        {
          setAudioStream(false);
        }
        /* Still queue for Comm_PollHostCommand() so the NS app sees every
         * byte as host activity (IDLE-timer wake) and can handle any other
         * command itself. */
        nsCmdPush(byte);
        return -1;
      }
      return byte;
    default:
      return -1;
  }
}

void Service::setNsDriven(bool on)
{
  nsDriven = on;
}

void Service::submitExternalStatus(const FullStatus &st)
{
  nsTelemetryActive = true;
  status_ = st;
  if (!telemetryEnabled)
  {
    return;
  }
  sendUart(status_);
  sendTcp(status_);
}

Service &CommInit()
{
  /* Speed up the VCP link (USB-bridged by the ST-LINK) beyond the 115200
   * CubeMX default - must happen before anything prints or a host tool
   * expecting CFG_CONSOLE_BAUDRATE will see nothing. */
  huart1.Init.BaudRate = CFG_CONSOLE_BAUDRATE;
  (void)HAL_UART_Init(&huart1);

  static Service service;
  service.init();
  service.setNsDriven(true);
  return service;
}

uint32_t Service::wifiTcpLinkBits() const
{
  uint32_t bits = 0;
  if (status_.wifi_alive != 0U)
  {
    bits |= (1U << 1);
  }
  if (comm_wifi::HasClient())
  {
    bits |= (1U << 3);
  }
  return bits;
}

void Service::copyMcuStatusInto(FullStatus &dst) const
{
  dst.die_temp_x100 = status_.die_temp_x100;
  dst.vdda_mv = status_.vdda_mv;
  dst.sysclk_hz = status_.sysclk_hz;
  dst.hclk_hz = status_.hclk_hz;
  dst.reset_cause = status_.reset_cause;
  dst.cpu_load_pct = status_.cpu_load_pct;
  dst.flash_kb = status_.flash_kb;
  dst.uid[0] = status_.uid[0];
  dst.uid[1] = status_.uid[1];
  dst.uid[2] = status_.uid[2];
  dst.idcode = status_.idcode;
  dst.ram_used = status_.ram_used;
  dst.ram_total = status_.ram_total;
  dst.heap_used = status_.heap_used;
  dst.heap_free = status_.heap_free;
  dst.flash_used = status_.flash_used;
  dst.flash_total = status_.flash_total;
  dst.ble_alive = status_.ble_alive;
  dst.wifi_alive = status_.wifi_alive;
}

void Service::poll()
{
  /* One-shot phase profile: accumulated per second, printed once (temporary
   * diagnostic for the frame-rate ceiling) */
  static uint32_t profCollect = 0, profUart = 0, profBle = 0, profTcp = 0;
  static uint32_t profStart = 0, profPrints = 0;
  static uint32_t profFrames = 0, profLoops = 0;

  if (nsDriven)
  {
    /* In NonSecure-driven mode nobody else pumps the console RX path
     * (App_Main()'s loop, which used to do this, doesn't run) - poll() is
     * the only place left, since Comm_Poll() is the NS app's single pump
     * point for the whole comm service.
     *
     * Drain the WHOLE RX ring here, not just one byte: Console_GetChar(0)
     * used to be called once per poll() and returned at most one byte per
     * call, capping inbound UART throughput at one byte per superloop
     * iteration. Combined with the RX DMA ring's lack of overrun detection
     * (fixed in console.cpp - a lapped ring used to silently alias to
     * "empty" forever), any stretch where poll() ran slower than the host
     * was sending could starve RX permanently. A bounded block read drains
     * everything currently pending in one shot, so a slow loop iteration
     * costs latency, never permanent RX loss. */
    uint8_t rxBlock[256];
    size_t n = Console_ReadBlock(rxBlock, sizeof(rxBlock));
    if (n > 0U)
    {
      for (size_t i = 0; i < n; i++)
      {
        (void)processRxByte(rxBlock[i], false);
      }
      if (uartLink_.state != LinkState::Active)
      {
        state_log::Push(state_log::Event::UartActive, 0U);
      }
      uartLink_.state = LinkState::Active;
      uartLink_.lastActivityMs = HAL_GetTick();
    }
  }

  loopCount_++;
  uint32_t now = HAL_GetTick();

  /* Per-link state update. Each link is Active while a peer is present and
   * traffic is recent; it ages back to Idle after kLinkIdleMs of silence.
   * These drive the TCP-accept throttle below (and link exclusivity). UART is
   * DMA-driven and always allowed to interrupt, so it is never gated on the
   * other links - it only reports its own state.
   *
   * kLinkIdleMs was originally 3000 - identical to the NonSecure device
   * machine's CFG_IDLE_TIMEOUT_MS. That resonance was a real bug: the instant
   * the NS device entered IDLE, uartLink_ aged out in the same tick, dropping
   * otherLinkActive below and springing the TCP accept interval back from
   * 60 s to the brisk 5 s cadence - at exactly the moment the shared poll()
   * loop could least afford a ~300 ms accept stall. Set well above the device
   * idle timeout so the link-level bookkeeping never flips in lockstep with
   * the device-level one. */
  constexpr uint32_t kLinkIdleMs = 10000U;
  LinkState blePrev = bleLink_.state;
  bleLink_.state = comm_ble::IsConnected() ? LinkState::Active : LinkState::Idle;
  if (bleLink_.state == LinkState::Active)
  {
    bleLink_.lastActivityMs = now;
  }
  if (bleLink_.state != blePrev)
  {
    state_log::Push(bleLink_.state == LinkState::Active ? state_log::Event::BleActive
                                                        : state_log::Event::BleIdle,
                    0U);
  }
  LinkState tcpPrev = tcpLink_.state;
  tcpLink_.state = comm_wifi::HasClient() ? LinkState::Active : LinkState::Idle;
  if (tcpLink_.state == LinkState::Active)
  {
    tcpLink_.lastActivityMs = now;
  }
  if (tcpLink_.state != tcpPrev)
  {
    state_log::Push(tcpLink_.state == LinkState::Active ? state_log::Event::TcpActive
                                                        : state_log::Event::TcpIdle,
                    0U);
  }
  if (uartLink_.state == LinkState::Active &&
      static_cast<int32_t>(now - uartLink_.lastActivityMs) >= (int32_t)kLinkIdleMs)
  {
    uartLink_.state = LinkState::Idle;
    state_log::Push(state_log::Event::UartIdle, 0U);
  }
  /* Once the NonSecure app has submitted at least one status via
   * Comm_SendTelemetry(), Secure stops producing/pushing its own status on
   * the periodic tick - submitExternalStatus() already sent it. Sensor
   * collection itself moves to NonSecure in a later phase; for now this
   * just avoids sending two competing status streams. */
  if (!nsTelemetryActive && static_cast<int32_t>(now - nextFullTick_) >= 0)
  {
    nextFullTick_ += kFullPeriodMs;
    /* After a long stall (e.g. the 10 s blocking TCP accept) skip the
     * missed periods instead of bursting the backlog */
    if (static_cast<int32_t>(now - nextFullTick_) > 1000)
    {
      nextFullTick_ = now + kFullPeriodMs;
    }
    uint32_t t0 = HAL_GetTick();
    collect(status_);
    uint32_t t1 = HAL_GetTick();
    if (telemetryEnabled)
    {
      sendUart(status_);
    }
    uint32_t t2 = HAL_GetTick();
    if (telemetryEnabled)
    {
      sendTcp(status_);
    }
    profCollect += t1 - t0;
    profUart += t2 - t1;
    profTcp += HAL_GetTick() - t2;
    profFrames++;
  }
  else if (nsTelemetryActive)
  {
    /* NS has taken over the FullStatus_t stream, so collect() (and its
     * UART/TCP push) above is skipped - but status_'s MCU-info fields and
     * ble_alive have no NonSecure-side source (see copyMcuStatusInto()'s
     * doc comment), so they still need refreshing here on their own
     * schedule for Comm_GetMcuInfo() callers to read anything but zeros.
     * wifi_alive isn't touched here: it only ever reflects whether
     * comm_wifi::Init() succeeded at boot (initRadio()), not a live link
     * state, so it never needs refreshing after init. */
    refreshSlowSensors(status_);
    status_.ble_alive = comm_ble::IsAlive() ? 1U : 0U;
  }
  profLoops++;

  /* BLE host activity: a GATT write on fe41 (e.g. the PC's 1 Hz keep-alive
   * byte) counts as host traffic for the NS idle timer, mirroring how any
   * inbound UART/TCP byte sets nsActivity in processRxByte(). Without this the
   * board slid back to IDLE ~3 s after the last frame even while a BLE central
   * was actively polling it. */
  if (nsDriven && comm_ble::TakeHostActivity())
  {
    nsActivity = true;
  }

  /* Recording control: BLE write (fe41) frames are parsed into bleRecCmd by
   * comm_ble.cpp's GATT write callback; this is the only place that consumes
   * it. Starting a recording forces audioStream_ off so the two don't fight
   * over the same resampled-audio pipeline (see poll()'s audio block). */
  uint8_t recCmd = comm_ble::TakeRecCmd();
  if (recCmd == 1U)
  {
    audioStream_ = false;
    recorder::Start();
  }
  else if (recCmd == 2U)
  {
    recorder::Stop();
    comm_ble::StartRecTx();
  }
  comm_ble::PumpRecTx();

  if (telemetryEnabled && static_cast<int32_t>(now - nextBleTick_) >= 0)
  {
    nextBleTick_ += kBlePeriodMs;
    /* Give a BLE audio transfer the notify link (and UART4) to itself: while
     * PumpRecTx() is draining a recording, skip SendStatus() entirely instead
     * of interleaving 10 Hz sensor notifies with REC_CHUNK notifies on the
     * same fe42 characteristic / blocking UART4 AT transmit. nextBleTick_
     * still advances on schedule so telemetry resumes at the normal cadence
     * (no backlog burst) the moment the recording finishes. */
    if (!comm_ble::IsRecTxActive())
    {
      uint32_t t0 = HAL_GetTick();
      comm_ble::SendStatus(status_);
      uint32_t dt = HAL_GetTick() - t0;
      profBle += dt;
      /* Record the BLE comm-cycle return time/duration. Step 7's NonSecure
       * cadence scheduler and log read-out use this to keep PC arrival steady. */
      if (bleLink_.state == LinkState::Active)
      {
        state_log::Push(state_log::Event::CommReturn, dt);
      }
    }
  }
  if (telemetryEnabled)
  {
    /* Skip the TCP service while the NS app is idle. pollTcp()'s 1 Hz
     * MX_WIFI_Socket_accept() blocks for hundreds of ms with no client, and
     * its "let a pending console key win" guard checks UART_FLAG_RXNE - which
     * never sets, because console RX is DMA-driven and the DMA clears it. In
     * IDLE the NS loop spins with no sensor work to slow it down, so poll()
     * runs into that blocking accept constantly and the console byte that is
     * supposed to wake the board never gets drained: the board could enter
     * IDLE but never leave it. Idle means nothing to serve over TCP anyway;
     * the console/BLE wake paths stay live. */

    /* Stretch the no-client TCP accept poll so its ~300 ms module-side block
     * cannot stall whichever link is actually carrying data. Originally this
     * only fired for an Active BLE/TCP link, which left a UART/VCP-only session
     * exposed: with no BLE central and no TCP client, the accept ran every 5 s
     * and its ~300 ms stall dropped the UART telemetry from 50 Hz toward
     * ~10 Hz. Now the accept is stretched to 60 s whenever *any* of these hold:
     *   - a BLE central or TCP client link is Active (original case), OR
     *   - a UART/console session is Active (the 50 Hz telemetry case), OR
     *   - Wi-Fi is not up (no IP): there is no point polling accept with no
     *     network, and PollRecv already early-returns when the listen socket
     *     is absent - this just avoids the interval bookkeeping.
     * Only when Wi-Fi is up AND no link is active do we keep the brisk 5 s
     * cadence, so a fresh TCP client still connects promptly. UART itself is
     * never gated by this - it is DMA-driven and always drained at the top of
     * poll(); this only decides how often the blocking accept is attempted. */
    const bool otherLinkActive = (bleLink_.state == LinkState::Active) ||
                                 (tcpLink_.state == LinkState::Active) ||
                                 (uartLink_.state == LinkState::Active) ||
                                 !comm_wifi::NetUp();
    comm_wifi::SetAcceptInterval(otherLinkActive ? 60000U : 5000U);
    uint32_t t0 = HAL_GetTick();
    pollTcp();
    profTcp += HAL_GetTick() - t0;
  }

  if (profStart == 0U)
  {
    profStart = now;
  }
  else if (now - profStart >= 2000U && profPrints < 3U)
  {
    profPrints++;
    printf("[PROF] per2s: collect=%lums uart=%lums ble=%lums tcp=%lums frames=%lu loops=%lu "
           "rx_overrun=%lu\r\n",
           profCollect, profUart, profBle, profTcp, profFrames, profLoops,
           (unsigned long)Console_GetRxOverrunCount());
    profCollect = profUart = profBle = profTcp = 0;
    profFrames = profLoops = 0;
    profStart = now;
  }

  /* Periodic state-log dump (every ~3 s, standing - not capped like PROF) so
   * the SRAM ring is observable over the console. Diagnostic only; the ring
   * itself records continuously regardless of this print. */
  static uint32_t nextSlogTick = 0;
  if (static_cast<int32_t>(now - nextSlogTick) >= 0)
  {
    nextSlogTick = now + 3000U;
    uint32_t sz = state_log::Size();
    uint32_t from = (sz > 6U) ? (sz - 6U) : 0U;
    printf("[SLOG] count=%lu size=%lu:", (unsigned long)state_log::Count(),
           (unsigned long)sz);
    for (uint32_t i = from; i < sz; i++)
    {
      state_log::Record rec;
      if (state_log::Get(i, rec))
      {
        printf(" [t=%lu ev=%u ret=%lu]", (unsigned long)rec.tick_ms,
               (unsigned)rec.event, (unsigned long)rec.ret_val);
      }
    }
    printf("\r\n");
  }

  /* PCM streaming: feed each ready buffer half into the pitch-correction
   * queue, then drain it in 512-sample frames. Feeding (rather than
   * resampling each 512-sample window in isolation) keeps the resampling
   * phase continuous across window boundaries - see
   * audio_capture::FeedForResample() for why that matters. The correction
   * ratio (audio_capture::kResampleStep) was calibrated empirically against
   * on-target FFT sweeps (pc_side/mic_freq_response/sweep.py), not derived
   * from the nominal/measured sample-rate ratio: the DMA half/full-complete
   * interrupt cadence runs measurably faster than that simple ratio would
   * predict, for reasons not fully root-caused (see 計画_...md 付録). */
  /* recorder::Active() shares this same resampled-audio pipeline (both pull
   * from the g_AudioEvents-driven DMA halves via FeedForResample/PopResampled),
   * so the two must stay mutually exclusive: whichever caller starts a
   * recording is responsible for turning audioStream_ off first (see
   * recorder.hpp), otherwise the 512-sample windows would be split between
   * the UART/TCP frame encoder and the recording ring instead of each
   * getting a full copy. */
  if (audioStream_ || recorder::Active())
  {
    uint32_t events = g_AudioEvents;
    if (events != 0U)
    {
      g_AudioEvents = 0;
      const int16_t *buf = audio_capture::Buffer();
      const int16_t *half =
          (events & 1U) != 0U ? &buf[0] : &buf[audio_capture::Samples() / 2];
      audio_capture::FeedForResample(half, 512U);
      audio_capture::FeedForResample(half + 512, 512U);
    }
    while (audio_capture::ResampledAvailable() >= 512U)
    {
      static int16_t corrected[512];
      audio_capture::PopResampled(corrected, 512U);
      if (audioStream_)
      {
        size_t len = Frame_Encode(
            FRAME_CMD_AUDIO, audioSeq_++,
            reinterpret_cast<const uint8_t *>(corrected), 1024U,
            audioFrame, sizeof(audioFrame));
        if (len > 0U)
        {
          /* audio must not drop: wait for the in-flight frame (<= 12 ms) */
          (void)comm_uart::SendAsync(audioFrame, len, 15);
          comm_wifi::SendRaw(audioFrame, static_cast<int32_t>(len));
        }
      }
      else
      {
        recorder::FeedPcm(corrected, 512U);
      }
    }
  }
}

} // namespace telemetry

extern "C" void Comm_Init(void)
{
  telemetry::CommInit();
}

/* ---- CommBridge_*: extern "C" bridges for the Comm_* CMSE gateways
 * (secure_nsc.c). Only ever receive Secure-local pointers (the gateways
 * already validated/copied any NonSecure-origin data). ---- */
extern "C" void CommBridge_Poll(void)
{
  if (telemetry::g_service != nullptr)
  {
    telemetry::g_service->poll();
  }
}

extern "C" int CommBridge_SendTelemetry(const FullStatus_t *st)
{
  if (telemetry::g_service == nullptr)
  {
    return -2;
  }
  static_assert(sizeof(FullStatus_t) == sizeof(telemetry::FullStatus),
               "FullStatus_t / telemetry::FullStatus must stay byte-identical");
  telemetry::FullStatus local;
  memcpy(&local, st, sizeof(local));
  telemetry::g_service->submitExternalStatus(local);
  return 0;
}

extern "C" int CommBridge_PollHostCommand(uint8_t *out)
{
  using namespace telemetry;
  bool activity = nsActivity;
  nsActivity = false;
  if (nsCmdTail != nsCmdHead)
  {
    *out = nsCmdRing[nsCmdTail];
    nsCmdTail = static_cast<uint8_t>((nsCmdTail + 1U) % sizeof(nsCmdRing));
    return COMM_POLL_BYTE;
  }
  return activity ? COMM_POLL_ACTIVITY : COMM_POLL_NONE;
}

extern "C" void CommBridge_SetTelemetryEnabled(uint32_t on)
{
  telemetry::telemetryEnabled = (on != 0U);
}

namespace telemetry
{
uint32_t GetDeviceState() { return deviceState; }
} // namespace telemetry

extern "C" void CommBridge_SetDeviceState(uint32_t state)
{
  using namespace telemetry;
  /* AppState_t: 0=STATE_IDLE, 1=STATE_ACTIVE_ACQUIRE, 2=STATE_ACTIVE_COMM
   * (NonSecure/Core/Inc/app_state.h - duplicated here as plain values since
   * this boundary only ever carries a uint32_t, not the enum type). Log only
   * the IDLE<->ACTIVE edge, not every ACQUIRE<->COMM sub-state flip within
   * ACTIVE (that would flood the 64-entry SRAM ring for no diagnostic gain). */
  bool wasIdle = (deviceState == 0U);
  bool isIdle = (state == 0U);
  if (wasIdle != isIdle)
  {
    state_log::Push(isIdle ? state_log::Event::DeviceIdle : state_log::Event::DeviceActive,
                    state);
  }
  deviceState = state;
}

extern "C" uint32_t CommBridge_GetLinkStatus(void)
{
  using namespace telemetry;
  uint32_t bits = 0;
  if (comm_ble::IsAlive())
  {
    bits |= (1U << 0);
  }
  if (comm_ble::IsConnected())
  {
    bits |= (1U << 2);
  }
  if (g_service != nullptr)
  {
    bits |= g_service->wifiTcpLinkBits();
  }
  return bits;
}

extern "C" uint32_t CommBridge_GetAudioBuffer(int16_t *dst, uint32_t maxSamples)
{
  /* dst is a Secure-local buffer here (the CMSE gateway validated the
   * NonSecure origin). Copy the live mic capture window from the Secure
   * audio DMA buffer. */
  uint32_t total = audio_capture::Samples();
  uint32_t n = (maxSamples < total) ? maxSamples : total;
  memcpy(dst, audio_capture::Buffer(), n * sizeof(int16_t));
  return n;
}

extern "C" void CommBridge_GetMcuInfo(FullStatus_t *dst)
{
  /* dst is a Secure-local buffer here (the CMSE gateway validated the
   * NonSecure origin and copied it in). Only overwrite the MCU-info fields
   * and ble_alive/wifi_alive - see telemetry::Service::copyMcuStatusInto()'s
   * doc comment for why the NS app needs this instead of filling the fields
   * itself. */
  if (telemetry::g_service == nullptr)
  {
    return;
  }
  static_assert(sizeof(FullStatus_t) == sizeof(telemetry::FullStatus),
               "FullStatus_t / telemetry::FullStatus must stay byte-identical");
  telemetry::FullStatus local;
  memcpy(&local, dst, sizeof(local));
  telemetry::g_service->copyMcuStatusInto(local);
  memcpy(dst, &local, sizeof(local));
}


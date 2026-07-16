/**
  ******************************************************************************
  * @file    comm_ble.cpp
  * @brief   comm_ble.hpp の STM32WB5MMG(UART4) 向け実装【port層】。
  ******************************************************************************
  */
#include "comm_ble.hpp"

#include "app_config.h"    /* CFG_BLE_BAUDRATE */
#include "frame_codec.h"   /* Frame_Encode, FRAME_CMD_STATUS_MINI */
#include "main.h"
#include "recorder.hpp"

#include "b_u585i_iot02a.h" /* BSP_LED_On/Off, LED_GREEN */

#include "stm32wb_at.h"
#include "stm32wb_at_ble.h"
#include "stm32wb_at_client.h"

#include <cstdio>
#include <cstring>

extern UART_HandleTypeDef huart4; /* STM32WB5MMG BLE module (AT server) */

namespace comm_ble
{
namespace
{
/* --- BLE AT link state (set from AT reply/event callbacks) --- */
volatile bool bleLinkOk = false;
volatile bool bleConnected = false;
uint8_t bleAtBuffer[160]; /* long +BLE_EVT_WRITE events exceed 64 chars */
uint8_t bleRxByte;
bool bleGlueReady = false;

uint8_t bleSeq_ = 0;   /* Service のメンバから移す（BLE専用のシーケンス番号） */

/* --- BLE recording control/transfer state --- */
Frame_Decoder bleWriteDecoder;
bool bleWriteDecoderInit = false;
volatile uint8_t bleRecCmd = 0; /* 0=none, 1=start, 2=stop (set by the GATT
                                    write callback, consumed once) */
volatile bool bleHostActivity = false; /* any GATT write (fe41) since the last
                                    TakeHostActivity(): keeps the NS idle timer
                                    awake over BLE, mirroring how any inbound
                                    UART/TCP byte sets nsActivity */
bool bleRecSendPending = false;
uint32_t bleRecOff = 0;
uint16_t bleRecChunkSeq = 0;

/* Raw AT exchange (polling, before the interrupt-driven client starts):
 * prints the module's literal reply so protocol mismatches are visible. */
void bleRawProbe(const char *cmd)
{
  uint8_t buf[96];
  uint16_t n = 0;
  HAL_UART_Transmit(&huart4, reinterpret_cast<const uint8_t *>(cmd),
                    static_cast<uint16_t>(strlen(cmd)), 200);
  uint32_t t0 = HAL_GetTick();
  while ((HAL_GetTick() - t0) < 700U && n < sizeof(buf) - 1U)
  {
    uint8_t c;
    if (HAL_UART_Receive(&huart4, &c, 1, 50) == HAL_OK)
    {
      buf[n++] = (c == '\r' || c == '\n') ? '|' : c;
    }
  }
  buf[n] = 0;
  printf("[BLE-RAW] %s-> %u bytes: %s\r\n", cmd, n, buf);
}

} // namespace

namespace
{
/* One AT bring-up attempt at CFG_BLE_BAUDRATE. Re-inits the UART, the AT LL
 * glue and the client, queries BLE_TEST and waits for the reply (parsed in the
 * RX interrupt, which sets bleLinkOk). Returns true only if the module
 * answered. Factored out so Init() can retry it: a flaky first attempt (e.g. a
 * garbled first byte at higher baud) often succeeds on a re-try.
 *
 * withProbe: only the first attempt runs the three raw AT probes - they exist
 * purely to print the module's literal reply for debugging and each blocks up
 * to ~700 ms, so retries skip them and just re-drive the AT client. The
 * retry is meant to be "re-send the AT handshake a couple more times", not to
 * repeat the full diagnostic + long waits. */
bool bringUpOnce(bool withProbe)
{
  huart4.Init.BaudRate = CFG_BLE_BAUDRATE;
  (void)HAL_UART_Init(&huart4);
  if (withProbe)
  {
    bleRawProbe("AT\r\n");
    bleRawProbe("AT+BLE_TEST?\r\n");
    bleRawProbe("AT+BLE_SVC=1\r\n");
  }

  /* BLE module: AT client over UART4.
   * Note: the library never calls stm32wb_at_ll_Init itself. */
  bleLinkOk = false;
  if (stm32wb_at_ll_Init() == 0U &&
      stm32wb_at_Init(bleAtBuffer, sizeof(bleAtBuffer)) == 0U &&
      stm32wb_at_client_Init() == 0U)
  {
    bleGlueReady = true;
    (void)stm32wb_at_client_Query(BLE_TEST);
    HAL_Delay(200); /* wait for the reply (parsed in RX interrupt) */
    if (bleLinkOk)
    {
      /* Start the P2P server application + advertising on the module */
      stm32wb_at_BLE_SVC_t svc = {};
      svc.index = 1;
      (void)stm32wb_at_client_Set(BLE_SVC, &svc);
    }
  }
  return bleLinkOk;
}
} // namespace

bool Init()
{
  for (uint32_t attempt = 1U; attempt <= CFG_BLE_INIT_RETRIES; attempt++)
  {
    if (bringUpOnce(attempt == 1U))
    {
      if (attempt > 1U)
      {
        printf("[BLE] link up after %lu attempts\r\n", (unsigned long)attempt);
      }
      return true;
    }
    printf("[BLE] bring-up attempt %lu/%lu failed at %lu baud\r\n",
           (unsigned long)attempt, (unsigned long)CFG_BLE_INIT_RETRIES,
           (unsigned long)CFG_BLE_BAUDRATE);
  }
  /* Still BLE=NG after all retries: drop the baud on both sides and reflash. */
  return false;
}

bool IsAlive() { return bleLinkOk; }

bool IsConnected() { return bleConnected; }

void SendStatus(const telemetry::FullStatus &st)
{
  if (!bleLinkOk || !bleConnected)
  {
    return;
  }

  telemetry::MiniStatus mini = {};
  mini.button = st.button;
  mini.temp_x100 = st.temp_x100;
  mini.hum_x100 = st.hum_x100;
  mini.press_x10 = static_cast<uint16_t>(st.press_x100 / 10U);
  mini.light_raw16 = static_cast<uint16_t>(st.light_raw > 0xFFFFU ? 0xFFFFU : st.light_raw);
  mini.tof_mm = st.tof_mm;
  for (int i = 0; i < 3; i++)
  {
    mini.acc_mg[i] = st.acc_mg[i];
    mini.gyro_dps10[i] = st.gyro_dps10[i];
    mini.mag_mgauss[i] = st.mag_mgauss[i];
  }
  mini.audio_rms = st.audio_rms;
  mini.audio_peak = st.audio_peak;
  mini.uptime_s = static_cast<uint16_t>(st.uptime_ms / 1000U);
  mini.die_temp_x100 = st.die_temp_x100;
  mini.flags = static_cast<uint8_t>((st.ble_alive != 0U ? 1U : 0U) |
                                    (st.wifi_alive != 0U ? 2U : 0U) |
                                    (st.tof_ok != 0U ? 4U : 0U));
  mini.cpu_load_pct = st.cpu_load_pct;

  stm32wb_at_BLE_NOTIF_VAL_t notif = {};
  notif.svc_index = 1; /* P2P server */
  notif.char_index = 2; /* notify characteristic (fe42) */
  notif.val_tab_len = static_cast<uint8_t>(
      Frame_Encode(FRAME_CMD_STATUS_MINI, bleSeq_++,
                   reinterpret_cast<const uint8_t *>(&mini), sizeof(mini),
                   notif.val_tab, sizeof(notif.val_tab)));
  if (notif.val_tab_len > 0U)
  {
    (void)stm32wb_at_client_Set(BLE_NOTIF_VAL, &notif);
  }
}

/* recorder::Stop() flips bleRecCmd==2 handling (see comm_service.cpp's
 * poll(), which calls recorder::Stop() then sets bleRecSendPending) - this
 * function just drains the ring a few chunks per call. REC_CHUNK/REC_END
 * are NOT run through Frame_Encode: a 47-byte frame_codec frame already
 * barely fit the AT-server's 64-byte characteristic (see the str_received[64]
 * overflow this project hit at 47 bytes), and REC_CHUNK's ~58-byte ADPCM
 * payload plus an 8-byte frame_codec overhead would risk the same margin -
 * so this uses a minimal raw TLV instead: [cmd u8][seq u16 LE][payload]. */
void PumpRecTx()
{
  if (!bleRecSendPending || !bleLinkOk || !bleConnected)
  {
    return;
  }

  constexpr uint32_t kChunkPayload = 58U;
  constexpr uint32_t kChunksPerPump = 2U; /* ~200 ms/notify at 9600 baud */

  for (uint32_t i = 0; i < kChunksPerPump; i++)
  {
    uint8_t raw[3U + kChunkPayload];
    uint32_t n = recorder::Read(bleRecOff, &raw[3], kChunkPayload);
    if (n == 0U)
    {
      /* Ring drained: send REC_END with the total sample count + CRC16
       * over everything sent so far (offset 0..bleRecOff), so the PC can
       * verify nothing was dropped mid-stream. */
      uint8_t end[1U + 4U + 2U];
      end[0] = FRAME_CMD_REC_END;
      uint32_t total = recorder::TotalSamples();
      end[1] = static_cast<uint8_t>(total & 0xFFU);
      end[2] = static_cast<uint8_t>((total >> 8) & 0xFFU);
      end[3] = static_cast<uint8_t>((total >> 16) & 0xFFU);
      end[4] = static_cast<uint8_t>((total >> 24) & 0xFFU);
      uint16_t crc = Frame_Crc16(end, 5U);
      end[5] = static_cast<uint8_t>(crc & 0xFFU);
      end[6] = static_cast<uint8_t>(crc >> 8);

      stm32wb_at_BLE_NOTIF_VAL_t notif = {};
      notif.svc_index = 1;
      notif.char_index = 2;
      notif.val_tab_len = sizeof(end);
      memcpy(notif.val_tab, end, sizeof(end));
      (void)stm32wb_at_client_Set(BLE_NOTIF_VAL, &notif);

      bleRecSendPending = false;
      bleRecOff = 0;
      return;
    }

    raw[0] = FRAME_CMD_REC_CHUNK;
    raw[1] = static_cast<uint8_t>(bleRecChunkSeq & 0xFFU);
    raw[2] = static_cast<uint8_t>(bleRecChunkSeq >> 8);
    bleRecChunkSeq++;
    bleRecOff += n;

    stm32wb_at_BLE_NOTIF_VAL_t notif = {};
    notif.svc_index = 1;
    notif.char_index = 2;
    notif.val_tab_len = static_cast<uint8_t>(3U + n);
    memcpy(notif.val_tab, raw, 3U + n);
    (void)stm32wb_at_client_Set(BLE_NOTIF_VAL, &notif);
  }
}

uint8_t TakeRecCmd()
{
  uint8_t cmd = bleRecCmd;
  bleRecCmd = 0U;
  return cmd;
}

bool TakeHostActivity()
{
  bool active = bleHostActivity;
  bleHostActivity = false;
  return active;
}

void StartRecTx()
{
  bleRecSendPending = true;
  bleRecOff = 0;
}

} // namespace comm_ble

/* ---- STM32WB AT transport glue (C linkage, UART4 interrupt driven) ------- */

extern "C" uint8_t stm32wb_at_ll_Init(void)
{
  huart4.Init.BaudRate = CFG_BLE_BAUDRATE;
  if (HAL_UART_Init(&huart4) != HAL_OK)
  {
    return 1;
  }
  HAL_NVIC_SetPriority(UART4_IRQn, 14, 0);
  HAL_NVIC_EnableIRQ(UART4_IRQn);
  return 0; /* RX is armed by stm32wb_at_ll_Async_receive() */
}

extern "C" uint8_t stm32wb_at_ll_DeInit(void)
{
  HAL_NVIC_DisableIRQ(UART4_IRQn);
  return 0;
}

extern "C" uint8_t stm32wb_at_ll_Transmit(uint8_t *pBuff, uint16_t Size)
{
  return (HAL_UART_Transmit(&huart4, pBuff, Size, 1000) == HAL_OK) ? 0 : 1;
}

extern "C" void stm32wb_at_ll_Async_receive(uint8_t new_frame)
{
  (void)new_frame;
  (void)HAL_UART_Receive_IT(&huart4, &comm_ble::bleRxByte, 1);
}

/* BLE(UART4)の受信完了割り込み。
 * 【重要】プロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == UART4)
  {
    /* Any reply from the module proves the AT link is alive, even when the
     * reply text is not a parsable +BLE_... response */
    comm_ble::bleLinkOk = true;
    (void)stm32wb_at_Received(comm_ble::bleRxByte);
  }
  /* USART1 RX is DMA-driven (console.cpp); Console_GetChar() polls the
   * circular buffer directly, no callback needed. */
}

/* ---- AT reply / event callbacks (override the library weak defaults) ----- */

extern "C" uint8_t stm32wb_at_BLE_TEST_cb(stm32wb_at_BLE_TEST_t *param)
{
  (void)param;
  comm_ble::bleLinkOk = true;
  return 0;
}

extern "C" uint8_t stm32wb_at_BLE_EVT_CONN_cb(stm32wb_at_BLE_EVT_CONN_t *param)
{
  comm_ble::bleConnected = (param->status != 0U);
  printf("[TLM] BLE central %s\r\n", comm_ble::bleConnected ? "connected" : "disconnected");
  return 0;
}

/* PC -> board data path: GATT write on the P2P write characteristic (fe41) */
extern "C" uint8_t stm32wb_at_BLE_EVT_WRITE_cb(stm32wb_at_BLE_EVT_WRITE_t *param)
{
  printf("[TLM] BLE write: svc=%u char=%u len=%u val0=0x%02X\r\n",
         param->svc_index, param->char_index, param->val_tab_len,
         param->val_tab_len > 0U ? param->val_tab[0] : 0U);

  /* Any GATT write from the host is host activity - this is how the PC keeps
   * the board ACTIVE over BLE (app.py sends a 1 Hz keep-alive byte on fe41),
   * exactly like an inbound UART/TCP byte does over the wired links. Consumed
   * in Service::poll() via comm_ble::TakeHostActivity(). */
  comm_ble::bleHostActivity = true;

  if ((param->svc_index == 1U) && (param->val_tab_len > 0U))
  {
    if (param->val_tab[0] != 0U)
    {
      BSP_LED_On(LED_GREEN);
    }
    else
    {
      BSP_LED_Off(LED_GREEN);
    }
  }

  /* Recording control: PC sends a normal frame_codec REC_START/REC_STOP
   * frame (via BleTransport.write on fe41), which this feeds through the
   * shared Frame_Decoder byte by byte to reuse the same SOF/CRC/EOF framing
   * as every other transport. */
  if (!comm_ble::bleWriteDecoderInit)
  {
    Frame_DecoderInit(&comm_ble::bleWriteDecoder);
    comm_ble::bleWriteDecoderInit = true;
  }
  for (uint8_t i = 0; i < param->val_tab_len; i++)
  {
    uint8_t cmd, seq;
    const uint8_t *payload;
    uint16_t len;
    Frame_FeedResult r = Frame_DecoderFeed(&comm_ble::bleWriteDecoder,
                                           param->val_tab[i], &cmd, &seq,
                                           &payload, &len);
    if (r == FRAME_FEED_COMPLETE)
    {
      if (cmd == FRAME_CMD_REC_START)
      {
        comm_ble::bleRecCmd = 1U;
      }
      else if (cmd == FRAME_CMD_REC_STOP)
      {
        comm_ble::bleRecCmd = 2U;
      }
    }
  }
  return 0;
}

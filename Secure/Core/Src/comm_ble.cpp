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
uint8_t bleAtBuffer[560]; /* long AT lines: a 240-byte REC_CHUNK notify is a
                             ~505-char AT+BLE_NOTIF_VAL hex line (was 160/320) */
uint8_t bleRxByte;
bool bleGlueReady = false;

uint8_t bleSeq_ = 0;   /* Service のメンバから移す（BLE専用のシーケンス番号） */

/* REC_CHUNK 1個あたりのADPCMペイロード長。REC_GET の応答オフセットは
 * seq*kRecChunkPayload バイト目。58→120→240 に拡張(notify GATT特性長を WB5MMG
 * 側で 64→128→248 に広げ、AT/notify バッファも 560 へ拡張済み)。240+3=243 は
 * ATT_MTU(251)-3=248 に収まる上限付近で、1 notify で1チャンク送れる。チャンク数が
 * 更に半減し転送が速くなる。 */
constexpr uint32_t kRecChunkPayload = 240U;

/* --- BLE recording control/transfer state --- */
Frame_Decoder bleWriteDecoder;
bool bleWriteDecoderInit = false;
volatile uint8_t bleRecCmd = 0; /* 0=none, 1=start, 2=stop (set by the GATT
                                    write callback, consumed once) */
volatile bool bleHostActivity = false; /* any GATT write (fe41) since the last
                                    TakeHostActivity(): keeps the NS idle timer
                                    awake over BLE, mirroring how any inbound
                                    UART/TCP byte sets nsActivity */

/* 録音転送は連続ストリーミング方式: recorder::FeedPcm() が録音しながら
 * kBlockSamples ごとに完結ADPCMブロックをリングへ積み、PumpRecStream() が
 * ペーシングしつつ recorder::PopBlock() で1つずつ取り出して REC_CHUNK として
 * 送る。録音中も送信が並行して進むので、RAM上の録音バッファに依らず(リングの
 * 送信待ち分だけ)無制限長の録音ができる。REC_STOP 後は残りのブロックを送り
 * 切ったら REC_END(総ブロック数)を1回送って終える。1ブロック=1チャンクなので
 * チャンク欠落はその区間の音飛びで済み、他ブロックの predictor には波及しない
 * (各ブロックが自己完結ヘッダを持つため)。取りこぼしを追加で再送する仕組みは
 * 持たない(送信済みブロックはリングから解放され再送できないため)。 */
bool bleStreamActive = false;   /* REC_STOP 後、残りを送り切るまで true を維持 */
bool bleRecEndPending = false;  /* 送り切った直後、REC_END を1回返す */

/* notify 送信のペーシング用。前回チャンク送信 tick。 */
uint32_t bleLastChunkTick = 0;
constexpr uint32_t kRecNotifyGapMs = 12U;

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
  /* bits 0-2: existing link/sensor flags. bits 3-4: NonSecure device state
   * (telemetry::GetDeviceState(), forwarded from AppState_t via
   * Comm_SetDeviceState() - 0=IDLE, 1=ACTIVE_ACQUIRE, 2=ACTIVE_COMM,
   * 3=ACTIVE_BLE_REC). Fits in 2 bits (max value 3) with no MiniStatus size
   * change; bits 5-7 stay free for later use. */
  uint32_t devState = telemetry::GetDeviceState() & 0x3U;
  mini.flags = static_cast<uint8_t>((st.ble_alive != 0U ? 1U : 0U) |
                                    (st.wifi_alive != 0U ? 2U : 0U) |
                                    (st.tof_ok != 0U ? 4U : 0U) |
                                    (devState << 3));
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

/* REC_CHUNK/REC_INFO は Frame_Encode を通さない: 47バイトの frame_codec フレーム
 * でも AT サーバの64バイト特性(str_received[64])のマージンぎりぎりなので、生TLV
 * [cmd u8][seq u16 LE][payload] を使う。 */

/* 録音が完全に終わった(Stop済み・リングも空)ことを示す REC_END を1つ notify
 * で返す。ペイロードは [total_samples u32 LE][total_blocks u16 LE]。ストリーミング
 * では録音完了前に総数が分からないため、最後に一度だけ送る形にした
 * (旧 REC_INFO と同じレイアウトだが総ブロック数を運ぶ)。 */
void sendRecEndNotify(uint32_t totalSamples, uint32_t totalBlocks)
{
  uint8_t info[1U + 4U + 2U];
  info[0] = FRAME_CMD_REC_END;
  info[1] = static_cast<uint8_t>(totalSamples & 0xFFU);
  info[2] = static_cast<uint8_t>((totalSamples >> 8) & 0xFFU);
  info[3] = static_cast<uint8_t>((totalSamples >> 16) & 0xFFU);
  info[4] = static_cast<uint8_t>((totalSamples >> 24) & 0xFFU);
  info[5] = static_cast<uint8_t>(totalBlocks & 0xFFU);
  info[6] = static_cast<uint8_t>((totalBlocks >> 8) & 0xFFU);

  stm32wb_at_BLE_NOTIF_VAL_t notif = {};
  notif.svc_index = 1;
  notif.char_index = 2;
  notif.val_tab_len = sizeof(info);
  memcpy(notif.val_tab, info, sizeof(info));
  (void)stm32wb_at_client_Set(BLE_NOTIF_VAL, &notif);
}

/* REC_START を受けてストリーミング転送状態に入る。Service::poll() が
 * recorder::Start() の直後に呼ぶ。 */
void SendRecInfo_Arm()
{
  bleStreamActive = true;
  bleRecEndPending = false;
  BSP_LED_On(LED_GREEN); /* 録音送信中は緑LEDを点灯し続ける(REC_END送出まで) */
}

/* 毎 poll 呼ぶ: ペーシングしつつ recorder のリングから1ブロックずつ取り出して
 * REC_CHUNK として送る。録音中(recorder::Active())でも録音停止後(残り送信中)
 * でも動く。全部送り切って recorder が非アクティブになったら REC_END を1回
 * 返してストリーミングを終える。 */
void SendRecInfo()
{
  if (!bleStreamActive || !bleLinkOk || !bleConnected)
  {
    return;
  }

  if (bleRecEndPending)
  {
    sendRecEndNotify(recorder::TotalSamples(), recorder::TotalBlocks());
    bleRecEndPending = false;
    bleStreamActive = false;
    BSP_LED_Off(LED_GREEN);
    return;
  }

  /* ペーシング。 */
  uint32_t nowTick = HAL_GetTick();
  if ((nowTick - bleLastChunkTick) < kRecNotifyGapMs)
  {
    return;
  }

  uint8_t raw[3U + recorder::kBlockBytes];
  uint32_t len = 0U;
  uint32_t seq = 0U;
  if (recorder::PopBlock(&raw[3], &len, &seq))
  {
    raw[0] = FRAME_CMD_REC_CHUNK;
    raw[1] = static_cast<uint8_t>(seq & 0xFFU);
    raw[2] = static_cast<uint8_t>(seq >> 8);

    stm32wb_at_BLE_NOTIF_VAL_t notif = {};
    notif.svc_index = 1;
    notif.char_index = 2;
    notif.val_tab_len = static_cast<uint8_t>(3U + len);
    memcpy(notif.val_tab, raw, 3U + len);
    (void)stm32wb_at_client_Set(BLE_NOTIF_VAL, &notif);
    bleLastChunkTick = nowTick;
    return;
  }

  /* リングが空。録音がまだアクティブなら次のブロックを待つ。停止済みなら
   * 送り切ったということなので REC_END を次回返す。 */
  if (!recorder::Active())
  {
    bleRecEndPending = true;
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

bool IsRecTxActive()
{
  /* ストリーミング中(録音中または残ブロック送信中)は転送中扱い(この間テレメトリ
   * を止めて notify リンクを録音転送専用に譲る)。 */
  return bleStreamActive;
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

  /* 録音送信中は緑LEDを SendRecInfo_Arm()/SendRecInfo() が専有する(点灯し続ける)
   * ので、keep-alive バイトによる通常のLED制御はここで無効化する。 */
  if (!comm_ble::bleStreamActive && (param->svc_index == 1U) && (param->val_tab_len > 0U))
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

/**
  ******************************************************************************
  * @file    telemetry.cpp
  * @brief   Continuous board-status telemetry implementation.
  ******************************************************************************
  */
#include "telemetry.hpp"

#include "app_config.h"
#include "frame_codec.h"
#include "main.h"

#include "b_u585i_iot02a.h"
#include "b_u585i_iot02a_audio.h"
#include "b_u585i_iot02a_env_sensors.h"
#include "b_u585i_iot02a_light_sensor.h"
#include "b_u585i_iot02a_motion_sensors.h"
#include "b_u585i_iot02a_ranging_sensor.h"

#include "stm32wb_at.h"
#include "stm32wb_at_ble.h"
#include "stm32wb_at_client.h"

#include "mx_wifi.h"
#include "io_pattern/mx_wifi_io.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>

extern UART_HandleTypeDef huart1; /* VCP console / telemetry stream */
extern UART_HandleTypeDef huart4; /* STM32WB5MMG BLE module (AT server) */
extern "C" SPI_HandleTypeDef hspi2; /* EMW3080 Wi-Fi module */

/* CubeMX-generated ADF1 handle (main.c), released before the BSP takes over */
extern "C" MDF_HandleTypeDef AdfHandle0;

/* Shared BSP audio DMA event flags (defined at the end of this file) */
extern "C" volatile uint32_t g_AudioEvents;
extern "C" volatile uint32_t g_AudioErrors;

/* Linker symbols for memory statistics */
extern "C" uint8_t _end;    /* end of .bss (start of heap)   */
extern "C" uint8_t _sdata;  /* start of .data                */
extern "C" uint8_t _edata;  /* end of .data                  */
extern "C" uint8_t _etext;  /* end of .text (flash)          */

namespace telemetry
{

namespace
{
constexpr uint32_t kFullPeriodMs = CFG_TLM_FULL_PERIOD_MS;
constexpr uint32_t kTofPeriodMs = 1000; /* ToF read: 1 Hz */
constexpr uint32_t kBlePeriodMs = CFG_TLM_BLE_PERIOD_MS;
constexpr size_t kAudioSamples = 2048;   /* circular capture buffer */

int16_t audioBuf[kAudioSamples];
uint8_t audioFrame[1024 + FRAME_OVERHEAD]; /* PCM streaming TX buffer */

/* --- BLE AT link state (set from AT reply/event callbacks) --- */
volatile bool bleLinkOk = false;
volatile bool bleConnected = false;
uint8_t bleAtBuffer[64];
uint8_t bleRxByte;
bool bleGlueReady = false;

/* --- Wi-Fi module pins (EMW3080) --- */
void wifiPinsInit()
{
  GPIO_InitTypeDef gpio = {};
  HAL_PWREx_EnableVddIO2(); /* PG[15:2] */

  /* Reconfigure SPI2 for the EMW3080 (the CubeMX defaults use 4-bit frames,
   * hardware NSS and an 80 Mbps clock - the module needs 8-bit, soft NSS,
   * 20 Mbps, same as the ST reference application) */
  (void)HAL_SPI_DeInit(&hspi2);
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  /* 10 MHz: the polled (no DMA) receive path overruns at the reference
   * 20 MHz clock once responses exceed ~100 bytes (scan, ip_attr) */
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi2.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  (void)HAL_SPI_Init(&hspi2);
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  HAL_GPIO_WritePin(MXCHIP_NSS_GPIO_Port, MXCHIP_NSS_Pin, GPIO_PIN_SET);
  gpio.Pin = MXCHIP_NSS_Pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(MXCHIP_NSS_GPIO_Port, &gpio);

  HAL_GPIO_WritePin(MXCHIP_RESET_GPIO_Port, MXCHIP_RESET_Pin, GPIO_PIN_RESET);
  gpio.Pin = MXCHIP_RESET_Pin;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(MXCHIP_RESET_GPIO_Port, &gpio);

  /* FLOW / NOTIFY: rising-edge interrupts feeding the mx_wifi driver */
  gpio.Mode = GPIO_MODE_IT_RISING;
  gpio.Pull = GPIO_NOPULL;
  gpio.Pin = MXCHIP_FLOW_Pin;
  HAL_GPIO_Init(MXCHIP_FLOW_GPIO_Port, &gpio);
  gpio.Pin = MXCHIP_NOTIFY_Pin;
  HAL_GPIO_Init(MXCHIP_NOTIFY_GPIO_Port, &gpio);

  HAL_NVIC_SetPriority(MXCHIP_FLOW_EXTI_IRQn, 13, 0);
  HAL_NVIC_EnableIRQ(MXCHIP_FLOW_EXTI_IRQn);
  HAL_NVIC_SetPriority(MXCHIP_NOTIFY_EXTI_IRQn, 13, 0);
  HAL_NVIC_EnableIRQ(MXCHIP_NOTIFY_EXTI_IRQn);
}

/* --- Wi-Fi TCP server state (single client) --- */
bool wifiNetUp = false;      /* joined the AP, has an IP */
int32_t tcpListenFd = -1;
int32_t tcpClientFd = -1;
uint32_t nextAcceptTick = 0;

constexpr uint16_t mxHtons(uint16_t v)
{
  return static_cast<uint16_t>((v << 8) | (v >> 8));
}

volatile uint8_t wifiLastEvent = 0; /* MWIFI_EVENT_... */

void wifiStatusCb(uint8_t cate, uint8_t event, void *arg)
{
  (void)arg;
  static const char *const names[] = {"NONE", "STA_DOWN", "STA_UP", "STA_GOT_IP",
                                      "AP_DOWN", "AP_UP"};
  wifiLastEvent = event;
  printf("[TLM] wifi event: cate=%u %s\r\n", cate,
         event <= 5U ? names[event] : "?");
}

/* Full EMW3080 bring-up through the mx_wifi driver (bare metal, SPI). */
bool wifiModuleInit()
{
  static bool probed = false;
  wifiPinsInit();
  wifiNetUp = false;
  tcpListenFd = -1;
  tcpClientFd = -1;

  if (!probed)
  {
    if (mxwifi_probe(nullptr) != 0)
    {
      printf("[TLM] mx_wifi probe failed\r\n");
      return false;
    }
    probed = true;
  }

  MX_WIFIObject_t *obj = wifi_obj_get();
  if (MX_WIFI_HardResetModule(obj) != MX_WIFI_STATUS_OK)
  {
    printf("[TLM] EMW3080 hard reset failed\r\n");
    return false;
  }
  if (MX_WIFI_Init(obj) != MX_WIFI_STATUS_OK)
  {
    printf("[TLM] EMW3080 init failed (no reply on SPI)\r\n");
    return false;
  }
  printf("[TLM] EMW3080 FW=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
         obj->SysInfo.FW_Rev, obj->SysInfo.MAC[0], obj->SysInfo.MAC[1],
         obj->SysInfo.MAC[2], obj->SysInfo.MAC[3], obj->SysInfo.MAC[4],
         obj->SysInfo.MAC[5]);

  if (CFG_WIFI_SSID[0] != '\0')
  {
    (void)MX_WIFI_RegisterStatusCallback(obj, wifiStatusCb, nullptr);

    /* Survey the neighborhood first: shows whether the AP is visible and on
     * a supported band/channel */
    if (MX_WIFI_Scan(obj, MC_SCAN_PASSIVE, nullptr, 0) == MX_WIFI_STATUS_OK)
    {
      static mwifi_ap_info_t aps[10];
      int8_t apNum = MX_WIFI_Get_scan_result(obj, reinterpret_cast<uint8_t *>(aps), 10);
      printf("[TLM] scan: %d APs\r\n", apNum);
      for (int8_t i = 0; i < apNum; i++)
      {
        printf("  ch%2ld rssi=%ld \"%s\"\r\n", (long)aps[i].channel,
               (long)aps[i].rssi, aps[i].ssid);
      }
    }

    printf("[TLM] joining AP \"%s\"...\r\n", CFG_WIFI_SSID);
    obj->NetSettings.DHCP_IsEnabled = 1; /* default 0 = static IP 0.0.0.0! */
    int32_t joinRet = MX_WIFI_Connect(obj, CFG_WIFI_SSID, CFG_WIFI_PASSWORD,
                                      MX_WIFI_SEC_AUTO);
    printf("[TLM] MX_WIFI_Connect ret=%ld\r\n", (long)joinRet);
    if (joinRet == MX_WIFI_STATUS_OK)
    {
      /* MX_WIFI_Connect returns before DHCP completes: poll for the lease */
      uint8_t ip[4] = {};
      for (int i = 0; i < 20; i++)
      {
        HAL_Delay(500);
        (void)MX_WIFI_GetIPAddress(obj, ip, MC_STATION);
        if ((ip[0] | ip[1] | ip[2] | ip[3]) != 0U)
        {
          break;
        }
      }
      printf("[TLM] Wi-Fi connected, IP=%u.%u.%u.%u\r\n", ip[0], ip[1], ip[2], ip[3]);
      wifiNetUp = (ip[0] | ip[1] | ip[2] | ip[3]) != 0U;
      if (!wifiNetUp)
      {
        printf("[TLM] DHCP lease not obtained within 10 s\r\n");
      }
    }
    else
    {
      printf("[TLM] AP join failed (check CFG_WIFI_SSID/PASSWORD)\r\n");
    }
  }
  else
  {
    printf("[TLM] CFG_WIFI_SSID empty - module verified, not joining an AP\r\n");
  }
  return true;
}

/* Open the telemetry TCP server socket on the module's stack. */
void tcpServerInit()
{
  if (!wifiNetUp)
  {
    return;
  }
  MX_WIFIObject_t *obj = wifi_obj_get();

  tcpListenFd = MX_WIFI_Socket_create(obj, MX_AF_INET, MX_SOCK_STREAM, MX_IPPROTO_TCP);
  if (tcpListenFd < 0)
  {
    printf("[TLM] TCP socket create failed (%ld)\r\n", tcpListenFd);
    return;
  }

  struct mx_sockaddr_in addr = {};
  addr.sin_len = sizeof(addr);
  addr.sin_family = MX_AF_INET;
  addr.sin_port = mxHtons(CFG_WIFI_TCP_PORT);
  addr.sin_addr.s_addr = 0; /* INADDR_ANY */
  if (MX_WIFI_Socket_bind(obj, tcpListenFd,
                          reinterpret_cast<struct mx_sockaddr *>(&addr),
                          sizeof(addr)) != MX_WIFI_STATUS_OK ||
      MX_WIFI_Socket_listen(obj, tcpListenFd, 1) != MX_WIFI_STATUS_OK)
  {
    printf("[TLM] TCP bind/listen failed\r\n");
    (void)MX_WIFI_Socket_close(obj, tcpListenFd);
    tcpListenFd = -1;
    return;
  }
  printf("[TLM] TCP server listening on port %u\r\n", CFG_WIFI_TCP_PORT);
}

void tcpCloseClient()
{
  if (tcpClientFd >= 0)
  {
    (void)MX_WIFI_Socket_close(wifi_obj_get(), tcpClientFd);
    tcpClientFd = -1;
    printf("[TLM] TCP client closed\r\n");
  }
}

/* ADF1 kernel clock: CubeMX MspInit forces HCLK; restore the BSP's PLL3 */
void reselectAudioPll3()
{
  RCC_PeriphCLKInitTypeDef cfg = {};
  cfg.PLL3.PLL3Source = RCC_PLLSOURCE_MSI;
  cfg.PLL3.PLL3M = 1;
  cfg.PLL3.PLL3N = 80;
  cfg.PLL3.PLL3P = 28;
  cfg.PLL3.PLL3Q = 28;
  cfg.PLL3.PLL3R = 2;
  cfg.PLL3.PLL3ClockOut = RCC_PLL3_DIVQ;
  cfg.PeriphClockSelection = RCC_PERIPHCLK_MDF1;
  cfg.Mdf1ClockSelection = RCC_MDF1CLKSOURCE_PLL3;
  (void)HAL_RCCEx_PeriphCLKConfig(&cfg);
}
} // namespace

void Service::initAudio()
{
  audioOk_ = false;

  static bool mxAdfReleased = false;
  if (!mxAdfReleased)
  {
    HAL_MDF_DeInit(&AdfHandle0);
    mxAdfReleased = true;
  }

  /* MIC2 (MDF1): the DMA-verified microphone path (MIC1/ADF1 only works in
   * polling mode with TrustZone, see 開発状況記録) */
  BSP_AUDIO_Init_t init = {};
  init.Device = AUDIO_IN_DEVICE_DIGITAL_MIC2;
  init.SampleRate = CFG_AUDIO_SAMPLE_RATE;
  init.BitsPerSample = AUDIO_RESOLUTION_16B;
  init.ChannelsNbr = 1;
  init.Volume = 100;
  if (BSP_AUDIO_IN_Init(0, &init) != BSP_ERROR_NONE)
  {
    printf("[TLM] audio init failed\r\n");
    return;
  }
  reselectAudioPll3();

  /* TrustZone: secure+privileged DMA channel, secure source/destination */
  (void)HAL_DMA_ConfigChannelAttributes(
      &haudio_mdf[1],
      DMA_CHANNEL_SEC | DMA_CHANNEL_PRIV | DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC);

  if (BSP_AUDIO_IN_Record(0, reinterpret_cast<uint8_t *>(audioBuf),
                          sizeof(audioBuf)) != BSP_ERROR_NONE)
  {
    printf("[TLM] audio record start failed\r\n");
    BSP_AUDIO_IN_DeInit(0);
    return;
  }
  audioOk_ = true;
}

void Service::initSensors()
{
  sensorsOk_ = true;

  /* ToF first: its I2C recovery must run before any other I2C2 user */
  tofOk_ = false;
  if (BSP_RANGING_SENSOR_Init(0) == BSP_ERROR_NONE)
  {
    RANGING_SENSOR_ProfileConfig_t profile = {};
    profile.RangingProfile = RS_PROFILE_4x4_CONTINUOUS;
    profile.TimingBudget = 30;
    profile.Frequency = 5;
    profile.EnableAmbient = 0;
    profile.EnableSignal = 0;
    if (BSP_RANGING_SENSOR_ConfigProfile(0, &profile) == BSP_ERROR_NONE &&
        BSP_RANGING_SENSOR_Start(0, RS_MODE_ASYNC_CONTINUOUS) == BSP_ERROR_NONE)
    {
      tofOk_ = true;
    }
  }
  if (!tofOk_)
  {
    printf("[TLM] ToF init failed\r\n");
  }

  if (BSP_ENV_SENSOR_Init(0, ENV_TEMPERATURE | ENV_HUMIDITY) != BSP_ERROR_NONE ||
      BSP_ENV_SENSOR_Enable(0, ENV_TEMPERATURE) != BSP_ERROR_NONE ||
      BSP_ENV_SENSOR_Enable(0, ENV_HUMIDITY) != BSP_ERROR_NONE ||
      BSP_ENV_SENSOR_Init(1, ENV_PRESSURE) != BSP_ERROR_NONE ||
      BSP_ENV_SENSOR_Enable(1, ENV_PRESSURE) != BSP_ERROR_NONE)
  {
    printf("[TLM] env sensor init failed\r\n");
    sensorsOk_ = false;
  }

  if (BSP_MOTION_SENSOR_Init(0, MOTION_ACCELERO | MOTION_GYRO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(0, MOTION_ACCELERO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(0, MOTION_GYRO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Init(1, MOTION_MAGNETO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(1, MOTION_MAGNETO) != BSP_ERROR_NONE)
  {
    printf("[TLM] motion sensor init failed\r\n");
    sensorsOk_ = false;
  }

  if (BSP_LIGHT_SENSOR_Init(0) != BSP_ERROR_NONE ||
      BSP_LIGHT_SENSOR_Start(0, LIGHT_SENSOR_MODE_CONTINUOUS) != BSP_ERROR_NONE)
  {
    printf("[TLM] light sensor init failed\r\n");
    sensorsOk_ = false;
  }

  (void)BSP_PB_Init(BUTTON_USER, BUTTON_MODE_GPIO);
}

/* Raw AT exchange (polling, before the interrupt-driven client starts):
 * prints the module's literal reply so protocol mismatches are visible. */
static void bleRawProbe(const char *cmd)
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

void Service::initRadio()
{
  status_.wifi_alive = wifiModuleInit() ? 1U : 0U;

  /* Debug: check the module's AT dialect with raw polled exchanges
   * (before interrupt-driven RX takes over the UART) */
  huart4.Init.BaudRate = CFG_BLE_BAUDRATE;
  (void)HAL_UART_Init(&huart4);
  bleRawProbe("AT\r\n");
  bleRawProbe("AT+BLE_TEST?\r\n");
  bleRawProbe("AT+BLE_SVC=1\r\n");

  /* BLE module: AT client over UART4 at 9600 baud.
   * Note: the library never calls stm32wb_at_ll_Init itself. */
  bleLinkOk = false;
  if (stm32wb_at_ll_Init() == 0U &&
      stm32wb_at_Init(bleAtBuffer, sizeof(bleAtBuffer)) == 0U &&
      stm32wb_at_client_Init() == 0U)
  {
    bleGlueReady = true;
    (void)stm32wb_at_client_Query(BLE_TEST);
    HAL_Delay(500); /* wait for the reply (parsed in RX interrupt) */
    if (bleLinkOk)
    {
      /* Start the P2P server application + advertising on the module */
      stm32wb_at_BLE_SVC_t svc = {};
      svc.index = 1;
      (void)stm32wb_at_client_Set(BLE_SVC, &svc);
    }
  }
  status_.ble_alive = bleLinkOk ? 1U : 0U;
  printf("[TLM] radio: BLE=%s WiFi=%s\r\n", bleLinkOk ? "OK" : "NG",
         status_.wifi_alive != 0U ? "OK" : "NG");

  tcpServerInit();
}

void Service::init()
{
  printf("[TLM] initializing telemetry sources...\r\n");
  initSensors();
  initAudio();
  initRadio();
  nextFullTick_ = HAL_GetTick();
  nextTofTick_ = HAL_GetTick();
  nextBleTick_ = HAL_GetTick() + 500U;
  printf("[TLM] streaming: UART 5Hz (133B frames), BLE 1Hz (compact)\r\n");
}

void Service::collect(FullStatus &st)
{
  st.ver = 1;
  st.uptime_ms = HAL_GetTick();
  st.button = (BSP_PB_GetState(BUTTON_USER) == 1) ? 1U : 0U;

  float f = 0.0f;
  if (BSP_ENV_SENSOR_GetValue(0, ENV_TEMPERATURE, &f) == BSP_ERROR_NONE)
  {
    st.temp_x100 = static_cast<int16_t>(f * 100.0f);
  }
  if (BSP_ENV_SENSOR_GetValue(0, ENV_HUMIDITY, &f) == BSP_ERROR_NONE)
  {
    st.hum_x100 = static_cast<uint16_t>(f * 100.0f);
  }
  if (BSP_ENV_SENSOR_GetValue(1, ENV_PRESSURE, &f) == BSP_ERROR_NONE)
  {
    st.press_x100 = static_cast<uint32_t>(f * 100.0f);
  }

  BSP_MOTION_SENSOR_Axes_t axes = {};
  if (BSP_MOTION_SENSOR_GetAxes(0, MOTION_ACCELERO, &axes) == BSP_ERROR_NONE)
  {
    st.acc_mg[0] = static_cast<int16_t>(axes.xval);
    st.acc_mg[1] = static_cast<int16_t>(axes.yval);
    st.acc_mg[2] = static_cast<int16_t>(axes.zval);
  }
  if (BSP_MOTION_SENSOR_GetAxes(0, MOTION_GYRO, &axes) == BSP_ERROR_NONE)
  {
    st.gyro_dps10[0] = static_cast<int16_t>(axes.xval / 100); /* mdps -> dps*10 */
    st.gyro_dps10[1] = static_cast<int16_t>(axes.yval / 100);
    st.gyro_dps10[2] = static_cast<int16_t>(axes.zval / 100);
  }
  if (BSP_MOTION_SENSOR_GetAxes(1, MOTION_MAGNETO, &axes) == BSP_ERROR_NONE)
  {
    st.mag_mgauss[0] = static_cast<int16_t>(axes.xval);
    st.mag_mgauss[1] = static_cast<int16_t>(axes.yval);
    st.mag_mgauss[2] = static_cast<int16_t>(axes.zval);
  }

  uint32_t light[LIGHT_SENSOR_MAX_CHANNELS] = {};
  if (BSP_LIGHT_SENSOR_GetValues(0, light) == BSP_ERROR_NONE)
  {
    st.light_raw = light[0];
  }

  /* ToF is I2C-heavy: refresh at its own slower rate */
  if (tofOk_ && static_cast<int32_t>(HAL_GetTick() - nextTofTick_) >= 0)
  {
    nextTofTick_ += kTofPeriodMs;
    static RANGING_SENSOR_Result_t result;
    if (BSP_RANGING_SENSOR_GetDistance(0, &result) == BSP_ERROR_NONE)
    {
      st.tof_mm = static_cast<uint16_t>(result.ZoneResult[0].Distance[0]);
      st.tof_ok = 1;
    }
    else
    {
      st.tof_ok = 0;
    }
  }

  /* Audio level from the live circular DMA buffer */
  if (audioOk_)
  {
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
    constexpr size_t stride = kAudioSamples / 32U;
    for (size_t i = 0; i < 32U; i++)
    {
      st.wave[i] = static_cast<int16_t>(audioBuf[i * stride] - mean);
    }
  }

  /* Memory statistics */
  struct mallinfo mi = mallinfo();
  uint32_t staticRam = reinterpret_cast<uint32_t>(&_end) - 0x30000000UL;
  st.heap_used = static_cast<uint32_t>(mi.uordblks);
  st.heap_free = static_cast<uint32_t>(mi.fordblks);
  st.ram_used = staticRam + st.heap_used;
  st.ram_total = 256U * 1024U;
  st.flash_used = (reinterpret_cast<uint32_t>(&_etext) - 0x0C000000UL) +
                  (reinterpret_cast<uint32_t>(&_edata) - reinterpret_cast<uint32_t>(&_sdata));
  st.flash_total = 1024U * 1024U;

  st.ble_alive = bleLinkOk ? 1U : 0U;
}

void Service::sendUart(const FullStatus &st)
{
  uint8_t frame[sizeof(FullStatus) + FRAME_OVERHEAD];
  size_t len = Frame_Encode(FRAME_CMD_STATUS, uartSeq_++,
                            reinterpret_cast<const uint8_t *>(&st), sizeof(st),
                            frame, sizeof(frame));
  if (len > 0U)
  {
    (void)HAL_UART_Transmit(&huart1, frame, static_cast<uint16_t>(len), 100);
  }
}

void Service::sendBle(const FullStatus &st)
{
  if (!bleLinkOk || !bleConnected)
  {
    return;
  }

  MiniStatus mini = {};
  mini.button = st.button;
  mini.temp_x100 = st.temp_x100;
  mini.hum_x100 = st.hum_x100;
  mini.press_x10 = static_cast<uint16_t>(st.press_x100 / 10U);
  mini.light_raw16 = static_cast<uint16_t>(st.light_raw > 0xFFFFU ? 0xFFFFU : st.light_raw);
  mini.tof_mm = st.tof_mm;
  int32_t lvl = st.audio_rms / 128; /* 0..255 rough scale */
  mini.audio_level = static_cast<uint8_t>(lvl > 255 ? 255 : lvl);

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

void Service::sendTcp(const FullStatus &st)
{
  if (tcpClientFd < 0)
  {
    return;
  }
  uint8_t frame[sizeof(FullStatus) + FRAME_OVERHEAD];
  size_t len = Frame_Encode(FRAME_CMD_STATUS, tcpSeq_++,
                            reinterpret_cast<const uint8_t *>(&st), sizeof(st),
                            frame, sizeof(frame));
  if (len > 0U)
  {
    int32_t sent = MX_WIFI_Socket_send(wifi_obj_get(), tcpClientFd, frame,
                                       static_cast<int32_t>(len), 0);
    if (sent <= 0)
    {
      printf("[TLM] TCP send failed (%ld), dropping client\r\n", (long)sent);
      tcpCloseClient();
    }
  }
}

void Service::pollTcp()
{
  if (tcpListenFd < 0)
  {
    return;
  }
  MX_WIFIObject_t *obj = wifi_obj_get();
  uint32_t now = HAL_GetTick();

  if (tcpClientFd < 0)
  {
    /* Poll for a pending connection at 1 Hz; log how long accept blocks so
     * a module-side blocking accept is visible in the console */
    if (static_cast<int32_t>(now - nextAcceptTick) < 0)
    {
      return;
    }
    nextAcceptTick = now + 1000U;

    struct mx_sockaddr_in ca = {};
    uint32_t calen = sizeof(ca);
    uint32_t t0 = HAL_GetTick();
    int32_t fd = MX_WIFI_Socket_accept(obj, tcpListenFd,
                                       reinterpret_cast<struct mx_sockaddr *>(&ca), &calen);
    uint32_t dt = HAL_GetTick() - t0;
    if (fd >= 0)
    {
      tcpClientFd = fd;
      uint32_t ip = ca.sin_addr.s_addr;
      printf("[TLM] TCP client connected from %lu.%lu.%lu.%lu (accept took %lu ms)\r\n",
             ip & 0xFFU, (ip >> 8) & 0xFFU, (ip >> 16) & 0xFFU, (ip >> 24) & 0xFFU, dt);
      int32_t tmo = 10; /* ms: keep the recv poll cheap */
      (void)MX_WIFI_Socket_setsockopt(obj, tcpClientFd, MX_SOL_SOCKET,
                                      MX_SO_RCVTIMEO, &tmo, sizeof(tmo));
    }
    else if (dt > 100U)
    {
      static bool warned = false;
      if (!warned)
      {
        warned = true;
        printf("[TLM] note: accept with no client blocks %lu ms\r\n", dt);
      }
    }
    return;
  }

  /* Client connected: poll for inbound commands */
  uint8_t buf[64];
  int32_t n = MX_WIFI_Socket_recv(obj, tcpClientFd, buf, sizeof(buf), 0);
  if (n > 0)
  {
    for (int32_t i = 0; i < n; i++)
    {
      char c = static_cast<char>(buf[i]);
      if (c == 'p' || c == 'P')
      {
        char msg[48];
        int m = snprintf(msg, sizeof(msg), "[TCP] PONG uptime=%lu\r\n", HAL_GetTick());
        (void)MX_WIFI_Socket_send(obj, tcpClientFd,
                                  reinterpret_cast<uint8_t *>(msg), m, 0);
      }
      else if (c == 'l' || c == 'L')
      {
        BSP_LED_Toggle(LED_GREEN);
        const char *msg = "[TCP] LED toggled\r\n";
        (void)MX_WIFI_Socket_send(obj, tcpClientFd,
                                  reinterpret_cast<const uint8_t *>(msg),
                                  static_cast<int32_t>(strlen(msg)), 0);
      }
    }
  }
}

void Service::setAudioStream(bool enable)
{
  audioStream_ = enable && audioOk_;
  g_AudioEvents = 0;
  printf("[TLM] audio streaming %s\r\n", audioStream_ ? "ON (16kHz mono)" : "OFF");
}

void Service::poll()
{
  uint32_t now = HAL_GetTick();
  if (static_cast<int32_t>(now - nextFullTick_) >= 0)
  {
    nextFullTick_ += kFullPeriodMs;
    /* After a long stall (e.g. the 10 s blocking TCP accept) skip the
     * missed periods instead of bursting the backlog */
    if (static_cast<int32_t>(now - nextFullTick_) > 1000)
    {
      nextFullTick_ = now + kFullPeriodMs;
    }
    collect(status_);
    sendUart(status_);
    sendTcp(status_);
  }
  if (static_cast<int32_t>(now - nextBleTick_) >= 0)
  {
    nextBleTick_ += kBlePeriodMs;
    sendBle(status_);
  }
  pollTcp();

  /* PCM streaming: forward each ready buffer half as two 512-sample frames */
  if (audioStream_)
  {
    uint32_t events = g_AudioEvents;
    if (events != 0U)
    {
      g_AudioEvents = 0;
      const int16_t *half =
          (events & 1U) != 0U ? &audioBuf[0] : &audioBuf[kAudioSamples / 2];
      for (int part = 0; part < 2; part++)
      {
        size_t len = Frame_Encode(
            FRAME_CMD_AUDIO, audioSeq_++,
            reinterpret_cast<const uint8_t *>(half + part * 512), 1024U,
            audioFrame, sizeof(audioFrame));
        if (len > 0U)
        {
          (void)HAL_UART_Transmit(&huart1, audioFrame, static_cast<uint16_t>(len), 100);
        }
      }
    }
  }
}

} // namespace telemetry

/* ---- Shared BSP audio callbacks (single definition for the whole app) ---- */
extern "C" volatile uint32_t g_AudioEvents = 0; /* bit0 = half, bit1 = full */
extern "C" volatile uint32_t g_AudioErrors = 0;

extern "C" void BSP_AUDIO_IN_HalfTransfer_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioEvents |= 1U;
}

extern "C" void BSP_AUDIO_IN_TransferComplete_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioEvents |= 2U;
}

extern "C" void BSP_AUDIO_IN_Error_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioErrors = g_AudioErrors + 1U;
}

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
  (void)HAL_UART_Receive_IT(&huart4, &telemetry::bleRxByte, 1);
}

extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == UART4)
  {
    /* Any reply from the module proves the AT link is alive, even when the
     * reply text is not a parsable +BLE_... response */
    telemetry::bleLinkOk = true;
    (void)stm32wb_at_Received(telemetry::bleRxByte);
  }
}

/* ---- EMW3080 handshake interrupts (FLOW=PG15, NOTIFY=PD14) ---------------- */
extern "C" void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == MXCHIP_FLOW_Pin || GPIO_Pin == MXCHIP_NOTIFY_Pin)
  {
    mxchip_WIFI_ISR(GPIO_Pin);
  }
}

/* ---- AT reply / event callbacks (override the library weak defaults) ----- */

extern "C" uint8_t stm32wb_at_BLE_TEST_cb(stm32wb_at_BLE_TEST_t *param)
{
  (void)param;
  telemetry::bleLinkOk = true;
  return 0;
}

extern "C" uint8_t stm32wb_at_BLE_EVT_CONN_cb(stm32wb_at_BLE_EVT_CONN_t *param)
{
  telemetry::bleConnected = (param->status != 0U);
  printf("[TLM] BLE central %s\r\n", telemetry::bleConnected ? "connected" : "disconnected");
  return 0;
}

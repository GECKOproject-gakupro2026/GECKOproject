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
#include "ota.hpp"

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
extern "C" void Secure_JumpToNonSecure(void);
extern "C" void BootGuard_ConfirmBoot(void); /* boot_guard.cpp, OTA Phase 2 */

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
constexpr uint32_t kEnvPeriodMs = CFG_TLM_ENV_PERIOD_MS;
constexpr uint32_t kLightPeriodMs = CFG_TLM_LIGHT_PERIOD_MS;
constexpr uint32_t kTofPeriodMs = CFG_TLM_TOF_PERIOD_MS;
constexpr uint32_t kMcuPeriodMs = CFG_TLM_MCU_PERIOD_MS;
constexpr uint32_t kBlePeriodMs = CFG_TLM_BLE_PERIOD_MS;

/* Internal ADC for die temperature and VDDA (via VREFINT) */
ADC_HandleTypeDef hadcMcu = {};
bool adcOk = false;

uint32_t adcReadChannel(uint32_t channel)
{
  ADC_ChannelConfTypeDef cfg = {};
  cfg.Channel = channel;
  cfg.Rank = ADC_REGULAR_RANK_1;
  cfg.SamplingTime = ADC_SAMPLETIME_814CYCLES;
  cfg.SingleDiff = ADC_SINGLE_ENDED;
  if (HAL_ADC_ConfigChannel(&hadcMcu, &cfg) != HAL_OK ||
      HAL_ADC_Start(&hadcMcu) != HAL_OK ||
      HAL_ADC_PollForConversion(&hadcMcu, 10) != HAL_OK)
  {
    return 0;
  }
  uint32_t v = HAL_ADC_GetValue(&hadcMcu);
  (void)HAL_ADC_Stop(&hadcMcu);
  return v;
}
constexpr size_t kAudioSamples = 2048;   /* circular capture buffer */

int16_t audioBuf[kAudioSamples];

} // namespace (reopened below)
} // namespace telemetry

/* Live microphone window for the AI inference path (C linkage) */
extern "C" const int16_t *Telemetry_GetAudioBuffer(uint32_t *count)
{
  *count = telemetry::kAudioSamples;
  return telemetry::audioBuf;
}

namespace telemetry
{
namespace
{
uint8_t audioFrame[1024 + FRAME_OVERHEAD]; /* PCM streaming TX buffer */

/* --- Non-blocking VCP transmit (interrupt driven, single in-flight buffer).
 * Status frames are droppable (next one comes in 20 ms); audio frames spin
 * briefly for the previous transfer instead. --- */
volatile bool uartTxBusy = false;
uint8_t uartTxBuf[1024 + FRAME_OVERHEAD];

bool uartSendAsync(const uint8_t *data, size_t len, uint32_t waitMs)
{
  uint32_t t0 = HAL_GetTick();
  while (uartTxBusy)
  {
    if (HAL_GetTick() - t0 >= waitMs)
    {
      return false;
    }
  }
  if (len > sizeof(uartTxBuf))
  {
    return false;
  }
  memcpy(uartTxBuf, data, len);
  uartTxBusy = true;
  if (HAL_UART_Transmit_IT(&huart1, uartTxBuf, static_cast<uint16_t>(len)) != HAL_OK)
  {
    uartTxBusy = false;
    return false;
  }
  return true;
}

/* --- BLE AT link state (set from AT reply/event callbacks) --- */
volatile bool bleLinkOk = false;
volatile bool bleConnected = false;
uint8_t bleAtBuffer[160]; /* long +BLE_EVT_WRITE events exceed 64 chars */
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
uint32_t tcpSendFails = 0;

/* OTA staging + per-link inbound frame decoders */
ota::Manager otaMgr;
Frame_Decoder uartDecoder = {};
Frame_Decoder tcpDecoder = {};
uint8_t respSeq = 0;

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

#if CFG_WIFI_MODE_SOFTAP
#error "Board SoftAP mode was retired; use the Windows 2.4 GHz hotspot and STA mode"
#endif

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
  /* lwip semantics: accept() honours SO_RCVTIMEO of the listening socket,
   * turning the module's ~10 s blocking accept into a cheap 100 ms poll */
  int32_t acceptTmo = 100;
  (void)MX_WIFI_Socket_setsockopt(obj, tcpListenFd, MX_SOL_SOCKET,
                                  MX_SO_RCVTIMEO, &acceptTmo, sizeof(acceptTmo));
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
  /* Fastest supported output data rates */
  (void)BSP_ENV_SENSOR_SetOutputDataRate(0, ENV_TEMPERATURE, 12.5f);
  (void)BSP_ENV_SENSOR_SetOutputDataRate(0, ENV_HUMIDITY, 12.5f);
  (void)BSP_ENV_SENSOR_SetOutputDataRate(1, ENV_PRESSURE, 75.0f);

  if (BSP_MOTION_SENSOR_Init(0, MOTION_ACCELERO | MOTION_GYRO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(0, MOTION_ACCELERO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(0, MOTION_GYRO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Init(1, MOTION_MAGNETO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(1, MOTION_MAGNETO) != BSP_ERROR_NONE)
  {
    printf("[TLM] motion sensor init failed\r\n");
    sensorsOk_ = false;
  }
  (void)BSP_MOTION_SENSOR_SetOutputDataRate(0, MOTION_ACCELERO, 208.0f);
  (void)BSP_MOTION_SENSOR_SetOutputDataRate(0, MOTION_GYRO, 208.0f);
  (void)BSP_MOTION_SENSOR_SetOutputDataRate(1, MOTION_MAGNETO, 100.0f);

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
  /* Interrupt-driven TX on the VCP link */
  HAL_NVIC_SetPriority(USART1_IRQn, 12, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);

  printf("[TLM] initializing telemetry sources...\r\n");
  initSensors();
  initAudio();
  initMcuInfo();
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

/* MCU identity + reset cause + internal ADC (called once) */
void Service::initMcuInfo()
{
  status_.reset_cause = static_cast<uint8_t>(RCC->CSR >> 24);
  __HAL_RCC_CLEAR_RESET_FLAGS();
  status_.sysclk_hz = HAL_RCC_GetSysClockFreq();
  status_.hclk_hz = HAL_RCC_GetHCLKFreq();
  status_.flash_kb = static_cast<uint16_t>(*reinterpret_cast<const uint16_t *>(FLASHSIZE_BASE));
  status_.uid[0] = HAL_GetUIDw0();
  status_.uid[1] = HAL_GetUIDw1();
  status_.uid[2] = HAL_GetUIDw2();
  status_.idcode = DBGMCU->IDCODE;

  adcOk = false;
  HAL_PWREx_EnableVddA(); /* release the analog supply isolation */
  __HAL_RCC_ADC12_CLK_ENABLE();
  RCC_PeriphCLKInitTypeDef clk = {};
  clk.PeriphClockSelection = RCC_PERIPHCLK_ADCDAC;
  clk.AdcDacClockSelection = RCC_ADCDACCLKSOURCE_HSI;
  if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
  {
    printf("[TLM] ADC kernel clock config failed\r\n");
  }

  hadcMcu.Instance = ADC1;
  hadcMcu.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV4;
  hadcMcu.Init.Resolution = ADC_RESOLUTION_14B;
  hadcMcu.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadcMcu.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadcMcu.Init.ContinuousConvMode = DISABLE;
  hadcMcu.Init.NbrOfConversion = 1;
  hadcMcu.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadcMcu.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  hadcMcu.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadcMcu.Init.OversamplingMode = DISABLE;
  HAL_StatusTypeDef initRet = HAL_ADC_Init(&hadcMcu);
  HAL_StatusTypeDef calRet = HAL_ERROR;
  if (initRet == HAL_OK)
  {
    calRet = HAL_ADCEx_Calibration_Start(&hadcMcu, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED);
  }
  adcOk = (initRet == HAL_OK && calRet == HAL_OK);
  if (!adcOk)
  {
    printf("[TLM] internal ADC init failed (init=%d cal=%d state=0x%lX err=0x%lX)\r\n",
           initRet, calRet, hadcMcu.State, hadcMcu.ErrorCode);
  }
}

/* Die temperature / VDDA / memory statistics / CPU load (every 500 ms) */
void Service::refreshMcuInfo(FullStatus &st)
{
  if (adcOk)
  {
    uint32_t vrefRaw = adcReadChannel(ADC_CHANNEL_VREFINT);
    uint32_t tempRaw = adcReadChannel(ADC_CHANNEL_TEMPSENSOR);
    if (vrefRaw != 0U)
    {
      uint32_t vdda = __HAL_ADC_CALC_VREFANALOG_VOLTAGE(ADC1, vrefRaw, ADC_RESOLUTION_14B);
      st.vdda_mv = static_cast<uint16_t>(vdda);
      int32_t tc = __HAL_ADC_CALC_TEMPERATURE(ADC1, vdda, tempRaw, ADC_RESOLUTION_14B);
      st.die_temp_x100 = static_cast<int16_t>(tc * 100);
    }
  }

  struct mallinfo mi = mallinfo();
  uint32_t staticRam = reinterpret_cast<uint32_t>(&_end) - 0x30000000UL;
  st.heap_used = static_cast<uint32_t>(mi.uordblks);
  st.heap_free = static_cast<uint32_t>(mi.fordblks);
  st.ram_used = staticRam + st.heap_used;
  st.ram_total = 256U * 1024U;
  st.flash_used = (reinterpret_cast<uint32_t>(&_etext) - 0x0C000000UL) +
                  (reinterpret_cast<uint32_t>(&_edata) - reinterpret_cast<uint32_t>(&_sdata));
  st.flash_total = 1024U * 1024U;

  /* CPU load: main-loop iterations in this window vs the best window seen */
  uint32_t now = HAL_GetTick();
  uint32_t win = now - loopWindowStart_;
  if (win >= 500U)
  {
    uint32_t rate = (loopCount_ * 1000U) / win;
    if (rate > loopMax_)
    {
      loopMax_ = rate;
    }
    st.cpu_load_pct = (loopMax_ > 0U)
        ? static_cast<uint8_t>(100U - (100U * rate) / loopMax_) : 0U;
    loopCount_ = 0;
    loopWindowStart_ = now;
  }
}

/* Slow sensors on their own schedules (ODR / integration-time limited) */
void Service::refreshSlowSensors(FullStatus &st)
{
  uint32_t now = HAL_GetTick();

  if (static_cast<int32_t>(now - nextEnvTick_) >= 0)
  {
    nextEnvTick_ += kEnvPeriodMs;
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
  }

  if (static_cast<int32_t>(now - nextLightTick_) >= 0)
  {
    nextLightTick_ += kLightPeriodMs;
    uint32_t light[LIGHT_SENSOR_MAX_CHANNELS] = {};
    if (BSP_LIGHT_SENSOR_GetValues(0, light) == BSP_ERROR_NONE)
    {
      st.light_raw = light[0];
    }
  }

  if (tofOk_ && static_cast<int32_t>(now - nextTofTick_) >= 0)
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
  st.button = (BSP_PB_GetState(BUTTON_USER) == 1) ? 1U : 0U;

  refreshSlowSensors(st);

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
    /* drop the frame when the previous one is still in flight */
    (void)uartSendAsync(frame, len, 0);
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
      /* one transient failure is tolerated; two in a row = client gone */
      tcpSendFails++;
      printf("[TLM] TCP send failed (%ld), fail#%lu\r\n", (long)sent, tcpSendFails);
      if (tcpSendFails >= 2U)
      {
        tcpCloseClient();
      }
    }
    else
    {
      tcpSendFails = 0;
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
    nextAcceptTick = now + 5000U; /* module-side accept blocks ~300 ms */

    /* A pending console key must win over the ~10 s blocking accept,
     * otherwise the interactive commands become unusable */
    if (__HAL_UART_GET_FLAG(&::huart1, UART_FLAG_RXNE))
    {
      return;
    }

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
      int plain = processRxByte(buf[i], true);
      if (plain < 0)
      {
        continue; /* consumed by the frame layer (OTA & co.) */
      }
      char c = static_cast<char>(plain);
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
    if (tcpClientFd >= 0)
    {
      (void)MX_WIFI_Socket_send(wifi_obj_get(), tcpClientFd, frame,
                                static_cast<int32_t>(n), 0);
    }
  }
  else
  {
    (void)uartSendAsync(frame, n, 20);
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
      HAL_Delay(300U);
      printf("[OTA] launching NonSecure application at 0x08100000\r\n");
      HAL_Delay(50U);
      /* Freshly-applied image: don't inherit the previous image's failed
       * boot count (OTA Phase 2 rollback guard, see boot_guard.hpp). */
      BootGuard_ConfirmBoot();
      Secure_JumpToNonSecure();
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
  switch (Frame_DecoderFeed(&dec, byte, &cmd, &seq, &payload, &len))
  {
    case FRAME_FEED_COMPLETE:
      handleFrame(cmd, seq, payload, len, fromTcp);
      return -1;
    case FRAME_FEED_PLAIN:
      return byte;
    default:
      return -1;
  }
}

void Service::poll()
{
  /* One-shot phase profile: accumulated per second, printed once (temporary
   * diagnostic for the frame-rate ceiling) */
  static uint32_t profCollect = 0, profUart = 0, profBle = 0, profTcp = 0;
  static uint32_t profStart = 0, profPrints = 0;
  static uint32_t profFrames = 0, profLoops = 0;

  loopCount_++;
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
    uint32_t t0 = HAL_GetTick();
    collect(status_);
    uint32_t t1 = HAL_GetTick();
    sendUart(status_);
    uint32_t t2 = HAL_GetTick();
    sendTcp(status_);
    profCollect += t1 - t0;
    profUart += t2 - t1;
    profTcp += HAL_GetTick() - t2;
    profFrames++;
  }
  profLoops++;
  if (static_cast<int32_t>(now - nextBleTick_) >= 0)
  {
    nextBleTick_ += kBlePeriodMs;
    uint32_t t0 = HAL_GetTick();
    sendBle(status_);
    profBle += HAL_GetTick() - t0;
  }
  {
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
    printf("[PROF] per2s: collect=%lums uart=%lums ble=%lums tcp=%lums frames=%lu loops=%lu\r\n",
           profCollect, profUart, profBle, profTcp, profFrames, profLoops);
    profCollect = profUart = profBle = profTcp = 0;
    profFrames = profLoops = 0;
    profStart = now;
  }

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
          /* audio must not drop: wait for the in-flight frame (<= 12 ms) */
          (void)uartSendAsync(audioFrame, len, 15);
          if (tcpClientFd >= 0)
          {
            (void)MX_WIFI_Socket_send(wifi_obj_get(), tcpClientFd, audioFrame,
                                      static_cast<int32_t>(len), 0);
          }
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

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    telemetry::uartTxBusy = false;
  }
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
  /* USART1 RX is DMA-driven (console.cpp); Console_GetChar() polls the
   * circular buffer directly, no callback needed. */
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

/* PC -> board data path: GATT write on the P2P write characteristic (fe41) */
extern "C" uint8_t stm32wb_at_BLE_EVT_WRITE_cb(stm32wb_at_BLE_EVT_WRITE_t *param)
{
  printf("[TLM] BLE write: svc=%u char=%u len=%u val0=0x%02X\r\n",
         param->svc_index, param->char_index, param->val_tab_len,
         param->val_tab_len > 0U ? param->val_tab[0] : 0U);
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
  return 0;
}

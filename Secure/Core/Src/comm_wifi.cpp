/**
  ******************************************************************************
  * @file    comm_wifi.cpp
  * @brief   comm_wifi.hpp の EMW3080(SPI2) 向け実装【port層】。
  ******************************************************************************
  */
#include "comm_wifi.hpp"

#include "app_config.h"
#include "main.h"

#include "mx_wifi.h"
#include "io_pattern/mx_wifi_io.h"

#include <cstdio>
#include <cstring>

extern "C" SPI_HandleTypeDef hspi2; /* EMW3080 Wi-Fi module */
extern UART_HandleTypeDef huart1;   /* used only for the accept-guard RXNE check */

namespace comm_wifi
{
namespace
{
/* --- Wi-Fi TCP server state (single client) --- */
bool wifiNetUp = false;      /* joined the AP, has an IP */
int32_t tcpListenFd = -1;
int32_t tcpClientFd = -1;
uint32_t nextAcceptTick = 0;
uint32_t tcpSendFails = 0;

volatile uint8_t wifiLastEvent = 0; /* MWIFI_EVENT_... */

constexpr uint16_t mxHtons(uint16_t v)
{
  return static_cast<uint16_t>((v << 8) | (v >> 8));
}

void wifiStatusCb(uint8_t cate, uint8_t event, void *arg)
{
  (void)arg;
  static const char *const names[] = {"NONE", "STA_DOWN", "STA_UP", "STA_GOT_IP",
                                      "AP_DOWN", "AP_UP"};
  wifiLastEvent = event;
  printf("[TLM] wifi event: cate=%u %s\r\n", cate,
         event <= 5U ? names[event] : "?");
}

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

} // namespace

bool Init()
{
  bool ok = wifiModuleInit();
  tcpServerInit();
  return ok;
}

bool NetUp() { return wifiNetUp; }

bool HasClient() { return tcpClientFd >= 0; }

void SendFrame(const uint8_t *frame, size_t len)
{
  if (tcpClientFd < 0)
  {
    return;
  }
  int32_t sent = MX_WIFI_Socket_send(wifi_obj_get(), tcpClientFd,
                                     const_cast<uint8_t *>(frame),
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

void SendRaw(const uint8_t *data, int32_t len)
{
  if (tcpClientFd < 0)
  {
    return;
  }
  (void)MX_WIFI_Socket_send(wifi_obj_get(), tcpClientFd,
                            const_cast<uint8_t *>(data), len, 0);
}

int32_t PollRecv(uint8_t *buf, size_t maxLen)
{
  if (tcpListenFd < 0)
  {
    return 0;
  }
  MX_WIFIObject_t *obj = wifi_obj_get();
  uint32_t now = HAL_GetTick();

  if (tcpClientFd < 0)
  {
    /* Poll for a pending connection at 1 Hz; log how long accept blocks so
     * a module-side blocking accept is visible in the console */
    if (static_cast<int32_t>(now - nextAcceptTick) < 0)
    {
      return 0;
    }
    nextAcceptTick = now + 5000U; /* module-side accept blocks ~300 ms */

    /* A pending console key must win over the ~10 s blocking accept,
     * otherwise the interactive commands become unusable */
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE))
    {
      return 0;
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
    return 0;
  }

  /* Client connected: poll for inbound commands */
  int32_t n = MX_WIFI_Socket_recv(obj, tcpClientFd, buf, static_cast<int32_t>(maxLen), 0);
  /* Note: recv error codes are unreliable for disconnect detection (the
   * module returns generic errors on a mere receive timeout) - the send
   * path in SendFrame() is the disconnect authority. */
  return (n > 0) ? n : 0;
}

} // namespace comm_wifi

/* Wi-Fiモジュールのイベント割り込み(EXTI)。
 * 【重要】プロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == MXCHIP_FLOW_Pin || GPIO_Pin == MXCHIP_NOTIFY_Pin)
  {
    mxchip_WIFI_ISR(GPIO_Pin);
  }
}

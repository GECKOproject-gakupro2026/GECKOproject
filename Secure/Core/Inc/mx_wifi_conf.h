/**
  ******************************************************************************
  * @file    mx_wifi_conf.h
  * @brief   EMW3080B (MXCHIP) Wi-Fi module configuration for this project:
  *          SPI transport, polling (no DMA), bare metal (no RTOS), and the
  *          module-internal TCP/IP stack (no bypass mode).
  ******************************************************************************
  */
#ifndef MX_WIFI_CONF_H
#define MX_WIFI_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "main.h"

/* ---- Board wiring (B-U585I-IOT02A, same as the ST NetXDuo examples) ------ */
#define MXCHIP_SPI                hspi2

#define MXCHIP_FLOW_Pin           GPIO_PIN_15
#define MXCHIP_FLOW_GPIO_Port     GPIOG
#define MXCHIP_FLOW_EXTI_IRQn     EXTI15_IRQn

#define MXCHIP_NOTIFY_Pin         GPIO_PIN_14
#define MXCHIP_NOTIFY_GPIO_Port   GPIOD
#define MXCHIP_NOTIFY_EXTI_IRQn   EXTI14_IRQn

#define MXCHIP_NSS_Pin            GPIO_PIN_12
#define MXCHIP_NSS_GPIO_Port      GPIOB

#define MXCHIP_RESET_Pin          GPIO_PIN_15
#define MXCHIP_RESET_GPIO_Port    GPIOF

/* ---- Transport / OS selection --------------------------------------------- */
#define MX_WIFI_USE_SPI                             (1)
#define DMA_ON_USE                                  (0)  /* polling SPI        */
#define MX_WIFI_NETWORK_BYPASS_MODE                 (0)  /* module TCP/IP stack */
#define MX_WIFI_TX_BUFFER_NO_COPY                   (0)
#define MX_WIFI_USE_CMSIS_OS                        (0)  /* bare metal          */

/* ---- Wi-Fi network credentials (set by the user, see app_config.h) ------- */
#include "app_config.h"
#define WIFI_SSID                                   CFG_WIFI_SSID
#define WIFI_PASSWORD                               CFG_WIFI_PASSWORD

#define MX_WIFI_PRODUCT_NAME                        ("MXCHIP-WIFI")
#define MX_WIFI_PRODUCT_ID                          ("EMW3080B")

#ifndef MX_WIFI_UART_BAUDRATE
#define MX_WIFI_UART_BAUDRATE                       (230400)
#endif /* MX_WIFI_UART_BAUDRATE */

#define MX_WIFI_MTU_SIZE                            (1500)
#define MX_WIFI_BUFFER_SIZE                         (2500)
#define MX_WIFI_IPC_PAYLOAD_SIZE                    (MX_WIFI_BUFFER_SIZE - 6)
#define MX_WIFI_SOCKET_DATA_SIZE                    (MX_WIFI_IPC_PAYLOAD_SIZE - 12)
#define MX_WIFI_CMD_TIMEOUT                         (10000)
#define MX_WIFI_MAX_SOCKET_NBR                      (8)
#define MX_WIFI_MAX_DETECTED_AP                     (10)

#define MX_WIFI_MAX_SSID_NAME_SIZE                  (32)
#define MX_WIFI_MAX_PSWD_NAME_SIZE                  (64)
#define MX_WIFI_PRODUCT_NAME_SIZE                   (32)
#define MX_WIFI_PRODUCT_ID_SIZE                     (32)
#define MX_WIFI_FW_REV_SIZE                         (24)

/* Thread settings are unused in bare-metal mode but must be defined */
#define MX_WIFI_SPI_THREAD_PRIORITY                 (OSPRIORITYNORMAL)
#define MX_WIFI_SPI_THREAD_STACK_SIZE               (256 * 4)
#define MX_WIFI_UART_THREAD_PRIORITY                (OSPRIORITYNORMAL)
#define MX_WIFI_UART_THREAD_STACK_SIZE              (256 * 4)
#define MX_WIFI_RECEIVED_THREAD_PRIORITY            (OSPRIORITYNORMAL)
#define MX_WIFI_RECEIVED_THREAD_STACK_SIZE          (384 * 4)
#define MX_WIFI_TRANSMIT_THREAD_PRIORITY            (OSPRIORITYNORMAL)
#define MX_WIFI_TRANSMIT_THREAD_STACK_SIZE          (256 * 4)

#define MX_WIFI_MAX_RX_BUFFER_COUNT                 (2)
#define MX_WIFI_MAX_TX_BUFFER_COUNT                 (4)

/* Statistics / debug off */
#define MX_STAT_ON                                  (0)
#define MX_STAT_DECLARE()
#define MX_STAT_INIT()
#define MX_STAT(A)
#define MX_STAT_LOG()

#if (MX_WIFI_USE_CMSIS_OS == 1)
#include "mx_wifi_cmsis_os.h"
#else
#include "mx_wifi_bare_os.h"
#endif /* MX_WIFI_USE_CMSIS_OS */

#ifdef __cplusplus
}
#endif

#endif /* MX_WIFI_CONF_H */

/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   User configuration for the B-U585I-IOT02A firmware.
  ******************************************************************************
  */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---- Audio --------------------------------------------------------------- */
#define CFG_AUDIO_SAMPLE_RATE    16000U
#define CFG_AUDIO_REC_SAMPLES    2048U   /* samples per capture; 128 ms @16 kHz */

/* ---- BLE module ----------------------------------------------------------
 * The custom AT-server firmware of the STM32WB5MMG (ble_module_fw_patch) runs
 * at 460800 baud. This MUST match hlpuart1.Init.BaudRate on the WB5MMG side;
 * a mismatch makes the AT link fail completely (BLE=NG). Divisor error is
 * WB LPUART1 -0.001% / U585 UART4 +0.064%, both well within tolerance.
 * (1 Mbaud was tried and failed on real hardware: link went BLE=NG.) */
#define CFG_BLE_BAUDRATE         460800U
#define CFG_BLE_REPLY_TIMEOUT_MS 1500U

/* ---- Wi-Fi module -------------------------------------------------------- */
#define CFG_WIFI_BOOT_TIMEOUT_MS 5000U

/* The board operates as a station and joins the PC's Windows mobile hotspot.
 * Keep the hotspot on 2.4 GHz: the EMW3080 does not support 5 GHz. */
#define CFG_WIFI_MODE_SOFTAP     0

/* Windows mobile hotspot credentials (see pc_side/wifi_hotspot.ps1). */
#define CFG_WIFI_SSID            "U585-IOT02A"
#define CFG_WIFI_PASSWORD        "u585iot02a"

#define CFG_WIFI_TCP_PORT        5000U

/* ---- Console / telemetry link (ST-LINK VCP over USB) ----------------------
 * The ST-LINK V3E VCP supports high baud rates; 921600 gives ~92 KB/s. */
#define CFG_CONSOLE_BAUDRATE     921600U

/* ---- Telemetry rates ------------------------------------------------------ */
#define CFG_TLM_FULL_PERIOD_MS   20U   /* full status over VCP/TCP: 50 Hz     */
#define CFG_TLM_ENV_PERIOD_MS    100U  /* HTS221/LPS22HH refresh (ODR limit)  */
#define CFG_TLM_LIGHT_PERIOD_MS  200U  /* VEML3235 refresh (integration time) */
#define CFG_TLM_TOF_PERIOD_MS    500U  /* VL53L5CX refresh (~35 ms of I2C per
                                          read - keep it off the fast path)   */
#define CFG_TLM_MCU_PERIOD_MS    500U  /* die temp / memory / CPU load        */
#define CFG_TLM_BLE_PERIOD_MS    1000U /* compact status over BLE: 1 Hz (the
                                          9600-baud AT link blocks ~200 ms per
                                          notification - keep it rare)        */

#endif /* APP_CONFIG_H */

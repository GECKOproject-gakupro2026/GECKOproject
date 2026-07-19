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
 * at CFG_BLE_BAUDRATE. This MUST match hlpuart1.Init.BaudRate on the WB5MMG
 * side; a mismatch makes the AT link fail completely (BLE=NG).
 *
 * Real-hardware findings on this board's WB<->U585 wiring:
 *   - 115200 : rock solid.
 *   - 230400 : this value - raised from 115200 because continuous-streaming
 *              BLE recording (recorder.cpp) generates ADPCM blocks faster
 *              than the AT-command round trip can notify them at 115200
 *              (measured ~15.7 chunks/s at 115200 vs ~26.6/s at 230400, both
 *              below the 33.9/s generation rate at 16 kHz - see recorder.hpp/
 *              CFG_AUDIO_SAMPLE_RATE for the sample-rate side of this
 *              trade-off). Still real-hardware verified stable: BLE=OK and
 *              telemetry notifies flow normally.
 *   - 460800 : does NOT work in practice despite reporting BLE=OK - ALL
 *              notifies (telemetry included, not just recording) silently
 *              stop arriving at the PC. Confirmed on real hardware
 *              (2026-07-19): bleak received zero notifications over several
 *              seconds of normal telemetry + REC_START. Do not use.
 *   - 921600 : does NOT link - every AT returns 0 bytes, BLE=NG, and the
 *              CFG_BLE_INIT_RETRIES bring-up retries below never recover it.
 *   - 1 Mbaud: never linked at all.
 * comm_ble::Init() retries the bring-up up to CFG_BLE_INIT_RETRIES times to
 * absorb a flaky first attempt; if it still fails, drop the baud on BOTH
 * sides and reflash. */
#define CFG_BLE_BAUDRATE         230400U
#define CFG_BLE_INIT_RETRIES     3U     /* AT bring-up attempts before BLE=NG */
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
#define CFG_TLM_BLE_PERIOD_MS    100U  /* compact status over BLE: 10 Hz. At
                                          115200 baud one MiniStatus notify
                                          blocks ~11 ms (was ~130 ms at 9600),
                                          so 10 Hz leaves plenty of headroom
                                          (UART ceiling ~90 Hz).              */

#endif /* APP_CONFIG_H */

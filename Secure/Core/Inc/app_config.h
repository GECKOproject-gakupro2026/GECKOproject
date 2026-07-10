/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   User configuration for the B-U585I-IOT02A hardware test firmware.
  *          Edit this file to enable/disable tests or tune parameters.
  ******************************************************************************
  */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---- Test selection (1 = run, 0 = skip) ---------------------------------- */
#define CFG_TEST_LED             1
#define CFG_TEST_BUTTON          1
#define CFG_TEST_ENV_SENSORS     1  /* HTS221 (temp/hum), LPS22HH (pressure)  */
#define CFG_TEST_MOTION_SENSORS  1  /* ISM330DHCX (acc/gyro), IIS2MDC (mag)   */
#define CFG_TEST_LIGHT_SENSOR    1  /* VEML6030 / VEML3235                    */
#define CFG_TEST_RANGING_SENSOR  1  /* VL53L5CX ToF (FW download takes ~3 s)  */
#define CFG_TEST_EEPROM          1  /* M24256 I2C EEPROM                      */
#define CFG_TEST_OSPI_NOR        1  /* MX25LM51245G 512Mbit NOR (OCTOSPI2)    */
#define CFG_TEST_OSPI_PSRAM      1  /* APS6408 64Mbit PSRAM (OCTOSPI1)        */
#define CFG_TEST_INTERNAL_FLASH  1  /* Non-volatile data page erase/program   */
#define CFG_TEST_SRAM_BUFFER     1  /* Volatile ring-buffer management        */
#define CFG_TEST_MIC1            1  /* MP23DB01HP #1 via ADF1                 */
#define CFG_TEST_MIC2            1  /* MP23DB01HP #2 via MDF1                 */
#define CFG_TEST_BLE_MODULE      1  /* STM32WB5MMG on UART4 (AT probe)        */
#define CFG_TEST_WIFI_MODULE     1  /* EMW3080 aliveness via NOTIFY/FLOW pins */
#define CFG_TEST_TRUSTZONE       1  /* TZEN/SAU/GTZC memory protection checks */

/* ---- Audio --------------------------------------------------------------- */
#define CFG_AUDIO_SAMPLE_RATE    16000U
#define CFG_AUDIO_REC_SAMPLES    4096U   /* samples per capture               */

/* ---- EEPROM -------------------------------------------------------------- */
#define CFG_EEPROM_TEST_ADDR     0x0100U /* byte address used by the R/W test */

/* ---- External NOR flash -------------------------------------------------- */
#define CFG_NOR_TEST_ADDR        0x03FF0000U /* last 64KB block               */

/* ---- Internal flash NV test ----------------------------------------------
 * Secure bank1 page 126 (0x0C0FC000, 8KB) - unused by the application image,
 * below the NSC region (page 127). */
#define CFG_NVTEST_ADDR          0x0C0FC000UL
#define CFG_NVTEST_BANK          FLASH_BANK_1
#define CFG_NVTEST_PAGE          126U

/* ---- BLE module ----------------------------------------------------------
 * The factory AT-server firmware of the STM32WB5MMG runs at 9600 baud. */
#define CFG_BLE_BAUDRATE         9600U
#define CFG_BLE_REPLY_TIMEOUT_MS 1500U

/* ---- Wi-Fi module -------------------------------------------------------- */
#define CFG_WIFI_BOOT_TIMEOUT_MS 5000U

/* Operating mode: 1 = SoftAP (the board emits its own Wi-Fi network and the
 * PC connects to it - no router needed), 0 = STA (join an existing AP) */
#define CFG_WIFI_MODE_SOFTAP     1

/* SoftAP settings (mode 1): connect your PC to this network, then open
 * the status monitor with Wi-Fi (TCP) target 192.168.4.1:5000 */
#define CFG_WIFI_AP_SSID         "U585-IOT02A"
#define CFG_WIFI_AP_PASSWORD     "u585iot02a"  /* WPA2, 8+ chars */
#define CFG_WIFI_AP_CHANNEL      6
#define CFG_WIFI_AP_IP           "192.168.4.1"

/* STA credentials (mode 0): the board joins this access point instead */
#define CFG_WIFI_SSID            "Buffalo-Wifi-2.4G"
#define CFG_WIFI_PASSWORD        "kubokihome"

#define CFG_WIFI_TCP_PORT        5000U

/* ---- Console menu -------------------------------------------------------- */
#define CFG_MENU_ENABLED         1   /* interactive menu after the auto run   */

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

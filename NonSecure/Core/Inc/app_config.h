/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   NonSecure application-layer tunables (TrustZone refactor Phase E:
  *          low-power idle behavior).
  ******************************************************************************
  */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Low-power / idle state machine ------------------------------------------- */
/* No host command for this long -> enter IDLE (test value; production would
 * be much longer). Any inbound host activity returns to ACTIVE immediately. */
#define CFG_IDLE_TIMEOUT_MS      3000U

/* IDLE indicator: RED LED (PH6) slow blink half-period. */
#define CFG_IDLE_LED_BLINK_MS    1000U

/* ACTIVE indicator: GREEN LED (PH7) heartbeat toggle period, and the
 * telemetry send cadence. */
#define CFG_ACTIVE_HB_MS          250U
#define CFG_ACTIVE_TELEMETRY_MS    20U   /* 50 Hz */

/* IDLE telemetry cadence. IDLE no longer silences the comm stack (see
 * app_loop.c) - only the slower rate distinguishes it from ACTIVE. */
#define CFG_IDLE_TELEMETRY_MS     200U   /* 5 Hz */

#endif /* APP_CONFIG_H */

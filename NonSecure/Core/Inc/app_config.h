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

/* IDLE beacon cadence. IDLE no longer acquires or sends sensor data at all
 * (state-machine rebuild) - it only pings this low-rate "alive, but idle"
 * beacon (FRAME_CMD_IDLE_BEACON, no sensor payload) so the PC can tell the
 * board apart from a dead link. Light/audio are still sampled for trigger
 * evaluation (see app_state.c's IDLE block), just not sent. */
#define CFG_IDLE_BEACON_MS        1000U   /* 1 Hz */

/* COMM cadence for the ACTIVE COMM->ACQUIRE->WAIT cycle (app_state.c). The
 * WAIT is computed from the *previous* COMM return time so frames arrive at
 * the PC at a steady cadence regardless of how long sensor acquisition took.
 * Kept equal to the ACTIVE telemetry rate; separated so it can be tuned
 * independently of the LED heartbeat. */
#define CFG_COMM_PERIOD_MS        CFG_ACTIVE_TELEMETRY_MS

/* Trigger thresholds (see triggers.h). 0 = disabled (default): the sensor's
 * baseline reading varies a lot by board placement/lighting/ambient noise,
 * so ship with these off and let the integrator tune them for the target
 * environment. st->light_raw / st->audio_rms are the fields compared
 * against these. */
#define CFG_TRIGGER_LIGHT_THRESHOLD   0U   /* raw light units, 0=disabled */
#define CFG_TRIGGER_AUDIO_THRESHOLD   0     /* RMS units, 0=disabled */

#endif /* APP_CONFIG_H */

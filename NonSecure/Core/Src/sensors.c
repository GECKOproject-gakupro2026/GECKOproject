/**
  ******************************************************************************
  * @file    sensors.c
  * @brief   TrustZone app-layer refactor Phase C: env/motion/light/ToF
  *          sensors (I2C1/I2C2, released NonSecure by Secure's GTZC flip in
  *          main.c) now live entirely in NonSecure. The BSP sensor drivers
  *          are self-contained - they call BSP_I2C1_Init()/BSP_I2C2_Init()
  *          internally (see b_u585i_iot02a_bus.c IOCtx.Init), so no manual
  *          HAL_I2C_Init() is needed here, unlike Secure's old main.c.
  ******************************************************************************
  */
#include "main.h"
#include "sensors.h"

#include "b_u585i_iot02a_env_sensors.h"
#include "b_u585i_iot02a_light_sensor.h"
#include "b_u585i_iot02a_motion_sensors.h"
#include "b_u585i_iot02a_ranging_sensor.h"

#include <stdio.h>

static uint8_t s_tofOk = 0U;
static uint32_t s_nextEnvTick, s_nextLightTick, s_nextTofTick;

/* env/light periods match the old Secure telemetry cadence; ToF stays at
 * its slower rate since the sensor's own timing budget limits it anyway. */
#define SENSORS_ENV_PERIOD_MS    100U
#define SENSORS_LIGHT_PERIOD_MS  200U
#define SENSORS_TOF_PERIOD_MS    500U

/* Common VL53L5CX bring-up: BSP init + profile config + start ranging.
 * Used both at boot and after Sensors_Resume() re-powers the sensor via LPn. */
static void tofInitAndStart(void)
{
  s_tofOk = 0U;
  if (BSP_RANGING_SENSOR_Init(0) == BSP_ERROR_NONE)
  {
    RANGING_SENSOR_ProfileConfig_t profile = {0};
    profile.RangingProfile = RS_PROFILE_4x4_CONTINUOUS;
    profile.TimingBudget = 30;
    profile.Frequency = 5;
    profile.EnableAmbient = 0;
    profile.EnableSignal = 0;
    if (BSP_RANGING_SENSOR_ConfigProfile(0, &profile) == BSP_ERROR_NONE &&
        BSP_RANGING_SENSOR_Start(0, RS_MODE_ASYNC_CONTINUOUS) == BSP_ERROR_NONE)
    {
      s_tofOk = 1U;
    }
  }
}

void Sensors_Init(void)
{
  tofInitAndStart();
  if (!s_tofOk)
  {
    printf("[SENS] ToF init failed\r\n");
  }

  if (BSP_ENV_SENSOR_Init(0, ENV_TEMPERATURE | ENV_HUMIDITY) != BSP_ERROR_NONE ||
      BSP_ENV_SENSOR_Enable(0, ENV_TEMPERATURE) != BSP_ERROR_NONE ||
      BSP_ENV_SENSOR_Enable(0, ENV_HUMIDITY) != BSP_ERROR_NONE ||
      BSP_ENV_SENSOR_Init(1, ENV_PRESSURE) != BSP_ERROR_NONE ||
      BSP_ENV_SENSOR_Enable(1, ENV_PRESSURE) != BSP_ERROR_NONE)
  {
    printf("[SENS] env sensor init failed\r\n");
  }
  (void)BSP_ENV_SENSOR_SetOutputDataRate(0, ENV_TEMPERATURE, 12.5f);
  (void)BSP_ENV_SENSOR_SetOutputDataRate(0, ENV_HUMIDITY, 12.5f);
  (void)BSP_ENV_SENSOR_SetOutputDataRate(1, ENV_PRESSURE, 75.0f);

  if (BSP_MOTION_SENSOR_Init(0, MOTION_ACCELERO | MOTION_GYRO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(0, MOTION_ACCELERO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(0, MOTION_GYRO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Init(1, MOTION_MAGNETO) != BSP_ERROR_NONE ||
      BSP_MOTION_SENSOR_Enable(1, MOTION_MAGNETO) != BSP_ERROR_NONE)
  {
    printf("[SENS] motion sensor init failed\r\n");
  }
  (void)BSP_MOTION_SENSOR_SetOutputDataRate(0, MOTION_ACCELERO, 208.0f);
  (void)BSP_MOTION_SENSOR_SetOutputDataRate(0, MOTION_GYRO, 208.0f);
  (void)BSP_MOTION_SENSOR_SetOutputDataRate(1, MOTION_MAGNETO, 100.0f);

  if (BSP_LIGHT_SENSOR_Init(0) != BSP_ERROR_NONE ||
      BSP_LIGHT_SENSOR_Start(0, LIGHT_SENSOR_MODE_CONTINUOUS) != BSP_ERROR_NONE)
  {
    printf("[SENS] light sensor init failed\r\n");
  }

  uint32_t now = HAL_GetTick();
  s_nextEnvTick = now;
  s_nextLightTick = now;
  s_nextTofTick = now;
}

/* Low-power mode: park the VL53L5CX in its own SLEEP power mode rather than
 * cutting power via the LPn pin. Stop() alone only halts the ranging loop and
 * leaves the module (and its emitter) powered, so it isn't enough; but the two
 * hardware routes out of that are both dead ends here:
 *   - LPn low = hardware shutdown. Coming back needs a full re-init, and
 *     VL53L5CX_Init() refuses to run while the driver's IsInitialized flag is
 *     set, so the re-init silently fails and ToF stays dead.
 *   - DeInit first to clear that flag. But BSP_RANGING_SENSOR_DeInit ->
 *     BSP_I2C2_DeInit -> HAL_GPIO_DeInit(PH4/PH5) + I2C2 clock disable tears
 *     down bus state the rest of the system is still standing on: the board
 *     went silent on UART (Secure comm stack dead) and never woke from IDLE.
 * SetPowerMode(SLEEP/WAKEUP) is pure I2C register traffic - it stops the
 * measurement and drops the sensor's draw without touching GPIO, clocks, or
 * driver state, so resume is just WAKEUP + Start(). */
void Sensors_Stop(void)
{
  if (!s_tofOk)
  {
    return;
  }
  (void)BSP_RANGING_SENSOR_Stop(0);
  (void)BSP_RANGING_SENSOR_SetPowerMode(0, RANGING_SENSOR_POWERMODE_SLEEP);
  s_tofOk = 0U; /* Refresh() reports tof_ok=0 while asleep */
}

void Sensors_Resume(void)
{
  if (BSP_RANGING_SENSOR_SetPowerMode(0, RANGING_SENSOR_POWERMODE_WAKEUP) != BSP_ERROR_NONE ||
      BSP_RANGING_SENSOR_Start(0, RS_MODE_ASYNC_CONTINUOUS) != BSP_ERROR_NONE)
  {
    printf("[SENS] ToF resume failed\r\n");
    return;
  }
  s_tofOk = 1U;
  s_nextTofTick = HAL_GetTick();
}

/* 照度だけの取得(自身の周期 s_nextLightTick で更新)。Sensors_Refresh() と
 * Sensors_RefreshLightOnly() の両方から呼ばれる共通実体。 */
static void refreshLight(FullStatus_t *st, uint32_t now)
{
  if ((int32_t)(now - s_nextLightTick) >= 0)
  {
    s_nextLightTick = now + SENSORS_LIGHT_PERIOD_MS;
    uint32_t light[LIGHT_SENSOR_MAX_CHANNELS] = {0};
    if (BSP_LIGHT_SENSOR_GetValues(0, light) == BSP_ERROR_NONE)
    {
      st->light_raw = light[0];
    }
  }
}

void Sensors_RefreshLightOnly(FullStatus_t *st)
{
  refreshLight(st, HAL_GetTick());
}

void Sensors_Refresh(FullStatus_t *st)
{
  uint32_t now = HAL_GetTick();

  /* Note: all three schedules re-anchor on `now` rather than accumulating
   * (`+= PERIOD`). Refresh doesn't run at all during IDLE, so an accumulating
   * deadline falls arbitrarily far behind and then fires every loop until it
   * catches up - hammering the sensors right after every wake. */

  if ((int32_t)(now - s_nextEnvTick) >= 0)
  {
    s_nextEnvTick = now + SENSORS_ENV_PERIOD_MS;
    float f = 0.0f;
    if (BSP_ENV_SENSOR_GetValue(0, ENV_TEMPERATURE, &f) == BSP_ERROR_NONE)
    {
      st->temp_x100 = (int16_t)(f * 100.0f);
    }
    if (BSP_ENV_SENSOR_GetValue(0, ENV_HUMIDITY, &f) == BSP_ERROR_NONE)
    {
      st->hum_x100 = (uint16_t)(f * 100.0f);
    }
    if (BSP_ENV_SENSOR_GetValue(1, ENV_PRESSURE, &f) == BSP_ERROR_NONE)
    {
      st->press_x100 = (uint32_t)(f * 100.0f);
    }
  }

  refreshLight(st, now);

  if (!s_tofOk)
  {
    /* ToF down (shut down for IDLE, or re-init failed): say so instead of
     * leaving the caller's static FullStatus holding the last good reading,
     * which made a dead sensor look alive with a frozen distance. */
    st->tof_ok = 0U;
    st->tof_mm = 0U;
  }
  else if ((int32_t)(now - s_nextTofTick) >= 0)
  {
    s_nextTofTick = now + SENSORS_TOF_PERIOD_MS;
    static RANGING_SENSOR_Result_t result;
    if (BSP_RANGING_SENSOR_GetDistance(0, &result) == BSP_ERROR_NONE)
    {
      st->tof_mm = (uint16_t)result.ZoneResult[0].Distance[0];
      st->tof_ok = 1U;
    }
    else
    {
      st->tof_ok = 0U;
    }
  }

  BSP_MOTION_SENSOR_Axes_t axes = {0};
  if (BSP_MOTION_SENSOR_GetAxes(0, MOTION_ACCELERO, &axes) == BSP_ERROR_NONE)
  {
    st->acc_mg[0] = (int16_t)axes.xval;
    st->acc_mg[1] = (int16_t)axes.yval;
    st->acc_mg[2] = (int16_t)axes.zval;
  }
  if (BSP_MOTION_SENSOR_GetAxes(0, MOTION_GYRO, &axes) == BSP_ERROR_NONE)
  {
    st->gyro_dps10[0] = (int16_t)(axes.xval / 100); /* mdps -> dps*10 */
    st->gyro_dps10[1] = (int16_t)(axes.yval / 100);
    st->gyro_dps10[2] = (int16_t)(axes.zval / 100);
  }
  if (BSP_MOTION_SENSOR_GetAxes(1, MOTION_MAGNETO, &axes) == BSP_ERROR_NONE)
  {
    st->mag_mgauss[0] = (int16_t)axes.xval;
    st->mag_mgauss[1] = (int16_t)axes.yval;
    st->mag_mgauss[2] = (int16_t)axes.zval;
  }
}

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

void Sensors_Stop(void)
{
  /* Low-power mode (Phase E/F): BSP_RANGING_SENSOR_Stop() only issues an I2C
   * stop-ranging command - the VL53L5CX module itself (and its activity LED)
   * stays powered. Drive LPn (PH1) low to put the sensor in hardware
   * shutdown, which actually cuts its power draw and the LED. */
  if (s_tofOk)
  {
    (void)BSP_RANGING_SENSOR_Stop(0);
  }
  HAL_GPIO_WritePin(VL53L5A1_LP_PORT, VL53L5A1_LP_PIN, GPIO_PIN_RESET);
}

void Sensors_Resume(void)
{
  /* LPn low->high is a hardware reset (XSHUT release): the sensor reboots
   * and needs a full re-init, not just Start(). Re-run BSP_RANGING_SENSOR_Init
   * + the same profile/start sequence as Sensors_Init(). */
  HAL_GPIO_WritePin(VL53L5A1_LP_PORT, VL53L5A1_LP_PIN, GPIO_PIN_SET);
  HAL_Delay(2); /* VL53L5CX boot time after LPn release (datasheet: <=1.2 ms) */

  tofInitAndStart();
  if (!s_tofOk)
  {
    printf("[SENS] ToF re-init after resume failed\r\n");
  }
}

void Sensors_Refresh(FullStatus_t *st)
{
  uint32_t now = HAL_GetTick();

  if ((int32_t)(now - s_nextEnvTick) >= 0)
  {
    s_nextEnvTick += SENSORS_ENV_PERIOD_MS;
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

  if ((int32_t)(now - s_nextLightTick) >= 0)
  {
    s_nextLightTick += SENSORS_LIGHT_PERIOD_MS;
    uint32_t light[LIGHT_SENSOR_MAX_CHANNELS] = {0};
    if (BSP_LIGHT_SENSOR_GetValues(0, light) == BSP_ERROR_NONE)
    {
      st->light_raw = light[0];
    }
  }

  if (s_tofOk && (int32_t)(now - s_nextTofTick) >= 0)
  {
    s_nextTofTick += SENSORS_TOF_PERIOD_MS;
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

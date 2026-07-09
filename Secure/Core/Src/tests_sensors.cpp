/**
  ******************************************************************************
  * @file    tests_sensors.cpp
  * @brief   I2C sensor tests: environment, motion, light, ranging, EEPROM.
  ******************************************************************************
  */
#include "test.hpp"
#include "app_config.h"
#include "b_u585i_iot02a_env_sensors.h"
#include "b_u585i_iot02a_motion_sensors.h"
#include "b_u585i_iot02a_light_sensor.h"
#include "b_u585i_iot02a_ranging_sensor.h"
#include "b_u585i_iot02a_eeprom.h"

#include <cstdio>
#include <cstring>

namespace apptest
{

Result testEnvSensors()
{
  bool ok = true;

  /* Instance 0 = HTS221 (temperature + humidity) */
  if (BSP_ENV_SENSOR_Init(0, ENV_TEMPERATURE | ENV_HUMIDITY) == BSP_ERROR_NONE &&
      BSP_ENV_SENSOR_Enable(0, ENV_TEMPERATURE) == BSP_ERROR_NONE &&
      BSP_ENV_SENSOR_Enable(0, ENV_HUMIDITY) == BSP_ERROR_NONE)
  {
    HAL_Delay(100); /* first conversion */
    float temp = 0.0f;
    float hum = 0.0f;
    if (BSP_ENV_SENSOR_GetValue(0, ENV_TEMPERATURE, &temp) == BSP_ERROR_NONE &&
        BSP_ENV_SENSOR_GetValue(0, ENV_HUMIDITY, &hum) == BSP_ERROR_NONE &&
        temp > -40.0f && temp < 85.0f && hum >= 0.0f && hum <= 100.0f)
    {
      printf("  HTS221: %.1f degC, %.1f %%RH\r\n", static_cast<double>(temp), static_cast<double>(hum));
    }
    else
    {
      printf("  HTS221 read failed or out of range\r\n");
      ok = false;
    }
  }
  else
  {
    printf("  HTS221 init failed\r\n");
    ok = false;
  }

  /* Instance 1 = LPS22HH (pressure + temperature) */
  if (BSP_ENV_SENSOR_Init(1, ENV_PRESSURE) == BSP_ERROR_NONE &&
      BSP_ENV_SENSOR_Enable(1, ENV_PRESSURE) == BSP_ERROR_NONE)
  {
    HAL_Delay(100);
    float press = 0.0f;
    if (BSP_ENV_SENSOR_GetValue(1, ENV_PRESSURE, &press) == BSP_ERROR_NONE &&
        press > 300.0f && press < 1200.0f)
    {
      printf("  LPS22HH: %.1f hPa\r\n", static_cast<double>(press));
    }
    else
    {
      printf("  LPS22HH read failed or out of range (%.1f)\r\n", static_cast<double>(press));
      ok = false;
    }
  }
  else
  {
    printf("  LPS22HH init failed\r\n");
    ok = false;
  }

  return ok ? Result::Pass : Result::Fail;
}

Result testMotionSensors()
{
  bool ok = true;

  /* Instance 0 = ISM330DHCX (accelerometer + gyroscope) */
  if (BSP_MOTION_SENSOR_Init(0, MOTION_ACCELERO | MOTION_GYRO) == BSP_ERROR_NONE &&
      BSP_MOTION_SENSOR_Enable(0, MOTION_ACCELERO) == BSP_ERROR_NONE &&
      BSP_MOTION_SENSOR_Enable(0, MOTION_GYRO) == BSP_ERROR_NONE)
  {
    HAL_Delay(50);
    BSP_MOTION_SENSOR_Axes_t acc = {};
    BSP_MOTION_SENSOR_Axes_t gyro = {};
    if (BSP_MOTION_SENSOR_GetAxes(0, MOTION_ACCELERO, &acc) == BSP_ERROR_NONE &&
        BSP_MOTION_SENSOR_GetAxes(0, MOTION_GYRO, &gyro) == BSP_ERROR_NONE)
    {
      printf("  ISM330DHCX acc[mg]=(%ld,%ld,%ld) gyro[mdps]=(%ld,%ld,%ld)\r\n",
             acc.xval, acc.yval, acc.zval, gyro.xval, gyro.yval, gyro.zval);
      /* On a table, |acc| should be roughly 1 g */
      int32_t magSq = acc.xval * acc.xval + acc.yval * acc.yval + acc.zval * acc.zval;
      if (magSq < 500 * 500 || magSq > 1500 * 1500)
      {
        printf("  accelerometer magnitude implausible\r\n");
        ok = false;
      }
    }
    else
    {
      printf("  ISM330DHCX read failed\r\n");
      ok = false;
    }
  }
  else
  {
    printf("  ISM330DHCX init failed\r\n");
    ok = false;
  }

  /* Instance 1 = IIS2MDC (magnetometer) */
  if (BSP_MOTION_SENSOR_Init(1, MOTION_MAGNETO) == BSP_ERROR_NONE &&
      BSP_MOTION_SENSOR_Enable(1, MOTION_MAGNETO) == BSP_ERROR_NONE)
  {
    HAL_Delay(50);
    BSP_MOTION_SENSOR_Axes_t mag = {};
    if (BSP_MOTION_SENSOR_GetAxes(1, MOTION_MAGNETO, &mag) == BSP_ERROR_NONE &&
        !(mag.xval == 0 && mag.yval == 0 && mag.zval == 0))
    {
      printf("  IIS2MDC mag[mGauss]=(%ld,%ld,%ld)\r\n", mag.xval, mag.yval, mag.zval);
    }
    else
    {
      printf("  IIS2MDC read failed\r\n");
      ok = false;
    }
  }
  else
  {
    printf("  IIS2MDC init failed\r\n");
    ok = false;
  }

  return ok ? Result::Pass : Result::Fail;
}

Result testLightSensor()
{
  uint32_t id = 0;
  if (BSP_LIGHT_SENSOR_Init(0) != BSP_ERROR_NONE)
  {
    printf("  light sensor init failed\r\n");
    return Result::Fail;
  }
  if (BSP_LIGHT_SENSOR_ReadID(0, &id) != BSP_ERROR_NONE)
  {
    printf("  light sensor ID read failed\r\n");
    return Result::Fail;
  }
  if (BSP_LIGHT_SENSOR_Start(0, LIGHT_SENSOR_MODE_CONTINUOUS) != BSP_ERROR_NONE)
  {
    printf("  light sensor start failed\r\n");
    return Result::Fail;
  }
  HAL_Delay(200); /* integration time */
  uint32_t values[LIGHT_SENSOR_MAX_CHANNELS] = {};
  int32_t ret = BSP_LIGHT_SENSOR_GetValues(0, values);
  BSP_LIGHT_SENSOR_Stop(0);
  if (ret != BSP_ERROR_NONE)
  {
    printf("  light sensor read failed\r\n");
    return Result::Fail;
  }
  printf("  VEML id=0x%02lX raw=%lu\r\n", id, values[0]);
  return Result::Pass;
}

Result testRangingSensor()
{
  printf("  initializing VL53L5CX (firmware download, ~3 s)...\r\n");
  if (BSP_RANGING_SENSOR_Init(0) != BSP_ERROR_NONE)
  {
    printf("  VL53L5CX init failed\r\n");
    return Result::Fail;
  }

  RANGING_SENSOR_ProfileConfig_t profile = {};
  profile.RangingProfile = RS_PROFILE_4x4_CONTINUOUS;
  profile.TimingBudget = 30;
  profile.Frequency = 10;
  profile.EnableAmbient = 1;
  profile.EnableSignal = 1;
  if (BSP_RANGING_SENSOR_ConfigProfile(0, &profile) != BSP_ERROR_NONE ||
      BSP_RANGING_SENSOR_Start(0, RS_MODE_ASYNC_CONTINUOUS) != BSP_ERROR_NONE)
  {
    printf("  VL53L5CX profile/start failed\r\n");
    return Result::Fail;
  }

  HAL_Delay(300);
  static RANGING_SENSOR_Result_t result;
  int32_t ret = BSP_RANGING_SENSOR_GetDistance(0, &result);
  BSP_RANGING_SENSOR_Stop(0);
  if (ret != BSP_ERROR_NONE)
  {
    printf("  VL53L5CX distance read failed\r\n");
    return Result::Fail;
  }
  printf("  VL53L5CX zone0 distance=%lu mm (zones=%lu)\r\n",
         result.ZoneResult[0].Distance[0], result.NumberOfZones);
  return Result::Pass;
}

Result testEeprom()
{
  if (BSP_EEPROM_Init(0) != BSP_ERROR_NONE)
  {
    printf("  EEPROM init failed\r\n");
    return Result::Fail;
  }
  if (BSP_EEPROM_IsDeviceReady(0) != BSP_ERROR_NONE)
  {
    printf("  EEPROM not ready\r\n");
    return Result::Fail;
  }

  uint8_t writeBuf[16];
  uint8_t readBuf[16] = {};
  uint8_t backup[16] = {};
  for (size_t i = 0; i < sizeof(writeBuf); i++)
  {
    writeBuf[i] = static_cast<uint8_t>(0xA5U ^ (i * 7U) ^ (HAL_GetTick() & 0xFFU));
  }

  /* Preserve previous content, write pattern, verify, restore */
  if (BSP_EEPROM_ReadBuffer(0, backup, CFG_EEPROM_TEST_ADDR, sizeof(backup)) != BSP_ERROR_NONE ||
      BSP_EEPROM_WriteBuffer(0, writeBuf, CFG_EEPROM_TEST_ADDR, sizeof(writeBuf)) != BSP_ERROR_NONE ||
      BSP_EEPROM_ReadBuffer(0, readBuf, CFG_EEPROM_TEST_ADDR, sizeof(readBuf)) != BSP_ERROR_NONE)
  {
    printf("  EEPROM R/W transaction failed\r\n");
    return Result::Fail;
  }
  bool match = (memcmp(writeBuf, readBuf, sizeof(writeBuf)) == 0);
  BSP_EEPROM_WriteBuffer(0, backup, CFG_EEPROM_TEST_ADDR, sizeof(backup));
  if (!match)
  {
    printf("  EEPROM verify mismatch\r\n");
    return Result::Fail;
  }
  printf("  M24256 write/read/restore at 0x%04X OK\r\n", CFG_EEPROM_TEST_ADDR);
  return Result::Pass;
}

} // namespace apptest

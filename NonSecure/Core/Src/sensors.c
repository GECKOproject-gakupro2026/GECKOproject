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
static uint32_t s_nextEnvTick, s_nextLightTick, s_nextTofTick, s_nextMotionTick;

/* 実行時にコマンド(FRAME_CMD_SET_SENSOR_RATE)で変更できる取得周期。
 * 既定値はすべて10Hz以上(要求「全センサー取得周期の最低値を10Hz」)。
 * motionは0=「周期なし、毎回読む」(ODR 208/208/100Hzで送信レート依存、
 * 既に10Hz超のため専用の間引きは不要)。 */
static uint16_t s_envPeriodMs    = 100U; /* 10Hz。HTS221のODR上限12.5Hzが
                                             実質の物理下限(1/7/12.5Hzの3段階
                                             しかない)ので、これが最善 */
static uint16_t s_lightPeriodMs  = 100U; /* 10Hz。要 IT100(積分時間) */
static uint16_t s_tofPeriodMs    = 100U; /* 10Hz。profile.Frequency=10(Step4で変更) */
static uint16_t s_motionPeriodMs = 0U;   /* 0=毎回(周期なし) */

/* Sensors_SetPeriod() の下限クランプ値。ハード制約を割るとI2Cがメインループを
 * 圧迫し、Phase Aで解消した停止クラスの問題を再発させかねない
 * (実装計画_統合.md §3)。 */
#define SENSORS_ENV_PERIOD_MIN_MS    80U   /* HTS221 ODR 12.5Hz = 80ms */
#define SENSORS_LIGHT_PERIOD_MIN_MS  50U   /* VEML3235 IT50 = 50ms */
#define SENSORS_TOF_PERIOD_MIN_MS    50U   /* I2C 約35ms/read */
#define SENSORS_PERIOD_MAX_MS        60000U

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
    profile.Frequency = 10;
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

  if (BSP_LIGHT_SENSOR_Init(0) != BSP_ERROR_NONE)
  {
    printf("[SENS] light sensor init failed\r\n");
  }
  /* VEML3235_Init() 自身が内部で VEML3235_Pwr_On() を呼びSHUTDOWNビットを
   * クリアするため、Init() が返った時点で既に「キャプチャ中」になっている
   * (Startを呼ぶ前でもドライバのIsStarted/IsContinuousフラグはInit内で
   * 立つ)。SetExposureTime()はキャプチャ中に呼んではいけない制約があるので、
   * 一度明示的にStopしてSHUTDOWNビットを立ててから設定変更し、Startで
   * 測定を再開する。逆順(Init直後にSetExposureTime)だと反映されない
   * (実機で確認済み: light_rawが既定の200ms周期のまま変わらなかった)。
   * 【重要】この引数はミリ秒ではなく VEML3235 の ALS_CONF レジスタの生の
   * ビット値そのもの(veml3235_reg.h の VEML3235_CONF_IT* 一式、ドライバが
   * config |= (uint16_t)ExposureTime とマスクなしで書き込むため)。
   * VEML3235_CONF_IT100 = (0x00UL << 6) = 0 なので下の "0U" がIT100を意味する
   * (単純に「100」を渡すとレジスタを壊す事故になるので要注意)。
   * 【実機で確認した既知の制約】IT100は正しく反映される(レジスタ読み戻しで
   * 確認済み)が、それでも実測の値更新は約200ms(5Hz)止まりで、s_lightPeriodMs
   * を100msにしても10Hzには届かない。VEML3235自体の内部測定サイクルが
   * IT設定と別に律速していると見られ、これ以上はデータシート精査が要る
   * ハード側の制約として実装計画_統合.md §3に記録済み。
   * 暗所での感度は落ちるトレードオフもある(実機で妥当性を確認すること)。 */
  (void)BSP_LIGHT_SENSOR_Stop(0);
  (void)BSP_LIGHT_SENSOR_SetExposureTime(0, 0U); /* VEML3235_CONF_IT100 */
  if (BSP_LIGHT_SENSOR_Start(0, LIGHT_SENSOR_MODE_CONTINUOUS) != BSP_ERROR_NONE)
  {
    printf("[SENS] light sensor start failed\r\n");
  }

  uint32_t now = HAL_GetTick();
  s_nextEnvTick = now;
  s_nextLightTick = now;
  s_nextTofTick = now;
  s_nextMotionTick = now;
}

/* コマンド(FRAME_CMD_SET_SENSOR_RATE)からの実行時周期変更。ハード制約を下限に
 * クランプする(§3参照)。sensor_id: 0=env, 1=light, 2=tof, 3=motion。 */
void Sensors_SetPeriod(uint8_t sensor_id, uint16_t period_ms)
{
  uint16_t clamped = period_ms;
  const char *name = "?";
  uint16_t minMs = 0U;

  switch (sensor_id)
  {
    case 0U: name = "env";    minMs = SENSORS_ENV_PERIOD_MIN_MS;   break;
    case 1U: name = "light";  minMs = SENSORS_LIGHT_PERIOD_MIN_MS; break;
    case 2U: name = "tof";    minMs = SENSORS_TOF_PERIOD_MIN_MS;   break;
    case 3U: name = "motion"; minMs = 0U;                          break;
    default:
      printf("[SENS] SetPeriod: unknown sensor_id=%u\r\n", sensor_id);
      return;
  }

  if (clamped > 0U && clamped < minMs)
  {
    clamped = minMs;
  }
  if (clamped > SENSORS_PERIOD_MAX_MS)
  {
    clamped = SENSORS_PERIOD_MAX_MS;
  }
  if (clamped != period_ms)
  {
    printf("[SENS] period clamped: %s requested=%u used=%u\r\n",
           name, period_ms, clamped);
  }

  switch (sensor_id)
  {
    case 0U: s_envPeriodMs = clamped;    break;
    case 1U: s_lightPeriodMs = clamped;  break;
    case 2U: s_tofPeriodMs = clamped;    break;
    case 3U: s_motionPeriodMs = clamped; break;
    default: break;
  }
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
    s_nextLightTick = now + s_lightPeriodMs;
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
    s_nextEnvTick = now + s_envPeriodMs;
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
    s_nextTofTick = now + s_tofPeriodMs;
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

  /* motion: 既定は s_motionPeriodMs=0(周期なし=毎回読む)。ODR 208/208/100Hzで
   * 送信レート(最大50Hz)に対し既に余裕があるため、通常はここが常に真になる。
   * コマンドで明示的に間引く(period_ms>0)ことも可能にしておく。 */
  if (s_motionPeriodMs == 0U || (int32_t)(now - s_nextMotionTick) >= 0)
  {
    if (s_motionPeriodMs != 0U)
    {
      s_nextMotionTick = now + s_motionPeriodMs;
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
}

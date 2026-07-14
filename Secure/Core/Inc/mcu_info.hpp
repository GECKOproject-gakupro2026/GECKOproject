/**
  ******************************************************************************
  * @file    mcu_info.hpp
  * @brief   MCU内蔵の自己診断情報（ダイ温度・電源電圧・メモリ使用量）。
  *
  *          【チップ依存・基板非依存】同じSTM32U5系なら基板が変わっても
  *          そのまま使える。別系統のMCUに移るときはここを書き直す。
  ******************************************************************************
  */
#ifndef MCU_INFO_HPP
#define MCU_INFO_HPP

#include "telemetry.hpp"   /* telemetry::FullStatus */

namespace mcu_info
{

/* 起動時に1回だけ呼ぶ。リセット要因・クロック・UID・IDCODEを st に埋め、
 * 内蔵ADC（VREFINT / 温度センサー）を較正して使える状態にする。 */
void Init(telemetry::FullStatus &st);

/* 定期的に呼ぶ。ダイ温度・VDDA・heap/RAM/Flash使用量を st に埋める。
 * CPU負荷(cpu_load_pct)はここでは埋めない（呼び出し側が計算する）。 */
void Refresh(telemetry::FullStatus &st);

} // namespace mcu_info

#endif /* MCU_INFO_HPP */

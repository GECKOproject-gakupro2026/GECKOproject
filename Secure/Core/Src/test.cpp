/**
  ******************************************************************************
  * @file    test.cpp
  * @brief   Test runner: executes every enabled hardware test, prints a
  *          summary and offers an interactive menu on the VCP console.
  ******************************************************************************
  */
#include "test.hpp"
#include "app_config.h"
#include "console.h"
#include "main.h"
#include "b_u585i_iot02a.h"

#include <cstdio>

/* Run indicator: SysTick (stm32u5xx_it.c) alternates LD6/LD7 while set */
extern "C" volatile uint32_t g_LedBlinkEnable = 0;

namespace apptest
{

namespace
{
const Case kCases[] = {
    {"LED (LD6/LD7)", testLed, CFG_TEST_LED == 1},
    {"User button", testButton, CFG_TEST_BUTTON == 1},
    /* The VL53L5CX I2C bus recovery must run before any other I2C2 user,
     * otherwise it leaves the bus pins in plain-GPIO state. */
    {"Ranging sensor (VL53L5CX)", testRangingSensor, CFG_TEST_RANGING_SENSOR == 1},
    {"Env sensors (HTS221/LPS22HH)", testEnvSensors, CFG_TEST_ENV_SENSORS == 1},
    {"Motion sensors (ISM330DHCX/IIS2MDC)", testMotionSensors, CFG_TEST_MOTION_SENSORS == 1},
    {"Light sensor (VEML)", testLightSensor, CFG_TEST_LIGHT_SENSOR == 1},
    {"EEPROM (M24256)", testEeprom, CFG_TEST_EEPROM == 1},
    {"OSPI NOR flash (MX25LM51245G)", testOspiNor, CFG_TEST_OSPI_NOR == 1},
    {"OSPI PSRAM (APS6408)", testOspiPsram, CFG_TEST_OSPI_PSRAM == 1},
    {"Internal flash NV data", testInternalFlash, CFG_TEST_INTERNAL_FLASH == 1},
    {"SRAM ring buffer", testSramBuffer, CFG_TEST_SRAM_BUFFER == 1},
    {"Microphone 1 (ADF1)", testMic1, CFG_TEST_MIC1 == 1},
    {"Microphone 2 (MDF1)", testMic2, CFG_TEST_MIC2 == 1},
    {"BLE module (STM32WB5MMG)", testBleModule, CFG_TEST_BLE_MODULE == 1},
    {"Wi-Fi module (EMW3080)", testWifiModule, CFG_TEST_WIFI_MODULE == 1},
    {"TrustZone/GTZC protection", testTrustZone, CFG_TEST_TRUSTZONE == 1},
};
constexpr size_t kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

const char *toString(Result r)
{
  switch (r)
  {
    case Result::Pass: return "PASS";
    case Result::Fail: return "FAIL";
    default:           return "SKIP";
  }
}
} // namespace

Result Runner::runOne(size_t index)
{
  const Case &c = cases_[index];
  printf("\r\n[TEST %02u] %s\r\n", static_cast<unsigned>(index + 1), c.name);
  Result r = c.enabled ? c.fn() : Result::Skip;
  results_[index] = r;
  hasRun_[index] = true;
  printf("[TEST %02u] %s -> %s\r\n", static_cast<unsigned>(index + 1), c.name, toString(r));
  return r;
}

void Runner::runAll()
{
  for (size_t i = 0; i < count_; i++)
  {
    runOne(i);
  }
}

void Runner::printSummary() const
{
  size_t pass = 0;
  size_t fail = 0;
  size_t skip = 0;
  printf("\r\n==================== SUMMARY ====================\r\n");
  for (size_t i = 0; i < count_; i++)
  {
    Result r = hasRun_[i] ? results_[i] : Result::Skip;
    printf("  %02u. %-38s %s\r\n", static_cast<unsigned>(i + 1), cases_[i].name, toString(r));
    if (r == Result::Pass) pass++;
    else if (r == Result::Fail) fail++;
    else skip++;
  }
  printf("  total=%u pass=%u fail=%u skip=%u\r\n",
         static_cast<unsigned>(count_), static_cast<unsigned>(pass),
         static_cast<unsigned>(fail), static_cast<unsigned>(skip));
  printf("================== SUMMARY END ==================\r\n");
}

bool Runner::allPassed() const
{
  for (size_t i = 0; i < count_; i++)
  {
    if (hasRun_[i] && results_[i] == Result::Fail)
    {
      return false;
    }
  }
  return true;
}

namespace
{
void interactiveMenu(Runner &runner)
{
  printf("\r\n[MENU] press 1-9/a-g to rerun a test, 'r' = run all, 'q' = quit\r\n");
  for (;;)
  {
    int ch = Console_GetChar(60000);
    if (ch < 0 || ch == 'q')
    {
      printf("[MENU] exit\r\n");
      return;
    }
    if (ch == 'r')
    {
      runner.runAll();
      runner.printSummary();
      continue;
    }
    size_t idx;
    if (ch >= '1' && ch <= '9')      idx = static_cast<size_t>(ch - '1');
    else if (ch >= 'a' && ch <= 'g') idx = static_cast<size_t>(ch - 'a') + 9U;
    else                             continue;
    if (idx < runner.count())
    {
      runner.runOne(idx);
    }
  }
}
} // namespace

} // namespace apptest

extern "C" void App_RunTestsOnce(void)
{
  using namespace apptest;
  static Runner runner(kCases, kCaseCount);
  runner.runAll();
  runner.printSummary();
  printf("[RESULT] %s\r\n", runner.allPassed() ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
}

extern "C" void App_TestMain(void)
{
  using namespace apptest;

  printf("\r\n\r\n===== B-U585I-IOT02A hardware test firmware =====\r\n");
  printf("SYSCLK=%lu Hz, HAL=%lu, build " __DATE__ " " __TIME__ "\r\n",
         HAL_RCC_GetSysClockFreq(), HAL_GetHalVersion());

  /* Run indicator: LD6/LD7 blink alternately while the firmware is running */
  BSP_LED_Init(LED_RED);
  BSP_LED_Init(LED_GREEN);
  BSP_LED_On(LED_RED);
  BSP_LED_Off(LED_GREEN);
  g_LedBlinkEnable = 1;

  static Runner runner(kCases, kCaseCount);
  runner.runAll();
  runner.printSummary();
  printf("[RESULT] %s\r\n", runner.allPassed() ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

#if CFG_MENU_ENABLED
  interactiveMenu(runner);
#endif
}

/**
  ******************************************************************************
  * @file    test.hpp
  * @brief   Minimal test framework for on-target hardware verification.
  ******************************************************************************
  */
#ifndef TEST_HPP
#define TEST_HPP

#include <cstddef>
#include <cstdint>

namespace apptest
{

enum class Result : uint8_t
{
  Pass,
  Fail,
  Skip
};

using TestFn = Result (*)();

struct Case
{
  const char *name;
  TestFn fn;
  bool enabled;
};

class Runner
{
public:
  static constexpr size_t kMaxCases = 24;

  Runner(const Case *cases, size_t count) : cases_(cases), count_(count) {}

  void runAll();
  Result runOne(size_t index);
  void printSummary() const;
  size_t count() const { return count_; }
  const Case &at(size_t index) const { return cases_[index]; }
  bool allPassed() const;

private:
  const Case *cases_;
  size_t count_;
  Result results_[kMaxCases] = {};
  bool hasRun_[kMaxCases] = {};
};

/* Test groups (implemented in tests_*.cpp) */
Result testLed();
Result testButton();
Result testEnvSensors();
Result testMotionSensors();
Result testLightSensor();
Result testRangingSensor();
Result testEeprom();
Result testOspiNor();
Result testOspiPsram();
Result testInternalFlash();
Result testSramBuffer();
Result testMic1();
Result testMic2();
Result testBleModule();
Result testWifiModule();
Result testTrustZone();

} // namespace apptest

#endif /* TEST_HPP */

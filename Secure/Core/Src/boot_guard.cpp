/**
  ******************************************************************************
  * @file    boot_guard.cpp
  * @brief   TAMP->BKP0R backed boot-attempt counter (OTA Phase 2).
  ******************************************************************************
  */
#include "boot_guard.hpp"

#include "main.h"

namespace boot_guard
{

namespace
{
void ensureBackupDomainReady()
{
  __HAL_RCC_RTCAPB_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
}
} // namespace

uint32_t attemptCount()
{
  ensureBackupDomainReady();
  return TAMP->BKP0R;
}

void noteBootAttempt()
{
  ensureBackupDomainReady();
  TAMP->BKP0R = TAMP->BKP0R + 1U;
}

void confirmBoot()
{
  ensureBackupDomainReady();
  TAMP->BKP0R = 0U;
}

} // namespace boot_guard

/* extern "C" bridges for the plain-C callers (secure_nsc.c, main.c) that
 * can't see the C++ namespace above directly. */
extern "C" void BootGuard_ConfirmBoot(void)
{
  boot_guard::confirmBoot();
}

extern "C" uint32_t BootGuard_AttemptCount(void)
{
  return boot_guard::attemptCount();
}

extern "C" void BootGuard_NoteBootAttempt(void)
{
  boot_guard::noteBootAttempt();
}

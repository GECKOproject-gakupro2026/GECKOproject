/**
  ******************************************************************************
  * @file    boot_guard.hpp
  * @brief   Boot-success tracking for the Stage-0 loader (OTA Phase 2).
  *          Uses TAMP->BKP0R (VBAT-domain backup register, survives a warm
  *          reset) as a boot-attempt counter: Stage-0 increments it before
  *          jumping to NonSecure, and NonSecure confirms a good boot via the
  *          NSC gateway, which clears it back to 0. If the counter reaches
  *          kMaxAttempts on the next Stage-0 pass, the NonSecure image is
  *          considered bad and the loader restores NOR backup slot B into
  *          Bank2 before retrying.
  ******************************************************************************
  */
#ifndef BOOT_GUARD_HPP
#define BOOT_GUARD_HPP

#include <cstdint>

namespace boot_guard
{

constexpr uint32_t kMaxAttempts = 3U;

/* Enables backup-domain write access and reads the current attempt count. */
uint32_t attemptCount();

/* Increments the attempt counter. Call right before jumping to NonSecure. */
void noteBootAttempt();

/* Clears the attempt counter back to 0. Called from the NSC gateway when
 * NonSecure confirms it has booted successfully. */
void confirmBoot();

} // namespace boot_guard

/* extern "C" bridges so main.c (Stage-0, plain C) can drive the guard. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t BootGuard_AttemptCount(void);
void BootGuard_NoteBootAttempt(void);
#ifdef __cplusplus
}
#endif

#endif /* BOOT_GUARD_HPP */

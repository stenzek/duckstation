#pragma once
#include "common/types.h"

class StateWrapper;
class CDImage;

namespace Achievements {

#ifdef WITH_CHEEVOS

// Implemented in Host.
extern bool Reset();
extern bool DoState(StateWrapper& sw);
extern void GameChanged(const std::string& path, CDImage* image);

/// Re-enables hardcode mode if it is enabled in the settings.
extern void ResetChallengeMode();

/// Forces hardcore mode off until next reset.
extern void DisableChallengeMode();

/// Prompts the user to disable hardcore mode, if they agree, returns true.
extern bool ConfirmChallengeModeDisable(const char* trigger);

/// Returns true if features such as save states should be disabled.
extern bool ChallengeModeActive();

#else

// Make noops when compiling without cheevos.
static inline bool Reset()
{
  return true;
}
static inline bool DoState(StateWrapper& sw)
{
  return true;
}
static constexpr inline bool ChallengeModeActive()
{
  return false;
}

static inline void ResetChallengeMode() {}

static inline void DisableChallengeMode() {}

static inline bool ConfirmChallengeModeDisable(const char* trigger)
{
  return true;
}

#endif

} // namespace Achievements

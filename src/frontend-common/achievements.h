#pragma once
#include "common/string.h"
#include "core/achievements.h"
#include "core/settings.h"
#include "core/types.h"
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CDImage;
class StateWrapper;

namespace Achievements {
enum class AchievementCategory : u32
{
  Local = 0,
  Core = 3,
  Unofficial = 5
};

struct Achievement
{
  u32 id;
  std::string title;
  std::string description;
  std::string memaddr;
  std::string badge_name;

  // badge paths are mutable because they're resolved when they're needed.
  mutable std::string locked_badge_path;
  mutable std::string unlocked_badge_path;

  u32 points;
  AchievementCategory category;
  bool locked;
  bool active;
};

struct Leaderboard
{
  u32 id;
  std::string title;
  std::string description;
  int format;
};

struct LeaderboardEntry
{
  std::string user;
  std::string formatted_score;
  u32 rank;
  bool is_self;
};

// RAIntegration only exists for Windows, so no point checking it on other platforms.
#ifdef WITH_RAINTEGRATION

bool IsUsingRAIntegration();

#else

static ALWAYS_INLINE bool IsUsingRAIntegration()
{
  return false;
}

#endif

bool IsActive();
bool IsLoggedIn();
bool ChallengeModeActive();
bool IsTestModeActive();
bool IsUnofficialTestModeActive();
bool IsRichPresenceEnabled();
bool HasActiveGame();

u32 GetGameID();

/// Acquires the achievements lock. Must be held when accessing any achievement state from another thread.
std::unique_lock<std::recursive_mutex> GetLock();

void Initialize();
void UpdateSettings(const Settings& old_config);

/// Called when the system is being reset. If it returns false, the reset should be aborted.
bool Reset();

/// Called when the system is being shut down. If Shutdown() returns false, the shutdown should be aborted.
bool Shutdown();

/// Called when the system is being paused and resumed.
void OnPaused(bool paused);

/// Called once a frame at vsync time on the CPU thread.
void FrameUpdate();

/// Called when the system is paused, because FrameUpdate() won't be getting called.
void ProcessPendingHTTPRequests();

/// Saves/loads state.
bool DoState(StateWrapper& sw);

/// Returns true if the current game has any achievements or leaderboards.
/// Does not need to have the lock held.
bool SafeHasAchievementsOrLeaderboards();

const std::string& GetUsername();
const std::string& GetRichPresenceString();

bool LoginAsync(const char* username, const char* password);
bool Login(const char* username, const char* password);
void Logout();

bool HasActiveGame();
void GameChanged(const std::string& path, CDImage* image);

/// Re-enables hardcode mode if it is enabled in the settings.
void ResetChallengeMode();

/// Forces hardcore mode off until next reset.
void DisableChallengeMode();

/// Prompts the user to disable hardcore mode, if they agree, returns true.
bool ConfirmChallengeModeDisable(const char* trigger);

/// Returns true if features such as save states should be disabled.
bool ChallengeModeActive();

const std::string& GetGameTitle();
const std::string& GetGameIcon();

bool EnumerateAchievements(std::function<bool(const Achievement&)> callback);
u32 GetUnlockedAchiementCount();
u32 GetAchievementCount();
u32 GetMaximumPointsForGame();
u32 GetCurrentPointsForGame();

bool EnumerateLeaderboards(std::function<bool(const Leaderboard&)> callback);
std::optional<bool> TryEnumerateLeaderboardEntries(u32 id, std::function<bool(const LeaderboardEntry&)> callback);
const Leaderboard* GetLeaderboardByID(u32 id);
u32 GetLeaderboardCount();
bool IsLeaderboardTimeType(const Leaderboard& leaderboard);

std::pair<u32, u32> GetAchievementProgress(const Achievement& achievement);
std::string GetAchievementProgressText(const Achievement& achievement);
const std::string& GetAchievementBadgePath(const Achievement& achievement);

void UnlockAchievement(u32 achievement_id, bool add_notification = true);
void SubmitLeaderboard(u32 leaderboard_id, int value);

#ifdef WITH_RAINTEGRATION
void SwitchToRAIntegration();

namespace RAIntegration {
void MainWindowChanged(void* new_handle);
void GameChanged();
std::vector<std::pair<int, const char*>> GetMenuItems();
void ActivateMenuItem(int item);
} // namespace RAIntegration
#endif
} // namespace Achievements

/// Functions implemented in the frontend.
namespace Host {
void OnAchievementsRefreshed();
void OnAchievementsChallengeModeChanged();
} // namespace Host

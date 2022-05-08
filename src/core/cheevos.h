#pragma once
#include "common/string.h"
#include "core/types.h"
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CDImage;
class StateWrapper;

namespace Cheevos {

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
  std::string locked_badge_path;
  std::string unlocked_badge_path;
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

extern bool g_active;
extern bool g_challenge_mode;
extern u32 g_game_id;

// RAIntegration only exists for Windows, so no point checking it on other platforms.
#ifdef WITH_RAINTEGRATION

extern bool g_using_raintegration;

static ALWAYS_INLINE bool IsUsingRAIntegration()
{
  return g_using_raintegration;
}

#else

static ALWAYS_INLINE bool IsUsingRAIntegration()
{
  return false;
}

#endif

ALWAYS_INLINE bool IsActive()
{
  return g_active;
}

ALWAYS_INLINE bool IsChallengeModeEnabled()
{
  return g_challenge_mode;
}

ALWAYS_INLINE bool IsChallengeModeActive()
{
  return g_active && g_challenge_mode;
}

ALWAYS_INLINE bool HasActiveGame()
{
  return g_game_id != 0;
}

ALWAYS_INLINE u32 GetGameID()
{
  return g_game_id;
}

bool Initialize(bool test_mode, bool use_first_disc_from_playlist, bool enable_rich_presence, bool challenge_mode,
                bool include_unofficial);
void Reset();
void Shutdown();

void Update();
bool DoState(StateWrapper& sw);

bool IsLoggedIn();
bool IsTestModeActive();
bool IsUnofficialTestModeActive();
bool IsUsingFirstDiscFromPlaylist();
bool IsRichPresenceEnabled();
const std::string& GetUsername();
const std::string& GetRichPresenceString();

bool LoginAsync(const char* username, const char* password);
bool Login(const char* username, const char* password);
void Logout();

bool HasActiveGame();
void GameChanged(const std::string& path, CDImage* image);

const std::string& GetGameTitle();
const std::string& GetGameDeveloper();
const std::string& GetGamePublisher();
const std::string& GetGameReleaseDate();
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
TinyString GetAchievementProgressText(const Achievement& achievement);

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

} // namespace Cheevos

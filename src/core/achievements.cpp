// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// TODO: Don't poll when booting the game, e.g. Crash Warped freaks out.

#include "achievements.h"
#include "achievements_private.h"
#include "bus.h"
#include "cheats.h"
#include "core.h"
#include "cpu_core.h"
#include "discord_presence.h"
#include "fullscreenui.h"
#include "fullscreenui_private.h"
#include "game_list.h"
#include "host.h"
#include "imgui_overlays.h"
#include "sound_effect_manager.h"
#include "system.h"
#include "video_thread.h"

#include "scmversion/scmversion.h"

#include "common/assert.h"
#include "common/binary_reader_writer.h"
#include "common/easing.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/ryml_helpers.h"
#include "common/scoped_guard.h"
#include "common/sha256_digest.h"
#include "common/small_string.h"
#include "common/string_pool.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "util/cd_image.h"
#include "util/http_cache.h"
#include "util/http_downloader.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/sqlite_helpers.h"
#include "util/state_wrapper.h"

#include <IconsEmoji.h>
#include <IconsFontAwesome.h>
#include <IconsPromptFont.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <rc_api_info.h>
#include <rc_api_runtime.h>
#include <rc_client.h>
#include <rc_consoles.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

LOG_CHANNEL(Achievements);

namespace Achievements {

static constexpr const char* INFO_SOUND_NAME = "sounds/achievements/message.wav";
static constexpr const char* UNLOCK_SOUND_NAME = "sounds/achievements/unlock.wav";
static constexpr const char* LBSUBMIT_SOUND_NAME = "sounds/achievements/lbsubmit.wav";
constexpr const char* const RA_LOGO_ICON_NAME = "images/ra-icon.webp";
constexpr const char* const RA_LOGO_SVG_ICON_NAME = "images/ra-icon.svg";
constexpr const char* const RA_REGISTER_URL = "https://retroachievements.org/createaccount.php";

static constexpr float LOGIN_NOTIFICATION_TIME = 5.0f;
static constexpr float ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME = 5.0f;
static constexpr float ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME_HC = 10.0f;
static constexpr float GAME_COMPLETE_NOTIFICATION_TIME = 20.0f;
static constexpr float CHALLENGE_STARTED_NOTIFICATION_TIME = 5.0f;
static constexpr float CHALLENGE_FAILED_NOTIFICATION_TIME = 5.0f;
static constexpr float LEADERBOARD_STARTED_NOTIFICATION_TIME = 3.0f;
static constexpr float LEADERBOARD_FAILED_NOTIFICATION_TIME = 3.0f;
static constexpr u16 LEADERBOARD_NOTIFICATION_MIN_WIDTH = 380;

// Some API calls are really slow. Set a longer timeout.
static constexpr u16 SERVER_CALL_TIMEOUT = 60;

// Update game list if it is more than a year old when logging in.
static constexpr s64 GAME_LIST_MAX_AGE_SECONDS = 365 * 24 * 60 * 60;
static constexpr const char* GAME_LIST_LAST_UPDATED_METADATA_KEY = "game_list_last_updated";

namespace {

struct LoginWithPasswordParameters
{
  const char* username;
  Error* error;
  rc_client_async_handle_t* request;
  bool is_temporary_client;
  bool result;
};

} // namespace

static void ReportError(std::string_view sv);
template<typename... T>
static void ReportFmtError(fmt::format_string<T...> fmt, T&&... args);
template<typename... T>
static void ReportRCError(int err, fmt::format_string<T...> fmt, T&&... args);
static void ClearGameInfo();
static bool TryLoggingInWithToken();
static void EnableHardcoreMode(bool display_message, bool display_game_summary);
static void OnHardcoreModeChanged(bool enabled, bool display_message, bool display_game_summary);
static bool IsRAIntegrationInitializing();
static void FinishInitialize();
static void FinishLogin();
static void BeginLoadGame();
static void UpdateGameSummary();
static void UpdateModeSettings(const Settings& old_config);
static DynamicHeapArray<u8> SaveStateToBuffer();
static void LoadStateFromBuffer(std::span<const u8> data, std::unique_lock<std::recursive_mutex>& lock);
static bool SaveStateToBuffer(std::span<u8> data);
static std::string GetImageURL(const char* image_name, u32 type);
static void PrefetchNextAchievementBadge();
static void PrefetchNextAchievementBadge(const rc_client_achievement_t* const last_cheevo);
static void PrefetchAllAchievementBadges();
static void UpdatePrefetchAchievementBadgesOSDMessage();

static TinyString DecryptLoginToken(std::string_view encrypted_token, std::string_view username);
static TinyString EncryptLoginToken(std::string_view token, std::string_view username);

static bool CreateClient(std::unique_lock<std::recursive_mutex>& lock, bool is_temporary_client);
static void DestroyClient(std::unique_lock<std::recursive_mutex>& lock);
static void ClientMessageCallback(const char* message, const rc_client_t* client);
static uint32_t ClientReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client);
static void ClientServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data,
                             rc_client_t* client);
static rc_api_server_response_t MakeRCAPIServerResponse(s32 status_code, const std::vector<u8>& data);
static void WaitForServerCallsWithYield(std::unique_lock<std::recursive_mutex>& lock);

static void ClientEventHandler(const rc_client_event_t* event, rc_client_t* client);
static void HandleResetEvent(const rc_client_event_t* event);
static void HandleUnlockEvent(const rc_client_event_t* event);
static void HandleGameCompleteEvent(const rc_client_event_t* event);
static void HandleSubsetCompleteEvent(const rc_client_event_t* event);
static void HandleLeaderboardStartedEvent(const rc_client_event_t* event);
static void HandleLeaderboardFailedEvent(const rc_client_event_t* event);
static void HandleLeaderboardSubmittedEvent(const rc_client_event_t* event);
static void HandleLeaderboardScoreboardEvent(const rc_client_event_t* event);
static void HandleLeaderboardTrackerShowEvent(const rc_client_event_t* event);
static void HandleLeaderboardTrackerHideEvent(const rc_client_event_t* event);
static void HandleLeaderboardTrackerUpdateEvent(const rc_client_event_t* event);
static void HandleAchievementChallengeIndicatorShowEvent(const rc_client_event_t* event);
static void HandleAchievementChallengeIndicatorHideEvent(const rc_client_event_t* event);
static void HandleAchievementProgressIndicatorShowEvent(const rc_client_event_t* event);
static void HandleAchievementProgressIndicatorHideEvent(const rc_client_event_t* event);
static void HandleAchievementProgressIndicatorUpdateEvent(const rc_client_event_t* event);
static void HandleServerErrorEvent(const rc_client_event_t* event);
static void HandleServerDisconnectedEvent(const rc_client_event_t* event);
static void HandleServerReconnectedEvent(const rc_client_event_t* event);

static void ClientLoginWithTokenCallback(int result, const char* error_message, rc_client_t* client, void* userdata);
static void ClientLoginWithPasswordCallback(int result, const char* error_message, rc_client_t* client, void* userdata);
static void ClientLoadGameCallback(int result, const char* error_message, rc_client_t* client, void* userdata);

static void DisplayHardcoreDeferredMessage();
static void DisplayAchievementSummary();
static void UpdateRichPresence(std::unique_lock<std::recursive_mutex>& lock);

static bool EnsureAchievementsDatabaseOpen(Error* error = nullptr);
static void CloseAchievementsDatabase();
static std::string GetAchievementsDatabaseMetadata(std::string_view key);
static bool SetAchievementsDatabaseMetadata(std::string_view key, std::string_view value, Error* error);
static bool CreateGameDatabaseFromSeedDatabase(Error* error);
static void UpdateGameDatabaseFromCurrentGame();

static void FetchGameListIfOutdated();
static bool BeginFetchGameListRequest(Error* error);
static void CancelFetchGameListRequest();
static void FetchGameListCallback(int result, const char* error_message, rc_client_game_list_t* game_list,
                                  rc_client_t* client, void* callback_userdata);
static bool WriteGameListToDatabase(const rc_client_game_list_t* game_list, Error* error);

static void FetchAllProgressIfMissing();
static bool BeginFetchAllProgressRequest(Error* error);
static void CancelFetchAllProgressRequest();
static void FetchAllProgressCallback(int result, const char* error_message, rc_client_all_user_progress_t* list,
                                     rc_client_t* client, void* callback_userdata);

static bool WriteAllProgressToDatabase(const rc_client_all_user_progress_t* allprog, Error* error);
static void UpdateProgressDatabaseFromCurrentGame();
static void ClearProgressDatabase();

static void LoadPinnedAchievements();
static void SetAchievementPinnedInDatabase(u32 achievement_id, bool pinned);

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

static void BeginLoadRAIntegration();
static void UnloadRAIntegration(std::unique_lock<std::recursive_mutex>& lock);

#endif

namespace {

struct State
{
  rc_client_t* client = nullptr;
  u16 pending_badge_downloads = 0;
  bool has_achievements : 1 = false;
  bool has_leaderboards : 1 = false;
  bool has_rich_presence : 1 = false;
  bool has_saved_credentials : 1 = false;
  bool reload_game_on_reset : 1 = false;

  std::recursive_mutex mutex; // large

  std::vector<LeaderboardTrackerIndicator> active_leaderboard_trackers;
  std::vector<ActiveChallengeIndicator> active_challenge_indicators;
  std::optional<AchievementProgressIndicator> active_progress_indicator;
  std::vector<PinnedAchievementIndicator> pinned_achievement_indicators;

  std::string http_user_agent_header;

  std::string logged_in_username;
  std::string logged_in_user_icon_url;

  std::string rich_presence_string;
  Timer::Value rich_presence_poll_time = 0;

  std::optional<GameHash> game_hash;
  u32 game_id = 0;

  std::string game_title;
  std::string game_badge_url;
  rc_client_user_game_summary_t game_summary = {};

  rc_client_async_handle_t* login_request = nullptr;
  rc_client_async_handle_t* load_game_request = nullptr;

  sqlite3* achievements_db = nullptr;
  SQLitePreparedStatement badge_lookup_stmt;
  SQLitePreparedStatement update_progress_stmt;

  rc_client_async_handle_t* fetch_game_list_request = nullptr;
  rc_client_async_handle_t* fetch_all_progress_request = nullptr;

  // used for GetAchievementBadgeURL() when the url fields aren't populated
  std::string temporary_url;

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  rc_client_async_handle_t* load_raintegration_request = nullptr;
  bool using_raintegration = false;
  bool raintegration_loading = false;
#endif
};

} // namespace

ALIGN_TO_CACHE_LINE static State s_state;

} // namespace Achievements

TinyString Achievements::GameHashToString(const std::optional<GameHash>& hash)
{
  TinyString ret;

  // Use a hash that will never match if we removed the disc. See rc_client_begin_change_media().
  if (!hash.has_value())
  {
    ret = "[NO HASH]";
  }
  else
  {
    ret.format("{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
               hash.value()[0], hash.value()[1], hash.value()[2], hash.value()[3], hash.value()[4], hash.value()[5],
               hash.value()[6], hash.value()[7], hash.value()[8], hash.value()[9], hash.value()[10], hash.value()[11],
               hash.value()[12], hash.value()[13], hash.value()[14], hash.value()[15]);
  }

  return ret;
}

std::unique_lock<std::recursive_mutex> Achievements::GetLock()
{
  return std::unique_lock(s_state.mutex);
}

rc_client_t* Achievements::GetClient()
{
  return s_state.client;
}

const rc_client_user_game_summary_t& Achievements::GetGameSummary()
{
  return s_state.game_summary;
}

std::vector<Achievements::LeaderboardTrackerIndicator>& Achievements::GetLeaderboardTrackerIndicators()
{
  return s_state.active_leaderboard_trackers;
}

std::vector<Achievements::ActiveChallengeIndicator>& Achievements::GetActiveChallengeIndicators()
{
  return s_state.active_challenge_indicators;
}

std::optional<Achievements::AchievementProgressIndicator>& Achievements::GetActiveProgressIndicator()
{
  return s_state.active_progress_indicator;
}

std::vector<Achievements::PinnedAchievementIndicator>& Achievements::GetPinnedAchievementIndicators()
{
  return s_state.pinned_achievement_indicators;
}

void Achievements::ReportError(std::string_view sv)
{
  ERROR_LOG(sv);
  Host::AddIconOSDMessage(OSDMessageType::Error, std::string(), ICON_EMOJI_WARNING, std::string(sv));
}

template<typename... T>
void Achievements::ReportFmtError(fmt::format_string<T...> fmt, T&&... args)
{
  TinyString str;
  fmt::vformat_to(std::back_inserter(str), fmt, fmt::make_format_args(args...));
  ReportError(str);
}

template<typename... T>
void Achievements::ReportRCError(int err, fmt::format_string<T...> fmt, T&&... args)
{
  TinyString str;
  fmt::vformat_to(std::back_inserter(str), fmt, fmt::make_format_args(args...));
  str.append_format("{} ({})", rc_error_str(err), err);
  ReportError(str);
}

std::string Achievements::GetImageURL(const char* image_name, u32 type)
{
  std::string ret;

  const rc_api_fetch_image_request_t image_request = {.image_name = image_name, .image_type = type};
  rc_api_request_t request;
  int result = rc_api_init_fetch_image_request(&request, &image_request);
  if (result == RC_OK)
    ret = request.url;

  rc_api_destroy_request(&request);
  return ret;
}

void Achievements::PrefetchNextAchievementBadge()
{
  if (!HasAchievements())
    return;

  // Find most recent unlock.
  rc_client_achievement_list_t* const achievements =
    rc_client_create_achievement_list(Achievements::GetClient(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
                                      RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  const rc_client_achievement_t* most_recent_unlock = nullptr;
  for (u32 i = 0; i < achievements->num_buckets; i++)
  {
    const rc_client_achievement_bucket_t& bucket = achievements->buckets[i];
    if (bucket.bucket_type != RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED &&
        bucket.bucket_type != RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED)
    {
      continue;
    }

    for (u32 j = 0; j < bucket.num_achievements; j++)
    {
      const rc_client_achievement_t* const cheevo = bucket.achievements[j];
      if (!most_recent_unlock || cheevo->unlock_time > most_recent_unlock->unlock_time)
        most_recent_unlock = cheevo;
    }
  }
  rc_client_destroy_achievement_list(achievements);

  if (most_recent_unlock)
    PrefetchNextAchievementBadge(most_recent_unlock);
}

void Achievements::PrefetchNextAchievementBadge(const rc_client_achievement_t* const last_cheevo)
{
  // Precache badge for the likely next achievement to avoid the badge load time.
  const rc_client_achievement_t* const next_cheevo =
    rc_client_get_next_achievement_info(s_state.client, last_cheevo, RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED);
  if (!next_cheevo)
    return;

  VERBOSE_LOG("Prefetching badge for likely next achievement '{}' ({})", next_cheevo->title, next_cheevo->badge_url);

  const std::string_view url = GetAchievementBadgeURL(next_cheevo, false);
  if (!url.empty())
    HTTPCache::Prefetch(url);
}

void Achievements::PrefetchAllAchievementBadges()
{
  // This is here so that we can hopefully avoid the delay in downloading the badge image on unlock.
  if (!HasAchievements())
    return;

  rc_client_achievement_list_t* const achievements =
    rc_client_create_achievement_list(Achievements::GetClient(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
                                      RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  if (!achievements)
    return;

  for (u32 i = 0; i < achievements->num_buckets; i++)
  {
    // Ignore unlocked achievements, since we're not going to be showing a notification for them.
    const rc_client_achievement_bucket_t& bucket = achievements->buckets[i];
    if (bucket.bucket_type != RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED)
      continue;

    for (u32 j = 0; j < bucket.num_achievements; j++)
    {
      const rc_client_achievement_t* const cheevo = bucket.achievements[j];
      const std::string_view url = GetAchievementBadgeURL(cheevo, false);
      if (!url.empty() && !HTTPCache::Contains(url))
      {
        s_state.pending_badge_downloads++;

        HTTPCache::Prefetch(url, [](bool) {
          const auto lock = GetLock();
          if (s_state.pending_badge_downloads > 0)
          {
            s_state.pending_badge_downloads--;
            UpdatePrefetchAchievementBadgesOSDMessage();
          }
        });
      }
    }
  }
  rc_client_destroy_achievement_list(achievements);

  if (s_state.pending_badge_downloads > 0)
    UpdatePrefetchAchievementBadgesOSDMessage();
}

void Achievements::UpdatePrefetchAchievementBadgesOSDMessage()
{
  if (s_state.pending_badge_downloads > 0)
  {
    Host::AddIconOSDMessage(OSDMessageType::Persistent, "AchievementsBadgePrefetch", OSDMessageIconType::Spinner, {},
                            {},
                            TRANSLATE_PLURAL_STR("Achievements", "Prefetching achievement badges (%n remaining)...",
                                                 "Achievement badge prefetch count", s_state.pending_badge_downloads));
  }
  else
  {
    Host::RemoveKeyedOSDMessage("AchievementsBadgePrefetch");
  }
}

bool Achievements::IsActive()
{
  return (s_state.client != nullptr);
}

bool Achievements::IsHardcoreModeActive()
{
  if (!s_state.client)
    return false;

  const auto lock = GetLock();
  return rc_client_get_hardcore_enabled(s_state.client);
}

bool Achievements::HasActiveGame()
{
  return s_state.game_id != 0;
}

u32 Achievements::GetGameID()
{
  return s_state.game_id;
}

bool Achievements::HasAchievementsOrLeaderboards()
{
  return s_state.has_achievements || s_state.has_leaderboards;
}

bool Achievements::HasAchievements()
{
  return s_state.has_achievements;
}

bool Achievements::HasLeaderboards()
{
  return s_state.has_leaderboards;
}

bool Achievements::HasRichPresence()
{
  return s_state.has_rich_presence;
}

const std::string& Achievements::GetCurrentGameTitle()
{
  return s_state.game_title;
}

const std::string& Achievements::GetCurrentGameBadgeURL()
{
  return s_state.game_badge_url;
}

const std::string& Achievements::GetRichPresenceString()
{
  return s_state.rich_presence_string;
}

void Achievements::ProcessStartup()
{
  // Called on startup, no need to grab lock just to populate has saved credentials.
  {
    const auto lock = Core::GetSettingsLock();
    const SettingsInterface* si = Core::GetBaseSettingsLayer();
    std::string_view username, token;
    s_state.has_saved_credentials = (si->LookupValue("Cheevos", "Username", &username) && !username.empty() &&
                                     si->LookupValue("Cheevos", "Token", &token) && !token.empty());
  }
}

void Achievements::Initialize()
{
  // No need to do anything else if we're not enabled.
  if (!g_settings.achievements_enabled)
    return;

  auto lock = GetLock();
  Assert(!s_state.client);
  CreateClient(lock, false);
}

bool Achievements::CreateClient(std::unique_lock<std::recursive_mutex>& lock, bool is_temporary_client)
{
  Assert(!s_state.client);

  s_state.client = rc_client_create(ClientReadMemory, ClientServerCall);
  if (!s_state.client)
  {
    Host::ReportErrorAsync("Achievements Error", "rc_client_create() failed, cannot use achievements");
    return false;
  }

  rc_client_set_event_handler(s_state.client, ClientEventHandler);
  rc_client_set_allow_background_memory_reads(s_state.client, true);
  rc_client_enable_logging(s_state.client,
                           (Log::GetLogLevel() >= Log::Level::Verbose) ? RC_CLIENT_LOG_LEVEL_VERBOSE :
                                                                         RC_CLIENT_LOG_LEVEL_INFO,
                           ClientMessageCallback);

  // Populate user-agent.
  char rc_client_user_agent[128];
  rc_client_get_user_agent_clause(s_state.client, rc_client_user_agent, std::size(rc_client_user_agent));
  s_state.http_user_agent_header = fmt::format("User-Agent: {} {}", Host::GetHTTPUserAgent(), rc_client_user_agent);
  VERBOSE_LOG(s_state.http_user_agent_header);

  // Allow custom host to be overridden through config.
  if (std::string host = Core::GetBaseStringSettingValue("Cheevos", "Host"); !host.empty())
  {
    // drop trailing slash, rc_client appends its own
    while (!host.empty() && host.back() == '/')
      host.pop_back();
    if (!host.empty())
    {
      INFO_COLOR_LOG(StrongOrange, "Using alternative host for achievements: {}", host);
      rc_client_set_host(s_state.client, host.c_str());
    }
  }

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (g_settings.achievements_use_raintegration && !is_temporary_client)
    BeginLoadRAIntegration();
#endif

  // Hardcore starts off. We enable it on first boot.
  rc_client_set_hardcore_enabled(s_state.client, false);
  rc_client_set_unofficial_enabled(s_state.client, g_settings.achievements_unofficial_test_mode);
  rc_client_set_spectator_mode_enabled(s_state.client, g_settings.achievements_spectator_mode);
  rc_client_set_encore_mode_enabled(s_state.client,
                                    !g_settings.achievements_spectator_mode && g_settings.achievements_encore_mode);

  // We can't do an internal client login while using RAIntegration, since the two will conflict.
  // Temporary clients also don't fully login, since they themselves are used for login.
  if (!IsRAIntegrationInitializing() && !is_temporary_client)
    FinishInitialize();

  return true;
}

void Achievements::FinishInitialize()
{
  // Start logging in. This can take a while.
  if (!IsLoggedInOrLoggingIn())
  {
    TryLoggingInWithToken();
  }
  // Are we running a game?
  else if (System::IsValid())
  {
    BeginLoadGame();

    // Hardcore mode isn't enabled when achievements first starts, if a game is already running.
    if (IsLoggedInOrLoggingIn() && g_settings.achievements_hardcore_mode)
      DisplayHardcoreDeferredMessage();
  }

  Host::OnAchievementsActiveChanged(true);
}

void Achievements::DestroyClient(std::unique_lock<std::recursive_mutex>& lock)
{
  DebugAssert(IsActive());
  WaitForServerCallsWithYield(lock);

  ClearGameInfo();
  DisableHardcoreMode(false, false);
  CancelFetchGameListRequest();
  CancelFetchAllProgressRequest();

  if (s_state.login_request)
  {
    rc_client_abort_async(s_state.client, s_state.login_request);
    s_state.login_request = nullptr;
  }

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (s_state.using_raintegration)
  {
    UnloadRAIntegration(lock);
    return;
  }
  else
#endif
  {
    rc_client_destroy(s_state.client);
    s_state.client = nullptr;
  }

  Host::OnAchievementsActiveChanged(false);
}

bool Achievements::HasSavedCredentials()
{
  const auto lock = GetLock();
  return s_state.has_saved_credentials;
}

bool Achievements::TryLoggingInWithToken()
{
  const TinyString username = Core::GetTinyStringSettingValue("Cheevos", "Username");
  const TinyString api_token = Core::GetTinyStringSettingValue("Cheevos", "Token");
  if (username.empty() || api_token.empty())
    return false;

  INFO_LOG("Attempting token login with user '{}'...", username);

  // If we can't decrypt the token, it was an old config and we need to re-login.
  if (const TinyString decrypted_api_token = DecryptLoginToken(api_token, username); !decrypted_api_token.empty())
  {
    s_state.login_request = rc_client_begin_login_with_token(
      s_state.client, username.c_str(), decrypted_api_token.c_str(), ClientLoginWithTokenCallback, nullptr);
    if (!s_state.login_request)
    {
      WARNING_LOG("Creating login request failed.");
      return false;
    }

    return true;
  }
  else
  {
    WARNING_LOG("Invalid encrypted login token, requesting a new one.");
    Host::OnAchievementsLoginRequested(LoginRequestReason::TokenInvalid);
    return false;
  }
}

void Achievements::UpdateSettings(const Settings& old_config)
{
  auto lock = GetLock();

  if (g_settings.achievements_enabled != old_config.achievements_enabled)
  {
    // we're done here
    if (g_settings.achievements_enabled)
    {
      if (!IsActive())
        CreateClient(lock, false);
    }
    else
    {
      if (IsActive())
        DestroyClient(lock);
    }

    return;
  }

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (g_settings.achievements_use_raintegration != old_config.achievements_use_raintegration)
  {
    // RAIntegration requires a full client reload?
    if (IsActive())
      DestroyClient(lock);
    CreateClient(lock, false);
    return;
  }
#endif

  if (g_settings.achievements_hardcore_mode != old_config.achievements_hardcore_mode)
  {
    // Enables have to wait for reset, disables can go through immediately.
    if (!g_settings.achievements_hardcore_mode)
      DisableHardcoreMode(true, true);
  }

  // If a game is active and these settings changed, reload the game to apply them.
  // Just unload and reload without destroying the client to preserve hardcore mode.
  // NOTE: Can't change spectator mode while game is loaded.
  if (HasActiveGame() && (g_settings.achievements_encore_mode != old_config.achievements_encore_mode ||
                          g_settings.achievements_spectator_mode != old_config.achievements_spectator_mode ||
                          g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode))
  {
    // Save and restore state to preserve progress.
    const DynamicHeapArray<u8> state_data = SaveStateToBuffer();
    ClearGameInfo();
    UpdateModeSettings(old_config);
    BeginLoadGame();
    LoadStateFromBuffer(state_data.cspan(), lock);
    return;
  }
  else
  {
    UpdateModeSettings(old_config);
  }

  if (!g_settings.achievements_leaderboard_trackers)
    s_state.active_leaderboard_trackers.clear();

  // remove progress indicator because it won't remove normally
  if (g_settings.achievements_progress_indicator_mode == AchievementProgressIndicatorMode::Disabled)
    s_state.active_progress_indicator.reset();
}

void Achievements::UpdateModeSettings(const Settings& old_config)
{
  if (g_settings.achievements_encore_mode != old_config.achievements_encore_mode ||
      g_settings.achievements_spectator_mode != old_config.achievements_spectator_mode)
  {
    rc_client_set_encore_mode_enabled(s_state.client,
                                      !g_settings.achievements_spectator_mode && g_settings.achievements_encore_mode);
    rc_client_set_spectator_mode_enabled(s_state.client, g_settings.achievements_spectator_mode);
  }
  if (g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode)
    rc_client_set_unofficial_enabled(s_state.client, g_settings.achievements_unofficial_test_mode);
}

void Achievements::Shutdown()
{
  auto lock = GetLock();
  if (IsActive())
    DestroyClient(lock);

  CloseAchievementsDatabase();
}

void Achievements::ClientMessageCallback(const char* message, const rc_client_t* client)
{
  DEV_LOG(message);
}

uint32_t Achievements::ClientReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  if ((address + num_bytes) > 0x200400U) [[unlikely]]
    return 0;

  const u8* src = (address >= 0x200000U) ? CPU::g_state.scratchpad.data() : Bus::g_ram;
  const u32 offset = (address & Bus::RAM_2MB_MASK); // size guarded by check above

  switch (num_bytes)
  {
    case 1:
      std::memcpy(buffer, &src[offset], 1);
      break;
    case 2:
      std::memcpy(buffer, &src[offset], 2);
      break;
    case 4:
      std::memcpy(buffer, &src[offset], 4);
      break;
    default:
      [[unlikely]] std::memcpy(buffer, &src[offset], num_bytes);
      break;
  }

  return num_bytes;
}

void Achievements::ClientServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback,
                                    void* callback_data, rc_client_t* client)
{
  HTTPDownloader::RequestCallback hd_callback = [callback, callback_data](s32 status_code, Error& error,
                                                                          std::string& content_type,
                                                                          HTTPDownloader::RequestData& data) {
    if (status_code != HTTPDownloader::HTTP_STATUS_OK)
      ERROR_LOG("Server call failed: {}", error.GetDescription());

    const rc_api_server_response_t rr = MakeRCAPIServerResponse(status_code, data);
    const auto lock = GetLock();
    callback(&rr, callback_data);
  };

  const std::array<const char* const, 1> headers = {s_state.http_user_agent_header.c_str()};
  if (request->post_data)
  {
    // const auto pd = std::string_view(request->post_data);
    // Log_DevFmt("Server POST: {}", pd.substr(0, std::min<size_t>(pd.length(), 10)));
    HTTPDownloader::CreatePostRequest(request->url, request->post_data, &s_state, std::move(hd_callback), nullptr,
                                      headers, SERVER_CALL_TIMEOUT);
  }
  else
  {
    HTTPDownloader::CreateRequest(request->url, &s_state, std::move(hd_callback), nullptr, headers,
                                  SERVER_CALL_TIMEOUT);
  }
}

rc_api_server_response_t Achievements::MakeRCAPIServerResponse(s32 status_code, const std::vector<u8>& data)
{
  if (status_code < 0)
  {
    // assume all errors are retryable, except when it's cancelled
    const int rc_http_status_code = (status_code == HTTPDownloader::HTTP_STATUS_CANCELLED) ?
                                      RC_API_SERVER_RESPONSE_CLIENT_ERROR :
                                      RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;

    // rc_client assumes client provides the error message as replacement to the body when the request is cancelled
    // see rc_json_parse_server_response() around RC_API_SERVER_RESPONSE_CLIENT_ERROR.
    const char* error_message;
    if (status_code == HTTPDownloader::HTTP_STATUS_CANCELLED)
      error_message = "Request cancelled";
    else if (status_code == HTTPDownloader::HTTP_STATUS_TIMEOUT)
      error_message = "Request timed out";
    else
      error_message = "Request failed";
    return rc_api_server_response_t{
      .body = error_message,
      .body_length = std::strlen(error_message),
      .http_status_code = rc_http_status_code,
    };
  }
  else
  {
    return rc_api_server_response_t{
      .body = data.empty() ? nullptr : reinterpret_cast<const char*>(data.data()),
      .body_length = data.size(),
      .http_status_code = status_code,
    };
  }
}

void Achievements::WaitForServerCallsWithYield(std::unique_lock<std::recursive_mutex>& lock)
{
  HTTPDownloader::WaitForAllRequestsFromOwnerWithYield(
    &s_state, [&lock]() { lock.unlock(); }, [&lock]() { lock.lock(); });
}

void Achievements::IdleUpdate()
{
  if (!IsActive())
    return;

  const auto lock = GetLock();
  rc_client_idle(s_state.client);
}

void Achievements::FrameUpdate()
{
  if (!IsActive())
    return;

  auto lock = GetLock();
  rc_client_do_frame(s_state.client);

  UpdateRichPresence(lock);
}

void Achievements::ClientEventHandler(const rc_client_event_t* event, rc_client_t* client)
{
  switch (event->type)
  {
    case RC_CLIENT_EVENT_RESET:
      HandleResetEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      HandleUnlockEvent(event);
      break;

    case RC_CLIENT_EVENT_GAME_COMPLETED:
      HandleGameCompleteEvent(event);
      break;

    case RC_CLIENT_EVENT_SUBSET_COMPLETED:
      HandleSubsetCompleteEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
      HandleLeaderboardStartedEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
      HandleLeaderboardFailedEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
      HandleLeaderboardSubmittedEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
      HandleLeaderboardScoreboardEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
      HandleLeaderboardTrackerShowEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
      HandleLeaderboardTrackerHideEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
      HandleLeaderboardTrackerUpdateEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
      HandleAchievementChallengeIndicatorShowEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
      HandleAchievementChallengeIndicatorHideEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
      HandleAchievementProgressIndicatorShowEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
      HandleAchievementProgressIndicatorHideEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
      HandleAchievementProgressIndicatorUpdateEvent(event);
      break;

    case RC_CLIENT_EVENT_SERVER_ERROR:
      HandleServerErrorEvent(event);
      break;

    case RC_CLIENT_EVENT_DISCONNECTED:
      HandleServerDisconnectedEvent(event);
      break;

    case RC_CLIENT_EVENT_RECONNECTED:
      HandleServerReconnectedEvent(event);
      break;

    default:
      [[unlikely]] ERROR_LOG("Unhandled event: {}", event->type);
      break;
  }
}

void Achievements::UpdateGameSummary()
{
  rc_client_get_user_game_summary(s_state.client, &s_state.game_summary);
}

void Achievements::UpdateRichPresence(std::unique_lock<std::recursive_mutex>& lock)
{
  if (!s_state.has_rich_presence)
    return;

  // Limit rich presence updates to once per second, since it could change per frame.
  const Timer::Value now = Timer::GetCurrentValue();
  if (Timer::ConvertValueToSeconds(now - s_state.rich_presence_poll_time) < 1)
    return;

  s_state.rich_presence_poll_time = now;

  char buffer[512];
  const size_t res = rc_client_get_rich_presence_message(s_state.client, buffer, std::size(buffer));
  const std::string_view sv(buffer, res);
  if (s_state.rich_presence_string == sv)
    return;

  s_state.rich_presence_string.assign(sv);

  INFO_LOG("Rich presence updated: {}", s_state.rich_presence_string);

#ifdef ENABLE_DISCORD_PRESENCE
  DiscordPresence::UpdateDetails(GetCurrentGameBadgeURL(), s_state.rich_presence_string);
#endif
}

void Achievements::OnSystemStarting(bool disable_hardcore_mode)
{
  std::unique_lock lock(s_state.mutex);

  if (!IsActive() || IsRAIntegrationInitializing())
    return;

  // if we're not logged in, and there's no login request, retry logging in
  // this'll happen if we had no network connection on startup, but gained it before starting a game.
  if (!IsLoggedInOrLoggingIn())
  {
    WARNING_LOG("Not logged in on game booting, trying again.");
    TryLoggingInWithToken();
  }

  // HC should have been disabled, we're now enabling it
  // RAIntegration can enable hardcode mode outside of us, so we need to double-check
  if (rc_client_get_hardcore_enabled(s_state.client))
  {
    WARNING_LOG("Hardcore mode was enabled on system starting.");
    OnHardcoreModeChanged(true, false, false);
  }
  else
  {
    // only enable hardcore mode if we're logged in, or waiting for a login response
    if (!disable_hardcore_mode && g_settings.achievements_hardcore_mode && IsLoggedInOrLoggingIn())
      EnableHardcoreMode(false, false);
  }
}

void Achievements::OnSystemStarted()
{
  const auto lock = GetLock();
  if (!IsActive() || IsRAIntegrationInitializing())
    return;

  // now we can finally identify the game
  if (!s_state.load_game_request)
    BeginLoadGame();
}

void Achievements::OnSystemDestroyed()
{
  const auto lock = GetLock();

  s_state.game_hash.reset();

  if (IsActive())
  {
    ClearGameInfo();
    DisableHardcoreMode(false, false);
  }
}

void Achievements::OnSystemReset()
{
  const auto lock = GetLock();
  if (!IsActive() || IsRAIntegrationInitializing())
    return;

  // Do we need to enable hardcore mode?
  if (System::IsValid() && g_settings.achievements_hardcore_mode && !rc_client_get_hardcore_enabled(s_state.client) &&
      (s_state.load_game_request || s_state.has_achievements || s_state.has_leaderboards))
  {
    // This will raise the silly reset event, but we can safely ignore that since we're immediately resetting the client
    DEV_LOG("Enabling hardcore mode after reset");
    EnableHardcoreMode(true, true);
  }

  DEV_LOG("Reset client");
  rc_client_reset(s_state.client);

  // Was there a pending disc change?
  // Ensure the new game is fully loaded after the reset, and not just treated as a disc swap.
  if (s_state.reload_game_on_reset)
  {
    DEV_LOG("Reloading game after reset due to disc change");
    ClearGameInfo();
    BeginLoadGame();
  }
}

void Achievements::SetGameHash(const std::optional<GameHash>& hash)
{
  const auto lock = GetLock();

  // disc changed?
  if (s_state.game_hash == hash)
    return;

  s_state.game_hash = hash;
  INFO_COLOR_LOG(StrongOrange, "RA Hash: {}", GameHashToString(s_state.game_hash));

  // just set the hash if inactive
  if (!IsActive() || IsRAIntegrationInitializing())
  {
    DisableHardcoreMode(false, false);
    return;
  }

  // if session hasn't started yet, don't treat this as a change
  if (!System::IsValid())
    return;

  // cancel previous requests
  if (s_state.load_game_request)
  {
    rc_client_abort_async(s_state.client, s_state.load_game_request);
    s_state.load_game_request = nullptr;
  }

  s_state.load_game_request =
    rc_client_begin_change_media_from_hash(s_state.client, GameHashToString(s_state.game_hash).c_str(),
                                           ClientLoadGameCallback, reinterpret_cast<void*>(static_cast<uintptr_t>(1)));

  // Flag the disc change. That way we reload the game on reset instead of treating it as a swap.
  s_state.reload_game_on_reset = true;
}

std::optional<Achievements::GameHash> Achievements::GetGameHash()
{
  const auto lock = GetLock();
  return s_state.game_hash;
}

void Achievements::BeginLoadGame()
{
  if (!s_state.game_hash.has_value())
  {
    // no need to go through ClientLoadGameCallback, just bail out straight away
    DisableHardcoreMode(false, false);
    return;
  }

  s_state.load_game_request = rc_client_begin_load_game(s_state.client, GameHashToString(s_state.game_hash).c_str(),
                                                        ClientLoadGameCallback, nullptr);
}

void Achievements::ClientLoadGameCallback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  const bool was_disc_change = (userdata != nullptr);

  s_state.load_game_request = nullptr;

  if (result == RC_NO_GAME_LOADED)
  {
    // Unknown game.
    INFO_LOG("Unknown game '{}', disabling achievements.", GameHashToString(s_state.game_hash));
    if (was_disc_change)
      ClearGameInfo();

    DisableHardcoreMode(false, false);
    return;
  }
  else if (result == RC_LOGIN_REQUIRED)
  {
    // We would've asked to re-authenticate, so leave HC on for now.
    // Once we've done so, we'll reload the game.
    return;
  }
  else if (result == RC_HARDCORE_DISABLED)
  {
    if (error_message)
      ReportError(error_message);

    OnHardcoreModeChanged(false, true, false);
    return;
  }
  else if (result != RC_OK)
  {
    ReportFmtError("Loading game failed: {}", error_message);
    if (was_disc_change)
      ClearGameInfo();

    DisableHardcoreMode(false, false);
    return;
  }

  const rc_client_game_t* info = rc_client_get_game_info(s_state.client);
  if (!info)
  {
    ReportError("rc_client_get_game_info() returned NULL");
    if (was_disc_change)
      ClearGameInfo();

    DisableHardcoreMode(false, false);
    return;
  }

  const bool has_achievements = rc_client_has_achievements(client);
  bool has_leaderboards = rc_client_has_leaderboards(client);
  INFO_LOG("Game loaded: '{}' (ID: {}, Achievements: {}, Leaderboards: {})", info->title, info->id,
           has_achievements ? "Yes" : "No", has_leaderboards ? "Yes" : "No");

  // Only display summary if the game title has changed across discs.
  const bool display_summary = (s_state.game_id != info->id || s_state.game_title != info->title);

  // If the game has an RA entry but no achievements or leaderboards, we should not enforce hardcore mode.
  if (!has_achievements && !has_leaderboards)
  {
    WARNING_LOG("Game '{}' has no achievements or leaderboards, disabling hardcore mode.", info->title);
    DisableHardcoreMode(false, false);
  }

  s_state.game_id = info->id;
  s_state.game_title = info->title;
  s_state.has_achievements = has_achievements;
  s_state.has_leaderboards = has_leaderboards;
  s_state.has_rich_presence = rc_client_has_rich_presence(client);
  s_state.game_badge_url =
    info->badge_url ? std::string(info->badge_url) : GetImageURL(info->badge_name, RC_IMAGE_TYPE_GAME);

  // prefetch the game badge before any of the achievement badges, because the popup for the game summary
  // is going to display, and we don't want a placeholder stuck there until after the badges finish
  if (!s_state.game_badge_url.empty())
    HTTPCache::Prefetch(s_state.game_badge_url);

  // update progress database on first load, in case it was played on another PC
  UpdateGameSummary();

  // don't update the game database on disc change, because switching to unknown media will associate those hashes
  if (was_disc_change)
    UpdateGameDatabaseFromCurrentGame();

  // but the progress is fine since that's just game IDs
  UpdateProgressDatabaseFromCurrentGame();

#ifdef ENABLE_DISCORD_PRESENCE
  DiscordPresence::UpdateDetails(s_state.game_badge_url, s_state.rich_presence_string);
#endif

  if (g_settings.achievements_prefetch_badges)
    Achievements::PrefetchAllAchievementBadges();
  else
    PrefetchNextAchievementBadge();

  // needed for notifications
  SoundEffectManager::EnsureInitialized();

  if (display_summary)
    DisplayAchievementSummary();

  LoadPinnedAchievements();
}

void Achievements::ClearGameInfo()
{
#ifdef ENABLE_DISCORD_PRESENCE
  DiscordPresence::UpdateDetails({}, {});
#endif

  FullscreenUI::ClearAchievementsState();

  s_state.active_leaderboard_trackers = {};
  s_state.active_challenge_indicators = {};
  s_state.active_progress_indicator.reset();
  s_state.pinned_achievement_indicators = {};

  if (s_state.load_game_request)
  {
    rc_client_abort_async(s_state.client, s_state.load_game_request);
    s_state.load_game_request = nullptr;
  }
  rc_client_unload_game(s_state.client);

  s_state.game_id = 0;
  s_state.game_title = {};
  s_state.game_badge_url = {};
  s_state.reload_game_on_reset = false;
  s_state.has_achievements = false;
  s_state.has_leaderboards = false;
  s_state.has_rich_presence = false;
  s_state.rich_presence_string = {};
  s_state.game_summary = {};
}

void Achievements::DisplayAchievementSummary()
{
  if (g_settings.achievements_notifications)
  {
    SmallString summary;
    if (s_state.game_summary.num_core_achievements > 0)
    {
      summary.format(
        TRANSLATE_FS("Achievements", "{0}, {1}."),
        SmallString::from_format(TRANSLATE_PLURAL_FS("Achievements", "You have unlocked {} of %n achievements",
                                                     "Achievement popup", s_state.game_summary.num_core_achievements),
                                 s_state.game_summary.num_unlocked_achievements),
        SmallString::from_format(TRANSLATE_PLURAL_FS("Achievements", "and earned {} of %n points", "Achievement popup",
                                                     s_state.game_summary.points_core),
                                 s_state.game_summary.points_unlocked));

      summary.append('\n');
      if (IsHardcoreModeActive())
      {
        summary.append(
          TRANSLATE_SV("Achievements", "Hardcore mode is enabled. Cheats and save states are unavailable."));
      }
      else
      {
        summary.append(TRANSLATE_SV("Achievements", "Hardcore mode is disabled. Leaderboards will not be tracked."));
      }
    }
    else
    {
      summary.assign(TRANSLATE_SV("Achievements", "This game has no achievements."));
    }

    FullscreenUI::AddAchievementNotification("AchievementsSummary",
                                             IsHardcoreModeActive() ? ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME_HC :
                                                                      ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME,
                                             s_state.game_badge_url, s_state.game_title, std::string(summary),
                                             RA_LOGO_ICON_NAME, FullscreenUI::AchievementNotificationNoteType::Image);

    if (s_state.game_summary.num_unsupported_achievements > 0)
    {
      Host::AddIconOSDMessage(OSDMessageType::Error, "UnsupportedAchievements", ICON_EMOJI_WARNING,
                              TRANSLATE_STR("Achievements", "Unsupported Achievements"),
                              TRANSLATE_PLURAL_STR("Achievements", "%n achievements are not supported by DuckStation.",
                                                   "Achievement popup",
                                                   s_state.game_summary.num_unsupported_achievements));
    }
  }

  // Technically not going through the resource API, but since we're passing this to something else, we can't.
  if (g_settings.achievements_sound_effects)
    SoundEffectManager::EnqueueSoundEffect(INFO_SOUND_NAME);

  // Warn when spectator mode is enabled.
  if (rc_client_get_spectator_mode_enabled(s_state.client))
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Warning, "SpectatorOrEncoreMode", RA_LOGO_SVG_ICON_NAME,
      TRANSLATE_STR("Achievements", "Spectator mode enabled."),
      TRANSLATE_STR("Achievements", "All achievements are locked, and unlocks will not be recorded in your account."));
  }
  else if (rc_client_get_encore_mode_enabled(s_state.client))
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Warning, "SpectatorOrEncoreMode", RA_LOGO_SVG_ICON_NAME,
      TRANSLATE_STR("Achievements", "Encore mode enabled."),
      TRANSLATE_STR("Achievements",
                    "All achievements are locked, but unlocks will still be recorded in your account."));
  }
}

void Achievements::DisplayHardcoreDeferredMessage()
{
  if (g_settings.achievements_hardcore_mode && System::IsValid())
  {
    VideoThread::RunOnThread([]() {
      if (FullscreenUI::HasActiveWindow())
      {
        FullscreenUI::ShowToast(OSDMessageType::Info, {},
                                TRANSLATE_STR("Achievements", "Hardcore mode will be enabled on game restart."));
      }
      else
      {
        Host::AddIconOSDMessage(OSDMessageType::Info, "AchievementsHardcoreDeferred", ICON_EMOJI_TROPHY,
                                TRANSLATE_STR("Achievements", "Hardcore mode will be enabled on game restart."));
      }
    });
  }
}

void Achievements::HandleResetEvent(const rc_client_event_t* event)
{
  WARNING_LOG("Ignoring RC_CLIENT_EVENT_RESET.");
}

void Achievements::HandleUnlockEvent(const rc_client_event_t* event)
{
  const rc_client_achievement_t* const cheevo = event->achievement;
  DebugAssert(cheevo);

  INFO_LOG("Achievement {} ({}) for game {} unlocked", cheevo->id, cheevo->title, s_state.game_id);
  UpdateGameSummary();
  UpdateProgressDatabaseFromCurrentGame();
  SetAchievementPinned(cheevo->id, false);

  if (g_settings.achievements_notifications)
  {
    std::string title;
    if (cheevo->category == RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL)
      title = fmt::format(TRANSLATE_FS("Achievements", "{} (Unofficial)"), cheevo->title);
    else
      title = cheevo->title;

    std::string note;
    if (cheevo->points > 0)
      note = fmt::format(ICON_EMOJI_TROPHY " {}", cheevo->points);

    FullscreenUI::AddAchievementNotification(fmt::format("achievement_unlock_{}", cheevo->id),
                                             static_cast<float>(g_settings.achievements_notification_duration),
                                             std::string(GetAchievementBadgeURL(cheevo, false)), std::move(title),
                                             std::string(cheevo->description), std::move(note),
                                             (cheevo->points > 0) ?
                                               FullscreenUI::AchievementNotificationNoteType::Text :
                                               FullscreenUI::AchievementNotificationNoteType::None);

    PrefetchNextAchievementBadge(cheevo);
  }

  if (g_settings.achievements_sound_effects)
    SoundEffectManager::EnqueueSoundEffect(UNLOCK_SOUND_NAME);
}

void Achievements::HandleGameCompleteEvent(const rc_client_event_t* event)
{
  INFO_LOG("Game {} ({}) complete", s_state.game_id, s_state.game_title);
  UpdateGameSummary();

  if (g_settings.achievements_notifications)
  {
    std::string message = fmt::format(
      TRANSLATE_FS("Achievements", "Game complete.\n{0} and {1}."),
      TRANSLATE_PLURAL_STR("Achievements", "%n achievements", "Mastery popup",
                           s_state.game_summary.num_unlocked_achievements),
      TRANSLATE_PLURAL_STR("Achievements", "%n points", "Achievement points", s_state.game_summary.points_unlocked));

    FullscreenUI::AddAchievementNotification(
      "achievement_mastery", GAME_COMPLETE_NOTIFICATION_TIME, s_state.game_badge_url, s_state.game_title,
      std::move(message), ICON_EMOJI_TROPHY, FullscreenUI::AchievementNotificationNoteType::IconText);
  }
}

void Achievements::HandleSubsetCompleteEvent(const rc_client_event_t* event)
{
  INFO_LOG("Subset {} ({}) complete", event->subset->id, event->subset->title);
  UpdateGameSummary();

  if (g_settings.achievements_notifications && event->subset->badge_name[0] != '\0')
  {
    // Need to grab the icon for the subset.
    std::string badge_path = GetSubsetBadgeURL(event->subset);

    std::string message = fmt::format(
      TRANSLATE_FS("Achievements", "Subset complete.\n{0} and {1}."),
      TRANSLATE_PLURAL_STR("Achievements", "%n achievements", "Mastery popup",
                           s_state.game_summary.num_unlocked_achievements),
      TRANSLATE_PLURAL_STR("Achievements", "%n points", "Achievement points", s_state.game_summary.points_unlocked));

    FullscreenUI::AddAchievementNotification(
      "achievement_mastery", GAME_COMPLETE_NOTIFICATION_TIME, std::move(badge_path), std::string(event->subset->title),
      std::move(message), ICON_EMOJI_CHECKMARK_BUTTON, FullscreenUI::AchievementNotificationNoteType::IconText);
  }
}

void Achievements::HandleLeaderboardStartedEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} ({}) started", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications)
  {
    FullscreenUI::AddAchievementNotification(
      fmt::format("leaderboard_{}", event->leaderboard->id), LEADERBOARD_STARTED_NOTIFICATION_TIME,
      s_state.game_badge_url, std::string(event->leaderboard->title),
      TRANSLATE_STR("Achievements", "Leaderboard attempt started."), ICON_EMOJI_RED_FLAG,
      FullscreenUI::AchievementNotificationNoteType::IconText);
  }
}

void Achievements::HandleLeaderboardFailedEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} ({}) failed", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications)
  {
    FullscreenUI::AddAchievementNotification(
      fmt::format("leaderboard_{}", event->leaderboard->id), LEADERBOARD_FAILED_NOTIFICATION_TIME,
      s_state.game_badge_url, std::string(event->leaderboard->title),
      TRANSLATE_STR("Achievements", "Leaderboard attempt failed."), ICON_EMOJI_CROSS_MARK_BUTTON,
      FullscreenUI::AchievementNotificationNoteType::IconText);
  }
}

std::string_view Achievements::GetLeaderboardFormatIcon(u32 format)
{
  static const char* value_strings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
    ICON_EMOJI_CLOCK_FIVE_OCLOCK,
    ICON_EMOJI_DIRECT_HIT,
    ICON_EMOJI_CLIPBOARD,
  };

  return value_strings[std::min<u32>(format, std::size(value_strings) - 1)];
}

void Achievements::HandleLeaderboardSubmittedEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} ({}) submitted", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications)
  {
    static const char* value_strings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
      TRANSLATE_NOOP("Achievements", "Your Time: {}"),
      TRANSLATE_NOOP("Achievements", "Your Score: {}"),
      TRANSLATE_NOOP("Achievements", "Your Value: {}"),
    };

    std::string message =
      fmt::format("{} {}", GetLeaderboardFormatIcon(event->leaderboard->format),
                  TinyString::from_format(
                    fmt::runtime(Host::TranslateToStringView(
                      "Achievements",
                      value_strings[std::min<u8>(event->leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)])),
                    event->leaderboard->tracker_value ? event->leaderboard->tracker_value : "Unknown"));

    FullscreenUI::AddAchievementNotification(
      fmt::format("leaderboard_{}", event->leaderboard->id),
      static_cast<float>(g_settings.achievements_leaderboard_duration), s_state.game_badge_url,
      std::string(event->leaderboard->title), std::move(message),
      g_settings.achievements_spectator_mode ? std::string(ICON_EMOJI_CHART_UPWARDS_TREND) : std::string(),
      g_settings.achievements_spectator_mode ? FullscreenUI::AchievementNotificationNoteType::IconText :
                                               FullscreenUI::AchievementNotificationNoteType::Spinner,
      LEADERBOARD_NOTIFICATION_MIN_WIDTH);
  }

  if (g_settings.achievements_sound_effects)
    SoundEffectManager::EnqueueSoundEffect(LBSUBMIT_SOUND_NAME);
}

void Achievements::HandleLeaderboardScoreboardEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} scoreboard rank {} of {}", event->leaderboard_scoreboard->leaderboard_id,
          event->leaderboard_scoreboard->new_rank, event->leaderboard_scoreboard->num_entries);

  if (g_settings.achievements_leaderboard_notifications)
  {
    static const char* value_strings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
      TRANSLATE_NOOP("Achievements", "Your Time: {0} (Best: {1})"),
      TRANSLATE_NOOP("Achievements", "Your Score: {0} (Best: {1})"),
      TRANSLATE_NOOP("Achievements", "Your Value: {0} (Best: {1})"),
    };

    std::string message = fmt::format(
      "{} {}\n" ICON_EMOJI_CHART_UPWARDS_TREND " {}", GetLeaderboardFormatIcon(event->leaderboard->format),
      TinyString::from_format(
        fmt::runtime(Host::TranslateToStringView(
          "Achievements",
          value_strings[std::min<u8>(event->leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)])),
        event->leaderboard_scoreboard->submitted_score, event->leaderboard_scoreboard->best_score),
      TinyString::from_format(TRANSLATE_FS("Achievements", "Leaderboard Position: {0} of {1}"),
                              event->leaderboard_scoreboard->new_rank, event->leaderboard_scoreboard->num_entries));

    FullscreenUI::AddAchievementNotification(
      fmt::format("leaderboard_{}", event->leaderboard->id),
      static_cast<float>(g_settings.achievements_leaderboard_duration), s_state.game_badge_url,
      std::string(event->leaderboard->title), std::move(message), ICON_EMOJI_CHECKMARK_BUTTON,
      FullscreenUI::AchievementNotificationNoteType::IconText, LEADERBOARD_NOTIFICATION_MIN_WIDTH);
  }
}

void Achievements::HandleLeaderboardTrackerShowEvent(const rc_client_event_t* event)
{
  DEV_LOG("Showing leaderboard tracker: {}: {}", event->leaderboard_tracker->id, event->leaderboard_tracker->display);

  if (!g_settings.achievements_leaderboard_trackers)
    return;

  const u32 id = event->leaderboard_tracker->id;
  auto it = std::find_if(s_state.active_leaderboard_trackers.begin(), s_state.active_leaderboard_trackers.end(),
                         [id](const auto& it) { return it.tracker_id == id; });
  if (it != s_state.active_leaderboard_trackers.end())
  {
    WARNING_LOG("Leaderboard tracker {} already active", id);
    it->text = event->leaderboard_tracker->display;
    it->active = true;
    return;
  }

  s_state.active_leaderboard_trackers.push_back(LeaderboardTrackerIndicator{
    .tracker_id = id,
    .text = event->leaderboard_tracker->display,
    .time = 0.0f,
    .active = true,
  });
}

void Achievements::HandleLeaderboardTrackerHideEvent(const rc_client_event_t* event)
{
  const u32 id = event->leaderboard_tracker->id;
  DEV_LOG("Hiding leaderboard tracker: {}", id);

  auto it = std::find_if(s_state.active_leaderboard_trackers.begin(), s_state.active_leaderboard_trackers.end(),
                         [id](const auto& it) { return it.tracker_id == id; });
  if (it == s_state.active_leaderboard_trackers.end())
    return;

  it->active = false;
  it->time = std::min(it->time, INDICATOR_FADE_OUT_TIME);
}

void Achievements::HandleLeaderboardTrackerUpdateEvent(const rc_client_event_t* event)
{
  const u32 id = event->leaderboard_tracker->id;
  DEV_LOG("Updating leaderboard tracker: {}: {}", id, event->leaderboard_tracker->display);

  auto it = std::find_if(s_state.active_leaderboard_trackers.begin(), s_state.active_leaderboard_trackers.end(),
                         [id](const auto& it) { return it.tracker_id == id; });
  if (it == s_state.active_leaderboard_trackers.end())
    return;

  it->text = event->leaderboard_tracker->display;
  it->active = true;
}

void Achievements::HandleAchievementChallengeIndicatorShowEvent(const rc_client_event_t* event)
{
  if (const auto it =
        std::find_if(s_state.active_challenge_indicators.begin(), s_state.active_challenge_indicators.end(),
                     [event](const ActiveChallengeIndicator& it) { return it.achievement == event->achievement; });
      it != s_state.active_challenge_indicators.end())
  {
    it->active = true;
    return;
  }

  const std::string_view badge_url = GetAchievementBadgeURL(event->achievement, false);

  // we still track these even if the option is disabled, so that they can be displayed in the pause menu
  if (g_settings.achievements_challenge_indicator_mode == AchievementChallengeIndicatorMode::Notification)
  {
    FullscreenUI::AddAchievementNotification(
      fmt::format("AchievementChallenge{}", event->achievement->id), CHALLENGE_STARTED_NOTIFICATION_TIME,
      std::string(badge_url),
      fmt::format(TRANSLATE_FS("Achievements", "Challenge Started: {}"),
                  event->achievement->title ? event->achievement->title : ""),
      fmt::format(ICON_EMOJI_DIRECT_HIT " {}", event->achievement->description ? event->achievement->description : ""),
      {}, FullscreenUI::AchievementNotificationNoteType::None, 0, true);
  }

  s_state.active_challenge_indicators.push_back(
    ActiveChallengeIndicator{.achievement = event->achievement,
                             .badge_url = std::string(badge_url),
                             .time_remaining = LEADERBOARD_STARTED_NOTIFICATION_TIME,
                             .opacity = 0.0f,
                             .active = true});

  DEV_LOG("Show challenge indicator for {} ({})", event->achievement->id, event->achievement->title);
}

void Achievements::HandleAchievementChallengeIndicatorHideEvent(const rc_client_event_t* event)
{
  auto it = std::find_if(s_state.active_challenge_indicators.begin(), s_state.active_challenge_indicators.end(),
                         [event](const ActiveChallengeIndicator& it) { return it.achievement == event->achievement; });
  if (it == s_state.active_challenge_indicators.end())
    return;

  DEV_LOG("Hide challenge indicator for {} ({})", event->achievement->id, event->achievement->title);

  if (g_settings.achievements_challenge_indicator_mode == AchievementChallengeIndicatorMode::Notification &&
      event->achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE)
  {
    FullscreenUI::AddAchievementNotification(
      fmt::format("AchievementChallenge{}", event->achievement->id), CHALLENGE_FAILED_NOTIFICATION_TIME, it->badge_url,
      fmt::format(TRANSLATE_FS("Achievements", "Challenge Failed: {}"),
                  event->achievement->title ? event->achievement->title : ""),
      fmt::format(ICON_EMOJI_CROSS_MARK_BUTTON " {}",
                  event->achievement->description ? event->achievement->description : ""),
      {}, FullscreenUI::AchievementNotificationNoteType::None, 0, true);
  }
  if (g_settings.achievements_challenge_indicator_mode == AchievementChallengeIndicatorMode::Notification ||
      g_settings.achievements_challenge_indicator_mode == AchievementChallengeIndicatorMode::Disabled)
  {
    // remove it here, because it won't naturally decay
    s_state.active_challenge_indicators.erase(it);
    return;
  }

  it->active = false;
}

void Achievements::HandleAchievementProgressIndicatorShowEvent(const rc_client_event_t* event)
{
  DEV_LOG("Showing progress indicator: {} ({}): {}", event->achievement->id, event->achievement->title,
          event->achievement->measured_progress);

  // Don't show pinned achievements.
  if (IsAchievementPinned(event->achievement->id))
  {
    DEV_COLOR_LOG(StrongYellow, "Not showing progress indicator for pinned achievement {}", event->achievement->id);
    return;
  }

  if (!s_state.active_progress_indicator.has_value())
    s_state.active_progress_indicator.emplace();

  s_state.active_progress_indicator->achievement = event->achievement;
  s_state.active_progress_indicator->badge_url = GetAchievementBadgeURL(event->achievement, false);
  s_state.active_progress_indicator->time = 0.0f;
  s_state.active_progress_indicator->active = true;
  FullscreenUI::UpdateAchievementsLastProgressUpdate(event->achievement);
}

void Achievements::HandleAchievementProgressIndicatorHideEvent(const rc_client_event_t* event)
{
  if (!s_state.active_progress_indicator.has_value())
    return;

  DEV_LOG("Hiding progress indicator");

  s_state.active_progress_indicator->active = false;
  s_state.active_progress_indicator->time = std::min(s_state.active_progress_indicator->time, INDICATOR_FADE_OUT_TIME);
}

void Achievements::HandleAchievementProgressIndicatorUpdateEvent(const rc_client_event_t* event)
{
  DEV_LOG("Updating progress indicator: {} ({}): {}", event->achievement->id, event->achievement->title,
          event->achievement->measured_progress);
  if (!s_state.active_progress_indicator.has_value())
    return;

  s_state.active_progress_indicator->achievement = event->achievement;
  s_state.active_progress_indicator->active = true;
  FullscreenUI::UpdateAchievementsLastProgressUpdate(event->achievement);
}

void Achievements::HandleServerErrorEvent(const rc_client_event_t* event)
{
  ERROR_LOG("Server error in {}:\n{}", event->server_error->api ? event->server_error->api : "UNKNOWN",
            event->server_error->error_message ? event->server_error->error_message : "UNKNOWN");
  Host::AddIconOSDMessage(OSDMessageType::Error, {}, ICON_EMOJI_WARNING,
                          fmt::format(TRANSLATE_FS("Achievements", "Server error in {}"),
                                      event->server_error->api ? event->server_error->api : "UNKNOWN"),
                          event->server_error->error_message ? event->server_error->error_message : "UNKNOWN");
}

void Achievements::HandleServerDisconnectedEvent(const rc_client_event_t* event)
{
  WARNING_LOG("Server disconnected.");

  Host::AddIconOSDMessage(
    OSDMessageType::Error, "AchievementsDisconnected", ICON_EMOJI_WARNING,
    TRANSLATE_STR("Achievements", "Achievements Disconnected"),
    TRANSLATE_STR("Achievements",
                  "An unlock request could not be completed.\nWe will keep trying to submit this request."));
}

void Achievements::HandleServerReconnectedEvent(const rc_client_event_t* event)
{
  WARNING_LOG("Server reconnected.");

  Host::AddIconOSDMessage(OSDMessageType::Warning, "AchievementsDisconnected", RA_LOGO_SVG_ICON_NAME,
                          TRANSLATE_STR("Achievements", "Achievements Reconnected"),
                          TRANSLATE_STR("Achievements", "All pending unlock requests have completed."));
}

void Achievements::EnableHardcoreMode(bool display_message, bool display_game_summary)
{
  DebugAssert(IsActive());
  if (rc_client_get_hardcore_enabled(s_state.client))
    return;

  rc_client_set_hardcore_enabled(s_state.client, true);
  OnHardcoreModeChanged(true, display_message, display_game_summary);
}

void Achievements::DisableHardcoreMode(bool show_message, bool display_game_summary)
{
  if (!IsActive())
    return;

  const auto lock = GetLock();
  if (!rc_client_get_hardcore_enabled(s_state.client))
    return;

  rc_client_set_hardcore_enabled(s_state.client, false);
  OnHardcoreModeChanged(false, show_message, display_game_summary);
}

void Achievements::OnHardcoreModeChanged(bool enabled, bool display_message, bool display_game_summary)
{
  INFO_COLOR_LOG(StrongYellow, "Hardcore mode/restrictions are now {}.", enabled ? "ACTIVE" : "inactive");

  if (System::IsValid() && display_message)
  {
    Host::AddIconOSDMessage(OSDMessageType::Info, "AchievementsHardcoreModeChanged", RA_LOGO_SVG_ICON_NAME,
                            enabled ? TRANSLATE_STR("Achievements", "Hardcore mode enabled.") :
                                      TRANSLATE_STR("Achievements", "Hardcore mode disabled."),
                            enabled ? TRANSLATE_STR("Achievements", "Restrictions are now active.") :
                                      TRANSLATE_STR("Achievements", "Restrictions are no longer active."));
  }

  DebugAssert((rc_client_get_hardcore_enabled(s_state.client) != 0) == enabled);

  // Reload setting to permit cheating-like things if we were just disabled.
  if (System::IsValid())
  {
    // Make sure a pre-existing cheat file hasn't been loaded when resetting after enabling HC mode.
    Cheats::ReloadCheats(true, true, false, true, true);

    // Defer settings update in case something is using it.
    Host::RunOnCoreThread([]() { System::ApplySettings(false); });
  }
  else if (System::GetState() == System::State::Starting)
  {
    // Initial HC enable, activate restrictions.
    System::ApplySettings(false);
  }

  // Toss away UI state, because it's invalid now
  FullscreenUI::ClearAchievementsState();

  if (HasActiveGame() && display_game_summary)
  {
    UpdateGameSummary();
    DisplayAchievementSummary();
  }

  Host::OnAchievementsHardcoreModeChanged(enabled);
}

void Achievements::LoadStateFromBuffer(std::span<const u8> data, std::unique_lock<std::recursive_mutex>& lock)
{
  // if we're active, make sure we've downloaded and activated all the achievements
  // before deserializing, otherwise that state's going to get lost.
  if (s_state.load_game_request)
  {
    // Fallback to game icon if we don't have a cover.
    std::string image = System::GetImageForLoadingScreen(System::GetGamePath());
    FullscreenUI::OpenOrUpdateLoadingScreen(image.empty() ? s_state.game_badge_url : image,
                                            TRANSLATE_SV("Achievements", "Downloading achievements data..."));

    WaitForServerCallsWithYield(lock);

    FullscreenUI::CloseLoadingScreen();
  }

  if (data.empty())
  {
    // reset runtime, no data (state might've been created without cheevos)
    WARNING_LOG("State is missing cheevos data, resetting runtime");
    rc_client_reset(s_state.client);
    return;
  }

  const int result = rc_client_deserialize_progress_sized(s_state.client, data.data(), data.size());
  if (result != RC_OK)
    ERROR_LOG("Failed to deserialize cheevos state ({}/{}), runtime was reset", rc_error_str(result), result);
}

bool Achievements::SaveStateToBuffer(std::span<u8> data)
{
  const int result = rc_client_serialize_progress_sized(s_state.client, data.data(), data.size());
  if (result != RC_OK)
  {
    // set data to zero, effectively serializing nothing
    ERROR_LOG("Failed to serialize cheevos state ({}/{})", rc_error_str(result), result);
    return false;
  }

  return true;
}

DynamicHeapArray<u8> Achievements::SaveStateToBuffer()
{
  DynamicHeapArray<u8> ret;
  if (const size_t data_size = rc_client_progress_size(s_state.client); data_size > 0)
  {
    ret.resize(data_size);
    if (!SaveStateToBuffer(ret))
      ret.deallocate();
  }

  return ret;
}

bool Achievements::DoState(StateWrapper& sw)
{
  static constexpr u32 REQUIRED_VERSION = 56;

  // if we're inactive, we still need to skip the data (if any)
  if (!IsActive())
  {
    u32 data_size = 0;
    sw.DoEx(&data_size, REQUIRED_VERSION, 0u);
    if (data_size > 0)
      sw.SkipBytes(data_size);

    return !sw.HasError();
  }

  std::unique_lock lock(s_state.mutex);

  if (sw.IsReading())
  {
    u32 data_size = 0;
    sw.DoEx(&data_size, REQUIRED_VERSION, 0u);

    const std::span<u8> data = sw.GetDeferredBytes(data_size);
    if (sw.HasError())
      return false;

    LoadStateFromBuffer(data, lock);
    return true;
  }
  else
  {
    const size_t size_pos = sw.GetPosition();

    u32 data_size = static_cast<u32>(rc_client_progress_size(s_state.client));
    sw.Do(&data_size);

    if (data_size > 0)
    {
      const std::span<u8> data = sw.GetDeferredBytes(data_size);
      if (!sw.HasError()) [[likely]]
      {
        if (!SaveStateToBuffer(data))
        {
          // set data to zero, effectively serializing nothing
          data_size = 0;
          sw.SetPosition(size_pos);
          sw.Do(&data_size);
        }
      }
    }

    return !sw.HasError();
  }
}

std::string_view Achievements::GetAchievementBadgeURL(const rc_client_achievement_t* achievement, bool locked)
{
  // RAIntegration doesn't set the URL fields.
  if (const char* url_ptr = locked ? achievement->badge_locked_url : achievement->badge_url)
  {
    const std::string_view url(url_ptr);
    if (url.empty()) [[unlikely]]
      ReportFmtError("Achievement {} with badge name {} has no badge URL", achievement->id, achievement->badge_name);

    return url;
  }
  else
  {
    s_state.temporary_url =
      GetImageURL(achievement->badge_name, locked ? RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED : RC_IMAGE_TYPE_ACHIEVEMENT);
    return s_state.temporary_url;
  }
}

std::string Achievements::GetUserBadgeURL(const char* username)
{
  return GetImageURL(username, RC_IMAGE_TYPE_USER);
}

std::string Achievements::GetSubsetBadgeURL(const rc_client_subset_t* subset)
{
  if (!subset->badge_url)
    return GetImageURL(subset->badge_name, RC_IMAGE_TYPE_GAME);
  else
    return subset->badge_url;
}

bool Achievements::IsLoggedIn()
{
  return (rc_client_get_user_info(s_state.client) != nullptr);
}

bool Achievements::IsLoggedInOrLoggingIn()
{
  return (IsLoggedIn() || s_state.login_request);
}

bool Achievements::Login(const char* username, const char* password, Error* error)
{
  auto lock = GetLock();

  // We need to use a temporary client if achievements aren't currently active.
  const bool is_temporary_client = (s_state.client == nullptr);
  if (is_temporary_client && !CreateClient(lock, true))
  {
    Error::SetString(error, "Failed to create client.");
    return false;
  }

  LoginWithPasswordParameters params = {username, error, nullptr, is_temporary_client, false};
  params.request =
    rc_client_begin_login_with_password(s_state.client, username, password, ClientLoginWithPasswordCallback, &params);
  if (!params.request)
  {
    Error::SetString(error, "Failed to create login request.");
    return false;
  }

  // Wait until the login request completes.
  WaitForServerCallsWithYield(lock);
  Assert(!params.request);

  // Did we get enabled and disabled in the meantime?
  if (!s_state.client)
    return params.result;

  // Did we get enabled? Leave the client if so
  if (!g_settings.achievements_enabled)
  {
    DestroyClient(lock);
  }
  else
  {
    FinishLogin();
    FinishInitialize();
  }

  // Success? Assume the callback set the error message.
  return params.result;
}

void Achievements::ClientLoginWithPasswordCallback(int result, const char* error_message, rc_client_t* client,
                                                   void* userdata)
{
  Assert(userdata);

  LoginWithPasswordParameters* params = static_cast<LoginWithPasswordParameters*>(userdata);
  params->request = nullptr;

  if (result != RC_OK)
  {
    ERROR_LOG("Login failed: {}: {}", rc_error_str(result), error_message ? error_message : "Unknown");
    Error::SetString(params->error,
                     fmt::format("{}: {}", rc_error_str(result), error_message ? error_message : "Unknown"));
    params->result = false;
    return;
  }

  // Grab the token from the client, and save it to the config.
  const rc_client_user_t* user = rc_client_get_user_info(client);
  if (!user || !user->token)
  {
    ERROR_LOG("rc_client_get_user_info() returned NULL");
    Error::SetString(params->error, "rc_client_get_user_info() returned NULL");
    params->result = false;
    return;
  }

  params->result = true;

  // Store configuration.
  Core::SetBaseStringSettingValue("Cheevos", "Username", params->username);
  Core::SetBaseStringSettingValue("Cheevos", "Token", EncryptLoginToken(user->token, params->username));
  Core::SetBaseStringSettingValue("Cheevos", "LoginTimestamp", fmt::format("{}", std::time(nullptr)).c_str());
  Host::CommitBaseSettingChanges();
  s_state.has_saved_credentials = true;
}

void Achievements::ClientLoginWithTokenCallback(int result, const char* error_message, rc_client_t* client,
                                                void* userdata)
{
  s_state.login_request = nullptr;

  if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN)
  {
    ERROR_LOG("Login failed due to invalid token: {}: {}", rc_error_str(result), error_message);
    Host::OnAchievementsLoginRequested(LoginRequestReason::TokenInvalid);
    return;
  }
  else if (result != RC_OK)
  {
    ERROR_LOG("Login failed: {}: {}", rc_error_str(result), error_message);

    // only display user error if they've started a game
    if (System::IsValid())
    {
      Host::AddIconOSDMessage(
        OSDMessageType::Error, "AchievementsLoginFailed", ICON_EMOJI_NO_ENTRY_SIGN,
        TRANSLATE_STR("Achievements", "RetroAchievements Login Failed"),
        fmt::format(
          TRANSLATE_FS("Achievements", "Achievement unlocks will not be submitted for this session.\nError: {}"),
          error_message));
    }

    return;
  }

  // Should be active here.
  DebugAssert(client == s_state.client);
  FinishLogin();

  // Triggered through FinishInitialize(), which didn't load the game yet, so we need to do it here.
  if (!s_state.load_game_request && System::IsValid())
    BeginLoadGame();
}

void Achievements::FinishLogin()
{
  const rc_client_user_t* const user = rc_client_get_user_info(s_state.client);
  if (!user)
    return;

  s_state.logged_in_username = user->username ? std::string(user->username) : std::string();
  s_state.logged_in_user_icon_url = (user->avatar_url && user->avatar_url[0] != '\0') ?
                                      std::string(user->avatar_url) :
                                      GetImageURL(user->username, RC_IMAGE_TYPE_USER);

  FetchGameListIfOutdated();
  FetchAllProgressIfMissing();

  Host::OnAchievementsLoginSuccess(user->username, user->score, user->score_softcore, user->num_unread_messages);

  if (g_settings.achievements_notifications)
  {
    //: Summary for login notification.
    std::string summary = fmt::format(TRANSLATE_FS("Achievements", "Score: {} ({} softcore)\nUnread messages: {}"),
                                      user->score, user->score_softcore, user->num_unread_messages);

    FullscreenUI::AddAchievementNotification("achievements_login", LOGIN_NOTIFICATION_TIME,
                                             s_state.logged_in_user_icon_url, user->display_name, std::move(summary),
                                             RA_LOGO_ICON_NAME, FullscreenUI::AchievementNotificationNoteType::Image);
  }
}

const std::string& Achievements::GetLoggedInUserName()
{
  return s_state.logged_in_username;
}

const std::string& Achievements::GetLoggedInUserIconURL()
{
  return s_state.logged_in_user_icon_url;
}

SmallString Achievements::GetLoggedInUserPointsSummary()
{
  SmallString ret;

  const rc_client_user_t* user = rc_client_get_user_info(s_state.client);
  if (!user) [[unlikely]]
    return ret;

  //: Score summary, shown in Big Picture mode.
  ret.format(TRANSLATE_FS("Achievements", "Score: {} ({} softcore)"), user->score, user->score_softcore);
  return ret;
}

std::string Achievements::GetProfileURL(std::string_view username)
{
  return fmt::format("https://retroachievements.org/user/{}", Path::URLEncode(username));
}

u32 Achievements::GetPauseThrottleFrames()
{
  if (!IsActive())
    return 0;

  const auto lock = GetLock();
  if (!IsHardcoreModeActive())
    return 0;

  u32 frames_remaining = 0;
  return rc_client_can_pause(s_state.client, &frames_remaining) ? 0 : frames_remaining;
}

u32 Achievements::GetPendingUnlockCount()
{
  if (!IsActive())
    return 0;

  const auto lock = GetLock();
  return rc_client_get_award_achievement_pending_count(s_state.client);
}

void Achievements::Logout()
{
  const auto lock = GetLock();

  if (IsActive())
  {
    if (HasActiveGame())
    {
      ClearGameInfo();
      DisableHardcoreMode(false, false);
    }

    CancelFetchGameListRequest();
    CancelFetchAllProgressRequest();

    INFO_LOG("Logging out...");
    rc_client_logout(s_state.client);
    s_state.logged_in_username = {};
    s_state.logged_in_user_icon_url = {};
  }

  INFO_LOG("Clearing credentials...");
  s_state.has_saved_credentials = false;
  Core::DeleteBaseSettingValue("Cheevos", "Username");
  Core::DeleteBaseSettingValue("Cheevos", "Token");
  Core::DeleteBaseSettingValue("Cheevos", "LoginTimestamp");
  Host::CommitBaseSettingChanges();

  ClearProgressDatabase();
}

void Achievements::ConfirmHardcoreModeDisableAsync(std::string_view trigger, std::function<void(bool)> callback)
{
  Host::ConfirmMessageAsync(
    RA_LOGO_SVG_ICON_NAME, TRANSLATE_STR("Achievements", "Confirm Hardcore Mode Disable"),
    fmt::format(TRANSLATE_FS("Achievements", "{0} cannot be performed while hardcore mode is active. Do you want to "
                                             "disable hardcore mode? {0} will be cancelled if you select No."),
                trigger),
    [callback = std::move(callback)](bool res) mutable {
      // don't run the callback in the middle of rendering the UI
      Host::RunOnCoreThread([callback = std::move(callback), res]() {
        if (res)
          DisableHardcoreMode(true, true);
        if (callback)
          callback(res);
      });
    });
}

#if defined(_WIN32)
#include "common/windows_headers.h"
#elif !defined(__ANDROID__)
#include <unistd.h>
#endif

#include "common/thirdparty/SmallVector.h"
#include "common/thirdparty/aes.h"

#ifndef __ANDROID__

static TinyString GetLoginEncryptionMachineKey()
{
  TinyString ret;

#ifdef _WIN32
  HKEY hKey;
  DWORD error;
  if ((error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ, &hKey)) !=
      ERROR_SUCCESS)
  {
    WARNING_LOG("Open SOFTWARE\\Microsoft\\Cryptography failed for machine key failed: {}", error);
    return ret;
  }

  DWORD machine_guid_length;
  if ((error = RegGetValueA(hKey, NULL, "MachineGuid", RRF_RT_REG_SZ, NULL, NULL, &machine_guid_length)) !=
      ERROR_SUCCESS)
  {
    WARNING_LOG("Get MachineGuid failed: {}", error);
    RegCloseKey(hKey);
    return ret;
  }

  ret.resize(machine_guid_length);
  if ((error = RegGetValueA(hKey, NULL, "MachineGuid", RRF_RT_REG_SZ, NULL, ret.data(), &machine_guid_length)) !=
        ERROR_SUCCESS ||
      machine_guid_length <= 1)
  {
    WARNING_LOG("Read MachineGuid failed: {}", error);
    ret = {};
    RegCloseKey(hKey);
    return ret;
  }

  ret.resize(machine_guid_length);
  RegCloseKey(hKey);
#else
#if defined(__linux__)
  // use /etc/machine-id on Linux
  std::optional<std::string> machine_id = FileSystem::ReadFileToString("/etc/machine-id");
  if (machine_id.has_value())
    ret = std::string_view(machine_id.value());
#elif defined(__APPLE__)
  // use gethostuuid(2) on macOS
  const struct timespec ts{};
  uuid_t uuid{};
  if (gethostuuid(uuid, &ts) == 0)
    ret.append_hex(uuid, sizeof(uuid), false);
#endif

  if (ret.empty())
  {
    WARNING_LOG("Falling back to gethostid()");

    // fallback to POSIX gethostid()
    const long hostid = gethostid();
    ret.format("{:08X}", hostid);
  }
#endif

  return ret;
}

#endif

static std::array<u8, 32> GetLoginEncryptionKey(std::string_view username)
{
  // super basic key stretching
  static constexpr u32 EXTRA_ROUNDS = 100;

  SHA256Digest digest;

#ifndef __ANDROID__
  // Only use machine key if we're not running in portable mode.
  if (!EmuFolders::IsRunningInPortableMode())
  {
    const TinyString machine_key = GetLoginEncryptionMachineKey();
    if (!machine_key.empty())
      digest.Update(machine_key.cbspan());
    else
      WARNING_LOG("Failed to get machine key, token will be decipherable.");
  }
#endif

  // salt with username
  digest.Update(username.data(), username.length());

  std::array<u8, 32> key = digest.Final();

  for (u32 i = 0; i < EXTRA_ROUNDS; i++)
    key = SHA256Digest::GetDigest(key);

  return key;
}

TinyString Achievements::EncryptLoginToken(std::string_view token, std::string_view username)
{
  TinyString ret;
  if (token.empty() || username.empty())
    return ret;

  const auto key = GetLoginEncryptionKey(username);
  std::array<u32, AES_KEY_SCHEDULE_SIZE> key_schedule;
  aes_key_setup(&key[0], key_schedule.data(), 128);

  // has to be padded to the block size
  llvm::SmallVector<u8, 64> data(reinterpret_cast<const u8*>(token.data()),
                                 reinterpret_cast<const u8*>(token.data() + token.length()));
  data.resize(Common::AlignUpPow2(token.length(), AES_BLOCK_SIZE), 0);
  aes_encrypt_cbc(data.data(), data.size(), data.data(), key_schedule.data(), 128, &key[16]);

  // base64 encode it
  const std::span<const u8> data_span(data.data(), data.size());
  ret.resize(static_cast<u32>(StringUtil::EncodedBase64Length(data_span)));
  StringUtil::EncodeBase64(ret.span(), data_span);
  return ret;
}

TinyString Achievements::DecryptLoginToken(std::string_view encrypted_token, std::string_view username)
{
  TinyString ret;
  if (encrypted_token.empty() || username.empty())
    return ret;

  const size_t encrypted_data_length = StringUtil::DecodedBase64Length(encrypted_token);
  if (encrypted_data_length == 0 || (encrypted_data_length % AES_BLOCK_SIZE) != 0)
    return ret;

  const auto key = GetLoginEncryptionKey(username);
  std::array<u32, AES_KEY_SCHEDULE_SIZE> key_schedule;
  aes_key_setup(&key[0], key_schedule.data(), 128);

  // has to be padded to the block size
  llvm::SmallVector<u8, 64> encrypted_data;
  encrypted_data.resize(encrypted_data_length);
  if (StringUtil::DecodeBase64(std::span<u8>(encrypted_data.data(), encrypted_data.size()), encrypted_token) !=
      encrypted_data_length)
  {
    WARNING_LOG("Failed to base64 decode encrypted login token.");
    return ret;
  }

  aes_decrypt_cbc(encrypted_data.data(), encrypted_data.size(), encrypted_data.data(), key_schedule.data(), 128,
                  &key[16]);

  // remove any trailing null bytes
  const size_t real_length =
    StringUtil::Strnlen(reinterpret_cast<const char*>(encrypted_data.data()), encrypted_data_length);
  ret.append(reinterpret_cast<const char*>(encrypted_data.data()), static_cast<u32>(real_length));
  return ret;
}

bool Achievements::RefreshGameList(ProgressCallback* progress, Error* error)
{
  auto lock = GetLock();

  Error fetch_error;
  if (!BeginFetchGameListRequest(&fetch_error))
    return false;

  // refresh in progress
  progress->SetStatusText(TRANSLATE_SV("Achievements", "Refreshing game database..."));
  progress->SetProgressRange(0);
  progress->SetProgressValue(0);
  while (s_state.fetch_game_list_request)
    WaitForServerCallsWithYield(lock);

  if (fetch_error.IsValid())
  {
    ERROR_LOG("Failed to refresh game database: {}", fetch_error.GetDescription());
    if (error)
      *error = std::move(fetch_error);
    return false;
  }

  INFO_LOG("Successfully refreshed game database.");
  return true;
}

bool Achievements::BeginFetchGameListRequest(Error* error)
{
  if (!IsLoggedIn())
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "User is not logged in."));
    return false;
  }

  if (s_state.fetch_game_list_request)
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "Game database is already being updated."));
    return false;
  }

  s_state.fetch_game_list_request =
    rc_client_begin_fetch_game_list(s_state.client, RC_CONSOLE_PLAYSTATION, FetchGameListCallback, error);
  if (!s_state.fetch_game_list_request)
  {
    Error::SetStringView(error, "rc_client_begin_fetch_game_list() failed");
    return false;
  }

  return true;
}

void Achievements::CancelFetchGameListRequest()
{
  if (s_state.fetch_game_list_request)
  {
    rc_client_abort_async(s_state.client, s_state.fetch_game_list_request);
    s_state.fetch_game_list_request = nullptr;
  }
}

void Achievements::FetchGameListCallback(int result, const char* error_message, rc_client_game_list_t* game_list,
                                         rc_client_t* client, void* callback_userdata)
{
  s_state.fetch_game_list_request = nullptr;

  Error* error = static_cast<Error*>(callback_userdata);
  if (result != RC_OK)
  {
    ERROR_LOG("Fetch game list callback returned error: {}: {}", rc_error_str(result), error_message);
    Error::SetStringFmt(error, "{}: {}\n{}", TRANSLATE_SV("Achievements", "Refresh game list failed"),
                        rc_error_str(result), error_message);
    return;
  }

  WriteGameListToDatabase(game_list, error);
  rc_client_destroy_game_list(game_list);

  Host::RunOnCoreThread(&GameList::UpdateAllAchievementData);
}

bool Achievements::RefreshAllProgressDatabase(ProgressCallback* progress, Error* error)
{
  auto lock = GetLock();

  Error fetch_error;
  if (!BeginFetchAllProgressRequest(&fetch_error))
    return false;

  // refresh in progress
  progress->SetStatusText(TRANSLATE_SV("Achievements", "Refreshing achievement progress..."));
  progress->SetProgressRange(0);
  progress->SetProgressValue(0);
  while (s_state.fetch_all_progress_request)
    WaitForServerCallsWithYield(lock);

  if (fetch_error.IsValid())
  {
    ERROR_LOG("Failed to refresh progress database: {}", fetch_error.GetDescription());
    if (error)
      *error = std::move(fetch_error);
    return false;
  }

  INFO_LOG("Successfully refreshed progress database.");
  return true;
}

bool Achievements::BeginFetchAllProgressRequest(Error* error)
{
  if (!IsLoggedIn())
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "User is not logged in."));
    return false;
  }

  if (s_state.fetch_all_progress_request)
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "Progress is already being updated."));
    return false;
  }

  s_state.fetch_all_progress_request =
    rc_client_begin_fetch_all_user_progress(s_state.client, RC_CONSOLE_PLAYSTATION, FetchAllProgressCallback, error);
  if (!s_state.fetch_all_progress_request)
  {
    Error::SetStringView(error, "rc_client_begin_fetch_all_user_progress() failed");
    return false;
  }

  return true;
}

void Achievements::CancelFetchAllProgressRequest()
{
  if (s_state.fetch_all_progress_request)
  {
    rc_client_abort_async(s_state.client, s_state.fetch_all_progress_request);
    s_state.fetch_all_progress_request = nullptr;
  }
}

void Achievements::FetchAllProgressCallback(int result, const char* error_message, rc_client_all_user_progress_t* list,
                                            rc_client_t* client, void* callback_userdata)
{
  s_state.fetch_all_progress_request = nullptr;

  Error* error = static_cast<Error*>(callback_userdata);
  if (result != RC_OK)
  {
    ERROR_LOG("Fetch all progress callback returned error: {}: {}", rc_error_str(result), error_message);
    Error::SetStringFmt(error, "{}: {}\n{}", TRANSLATE_SV("Achievements", "Refresh all progress failed"),
                        rc_error_str(result), error_message);
    return;
  }

  WriteAllProgressToDatabase(list, error);
  rc_client_destroy_all_user_progress(list);

  Host::RunOnCoreThread(&GameList::UpdateAllAchievementData);
}

bool Achievements::EnsureAchievementsDatabaseOpen(Error* error)
{
  if (s_state.achievements_db) [[likely]]
    return true;
  else if (!g_dyn_sqlite.Open(error)) [[unlikely]]
    return false;

  const std::string path = Path::Combine(EmuFolders::Cache, "achievements.db");
  Error lerror;

  if (!(s_state.achievements_db = SQLiteHelpers::OpenAndCheckDatabase(path.c_str(), &lerror))) [[unlikely]]
  {
    ERROR_LOG("Failed to open achievements database: {}", lerror.GetDescription());
    return false;
  }

  // Use memory journal to avoid spamming files on disk.
  g_dyn_sqlite.sqlite3_exec(s_state.achievements_db, "PRAGMA synchronous=NORMAL;PRAGMA journal_mode=memory;", nullptr,
                            nullptr, nullptr);

  static constexpr const char* schema_sql = R"(
CREATE TABLE IF NOT EXISTS hashes (
  hash BLOB NOT NULL PRIMARY KEY,
  game_id INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS games (
  game_id INTEGER NOT NULL PRIMARY KEY,
  title TEXT NOT NULL,
  badge_url TEXT NOT NULL,
  num_achievements INTEGER NOT NULL,
  num_leaderboards INTEGER NOT NULL,
  num_points INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS progress (
  game_id INTEGER NOT NULL PRIMARY KEY,
  num_unlocked INTEGER NOT NULL DEFAULT 0,
  num_hc_unlocked INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS metadata (
  key TEXT NOT NULL PRIMARY KEY,
  value TEXT NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS pinned_achievements (
  game_id INTEGER NOT NULL,
  achievement_id INTEGER NOT NULL,
  PRIMARY KEY (game_id, achievement_id)
);
CREATE INDEX IF NOT EXISTS idx_pinned_achievements_game_id ON pinned_achievements (game_id);
)";

  if (!SQLiteHelpers::Execute(s_state.achievements_db, schema_sql, &lerror))
  {
    ERROR_LOG("Failed to create achievements database schema: {}", lerror.GetDescription());
    if (error)
      *error = std::move(lerror);
    CloseAchievementsDatabase();
    return false;
  }

  if (!s_state.badge_lookup_stmt.Prepare(s_state.achievements_db, "SELECT badge_url FROM games WHERE game_id = ?;",
                                         &lerror) ||
      !s_state.update_progress_stmt.Prepare(
        s_state.achievements_db,
        "INSERT INTO progress (game_id, num_unlocked, num_hc_unlocked) VALUES (?, ?, ?)"
        " ON CONFLICT(game_id) DO UPDATE SET num_unlocked=excluded.num_unlocked,"
        " num_hc_unlocked=excluded.num_hc_unlocked;",
        &lerror))
  {
    ERROR_LOG("Failed to create prepared statements: {}", lerror.GetDescription());
    if (error)
      *error = std::move(lerror);
    CloseAchievementsDatabase();
    return false;
  }

  // Seed the hashes table from the bundled YAML if it's empty (first run or DB was wiped).
  if (SQLitePreparedStatement count_stmt; count_stmt.Prepare(s_state.achievements_db, "SELECT COUNT(*) FROM hashes;") &&
                                          count_stmt.Step() == SQLITE_ROW && count_stmt.ColumnInt(0) == 0)
  {
    if (!CreateGameDatabaseFromSeedDatabase(&lerror))
      WARNING_LOG("Failed to seed hash database from YAML: {}", lerror.GetDescription());
  }

  return true;
}

void Achievements::CloseAchievementsDatabase()
{
  if (!s_state.achievements_db)
    return;

  s_state.update_progress_stmt.Destroy();
  s_state.badge_lookup_stmt.Destroy();

  g_dyn_sqlite.sqlite3_close(s_state.achievements_db);
  s_state.achievements_db = nullptr;
}

std::string Achievements::GetAchievementsDatabaseMetadata(std::string_view key)
{
  if (!EnsureAchievementsDatabaseOpen())
    return {};

  Error error;
  SQLitePreparedStatement stmt;
  if (!stmt.Prepare(s_state.achievements_db, "SELECT value FROM metadata WHERE key = ?;", &error)) [[unlikely]]
  {
    ERROR_LOG("Failed to prepare database metadata query: {}", error.GetDescription());
    return {};
  }

  stmt.BindText(1, key);
  const int rc = stmt.Step();
  if (rc != SQLITE_ROW)
  {
    if (rc != SQLITE_DONE) [[unlikely]]
    {
      SQLiteHelpers::SetError(&error, s_state.achievements_db);
      ERROR_LOG("Failed to execute database metadata query: {}", error.GetDescription());
    }

    return {};
  }

  return std::string(stmt.ColumnText(0));
}

bool Achievements::SetAchievementsDatabaseMetadata(std::string_view key, std::string_view value, Error* error)
{
  if (!EnsureAchievementsDatabaseOpen())
    return {};

  SQLitePreparedStatement stmt;
  if (!stmt.Prepare(
        s_state.achievements_db,
        "INSERT INTO metadata (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;", error))
    [[unlikely]]
  {
    return false;
  }

  stmt.BindText(1, key);
  stmt.BindText(2, value);
  return stmt.Execute(s_state.achievements_db, error);
}

bool Achievements::CreateGameDatabaseFromSeedDatabase(Error* error)
{
  Timer timer;

  std::optional<std::string> yaml_data = Host::ReadResourceFileToString("achievement_hashlib.yaml", false, error);
  if (!yaml_data.has_value())
  {
    Error::SetStringView(error, "Seed database is missing.");
    return false;
  }

  const ryml::Tree yaml = ryml::parse_in_place(
    to_csubstr("achievement_hashlib.yaml"), c4::substr(reinterpret_cast<char*>(yaml_data->data()), yaml_data->size()));
  const ryml::ConstNodeRef root = yaml.rootref();
  if (root.empty())
  {
    Error::SetStringView(error, "Seed database is empty.");
    return false;
  }

  INFO_LOG("Parsed {} entries in seed database in {} ms", root.num_children(), timer.GetTimeMillisecondsAndReset());

  // Replace all hash entries atomically.
  if (!SQLiteHelpers::BeginTransaction(s_state.achievements_db, error) ||
      !SQLiteHelpers::Execute(s_state.achievements_db, "DELETE FROM hashes;", error) ||
      !SQLiteHelpers::Execute(s_state.achievements_db, "DELETE FROM games;", error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  SQLitePreparedStatement hash_stmt, game_stmt;
  if (!game_stmt.Prepare(s_state.achievements_db,
                         "INSERT INTO games (game_id, title, badge_url, num_achievements, num_leaderboards, "
                         "num_points) VALUES (?, ?, ?, ?, ?, ?);",
                         error) ||
      !hash_stmt.Prepare(s_state.achievements_db, "INSERT INTO hashes (hash, game_id) VALUES (?, ?);", error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  u32 num_games = 0;
  u32 num_hashes = 0;
  std::string badge_name, badge_url;
  for (const ryml::ConstNodeRef& current : root.cchildren())
  {
    const std::optional<u32> game_id = StringUtil::FromChars<u32>(to_stringview(current.key()));
    std::string_view title;
    u32 num_achievements, num_leaderboards, num_points;
    if (!game_id.has_value() || !GetStringFromObject(current, "title", &title) ||
        !GetStringFromObject(current, "badgeName", &badge_name) ||
        !GetIntFromObject(current, "achievements", &num_achievements) ||
        !GetIntFromObject(current, "leaderboards", &num_leaderboards) ||
        !GetIntFromObject(current, "points", &num_points))
    {
      WARNING_LOG("Invalid game {} in hash database", to_stringview(current.key()));
      continue;
    }

    // inlined to avoid an extra allocation
    const rc_api_fetch_image_request_t badge_url_request = {.image_name = badge_name.c_str(),
                                                            .image_type = RC_IMAGE_TYPE_GAME};
    rc_api_request_t badge_url_apirequest;
    if (rc_api_init_fetch_image_request(&badge_url_apirequest, &badge_url_request) == RC_OK)
      badge_url = badge_url_apirequest.url;
    else
      badge_url.clear();
    rc_api_destroy_request(&badge_url_apirequest);

    game_stmt.BindInt(1, static_cast<int>(game_id.value()));
    game_stmt.BindText(2, title);
    game_stmt.BindText(3, badge_url);
    game_stmt.BindInt(4, static_cast<int>(num_achievements));
    game_stmt.BindInt(5, static_cast<int>(num_leaderboards));
    game_stmt.BindInt(6, static_cast<int>(num_points));

    if (!game_stmt.Execute(s_state.achievements_db, error)) [[unlikely]]
    {
      SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
      return false;
    }

    game_stmt.Reset();
    num_games++;

    if (const ryml::ConstNodeRef hashes = current.find_child(to_csubstr("hashes")); hashes.valid())
    {
      for (const ryml::ConstNodeRef& hash_node : hashes.cchildren())
      {
        GameHash hash;
        if (const std::string_view hash_str = to_stringview(hash_node.val());
            StringUtil::DecodeHex(hash, hash_str) != GAME_HASH_LENGTH)
        {
          WARNING_LOG("Invalid hash '{}' in game ID {}", hash_str, game_id.value());
          continue;
        }

        hash_stmt.BindBlob(1, hash);
        hash_stmt.BindInt(2, static_cast<int>(game_id.value()));
        if (!hash_stmt.Execute(s_state.achievements_db, error)) [[unlikely]]
        {
          SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
          return false;
        }

        hash_stmt.Reset();
        num_hashes++;
      }
    }
    else
    {
      WARNING_LOG("Game {} in hash database is missing hashes", to_stringview(current.key()));
    }
  }

  if (!SQLiteHelpers::CommitTransaction(s_state.achievements_db, error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  INFO_LOG("Wrote {} games and {} hashes to achievement database in {} ms", num_games, num_hashes,
           timer.GetTimeMilliseconds());
  return true;
}

void Achievements::UpdateGameDatabaseFromCurrentGame()
{
  if (s_state.game_id == 0 || !s_state.game_hash.has_value() || !EnsureAchievementsDatabaseOpen())
    return;

  // update hash
  Error error;
  SQLitePreparedStatement stmt;
  if (!stmt.Prepare(s_state.achievements_db, "SELECT game_id FROM hashes WHERE hash = ?;", &error))
  {
    ERROR_LOG("Failed to prepare hash query statement: {}", error.GetDescription());
    return;
  }

  stmt.BindBlob(1, s_state.game_hash.value());
  int step = stmt.Step();
  if (step != SQLITE_ROW && step != SQLITE_DONE)
  {
    SQLiteHelpers::SetError(&error, s_state.achievements_db);
    ERROR_LOG("Failed to execute hash query statement: {}", error.GetDescription());
    return;
  }

  if (step != SQLITE_ROW || static_cast<u32>(stmt.ColumnInt(0)) != s_state.game_id)
  {
    INFO_LOG("Updating game ID for hash {} to {} in database", GameHashToString(s_state.game_hash), s_state.game_id);
    if (!stmt.Prepare(
          s_state.achievements_db,
          "INSERT INTO hashes (hash, game_id) VALUES (?, ?) ON CONFLICT(hash) DO UPDATE SET game_id=excluded.game_id;",
          &error))
    {
      ERROR_LOG("Failed to prepare hash update statement: {}", error.GetDescription());
      return;
    }

    stmt.Reset();
    stmt.BindBlob(1, s_state.game_hash.value());
    stmt.BindInt(2, static_cast<int>(s_state.game_id));
    if (!stmt.Execute(s_state.achievements_db, &error))
    {
      ERROR_LOG("Failed to execute hash update statement: {}", error.GetDescription());
      return;
    }
  }
  stmt.Reset();

  // update game details
  const rc_client_game_t* ginfo = rc_client_get_game_info(s_state.client);
  DebugAssert(ginfo);
  rc_client_user_game_summary_t gsummary;
  rc_client_get_user_game_summary(s_state.client, &gsummary);

  // kinda horrible...
  u32 num_leaderbords = 0;
  if (s_state.has_leaderboards)
  {
    rc_client_leaderboard_list_t* lbinfo =
      rc_client_create_leaderboard_list(s_state.client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
    for (const rc_client_leaderboard_bucket_t& lbb :
         std::span<const rc_client_leaderboard_bucket_t>(lbinfo->buckets, lbinfo->num_buckets))
    {
      num_leaderbords += lbb.num_leaderboards;
    }
    rc_client_destroy_leaderboard_list(lbinfo);
  }

  // only update if it's unchanged
  if (!stmt.Prepare(s_state.achievements_db,
                    "SELECT title, badge_url, num_achievements, num_leaderboards, num_points FROM games WHERE "
                    "game_id = ?;",
                    &error))
  {
    ERROR_LOG("Failed to prepare game query statement: {}", error.GetDescription());
    return;
  }

  stmt.BindInt(1, static_cast<int>(s_state.game_id));
  step = stmt.Step();
  if (step != SQLITE_DONE && step != SQLITE_ROW)
  {
    SQLiteHelpers::SetError(&error, s_state.achievements_db);
    ERROR_LOG("Failed to execute game query statement: {}", error.GetDescription());
    return;
  }

  const std::string_view ginfo_title = ginfo->title ? std::string_view(ginfo->title) : std::string_view();
  const std::string_view ginfo_badge_url =
    ginfo->badge_url ? std::string_view(ginfo->badge_url) : s_state.game_badge_url;
  if (step != SQLITE_ROW || (!ginfo_title.empty() && stmt.ColumnText(0) != ginfo_title) ||
      (!ginfo_badge_url.empty() && stmt.ColumnText(1) != ginfo_badge_url) ||
      stmt.ColumnInt(2) != static_cast<int>(gsummary.num_core_achievements) ||
      stmt.ColumnInt(3) != static_cast<int>(num_leaderbords) ||
      stmt.ColumnInt(4) != static_cast<int>(gsummary.points_core))
  {
#if 0
    // For debugging
    const std::string_view db_title = stmt.ColumnText(0);
    const std::string_view db_badge_url = stmt.ColumnText(1);
    const int db_achievements = stmt.ColumnInt(2);
    const int db_leaderboards = stmt.ColumnInt(3);
    const int db_points = stmt.ColumnInt(4);
#endif

    INFO_LOG("Updating game details for game ID {} ({}) in database", s_state.game_id, ginfo_title);

    stmt.Reset();
    if (!stmt.Prepare(s_state.achievements_db,
                      "INSERT INTO games (game_id, title, badge_url, num_achievements, num_leaderboards, num_points) "
                      "VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(game_id) DO UPDATE SET title=excluded.title, "
                      "badge_url=excluded.badge_url, num_achievements=excluded.num_achievements, "
                      "num_leaderboards=excluded.num_leaderboards, num_points=excluded.num_points;",
                      &error))
    {
      ERROR_LOG("Failed to prepare game update statement: {}", error.GetDescription());
      return;
    }

    stmt.BindInt(1, static_cast<int>(s_state.game_id));
    stmt.BindText(2, ginfo_title);
    stmt.BindText(3, ginfo_badge_url);
    stmt.BindInt(4, static_cast<int>(gsummary.num_core_achievements));
    stmt.BindInt(5, static_cast<int>(num_leaderbords));
    stmt.BindInt(6, static_cast<int>(gsummary.points_core));
    if (!stmt.Execute(s_state.achievements_db, &error))
    {
      ERROR_LOG("Failed to execute game update statement: {}", error.GetDescription());
      return;
    }
  }
  else
  {
    DEV_LOG("No update needed for game ID {} ({}) in database", s_state.game_id, ginfo_title);
  }
}

void Achievements::FetchGameListIfOutdated()
{
  if (!EnsureAchievementsDatabaseOpen())
    return;

  std::optional<s64> last_updated =
    StringUtil::FromChars<s64>(GetAchievementsDatabaseMetadata(GAME_LIST_LAST_UPDATED_METADATA_KEY));
  const s64 current_time = static_cast<s64>(std::time(nullptr));
  const s64 max_age = current_time - GAME_LIST_MAX_AGE_SECONDS;
  if (last_updated.value_or(0) < max_age)
  {
    INFO_LOG("Game list is outdated, refreshing...");
    if (Error error; !BeginFetchGameListRequest(&error))
      ERROR_LOG("Failed to refresh game list: {}", error.GetDescription());
  }
  else
  {
    DEV_LOG("Game list is up to date (last updated {} seconds ago)", current_time - last_updated.value_or(0));
  }
}

bool Achievements::WriteGameListToDatabase(const rc_client_game_list_t* game_list, Error* error)
{
  if (!EnsureAchievementsDatabaseOpen(error))
    return false;

  Timer timer;

  // Replace all hash entries atomically.
  if (!SQLiteHelpers::BeginTransaction(s_state.achievements_db, error) ||
      !SQLiteHelpers::Execute(s_state.achievements_db, "DELETE FROM hashes;", error) ||
      !SQLiteHelpers::Execute(s_state.achievements_db, "DELETE FROM games;", error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  SQLitePreparedStatement hash_stmt, game_stmt;
  if (!game_stmt.Prepare(s_state.achievements_db,
                         "INSERT INTO games (game_id, title, badge_url, num_achievements, num_leaderboards, "
                         "num_points) VALUES (?, ?, ?, ?, ?, ?);",
                         error) ||
      !hash_stmt.Prepare(s_state.achievements_db, "INSERT INTO hashes (hash, game_id) VALUES (?, ?);", error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  u32 num_games = 0;
  u32 num_hashes = 0;
  for (const rc_client_game_list_entry_t& entry :
       std::span<const rc_client_game_list_entry_t>(game_list->entries, game_list->num_entries))
  {
    const std::string_view name = entry.name ? std::string_view(entry.name) : std::string_view();

    // ignore games which don't have any hashes, since they won't resolve
    if (entry.num_supported_hashes == 0)
    {
      WARNING_LOG("Skipping game ID {} ({}) in hash database since it has no supported hashes", entry.id, name);
      continue;
    }

    game_stmt.BindInt(1, static_cast<int>(entry.id));
    game_stmt.BindText(2, name);
    game_stmt.BindText(3, entry.image_url ? std::string_view(entry.image_url) : std::string_view());
    game_stmt.BindInt(4, static_cast<int>(entry.num_achievements));
    game_stmt.BindInt(5, static_cast<int>(entry.num_leaderboards));
    game_stmt.BindInt(6, static_cast<int>(entry.points));

    if (!game_stmt.Execute(s_state.achievements_db, error)) [[unlikely]]
    {
      SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
      return false;
    }

    game_stmt.Reset();
    num_games++;

    for (const char* hash_str : std::span<const char*>(entry.supported_hashes, entry.num_supported_hashes))
    {
      GameHash hash;
      if (StringUtil::DecodeHex(hash, hash_str) != GAME_HASH_LENGTH)
      {
        WARNING_LOG("Invalid hash '{}' in game ID {}", hash_str, entry.id);
        continue;
      }

      hash_stmt.BindBlob(1, hash);
      hash_stmt.BindInt(2, static_cast<int>(entry.id));
      if (!hash_stmt.Execute(s_state.achievements_db, error)) [[unlikely]]
      {
        SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
        return false;
      }

      hash_stmt.Reset();
      num_hashes++;
    }
  }

  if (!SetAchievementsDatabaseMetadata(GAME_LIST_LAST_UPDATED_METADATA_KEY,
                                       StringUtil::ToChars(static_cast<s64>(std::time(nullptr))), error))
  {
    return false;
  }

  if (!SQLiteHelpers::CommitTransaction(s_state.achievements_db, error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  INFO_LOG("Wrote {} games and {} hashes to achievement game database in {} ms", num_games, num_hashes,
           timer.GetTimeMilliseconds());
  return true;
}

void Achievements::FetchAllProgressIfMissing()
{
  if (!EnsureAchievementsDatabaseOpen())
    return;

  SQLitePreparedStatement stmt;
  if (!stmt.Prepare(s_state.achievements_db, "SELECT COUNT(*) FROM progress;") || stmt.Step() != SQLITE_ROW)
    return;

  const bool has_progress_database = (stmt.ColumnInt(0) > 0);
  if (has_progress_database)
  {
    DEV_LOG("Progress database already exists, skipping fetch");
    return;
  }

  INFO_LOG("Progress database is missing, updating...");
  BeginFetchAllProgressRequest(nullptr);
}

bool Achievements::WriteAllProgressToDatabase(const rc_client_all_user_progress_t* allprog, Error* error)
{
  if (!EnsureAchievementsDatabaseOpen(error))
    return false;

  if (!SQLiteHelpers::BeginTransaction(s_state.achievements_db, error) ||
      !SQLiteHelpers::Execute(s_state.achievements_db, "DELETE FROM progress;", error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  SQLitePreparedStatement stmt;
  if (!stmt.Prepare(s_state.achievements_db,
                    "INSERT INTO progress (game_id, num_unlocked, num_hc_unlocked) VALUES (?, ?, ?);", error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  u32 written = 0;
  for (const rc_client_all_user_progress_entry_t& entry :
       std::span<const rc_client_all_user_progress_entry_t>(allprog->entries, allprog->num_entries))
  {
    if ((entry.num_unlocked_achievements + entry.num_unlocked_achievements_hardcore) == 0)
      continue;

    stmt.BindInt(1, static_cast<int>(entry.game_id));
    stmt.BindInt(2, static_cast<int>(entry.num_unlocked_achievements));
    stmt.BindInt(3, static_cast<int>(entry.num_unlocked_achievements_hardcore));

    if (!stmt.Execute(s_state.achievements_db, error))
    {
      SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
      return false;
    }

    stmt.Reset();
    written++;
  }

  if (!SQLiteHelpers::CommitTransaction(s_state.achievements_db, error))
  {
    SQLiteHelpers::RollbackTransaction(s_state.achievements_db);
    return false;
  }

  INFO_LOG("Wrote {} games to progress database", written);
  return true;
}

void Achievements::UpdateProgressDatabaseFromCurrentGame()
{
  // don't write updates in spectator mode
  if (s_state.game_id == 0 || rc_client_get_spectator_mode_enabled(s_state.client))
    return;

  // query list to get both hardcore and softcore counts
  rc_client_achievement_list_t* const achievements =
    rc_client_create_achievement_list(s_state.client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, 0);
  u32 num_achievements = 0;
  u32 achievements_unlocked = 0;
  u32 achievements_unlocked_hardcore = 0;
  if (achievements)
  {
    for (const rc_client_achievement_bucket_t& bucket :
         std::span<const rc_client_achievement_bucket_t>(achievements->buckets, achievements->num_buckets))
    {
      for (const rc_client_achievement_t* achievement :
           std::span<const rc_client_achievement_t*>(bucket.achievements, bucket.num_achievements))
      {
        achievements_unlocked += BoolToUInt32((achievement->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) != 0);
        achievements_unlocked_hardcore +=
          BoolToUInt32((achievement->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) != 0);
      }

      num_achievements += bucket.num_achievements;
    }
    rc_client_destroy_achievement_list(achievements);
  }

  // update the game list, this should be fairly quick
  if (s_state.game_hash.has_value())
  {
    Host::RunOnCoreThread([game_hash = s_state.game_hash.value(), game_id = s_state.game_id, num_achievements,
                           achievements_unlocked, achievements_unlocked_hardcore]() {
      GameList::UpdateAchievementData(game_hash, game_id, num_achievements, achievements_unlocked,
                                      achievements_unlocked_hardcore);
    });
  }

  // Write progress synchronously, single-row upsert is fast and we already hold the mutex.
  // Skip if both counters are zero (e.g. immediately after a game reset).
  // TODO: Is this true?
  if ((achievements_unlocked > 0 || achievements_unlocked_hardcore > 0) && EnsureAchievementsDatabaseOpen())
  {
    s_state.update_progress_stmt.BindInt(1, static_cast<int>(s_state.game_id));
    s_state.update_progress_stmt.BindInt(2, static_cast<int>(achievements_unlocked));
    s_state.update_progress_stmt.BindInt(3, static_cast<int>(achievements_unlocked_hardcore));

    if (Error error; !s_state.update_progress_stmt.Execute(s_state.achievements_db, &error))
    {
      ERROR_LOG("Failed to upsert progress entry for game {}: {}", s_state.game_id, error.GetDescription());
    }
    else
    {
      INFO_LOG("Updated game {} with {}/{} unlocked", s_state.game_id, achievements_unlocked,
               achievements_unlocked_hardcore);
    }

    s_state.update_progress_stmt.Reset();
  }
}

void Achievements::ClearProgressDatabase()
{
  if (!EnsureAchievementsDatabaseOpen())
    return;

  if (Error error; !SQLiteHelpers::Execute(s_state.achievements_db, "DELETE FROM progress;", &error))
    ERROR_LOG("Failed to clear progress database: {}", error.GetDescription());
  else
    INFO_LOG("Cleared progress database");

  Host::RunOnCoreThread(&GameList::UpdateAllAchievementData);
}

Achievements::ProgressDatabase::ProgressDatabase() = default;

Achievements::ProgressDatabase::~ProgressDatabase() = default;

bool Achievements::ProgressDatabase::Load(Error* error)
{
  const auto lock = Achievements::GetLock();

  if (!EnsureAchievementsDatabaseOpen(error))
    return false;

  Timer timer;

  // preallocate storage to reduce fragmentation
  SQLitePreparedStatement stmt;
  if (stmt.Prepare(s_state.achievements_db, "SELECT COUNT(*) FROM hashes") && stmt.Step() == SQLITE_ROW)
  {
    const int count = stmt.ColumnInt(0);
    if (count > 0)
      m_hashes.reserve(static_cast<size_t>(count));
  }
  stmt.Reset();
  if (stmt.Prepare(s_state.achievements_db, "SELECT COUNT(*) FROM games") && stmt.Step() == SQLITE_ROW)
  {
    const int count = stmt.ColumnInt(0);
    if (count > 0)
      m_entries.reserve(static_cast<size_t>(count));
  }
  stmt.Reset();

  if (!stmt.Prepare(s_state.achievements_db, "SELECT hash, game_id FROM hashes", error))
    return false;

  // read data into cache
  for (;;)
  {
    const int rc = stmt.Step();
    if (rc == SQLITE_DONE)
      break;
    if (rc != SQLITE_ROW) [[unlikely]]
    {
      SQLiteHelpers::SetError(error, s_state.achievements_db, "Error while reading rows: ");
      return false;
    }

    const std::span<const u8> hash_blob = stmt.ColumnBlobBytes(0);
    if (hash_blob.size() != GAME_HASH_LENGTH) [[unlikely]]
    {
      ERROR_LOG("Invalid hash blob size in progress database: {}", hash_blob.size());
      continue;
    }

    HashEntry& entry = m_hashes.emplace_back();
    std::memcpy(entry.hash.data(), hash_blob.data(), GAME_HASH_LENGTH);
    entry.game_id = static_cast<u32>(stmt.ColumnInt(1));
  }
  stmt.Reset();

  if (!stmt.Prepare(s_state.achievements_db,
                    "SELECT games.game_id, games.num_achievements, progress.num_unlocked, progress.num_hc_unlocked "
                    "FROM games LEFT JOIN progress ON games.game_id = progress.game_id",
                    error))
  {
    return false;
  }

  // read data into cache
  for (;;)
  {
    const int rc = stmt.Step();
    if (rc == SQLITE_DONE)
      break;
    if (rc != SQLITE_ROW) [[unlikely]]
    {
      SQLiteHelpers::SetError(error, s_state.achievements_db, "Error while reading rows: ");
      return false;
    }

    Entry& entry = m_entries.emplace_back();
    entry.game_id = static_cast<u32>(stmt.ColumnInt(0));
    entry.num_achievements = static_cast<u16>(stmt.ColumnInt(1));
    entry.num_achievements_unlocked = static_cast<u16>(stmt.ColumnInt(2));
    entry.num_hc_achievements_unlocked = static_cast<u16>(stmt.ColumnInt(3));
  }

  INFO_LOG("Loaded progress database with {} hashes and {} games in {} ms", m_hashes.size(), m_entries.size(),
           timer.GetTimeMilliseconds());

  // sort for quick lookup
  m_hashes.shrink_to_fit();
  std::sort(m_hashes.begin(), m_hashes.end(),
            [](const HashEntry& lhs, const HashEntry& rhs) { return (lhs.hash < rhs.hash); });
  m_entries.shrink_to_fit();
  std::sort(m_entries.begin(), m_entries.end(),
            [](const Entry& lhs, const Entry& rhs) { return (lhs.game_id < rhs.game_id); });

  return true;
}

const Achievements::ProgressDatabase::Entry* Achievements::ProgressDatabase::LookupHash(const GameHash& hash) const
{
  const auto hash_iter =
    std::lower_bound(m_hashes.begin(), m_hashes.end(), hash,
                     [](const HashEntry& entry, const GameHash& search) { return (entry.hash < search); });
  if (hash_iter == m_hashes.end() || hash_iter->hash != hash)
    return nullptr;

  const auto game_iter = std::lower_bound(m_entries.begin(), m_entries.end(), hash_iter->game_id,
                                          [](const Entry& entry, u32 search) { return (entry.game_id < search); });
  return (game_iter != m_entries.end() && game_iter->game_id == hash_iter->game_id) ? &(*game_iter) : nullptr;
}

std::string Achievements::GetGameBadgeURL(u32 game_id)
{
  const auto lock = GetLock();

  // don't allow use of achievement badges if we're not logged in to achievements
  if (!s_state.has_saved_credentials || !EnsureAchievementsDatabaseOpen())
    return {};

  std::string ret;
  s_state.badge_lookup_stmt.BindInt(1, static_cast<int>(game_id));
  if (s_state.badge_lookup_stmt.Step() == SQLITE_ROW)
  {
    if (const char* badge_url = s_state.badge_lookup_stmt.ColumnTextCStr(0))
      ret = badge_url;
  }
  s_state.badge_lookup_stmt.Reset();
  return ret;
}

void Achievements::LoadPinnedAchievements()
{
  if (!HasAchievements() || !EnsureAchievementsDatabaseOpen())
    return;

  Error error;
  SQLitePreparedStatement query_stmt;
  if (!query_stmt.Prepare(s_state.achievements_db, "SELECT achievement_id FROM pinned_achievements WHERE game_id = ?",
                          &error)) [[unlikely]]
  {
    ERROR_LOG("Failed to prepare pinned achievements query: {}", error.GetDescription());
    return;
  }

  query_stmt.BindInt(1, static_cast<int>(s_state.game_id));

  for (;;)
  {
    const int rc = query_stmt.Step();
    if (rc == SQLITE_DONE)
      break;
    if (rc != SQLITE_ROW) [[unlikely]]
    {
      SQLiteHelpers::SetError(&error, s_state.achievements_db);
      ERROR_LOG("Failed to execute pinned achievements query: {}", error.GetDescription());
      break;
    }

    const u32 achievement_id = static_cast<u32>(query_stmt.ColumnInt(0));
    const rc_client_achievement_t* achievement = rc_client_get_achievement_info(s_state.client, achievement_id);
    if (!achievement)
    {
      WARNING_LOG("Pinned achievement {} not found in game, unpinning", achievement_id);
      SetAchievementPinnedInDatabase(achievement_id, false);
      continue;
    }

    if (achievement->state != RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE)
    {
      WARNING_LOG("Pinned achievement {} is unlocked, unpinning", achievement_id);
      SetAchievementPinnedInDatabase(achievement_id, false);
      continue;
    }

    PinnedAchievementIndicator indicator;
    indicator.achievement_id = achievement_id;
    indicator.badge_url = GetAchievementBadgeURL(achievement, false);
    s_state.pinned_achievement_indicators.push_back(std::move(indicator));
  }

  std::sort(s_state.pinned_achievement_indicators.begin(), s_state.pinned_achievement_indicators.end(),
            [](const PinnedAchievementIndicator& lhs, const PinnedAchievementIndicator& rhs) {
              return (lhs.achievement_id < rhs.achievement_id);
            });

  DEV_LOG("Loaded {} pinned achievements for game {}", s_state.pinned_achievement_indicators.size(), s_state.game_id);
}

bool Achievements::IsAchievementPinned(u32 achievement_id)
{
  const auto it = std::lower_bound(
    s_state.pinned_achievement_indicators.begin(), s_state.pinned_achievement_indicators.end(), achievement_id,
    [](const PinnedAchievementIndicator& ind, u32 search) { return ind.achievement_id < search; });
  return (it != s_state.pinned_achievement_indicators.end() && it->achievement_id == achievement_id);
}

void Achievements::SetAchievementPinned(u32 achievement_id, bool pinned)
{
  const auto it = std::lower_bound(
    s_state.pinned_achievement_indicators.begin(), s_state.pinned_achievement_indicators.end(), achievement_id,
    [](const PinnedAchievementIndicator& ind, u32 search) { return ind.achievement_id < search; });
  const bool is_pinned = (it != s_state.pinned_achievement_indicators.end() && it->achievement_id == achievement_id);
  if (is_pinned == pinned)
    return;

  if (is_pinned)
  {
    DEV_LOG("Unpinning achievement {}", achievement_id);
    s_state.pinned_achievement_indicators.erase(it);

    SetAchievementPinnedInDatabase(achievement_id, false);
  }
  else
  {
    const rc_client_achievement_t* achievement = rc_client_get_achievement_info(s_state.client, achievement_id);
    if (!achievement)
    {
      WARNING_LOG("Achievement {} not found", achievement_id);
      return;
    }

    DEV_LOG("Pinning achievement {}", achievement_id);

    PinnedAchievementIndicator indicator;
    indicator.achievement_id = achievement_id;
    indicator.badge_url = GetAchievementBadgeURL(achievement, false);
    s_state.pinned_achievement_indicators.insert(it, std::move(indicator));

    // Hide progress indicator if it was set
    if (s_state.active_progress_indicator.has_value() &&
        s_state.active_progress_indicator->achievement->id == achievement_id)
    {
      DEV_COLOR_LOG(StrongYellow, "Clearing progress indicator for achievement {} due to pin", achievement_id);
      s_state.active_progress_indicator.reset();
    }

    SetAchievementPinnedInDatabase(achievement_id, true);
  }
}

void Achievements::SetAchievementPinnedInDatabase(u32 achievement_id, bool pinned)
{
  if (!EnsureAchievementsDatabaseOpen())
    return;

  Error error;
  SQLitePreparedStatement stmt;
  if (pinned)
  {
    if (!stmt.Prepare(s_state.achievements_db,
                      "INSERT INTO pinned_achievements (game_id, achievement_id) VALUES (?, ?)", &error) ||
        !(stmt.BindInt(1, static_cast<int>(s_state.game_id)), stmt.BindInt(2, static_cast<int>(achievement_id)),
          stmt.Execute(s_state.achievements_db, &error)))
    {
      ERROR_LOG("Failed to pin achievement in database: {}", error.GetDescription());
    }
  }
  else
  {
    if (!stmt.Prepare(s_state.achievements_db,
                      "DELETE FROM pinned_achievements WHERE game_id = ? AND achievement_id = ?", &error) ||
        !(stmt.BindInt(1, static_cast<int>(s_state.game_id)), stmt.BindInt(2, static_cast<int>(achievement_id)),
          stmt.Execute(s_state.achievements_db, &error)))
    {
      ERROR_LOG("Failed to unpin achievement in database: {}", error.GetDescription());
    }
  }
}

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

#include "util/input_manager.h" // Host::GetTopLevelWindowInfo()

#include "common/windows_headers.h"

#include "rc_client_raintegration.h"

namespace Achievements {

static void FinishLoadRAIntegration();
static void FinishLoadRAIntegrationOnCoreThread();

static void RAIntegrationBeginLoadCallback(int result, const char* error_message, rc_client_t* client, void* userdata);
static void RAIntegrationEventHandler(const rc_client_raintegration_event_t* event, rc_client_t* client);
static void RAIntegrationWriteMemoryCallback(uint32_t address, uint8_t* buffer, uint32_t num_bytes,
                                             rc_client_t* client);
static void RAIntegrationGetGameNameCallback(char* buffer, uint32_t buffer_size, rc_client_t* client);

} // namespace Achievements

bool Achievements::IsUsingRAIntegration()
{
  return s_state.using_raintegration;
}

bool Achievements::IsRAIntegrationAvailable()
{
  return (FileSystem::FileExists(Path::Combine(EmuFolders::AppRoot, "RA_Integration-x64.dll").c_str()) ||
          FileSystem::FileExists(Path::Combine(EmuFolders::AppRoot, "RA_Integration.dll").c_str()));
}

bool Achievements::IsRAIntegrationInitializing()
{
  return (s_state.using_raintegration && (s_state.load_raintegration_request || s_state.raintegration_loading));
}

void Achievements::BeginLoadRAIntegration()
{
  // set the flag so we don't try to log in immediately, need to wait for RAIntegration to load first
  s_state.using_raintegration = true;
  s_state.raintegration_loading = true;

  const std::wstring wapproot = StringUtil::UTF8StringToWideString(EmuFolders::AppRoot);
  s_state.load_raintegration_request = rc_client_begin_load_raintegration_deferred(
    s_state.client, wapproot.c_str(), RAIntegrationBeginLoadCallback, nullptr);
}

void Achievements::RAIntegrationBeginLoadCallback(int result, const char* error_message, rc_client_t* client,
                                                  void* userdata)
{
  s_state.load_raintegration_request = nullptr;

  if (result != RC_OK)
  {
    s_state.raintegration_loading = false;

    std::string message = fmt::format("Failed to load RAIntegration:\n{}", error_message ? error_message : "");
    Host::ReportErrorAsync("RAIntegration Error", message);
    return;
  }

  INFO_COLOR_LOG(StrongGreen, "RAIntegration DLL loaded, initializing.");
  Host::RunOnUIThread(&Achievements::FinishLoadRAIntegration);
}

void Achievements::FinishLoadRAIntegration()
{
  const std::optional<WindowInfo> wi = Host::GetTopLevelWindowInfo();
  const auto lock = GetLock();

  // disabled externally?
  if (!s_state.using_raintegration)
    return;

  const char* error_message = nullptr;
  const int res = rc_client_finish_load_raintegration(
    s_state.client, (wi.has_value() && wi->type == WindowInfoType::Win32) ? static_cast<HWND>(wi->window_handle) : NULL,
    "DuckStation", g_scm_tag_str, &error_message);
  if (res != RC_OK)
  {
    std::string message = fmt::format("Failed to initialize RAIntegration:\n{}", error_message ? error_message : "");
    Host::ReportErrorAsync("RAIntegration Error", message);
    s_state.using_raintegration = false;
    Host::RunOnCoreThread(&Achievements::FinishLoadRAIntegrationOnCoreThread);
    return;
  }

  rc_client_raintegration_set_write_memory_function(s_state.client, RAIntegrationWriteMemoryCallback);
  rc_client_raintegration_set_console_id(s_state.client, RC_CONSOLE_PLAYSTATION);
  rc_client_raintegration_set_get_game_name_function(s_state.client, RAIntegrationGetGameNameCallback);
  rc_client_raintegration_set_event_handler(s_state.client, RAIntegrationEventHandler);

  Host::OnRAIntegrationMenuChanged();

  Host::RunOnCoreThread(&Achievements::FinishLoadRAIntegrationOnCoreThread);
}

void Achievements::FinishLoadRAIntegrationOnCoreThread()
{
  // note: this is executed even for the failure case.
  // we want to finish initializing with internal client if RAIntegration didn't load.
  const auto lock = GetLock();
  s_state.raintegration_loading = false;
  FinishInitialize();
}

void Achievements::UnloadRAIntegration(std::unique_lock<std::recursive_mutex>& lock)
{
  DebugAssert(s_state.using_raintegration && s_state.client);

  if (s_state.load_raintegration_request)
  {
    rc_client_abort_async(s_state.client, s_state.load_raintegration_request);
    s_state.load_raintegration_request = nullptr;
  }

  // Have to unload it on the UI thread, otherwise the DLL unload races the UI thread message processing.
  WaitForServerCallsWithYield(lock);
  s_state.raintegration_loading = false;
  s_state.using_raintegration = false;
  Host::RunOnUIThread([client = std::exchange(s_state.client, nullptr)]() {
    rc_client_unload_raintegration(client);
    rc_client_destroy(client);
  });

  Host::OnRAIntegrationMenuChanged();
}

void Achievements::RAIntegrationEventHandler(const rc_client_raintegration_event_t* event, rc_client_t* client)
{
  switch (event->type)
  {
    case RC_CLIENT_RAINTEGRATION_EVENT_MENUITEM_CHECKED_CHANGED:
    case RC_CLIENT_RAINTEGRATION_EVENT_MENU_CHANGED:
    {
      Host::OnRAIntegrationMenuChanged();
    }
    break;

    case RC_CLIENT_RAINTEGRATION_EVENT_HARDCORE_CHANGED:
    {
      // Could get called from a different thread...
      Host::RunOnCoreThread([]() {
        const auto lock = GetLock();
        OnHardcoreModeChanged(rc_client_get_hardcore_enabled(s_state.client) != 0, false, false);
      });
    }
    break;

    case RC_CLIENT_RAINTEGRATION_EVENT_PAUSE:
    {
      Host::RunOnCoreThread([]() { System::PauseSystem(true); });
    }
    break;

    default:
      ERROR_LOG("Unhandled RAIntegration event {}", static_cast<u32>(event->type));
      break;
  }
}

void Achievements::RAIntegrationWriteMemoryCallback(uint32_t address, uint8_t* buffer, uint32_t num_bytes,
                                                    rc_client_t* client)
{
  if ((address + num_bytes) > 0x200400U) [[unlikely]]
    return;

  // This can be called on the UI thread, so always queue it.
  llvm::SmallVector<u8, 16> data(buffer, buffer + num_bytes);
  Host::RunOnCoreThread([address, data = std::move(data)]() {
    u8* src = (address >= 0x200000U) ? CPU::g_state.scratchpad.data() : Bus::g_ram;
    const u32 offset = (address & Bus::RAM_2MB_MASK); // size guarded by check above

    switch (data.size())
    {
      case 1:
        std::memcpy(&src[offset], data.data(), 1);
        break;
      case 2:
        std::memcpy(&src[offset], data.data(), 2);
        break;
      case 4:
        std::memcpy(&src[offset], data.data(), 4);
        break;
      default:
        [[unlikely]] std::memcpy(&src[offset], data.data(), data.size());
        break;
    }
  });
}

void Achievements::RAIntegrationGetGameNameCallback(char* buffer, uint32_t buffer_size, rc_client_t* client)
{
  StringUtil::Strlcpy(buffer, System::GetGameTitle(), buffer_size);
}

#else

bool Achievements::IsUsingRAIntegration()
{
  return false;
}

bool Achievements::IsRAIntegrationAvailable()
{
  return false;
}

bool Achievements::IsRAIntegrationInitializing()
{
  return false;
}

#endif

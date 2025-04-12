// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// TODO: Don't poll when booting the game, e.g. Crash Warped freaks out.

#include "achievements.h"
#include "achievements_private.h"
#include "bios.h"
#include "bus.h"
#include "cheats.h"
#include "cpu_core.h"
#include "fullscreen_ui.h"
#include "game_list.h"
#include "gpu_thread.h"
#include "host.h"
#include "imgui_overlays.h"
#include "system.h"

#include "scmversion/scmversion.h"

#include "common/assert.h"
#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"
#include "common/ryml_helpers.h"
#include "common/scoped_guard.h"
#include "common/sha256_digest.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "util/cd_image.h"
#include "util/http_downloader.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/platform_misc.h"
#include "util/state_wrapper.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"
#include "IconsPromptFont.h"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "rc_api_runtime.h"
#include "rc_client.h"
#include "rc_consoles.h"

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
static constexpr const char* ACHEIVEMENT_DETAILS_URL_TEMPLATE = "https://retroachievements.org/achievement/{}";
static constexpr const char* PROFILE_DETAILS_URL_TEMPLATE = "https://retroachievements.org/user/{}";
static constexpr const char* CACHE_SUBDIRECTORY_NAME = "achievement_images";

static constexpr u32 LEADERBOARD_NEARBY_ENTRIES_TO_FETCH = 10;
static constexpr u32 LEADERBOARD_ALL_FETCH_SIZE = 20;

static constexpr float LOGIN_NOTIFICATION_TIME = 5.0f;
static constexpr float ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME = 5.0f;
static constexpr float ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME_HC = 10.0f;
static constexpr float GAME_COMPLETE_NOTIFICATION_TIME = 20.0f;
static constexpr float LEADERBOARD_STARTED_NOTIFICATION_TIME = 3.0f;
static constexpr float LEADERBOARD_FAILED_NOTIFICATION_TIME = 3.0f;

static constexpr float INDICATOR_FADE_IN_TIME = 0.1f;
static constexpr float INDICATOR_FADE_OUT_TIME = 0.3f;

// Some API calls are really slow. Set a longer timeout.
static constexpr float SERVER_CALL_TIMEOUT = 60.0f;

// Chrome uses 10 server calls per domain, seems reasonable.
static constexpr u32 MAX_CONCURRENT_SERVER_CALLS = 10;

namespace {

struct LoginWithPasswordParameters
{
  const char* username;
  Error* error;
  rc_client_async_handle_t* request;
  bool result;
};

struct LeaderboardTrackerIndicator
{
  u32 tracker_id;
  std::string text;
  float opacity;
  bool active;
};

struct AchievementChallengeIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_path;
  float opacity;
  bool active;
};

struct AchievementProgressIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_path;
  float opacity;
  bool active;
};

} // namespace

static TinyString GameHashToString(const GameHash& hash);

static void ReportError(std::string_view sv);
template<typename... T>
static void ReportFmtError(fmt::format_string<T...> fmt, T&&... args);
template<typename... T>
static void ReportRCError(int err, fmt::format_string<T...> fmt, T&&... args);
static void ClearGameInfo();
static void ClearGameHash();
static bool HasSavedCredentials();
static bool TryLoggingInWithToken();
static void EnableHardcodeMode(bool display_message, bool display_game_summary);
static void OnHardcoreModeChanged(bool enabled, bool display_message, bool display_game_summary);
static bool IsRAIntegrationInitializing();
static bool IsLoggedIn();
static bool IsLoggedInOrLoggingIn();
static void FinishInitialize();
static void FinishLogin(const rc_client_t* client);
static void ShowLoginNotification();
static bool IdentifyGame(CDImage* image);
static bool IdentifyCurrentGame();
static void BeginLoadGame();
static void UpdateGameSummary(bool update_progress_database);
static std::string GetImageURL(const char* image_name, u32 type);
static std::string GetLocalImagePath(const std::string_view image_name, u32 type);
static void DownloadImage(std::string url, std::string cache_path);
static const std::string& GetCachedAchievementBadgePath(const rc_client_achievement_t* achievement, bool locked);

static TinyString DecryptLoginToken(std::string_view encrypted_token, std::string_view username);
static TinyString EncryptLoginToken(std::string_view token, std::string_view username);

static bool CreateClient(rc_client_t** client, std::unique_ptr<HTTPDownloader>* http);
static void DestroyClient(rc_client_t** client, std::unique_ptr<HTTPDownloader>* http);
static void ClientMessageCallback(const char* message, const rc_client_t* client);
static uint32_t ClientReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client);
static void ClientServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data,
                             rc_client_t* client);

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

static void LeaderboardFetchNearbyCallback(int result, const char* error_message,
                                           rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                           void* callback_userdata);
static void LeaderboardFetchAllCallback(int result, const char* error_message, rc_client_leaderboard_entry_list_t* list,
                                        rc_client_t* client, void* callback_userdata);

#ifndef __ANDROID__
static void DrawAchievement(const rc_client_achievement_t* cheevo);
static void DrawLeaderboardListEntry(const rc_client_leaderboard_t* lboard);
static void DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, u32 index, bool is_self,
                                 float rank_column_width, float name_column_width, float time_column_width,
                                 float column_spacing);
#endif

static std::string GetHashDatabasePath();
static std::string GetProgressDatabasePath();
static void PreloadHashDatabase();
static bool LoadHashDatabase(const std::string& path, Error* error);
static bool CreateHashDatabaseFromSeedDatabase(const std::string& path, Error* error);
static void BeginRefreshHashDatabase();
static void FinishRefreshHashDatabase();
static void CancelHashDatabaseRequests();

static void FetchHashLibraryCallback(int result, const char* error_message, rc_client_hash_library_t* list,
                                     rc_client_t* client, void* callback_userdata);
static void FetchAllProgressCallback(int result, const char* error_message, rc_client_all_user_progress_t* list,
                                     rc_client_t* client, void* callback_userdata);
static void RefreshAllProgressCallback(int result, const char* error_message, rc_client_all_user_progress_t* list,
                                       rc_client_t* client, void* callback_userdata);

static void BuildHashDatabase(const rc_client_hash_library_t* hashlib, const rc_client_all_user_progress_t* allprog);
static bool SortAndSaveHashDatabase(Error* error);

static FileSystem::ManagedCFilePtr OpenProgressDatabase(bool for_write, bool truncate, Error* error);
static void BuildProgressDatabase(const rc_client_all_user_progress_t* allprog);
static void UpdateProgressDatabase();
static void ClearProgressDatabase();

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

static void BeginLoadRAIntegration();
static void UnloadRAIntegration();

#endif

namespace {

struct PauseMenuAchievementInfo
{
  std::string title;
  std::string description;
  std::string badge_path;
};

struct State
{
  rc_client_t* client = nullptr;
  bool has_achievements = false;
  bool has_leaderboards = false;
  bool has_rich_presence = false;

  std::recursive_mutex mutex; // large

  std::string rich_presence_string;
  Timer::Value rich_presence_poll_time = 0;

  std::vector<LeaderboardTrackerIndicator> active_leaderboard_trackers;
  std::vector<AchievementChallengeIndicator> active_challenge_indicators;
  std::optional<AchievementProgressIndicator> active_progress_indicator;

  rc_client_user_game_summary_t game_summary = {};
  u32 game_id = 0;

  std::unique_ptr<HTTPDownloader> http_downloader;

  std::string game_path;
  std::string game_title;
  std::string game_icon;
  std::string game_icon_url;
  std::optional<GameHash> game_hash;

  rc_client_async_handle_t* login_request = nullptr;
  rc_client_async_handle_t* load_game_request = nullptr;

  rc_client_achievement_list_t* achievement_list = nullptr;
  std::vector<std::tuple<const void*, std::string, bool>> achievement_badge_paths;

  std::optional<PauseMenuAchievementInfo> most_recent_unlock = {};
  std::optional<PauseMenuAchievementInfo> achievement_nearest_completion = {};

  rc_client_leaderboard_list_t* leaderboard_list = nullptr;
  const rc_client_leaderboard_t* open_leaderboard = nullptr;
  rc_client_async_handle_t* leaderboard_fetch_handle = nullptr;
  std::vector<rc_client_leaderboard_entry_list_t*> leaderboard_entry_lists;
  std::vector<std::pair<const rc_client_leaderboard_entry_t*, std::string>> leaderboard_user_icon_paths;
  rc_client_leaderboard_entry_list_t* leaderboard_nearby_entries;
  bool is_showing_all_leaderboard_entries = false;

  bool hashdb_loaded = false;
  std::vector<HashDatabaseEntry> hashdb_entries;

  rc_client_async_handle_t* fetch_hash_library_request = nullptr;
  rc_client_hash_library_t* fetch_hash_library_result = nullptr;
  rc_client_async_handle_t* fetch_all_progress_request = nullptr;
  rc_client_all_user_progress_t* fetch_all_progress_result = nullptr;
  rc_client_async_handle_t* refresh_all_progress_request = nullptr;

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  rc_client_async_handle_t* load_raintegration_request = nullptr;
  bool using_raintegration = false;
  bool raintegration_loading = false;
#endif
};

} // namespace

ALIGN_TO_CACHE_LINE static State s_state;

} // namespace Achievements

TinyString Achievements::GameHashToString(const GameHash& hash)
{
  return TinyString::from_format(
    "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", hash[0],
    hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], hash[8], hash[9], hash[10], hash[11], hash[12],
    hash[13], hash[14], hash[15]);
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

void Achievements::ReportError(std::string_view sv)
{
  ERROR_LOG(sv);
  Host::AddIconOSDWarning(std::string(), ICON_EMOJI_WARNING, std::string(sv), Host::OSD_CRITICAL_ERROR_DURATION);
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

std::optional<Achievements::GameHash> Achievements::GetGameHash(CDImage* image, u32* bytes_hashed)
{
  std::optional<GameHash> ret;

  std::string executable_name;
  std::vector<u8> executable_data;
  if (!System::ReadExecutableFromImage(image, &executable_name, &executable_data))
    return ret;

  return GetGameHash(executable_name, executable_data, bytes_hashed);
}

std::optional<Achievements::GameHash> Achievements::GetGameHash(const std::string_view executable_name,
                                                                std::span<const u8> executable_data,
                                                                u32* bytes_hashed /* = nullptr */)
{
  std::optional<GameHash> ret;

  // NOTE: Assumes executable_data is aligned to 4 bytes at least.. it should be.
  const BIOS::PSEXEHeader* header = reinterpret_cast<const BIOS::PSEXEHeader*>(executable_data.data());
  if (executable_data.size() < sizeof(BIOS::PSEXEHeader) || !BIOS::IsValidPSExeHeader(*header, executable_data.size()))
  {
    ERROR_LOG("PS-EXE header is invalid in '{}' ({} bytes)", executable_name, executable_data.size());
    return ret;
  }

  const u32 hash_size = std::min(header->file_size + 2048, static_cast<u32>(executable_data.size()));

  MD5Digest digest;
  digest.Update(executable_name.data(), static_cast<u32>(executable_name.size()));
  if (hash_size > 0)
    digest.Update(executable_data.data(), hash_size);

  ret = GameHash();
  digest.Final(ret.value());

  if (bytes_hashed)
    *bytes_hashed = hash_size;

  return ret;
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

std::string Achievements::GetLocalImagePath(const std::string_view image_name, u32 type)
{
  std::string_view prefix;
  std::string_view suffix;
  switch (type)
  {
    case RC_IMAGE_TYPE_GAME:
      prefix = "image"; // https://media.retroachievements.org/Images/{}.png
      break;

    case RC_IMAGE_TYPE_USER:
      prefix = "user"; // https://media.retroachievements.org/UserPic/{}.png
      break;

    case RC_IMAGE_TYPE_ACHIEVEMENT: // https://media.retroachievements.org/Badge/{}.png
      prefix = "badge";
      break;

    case RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED:
      prefix = "badge";
      suffix = "_lock";
      break;

    default:
      prefix = "badge";
      break;
  }

  std::string ret;
  if (!image_name.empty())
  {
    ret = fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}" FS_OSPATH_SEPARATOR_STR "{}_{}{}.png", EmuFolders::Cache,
                      CACHE_SUBDIRECTORY_NAME, prefix, Path::SanitizeFileName(image_name), suffix);
  }

  return ret;
}

void Achievements::DownloadImage(std::string url, std::string cache_path)
{
  auto callback = [cache_path = std::move(cache_path)](s32 status_code, const Error& error,
                                                       const std::string& content_type,
                                                       HTTPDownloader::Request::Data data) mutable {
    if (status_code != HTTPDownloader::HTTP_STATUS_OK)
    {
      ERROR_LOG("Failed to download badge '{}': {}", Path::GetFileName(cache_path), error.GetDescription());
      return;
    }

    Error write_error;
    if (!FileSystem::WriteBinaryFile(cache_path.c_str(), data, &write_error))
    {
      ERROR_LOG("Failed to write badge image to '{}': {}", cache_path, write_error.GetDescription());
      return;
    }

    GPUThread::RunOnThread(
      [cache_path = std::move(cache_path)]() { ImGuiFullscreen::InvalidateCachedTexture(cache_path); });
  };

  s_state.http_downloader->CreateRequest(std::move(url), std::move(callback));
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

const std::string& Achievements::GetGameTitle()
{
  return s_state.game_title;
}

const std::string& Achievements::GetGamePath()
{
  return s_state.game_path;
}

const std::string& Achievements::GetGameIconPath()
{
  return s_state.game_icon;
}

const std::string& Achievements::GetGameIconURL()
{
  return s_state.game_icon_url;
}

const std::string& Achievements::GetRichPresenceString()
{
  return s_state.rich_presence_string;
}

bool Achievements::Initialize()
{
  auto lock = GetLock();
  AssertMsg(g_settings.achievements_enabled, "Achievements are enabled");
  Assert(!s_state.client && !s_state.http_downloader);

  if (!CreateClient(&s_state.client, &s_state.http_downloader))
    return false;

  rc_client_set_event_handler(s_state.client, ClientEventHandler);

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (g_settings.achievements_use_raintegration)
    BeginLoadRAIntegration();
#endif

  // Hardcore starts off. We enable it on first boot.
  rc_client_set_hardcore_enabled(s_state.client, false);
  rc_client_set_encore_mode_enabled(s_state.client, g_settings.achievements_encore_mode);
  rc_client_set_unofficial_enabled(s_state.client, g_settings.achievements_unofficial_test_mode);
  rc_client_set_spectator_mode_enabled(s_state.client, g_settings.achievements_spectator_mode);

  // We can't do an internal client login while using RAIntegration, since the two will conflict.
  if (!IsRAIntegrationInitializing())
    FinishInitialize();

  return true;
}

void Achievements::FinishInitialize()
{
  // Start logging in. This can take a while.
  TryLoggingInWithToken();

  // Are we running a game?
  if (System::IsValid())
  {
    IdentifyCurrentGame();
    BeginLoadGame();

    // Hardcore mode isn't enabled when achievements first starts, if a game is already running.
    if (IsLoggedInOrLoggingIn() && g_settings.achievements_hardcore_mode)
      DisplayHardcoreDeferredMessage();
  }

  Host::OnAchievementsActiveChanged(true);
}

bool Achievements::CreateClient(rc_client_t** client, std::unique_ptr<HTTPDownloader>* http)
{
  rc_client_t* new_client = rc_client_create(ClientReadMemory, ClientServerCall);
  if (!new_client)
  {
    Host::ReportErrorAsync("Achievements Error", "rc_client_create() failed, cannot use achievements");
    return false;
  }

  rc_client_enable_logging(
    new_client, (Log::GetLogLevel() >= Log::Level::Verbose) ? RC_CLIENT_LOG_LEVEL_VERBOSE : RC_CLIENT_LOG_LEVEL_INFO,
    ClientMessageCallback);

  char rc_client_user_agent[128];
  rc_client_get_user_agent_clause(new_client, rc_client_user_agent, std::size(rc_client_user_agent));
  *http = HTTPDownloader::Create(fmt::format("{} {}", Host::GetHTTPUserAgent(), rc_client_user_agent));
  if (!*http)
  {
    Host::ReportErrorAsync("Achievements Error", "Failed to create HTTPDownloader, cannot use achievements");
    rc_client_destroy(new_client);
    return false;
  }

  (*http)->SetTimeout(SERVER_CALL_TIMEOUT);
  (*http)->SetMaxActiveRequests(MAX_CONCURRENT_SERVER_CALLS);

  rc_client_set_userdata(new_client, http->get());
  *client = new_client;
  return true;
}

void Achievements::DestroyClient(rc_client_t** client, std::unique_ptr<HTTPDownloader>* http)
{
  (*http)->WaitForAllRequests();

  rc_client_destroy(*client);
  *client = nullptr;

  http->reset();
}

bool Achievements::HasSavedCredentials()
{
  const TinyString username = Host::GetTinyStringSettingValue("Cheevos", "Username");
  const TinyString api_token = Host::GetTinyStringSettingValue("Cheevos", "Token");
  return (!username.empty() && !api_token.empty());
}

bool Achievements::TryLoggingInWithToken()
{
  const TinyString username = Host::GetTinyStringSettingValue("Cheevos", "Username");
  const TinyString api_token = Host::GetTinyStringSettingValue("Cheevos", "Token");
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
    WARNING_LOG("Invalid encrypted login token, requesitng a new one.");
    Host::OnAchievementsLoginRequested(LoginRequestReason::TokenInvalid);
    return false;
  }
}

void Achievements::UpdateSettings(const Settings& old_config)
{
  if (!g_settings.achievements_enabled)
  {
    // we're done here
    Shutdown();
    return;
  }

  if (!IsActive())
  {
    // we just got enabled
    Initialize();
    return;
  }

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (g_settings.achievements_use_raintegration != old_config.achievements_use_raintegration)
  {
    // RAIntegration requires a full client reload?
    Shutdown();
    Initialize();
    return;
  }
#endif

  if (g_settings.achievements_hardcore_mode != old_config.achievements_hardcore_mode)
  {
    // Enables have to wait for reset, disables can go through immediately.
    if (g_settings.achievements_hardcore_mode)
      DisplayHardcoreDeferredMessage();
    else
      DisableHardcoreMode(true, true);
  }

  // These cannot be modified while a game is loaded, so just toss state and reload.
  auto lock = GetLock();
  if (HasActiveGame())
  {
    lock.unlock();
    if (g_settings.achievements_encore_mode != old_config.achievements_encore_mode ||
        g_settings.achievements_spectator_mode != old_config.achievements_spectator_mode ||
        g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode)
    {
      Shutdown();
      Initialize();
      return;
    }
  }
  else
  {
    if (g_settings.achievements_encore_mode != old_config.achievements_encore_mode)
      rc_client_set_encore_mode_enabled(s_state.client, g_settings.achievements_encore_mode);
    if (g_settings.achievements_spectator_mode != old_config.achievements_spectator_mode)
      rc_client_set_spectator_mode_enabled(s_state.client, g_settings.achievements_spectator_mode);
    if (g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode)
      rc_client_set_unofficial_enabled(s_state.client, g_settings.achievements_unofficial_test_mode);
  }
}

void Achievements::Shutdown()
{
  if (!IsActive())
    return;

  auto lock = GetLock();
  Assert(s_state.client && s_state.http_downloader);

  ClearGameInfo();
  ClearGameHash();
  DisableHardcoreMode(false, false);
  CancelHashDatabaseRequests();

  if (s_state.login_request)
  {
    rc_client_abort_async(s_state.client, s_state.login_request);
    s_state.login_request = nullptr;
  }

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (s_state.using_raintegration)
  {
    UnloadRAIntegration();
    return;
  }
#endif

  DestroyClient(&s_state.client, &s_state.http_downloader);
  Host::OnAchievementsActiveChanged(false);
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
  HTTPDownloader::Request::Callback hd_callback = [callback, callback_data](s32 status_code, const Error& error,
                                                                            const std::string& content_type,
                                                                            HTTPDownloader::Request::Data data) {
    if (status_code != HTTPDownloader::HTTP_STATUS_OK)
      ERROR_LOG("Server call failed: {}", error.GetDescription());

    rc_api_server_response_t rr;
    rr.http_status_code = (status_code <= 0) ? (status_code == HTTPDownloader::HTTP_STATUS_CANCELLED ?
                                                  RC_API_SERVER_RESPONSE_CLIENT_ERROR :
                                                  RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR) :
                                               status_code;
    rr.body_length = data.size();
    rr.body = data.empty() ? nullptr : reinterpret_cast<const char*>(data.data());

    callback(&rr, callback_data);
  };

  HTTPDownloader* http = static_cast<HTTPDownloader*>(rc_client_get_userdata(client));

  // TODO: Content-type for post
  if (request->post_data)
  {
    // const auto pd = std::string_view(request->post_data);
    // Log_DevFmt("Server POST: {}", pd.substr(0, std::min<size_t>(pd.length(), 10)));
    http->CreatePostRequest(request->url, request->post_data, std::move(hd_callback));
  }
  else
  {
    http->CreateRequest(request->url, std::move(hd_callback));
  }
}

void Achievements::IdleUpdate()
{
  if (!IsActive())
    return;

  const auto lock = GetLock();

  s_state.http_downloader->PollRequests();
  rc_client_idle(s_state.client);
}

bool Achievements::NeedsIdleUpdate()
{
  if (!IsActive())
    return false;

  const auto lock = GetLock();
  return (s_state.http_downloader && s_state.http_downloader->HasAnyRequests());
}

void Achievements::FrameUpdate()
{
  if (!IsActive())
    return;

  auto lock = GetLock();

  s_state.http_downloader->PollRequests();
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

void Achievements::UpdateGameSummary(bool update_progress_database)
{
  rc_client_get_user_game_summary(s_state.client, &s_state.game_summary);

  if (update_progress_database)
    UpdateProgressDatabase();
}

void Achievements::UpdateRecentUnlockAndAlmostThere()
{
  const auto lock = GetLock();
  if (!HasActiveGame())
  {
    s_state.most_recent_unlock.reset();
    s_state.achievement_nearest_completion.reset();
    return;
  }

  rc_client_achievement_list_t* const achievements = rc_client_create_achievement_list(
    s_state.client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  if (!achievements)
  {
    s_state.most_recent_unlock.reset();
    s_state.achievement_nearest_completion.reset();
    return;
  }

  const rc_client_achievement_t* most_recent_unlock = nullptr;
  const rc_client_achievement_t* nearest_completion = nullptr;

  for (u32 i = 0; i < achievements->num_buckets; i++)
  {
    const rc_client_achievement_bucket_t& bucket = achievements->buckets[i];
    for (u32 j = 0; j < bucket.num_achievements; j++)
    {
      const rc_client_achievement_t* achievement = bucket.achievements[j];

      if (achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED)
      {
        if (!most_recent_unlock || achievement->unlock_time > most_recent_unlock->unlock_time)
          most_recent_unlock = achievement;
      }
      else
      {
        // find the achievement with the greatest normalized progress, but skip anything below 80%,
        // matching the rc_client definition of "almost there"
        const float percent_cutoff = 80.0f;
        if (achievement->measured_percent >= percent_cutoff &&
            (!nearest_completion || achievement->measured_percent > nearest_completion->measured_percent))
        {
          nearest_completion = achievement;
        }
      }
    }
  }

  // have to take a copy because with RAIntegration the achievement pointer does not persist
  static constexpr auto cache_info = [](const rc_client_achievement_t* achievement,
                                        std::optional<PauseMenuAchievementInfo>& info) {
    if (!achievement)
    {
      info.reset();
      return;
    }

    if (!info.has_value())
      info.emplace();

    info->title = achievement->title;
    info->description = achievement->description;
    info->badge_path = GetAchievementBadgePath(achievement, false);
  };

  cache_info(most_recent_unlock, s_state.most_recent_unlock);
  cache_info(nearest_completion, s_state.achievement_nearest_completion);

  rc_client_destroy_achievement_list(achievements);
}

void Achievements::UpdateRichPresence(std::unique_lock<std::recursive_mutex>& lock)
{
  // Limit rich presence updates to once per second, since it could change per frame.
  if (!s_state.has_rich_presence)
    return;

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
  Host::OnAchievementsRefreshed();

  lock.unlock();
  System::UpdateRichPresence(false);
  lock.lock();
}

void Achievements::OnSystemStarting(CDImage* image, bool disable_hardcore_mode)
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
      EnableHardcodeMode(false, false);
  }

  // now we can finally identify the game
  IdentifyGame(image);
  BeginLoadGame();
}

void Achievements::OnSystemDestroyed()
{
  ClearGameInfo();
  ClearGameHash();
  DisableHardcoreMode(false, false);
}

void Achievements::OnSystemReset()
{
  const auto lock = GetLock();
  if (!IsActive() || IsRAIntegrationInitializing())
    return;

  // Do we need to enable hardcore mode?
  if (System::IsValid() && g_settings.achievements_hardcore_mode && !rc_client_get_hardcore_enabled(s_state.client))
  {
    // This will raise the silly reset event, but we can safely ignore that since we're immediately resetting the client
    DEV_LOG("Enabling hardcore mode after reset");
    EnableHardcodeMode(true, true);
  }

  DEV_LOG("Reset client");
  rc_client_reset(s_state.client);
}

void Achievements::GameChanged(CDImage* image)
{
  std::unique_lock lock(s_state.mutex);

  if (!IsActive() || IsRAIntegrationInitializing())
    return;

  // disc changed?
  if (!IdentifyGame(image))
    return;

  // cancel previous requests
  if (s_state.load_game_request)
  {
    rc_client_abort_async(s_state.client, s_state.load_game_request);
    s_state.load_game_request = nullptr;
  }

  // Use a hash that will never match if we removed the disc. See rc_client_begin_change_media().
  TinyString game_hash_str;
  if (s_state.game_hash.has_value())
    game_hash_str = GameHashToString(s_state.game_hash.value());
  else
    game_hash_str = "[NO HASH]";

  s_state.load_game_request = rc_client_begin_change_media_from_hash(
    s_state.client, game_hash_str.c_str(), ClientLoadGameCallback, reinterpret_cast<void*>(static_cast<uintptr_t>(1)));
}

bool Achievements::IdentifyGame(CDImage* image)
{
  if (s_state.game_path == (image ? std::string_view(image->GetPath()) : std::string_view()))
  {
    WARNING_LOG("Game path is unchanged.");
    return false;
  }

  std::optional<GameHash> game_hash;
  if (image)
  {
    u32 bytes_hashed;
    game_hash = GetGameHash(image, &bytes_hashed);
    if (game_hash.has_value())
    {
      INFO_COLOR_LOG(StrongOrange, "RA Hash: {} ({} bytes hashed)", GameHashToString(game_hash.value()), bytes_hashed);
    }
    else
    {
      // If we are starting with this game and it's bad, notify the user that this is why.
      Host::AddIconOSDWarning(
        "AchievementsHashFailed", ICON_EMOJI_WARNING,
        TRANSLATE_STR("Achievements", "Failed to read executable from disc. Achievements disabled."),
        Host::OSD_ERROR_DURATION);
    }
  }

  s_state.game_path = image ? image->GetPath() : std::string();

  if (s_state.game_hash == game_hash)
  {
    // only the path has changed - different format/save state/etc.
    INFO_LOG("Detected path change to '{}'", s_state.game_path);
    return false;
  }

  s_state.game_hash = game_hash;
  return true;
}

bool Achievements::IdentifyCurrentGame()
{
  DebugAssert(System::IsValid());

  // this crap is only needed because we can't grab the image from the reader...
  std::unique_ptr<CDImage> temp_image;
  if (const std::string& disc_path = System::GetDiscPath(); !disc_path.empty())
  {
    Error error;
    temp_image = CDImage::Open(disc_path.c_str(), g_settings.cdrom_load_image_patches, &error);
    if (!temp_image)
      ERROR_LOG("Failed to open disc for late game identification: {}", error.GetDescription());
  }

  return IdentifyGame(temp_image.get());
}

void Achievements::BeginLoadGame()
{
  if (!s_state.game_hash.has_value())
  {
    // no need to go through ClientLoadGameCallback, just bail out straight away
    DisableHardcoreMode(false, false);
    return;
  }

  s_state.load_game_request = rc_client_begin_load_game(s_state.client, GameHashToString(s_state.game_hash.value()),
                                                        ClientLoadGameCallback, nullptr);
}

void Achievements::ClientLoadGameCallback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  const bool was_disc_change = (userdata != nullptr);

  s_state.load_game_request = nullptr;

  if (result == RC_NO_GAME_LOADED)
  {
    // Unknown game.
    INFO_LOG("Unknown game '{}', disabling achievements.", GameHashToString(s_state.game_hash.value()));
    if (was_disc_change)
      ClearGameInfo();

    DisableHardcoreMode(false, false);
    return;
  }
  else if (result == RC_LOGIN_REQUIRED)
  {
    // We would've asked to re-authenticate, so leave HC on for now.
    // Once we've done so, we'll reload the game.
    if (!HasSavedCredentials())
    {
      DisableHardcoreMode(false, false);
      return;
    }

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
  const bool has_leaderboards = rc_client_has_leaderboards(client, false);

  // Only display summary if the game title has changed across discs.
  const bool display_summary = (s_state.game_id != info->id || s_state.game_title != info->title);

  // If the game has a RetroAchievements entry but no achievements or leaderboards, enforcing hardcore mode
  // is pointless. Have to re-query leaderboards because hidden should still trip HC.
  if (!has_achievements && !rc_client_has_leaderboards(client, true))
    DisableHardcoreMode(false, false);

  s_state.game_id = info->id;
  s_state.game_title = info->title;
  s_state.has_achievements = has_achievements;
  s_state.has_leaderboards = has_leaderboards;
  s_state.has_rich_presence = rc_client_has_rich_presence(client);

  // ensure fullscreen UI is ready for notifications
  if (display_summary)
    GPUThread::RunOnThread(&FullscreenUI::Initialize);

  s_state.game_icon_url =
    info->badge_url ? std::string(info->badge_url) : GetImageURL(info->badge_name, RC_IMAGE_TYPE_GAME);
  s_state.game_icon = GetLocalImagePath(info->badge_name, RC_IMAGE_TYPE_GAME);
  if (!s_state.game_icon.empty() && !s_state.game_icon_url.empty() &&
      !FileSystem::FileExists(s_state.game_icon.c_str()))
    DownloadImage(s_state.game_icon_url, s_state.game_icon);

  // update progress database on first load, in case it was played on another PC
  UpdateGameSummary(true);

  if (display_summary)
    DisplayAchievementSummary();

  Host::OnAchievementsRefreshed();
}

void Achievements::ClearGameInfo()
{
  ClearUIState();

  if (s_state.load_game_request)
  {
    rc_client_abort_async(s_state.client, s_state.load_game_request);
    s_state.load_game_request = nullptr;
  }
  rc_client_unload_game(s_state.client);

  s_state.active_leaderboard_trackers = {};
  s_state.active_challenge_indicators = {};
  s_state.active_progress_indicator.reset();
  s_state.game_id = 0;
  s_state.game_title = {};
  s_state.game_icon = {};
  s_state.game_icon_url = {};
  s_state.has_achievements = false;
  s_state.has_leaderboards = false;
  s_state.has_rich_presence = false;
  s_state.rich_presence_string = {};
  s_state.game_summary = {};

  Host::OnAchievementsRefreshed();
}

void Achievements::ClearGameHash()
{
  s_state.game_path = {};
  s_state.game_hash.reset();
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

    GPUThread::RunOnThread([title = s_state.game_title, summary = std::string(summary.view()), icon = s_state.game_icon,
                            time = IsHardcoreModeActive() ? ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME_HC :
                                                            ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME]() mutable {
      if (!FullscreenUI::Initialize())
        return;

      ImGuiFullscreen::AddNotification("achievement_summary", time, std::move(title), std::move(summary),
                                       std::move(icon));
    });
  }

  // Technically not going through the resource API, but since we're passing this to something else, we can't.
  if (g_settings.achievements_sound_effects)
    PlatformMisc::PlaySoundAsync(EmuFolders::GetOverridableResourcePath(INFO_SOUND_NAME).c_str());
}

void Achievements::DisplayHardcoreDeferredMessage()
{
  if (g_settings.achievements_hardcore_mode && System::IsValid())
  {
    GPUThread::RunOnThread([]() {
      if (!FullscreenUI::Initialize())
        return;

      ImGuiFullscreen::ShowToast(std::string(),
                                 TRANSLATE_STR("Achievements", "Hardcore mode will be enabled on system reset."),
                                 Host::OSD_WARNING_DURATION);
    });
  }
}

void Achievements::HandleResetEvent(const rc_client_event_t* event)
{
  WARNING_LOG("Ignoring RC_CLIENT_EVENT_RESET.");
}

void Achievements::HandleUnlockEvent(const rc_client_event_t* event)
{
  const rc_client_achievement_t* cheevo = event->achievement;
  DebugAssert(cheevo);

  INFO_LOG("Achievement {} ({}) for game {} unlocked", cheevo->title, cheevo->id, s_state.game_id);
  UpdateGameSummary(true);

  if (g_settings.achievements_notifications)
  {
    std::string title;
    if (cheevo->category == RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL)
      title = fmt::format(TRANSLATE_FS("Achievements", "{} (Unofficial)"), cheevo->title);
    else
      title = cheevo->title;

    std::string badge_path = GetAchievementBadgePath(cheevo, false);

    GPUThread::RunOnThread([id = cheevo->id, duration = g_settings.achievements_notification_duration,
                            title = std::move(title), description = std::string(cheevo->description),
                            badge_path = std::move(badge_path)]() mutable {
      if (!FullscreenUI::Initialize())
        return;

      ImGuiFullscreen::AddNotification(fmt::format("achievement_unlock_{}", id), static_cast<float>(duration),
                                       std::move(title), std::move(description), std::move(badge_path));
    });
  }

  if (g_settings.achievements_sound_effects)
    PlatformMisc::PlaySoundAsync(EmuFolders::GetOverridableResourcePath(UNLOCK_SOUND_NAME).c_str());
}

void Achievements::HandleGameCompleteEvent(const rc_client_event_t* event)
{
  INFO_LOG("Game {} complete", s_state.game_id);
  UpdateGameSummary(false);

  if (g_settings.achievements_notifications)
  {
    std::string message = fmt::format(
      TRANSLATE_FS("Achievements", "Game complete.\n{0}, {1}."),
      TRANSLATE_PLURAL_STR("Achievements", "%n achievements", "Mastery popup",
                           s_state.game_summary.num_unlocked_achievements),
      TRANSLATE_PLURAL_STR("Achievements", "%n points", "Achievement points", s_state.game_summary.points_unlocked));

    GPUThread::RunOnThread(
      [title = s_state.game_title, message = std::move(message), icon = s_state.game_icon]() mutable {
        if (!FullscreenUI::Initialize())
          return;

        ImGuiFullscreen::AddNotification("achievement_mastery", GAME_COMPLETE_NOTIFICATION_TIME, std::move(title),
                                         std::move(message), std::move(icon));
      });
  }
}

void Achievements::HandleSubsetCompleteEvent(const rc_client_event_t* event)
{
  INFO_LOG("Subset {} ({}) complete", event->subset->title, event->subset->id);
  UpdateGameSummary(false);

  if (g_settings.achievements_notifications && event->subset->badge_name[0] != '\0')
  {
    // Need to grab the icon for the subset.
    std::string badge_path = GetLocalImagePath(event->subset->badge_name, RC_IMAGE_TYPE_GAME);
    if (!FileSystem::FileExists(badge_path.c_str()))
    {
      std::string url;
      if (IsUsingRAIntegration() || !event->subset->badge_url)
        url = GetImageURL(event->subset->badge_name, RC_IMAGE_TYPE_GAME);
      else
        url = event->subset->badge_url;
      DownloadImage(std::move(url), badge_path);
    }

    std::string title = event->subset->title;
    std::string message = fmt::format(
      TRANSLATE_FS("Achievements", "Subset complete.\n{0}, {1}."),
      TRANSLATE_PLURAL_STR("Achievements", "%n achievements", "Mastery popup",
                           s_state.game_summary.num_unlocked_achievements),
      TRANSLATE_PLURAL_STR("Achievements", "%n points", "Achievement points", s_state.game_summary.points_unlocked));

    GPUThread::RunOnThread(
      [title = std::move(title), message = std::move(message), badge_path = std::move(badge_path)]() mutable {
        if (!FullscreenUI::Initialize())
          return;

        ImGuiFullscreen::AddNotification("achievement_mastery", GAME_COMPLETE_NOTIFICATION_TIME, std::move(title),
                                         std::move(message), std::move(badge_path));
      });
  }
}

void Achievements::HandleLeaderboardStartedEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} ({}) started", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications)
  {
    std::string title = event->leaderboard->title;
    std::string message = TRANSLATE_STR("Achievements", "Leaderboard attempt started.");

    GPUThread::RunOnThread([id = event->leaderboard->id, title = std::move(title), message = std::move(message),
                            icon = s_state.game_icon]() mutable {
      if (!FullscreenUI::Initialize())
        return;

      ImGuiFullscreen::AddNotification(fmt::format("leaderboard_{}", id), LEADERBOARD_STARTED_NOTIFICATION_TIME,
                                       std::move(title), std::move(message), std::move(icon));
    });
  }
}

void Achievements::HandleLeaderboardFailedEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} ({}) failed", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications)
  {
    std::string title = event->leaderboard->title;
    std::string message = TRANSLATE_STR("Achievements", "Leaderboard attempt failed.");

    GPUThread::RunOnThread([id = event->leaderboard->id, title = std::move(title), message = std::move(message),
                            icon = s_state.game_icon]() mutable {
      if (!FullscreenUI::Initialize())
        return;

      ImGuiFullscreen::AddNotification(fmt::format("leaderboard_{}", id), LEADERBOARD_FAILED_NOTIFICATION_TIME,
                                       std::move(title), std::move(message), std::move(icon));
    });
  }
}

void Achievements::HandleLeaderboardSubmittedEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} ({}) submitted", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications)
  {
    static const char* value_strings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
      TRANSLATE_NOOP("Achievements", "Your Time: {}{}"),
      TRANSLATE_NOOP("Achievements", "Your Score: {}{}"),
      TRANSLATE_NOOP("Achievements", "Your Value: {}{}"),
    };

    std::string title = event->leaderboard->title;
    std::string message = fmt::format(
      fmt::runtime(Host::TranslateToStringView(
        "Achievements",
        value_strings[std::min<u8>(event->leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)])),
      event->leaderboard->tracker_value ? event->leaderboard->tracker_value : "Unknown",
      g_settings.achievements_spectator_mode ? std::string_view() : TRANSLATE_SV("Achievements", " (Submitting)"));

    GPUThread::RunOnThread([id = event->leaderboard->id, title = std::move(title), message = std::move(message),
                            icon = s_state.game_icon]() mutable {
      if (!FullscreenUI::Initialize())
        return;
      ImGuiFullscreen::AddNotification(fmt::format("leaderboard_{}", id),
                                       static_cast<float>(g_settings.achievements_leaderboard_duration),
                                       std::move(title), std::move(message), std::move(icon));
    });
  }

  if (g_settings.achievements_sound_effects)
    PlatformMisc::PlaySoundAsync(EmuFolders::GetOverridableResourcePath(LBSUBMIT_SOUND_NAME).c_str());
}

void Achievements::HandleLeaderboardScoreboardEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} scoreboard rank {} of {}", event->leaderboard_scoreboard->leaderboard_id,
          event->leaderboard_scoreboard->new_rank, event->leaderboard_scoreboard->num_entries);

  if (g_settings.achievements_leaderboard_notifications)
  {
    static const char* value_strings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
      TRANSLATE_NOOP("Achievements", "Your Time: {} (Best: {})"),
      TRANSLATE_NOOP("Achievements", "Your Score: {} (Best: {})"),
      TRANSLATE_NOOP("Achievements", "Your Value: {} (Best: {})"),
    };

    std::string title = event->leaderboard->title;
    std::string message = fmt::format(
      TRANSLATE_FS("Achievements", "{}\nLeaderboard Position: {} of {}"),
      fmt::format(fmt::runtime(Host::TranslateToStringView(
                    "Achievements",
                    value_strings[std::min<u8>(event->leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)])),
                  event->leaderboard_scoreboard->submitted_score, event->leaderboard_scoreboard->best_score),
      event->leaderboard_scoreboard->new_rank, event->leaderboard_scoreboard->num_entries);

    GPUThread::RunOnThread([id = event->leaderboard->id, title = std::move(title), message = std::move(message),
                            icon = s_state.game_icon]() mutable {
      if (!FullscreenUI::Initialize())
        return;

      ImGuiFullscreen::AddNotification(fmt::format("leaderboard_{}", id),
                                       static_cast<float>(g_settings.achievements_leaderboard_duration),
                                       std::move(title), std::move(message), std::move(icon));
    });
  }
}

void Achievements::HandleLeaderboardTrackerShowEvent(const rc_client_event_t* event)
{
  DEV_LOG("Showing leaderboard tracker: {}: {}", event->leaderboard_tracker->id, event->leaderboard_tracker->display);

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
    .opacity = 0.0f,
    .active = true,
  });
}

void Achievements::HandleLeaderboardTrackerHideEvent(const rc_client_event_t* event)
{
  const u32 id = event->leaderboard_tracker->id;
  auto it = std::find_if(s_state.active_leaderboard_trackers.begin(), s_state.active_leaderboard_trackers.end(),
                         [id](const auto& it) { return it.tracker_id == id; });
  if (it == s_state.active_leaderboard_trackers.end())
    return;

  DEV_LOG("Hiding leaderboard tracker: {}", id);
  it->active = false;
}

void Achievements::HandleLeaderboardTrackerUpdateEvent(const rc_client_event_t* event)
{
  const u32 id = event->leaderboard_tracker->id;
  auto it = std::find_if(s_state.active_leaderboard_trackers.begin(), s_state.active_leaderboard_trackers.end(),
                         [id](const auto& it) { return it.tracker_id == id; });
  if (it == s_state.active_leaderboard_trackers.end())
    return;

  DEV_LOG("Updating leaderboard tracker: {}: {}", event->leaderboard_tracker->id, event->leaderboard_tracker->display);

  it->text = event->leaderboard_tracker->display;
  it->active = true;
}

void Achievements::HandleAchievementChallengeIndicatorShowEvent(const rc_client_event_t* event)
{
  if (const auto it =
        std::find_if(s_state.active_challenge_indicators.begin(), s_state.active_challenge_indicators.end(),
                     [event](const AchievementChallengeIndicator& it) { return it.achievement == event->achievement; });
      it != s_state.active_challenge_indicators.end())
  {
    it->active = true;
    return;
  }

  s_state.active_challenge_indicators.push_back(
    AchievementChallengeIndicator{.achievement = event->achievement,
                                  .badge_path = GetAchievementBadgePath(event->achievement, false),
                                  .opacity = 0.0f,
                                  .active = true});

  DEV_LOG("Show challenge indicator for {} ({})", event->achievement->id, event->achievement->title);
}

void Achievements::HandleAchievementChallengeIndicatorHideEvent(const rc_client_event_t* event)
{
  auto it =
    std::find_if(s_state.active_challenge_indicators.begin(), s_state.active_challenge_indicators.end(),
                 [event](const AchievementChallengeIndicator& it) { return it.achievement == event->achievement; });
  if (it == s_state.active_challenge_indicators.end())
    return;

  DEV_LOG("Hide challenge indicator for {} ({})", event->achievement->id, event->achievement->title);
  it->active = false;
}

void Achievements::HandleAchievementProgressIndicatorShowEvent(const rc_client_event_t* event)
{
  DEV_LOG("Showing progress indicator: {} ({}): {}", event->achievement->id, event->achievement->title,
          event->achievement->measured_progress);

  if (!s_state.active_progress_indicator.has_value())
    s_state.active_progress_indicator.emplace();

  s_state.active_progress_indicator->achievement = event->achievement;
  s_state.active_progress_indicator->badge_path = GetAchievementBadgePath(event->achievement, false);
  s_state.active_progress_indicator->opacity = 0.0f;
  s_state.active_progress_indicator->active = true;
}

void Achievements::HandleAchievementProgressIndicatorHideEvent(const rc_client_event_t* event)
{
  if (!s_state.active_progress_indicator.has_value())
    return;

  DEV_LOG("Hiding progress indicator");
  s_state.active_progress_indicator->active = false;
}

void Achievements::HandleAchievementProgressIndicatorUpdateEvent(const rc_client_event_t* event)
{
  DEV_LOG("Updating progress indicator: {} ({}): {}", event->achievement->id, event->achievement->title,
          event->achievement->measured_progress);
  s_state.active_progress_indicator->achievement = event->achievement;
  s_state.active_progress_indicator->active = true;
}

void Achievements::HandleServerErrorEvent(const rc_client_event_t* event)
{
  std::string message =
    fmt::format(TRANSLATE_FS("Achievements", "Server error in {}:\n{}"),
                event->server_error->api ? event->server_error->api : "UNKNOWN",
                event->server_error->error_message ? event->server_error->error_message : "UNKNOWN");
  ERROR_LOG(message.c_str());
  Host::AddOSDMessage(std::move(message), Host::OSD_ERROR_DURATION);
}

void Achievements::HandleServerDisconnectedEvent(const rc_client_event_t* event)
{
  WARNING_LOG("Server disconnected.");

  GPUThread::RunOnThread([]() {
    if (!FullscreenUI::Initialize())
      return;

    ImGuiFullscreen::ShowToast(
      TRANSLATE_STR("Achievements", "Achievements Disconnected"),
      TRANSLATE_STR("Achievements",
                    "An unlock request could not be completed. We will keep retrying to submit this request."),
      Host::OSD_ERROR_DURATION);
  });
}

void Achievements::HandleServerReconnectedEvent(const rc_client_event_t* event)
{
  WARNING_LOG("Server reconnected.");

  GPUThread::RunOnThread([]() {
    if (!FullscreenUI::Initialize())
      return;

    ImGuiFullscreen::ShowToast(TRANSLATE_STR("Achievements", "Achievements Reconnected"),
                               TRANSLATE_STR("Achievements", "All pending unlock requests have completed."),
                               Host::OSD_INFO_DURATION);
  });
}

void Achievements::EnableHardcodeMode(bool display_message, bool display_game_summary)
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
    GPUThread::RunOnThread([enabled]() {
      if (!FullscreenUI::Initialize())
        return;

      ImGuiFullscreen::ShowToast(std::string(),
                                 enabled ? TRANSLATE_STR("Achievements", "Hardcore mode is now enabled.") :
                                           TRANSLATE_STR("Achievements", "Hardcore mode is now disabled."),
                                 Host::OSD_INFO_DURATION);
    });
  }

  if (HasActiveGame() && display_game_summary)
  {
    UpdateGameSummary(true);
    DisplayAchievementSummary();
  }

  DebugAssert((rc_client_get_hardcore_enabled(s_state.client) != 0) == enabled);

  // Reload setting to permit cheating-like things if we were just disabled.
  if (System::IsValid())
  {
    // Make sure a pre-existing cheat file hasn't been loaded when resetting after enabling HC mode.
    Cheats::ReloadCheats(true, true, false, true, true);

    // Defer settings update in case something is using it.
    Host::RunOnCPUThread([]() { System::ApplySettings(false); });
  }
  else if (System::GetState() == System::State::Starting)
  {
    // Initial HC enable, activate restrictions.
    System::ApplySettings(false);
  }

  // Toss away UI state, because it's invalid now
  ClearUIState();

  Host::OnAchievementsHardcoreModeChanged(enabled);
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
    // if we're active, make sure we've downloaded and activated all the achievements
    // before deserializing, otherwise that state's going to get lost.
    if (s_state.load_game_request)
    {
      // Messy because GPU-thread, but at least it looks pretty.
      GPUThread::RunOnThread([]() {
        FullscreenUI::OpenLoadingScreen(ImGuiManager::LOGO_IMAGE_NAME,
                                        TRANSLATE_SV("Achievements", "Downloading achievements data..."));
      });

      s_state.http_downloader->WaitForAllRequests();

      GPUThread::RunOnThread([]() { FullscreenUI::CloseLoadingScreen(); });
    }

    u32 data_size = 0;
    sw.DoEx(&data_size, REQUIRED_VERSION, 0u);
    if (data_size == 0)
    {
      // reset runtime, no data (state might've been created without cheevos)
      WARNING_LOG("State is missing cheevos data, resetting runtime");
      rc_client_reset(s_state.client);

      return !sw.HasError();
    }

    const std::span<u8> data = sw.GetDeferredBytes(data_size);
    if (sw.HasError())
      return false;

    const int result = rc_client_deserialize_progress_sized(s_state.client, data.data(), data_size);
    if (result != RC_OK)
    {
      WARNING_LOG("Failed to deserialize cheevos state ({}), resetting", result);
      rc_client_reset(s_state.client);
    }

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
        const int result = rc_client_serialize_progress_sized(s_state.client, data.data(), data_size);
        if (result != RC_OK)
        {
          // set data to zero, effectively serializing nothing
          WARNING_LOG("Failed to serialize cheevos state ({})", result);
          data_size = 0;
          sw.SetPosition(size_pos);
          sw.Do(&data_size);
        }
      }
    }

    return !sw.HasError();
  }
}

std::string Achievements::GetAchievementBadgePath(const rc_client_achievement_t* achievement, bool locked,
                                                  bool download_if_missing)
{
  const u32 image_type = locked ? RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED : RC_IMAGE_TYPE_ACHIEVEMENT;
  const std::string path = GetLocalImagePath(achievement->badge_name, image_type);
  if (download_if_missing && !path.empty() && !FileSystem::FileExists(path.c_str()))
  {
    std::string url;
    const char* url_ptr;

    // RAIntegration doesn't set the URL fields.
    if (IsUsingRAIntegration() || !(url_ptr = locked ? achievement->badge_locked_url : achievement->badge_url))
      url = GetImageURL(achievement->badge_name, image_type);
    else
      url = std::string(url_ptr);

    if (url.empty()) [[unlikely]]
      ReportFmtError("Acheivement {} with badge name {} has no badge URL", achievement->id, achievement->badge_name);
    else
      DownloadImage(std::string(url), path);
  }

  return path;
}

const std::string& Achievements::GetCachedAchievementBadgePath(const rc_client_achievement_t* achievement, bool locked)
{
  for (const auto& [l_cheevo, l_path, l_state] : s_state.achievement_badge_paths)
  {
    if (l_cheevo == achievement && l_state == locked)
      return l_path;
  }

  std::string path = GetAchievementBadgePath(achievement, locked);
  return std::get<1>(s_state.achievement_badge_paths.emplace_back(achievement, std::move(path), locked));
}

std::string Achievements::GetLeaderboardUserBadgePath(const rc_client_leaderboard_entry_t* entry)
{
  const std::string path = GetLocalImagePath(entry->user, RC_IMAGE_TYPE_USER);
  if (!FileSystem::FileExists(path.c_str()))
  {
    std::string url = GetImageURL(entry->user, RC_IMAGE_TYPE_USER);
    if (!url.empty())
      DownloadImage(std::move(url), path);
  }

  return path;
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
  rc_client_t* client = s_state.client;
  HTTPDownloader* http = s_state.http_downloader.get();
  const bool is_temporary_client = (client == nullptr);
  std::unique_ptr<HTTPDownloader> temporary_downloader;
  ScopedGuard temporary_client_guard = [&client, is_temporary_client, &temporary_downloader]() {
    if (is_temporary_client)
      DestroyClient(&client, &temporary_downloader);
  };
  if (is_temporary_client)
  {
    if (!CreateClient(&client, &temporary_downloader))
    {
      Error::SetString(error, "Failed to create client.");
      return false;
    }
    http = temporary_downloader.get();
  }

  LoginWithPasswordParameters params = {username, error, nullptr, false};

  params.request =
    rc_client_begin_login_with_password(client, username, password, ClientLoginWithPasswordCallback, &params);
  if (!params.request)
  {
    Error::SetString(error, "Failed to create login request.");
    return false;
  }

  // Wait until the login request completes.
  http->WaitForAllRequests();
  Assert(!params.request);

  // Success? Assume the callback set the error message.
  if (!params.result)
    return false;

  // If we were't a temporary client, get the game loaded.
  if (System::IsValid() && !is_temporary_client)
  {
    IdentifyCurrentGame();
    BeginLoadGame();
  }

  return true;
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
  Host::SetBaseStringSettingValue("Cheevos", "Username", params->username);
  Host::SetBaseStringSettingValue("Cheevos", "Token", EncryptLoginToken(user->token, params->username));
  Host::SetBaseStringSettingValue("Cheevos", "LoginTimestamp", fmt::format("{}", std::time(nullptr)).c_str());
  Host::CommitBaseSettingChanges();

  FinishLogin(client);
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
      std::string message = fmt::format(
        TRANSLATE_FS("Achievements", "Achievement unlocks will not be submitted for this session.\nError: {}"),
        error_message);
      GPUThread::RunOnThread([message = std::move(message)]() mutable {
        if (!GPUThread::HasGPUBackend() || !FullscreenUI::Initialize())
          return;

        ImGuiFullscreen::AddNotification("AchievementsLoginFailed", Host::OSD_ERROR_DURATION,
                                         TRANSLATE_STR("Achievements", "RetroAchievements Login Failed"),
                                         std::move(message), "images/warning.svg");
      });
    }

    return;
  }

  FinishLogin(client);
}

void Achievements::FinishLogin(const rc_client_t* client)
{
  const rc_client_user_t* user = rc_client_get_user_info(client);
  if (!user)
    return;

  PreloadHashDatabase();

  Host::OnAchievementsLoginSuccess(user->username, user->score, user->score_softcore, user->num_unread_messages);

  if (System::IsValid())
  {
    const auto lock = GetLock();
    if (s_state.client == client)
      Host::RunOnCPUThread(ShowLoginNotification);
  }
}

void Achievements::ShowLoginNotification()
{
  const rc_client_user_t* user = rc_client_get_user_info(s_state.client);
  if (!user)
    return;

  if (g_settings.achievements_notifications)
  {
    std::string badge_path = GetLoggedInUserBadgePath();
    std::string title = user->display_name;

    //: Summary for login notification.
    std::string summary = fmt::format(TRANSLATE_FS("Achievements", "Score: {} ({} softcore)\nUnread messages: {}"),
                                      user->score, user->score_softcore, user->num_unread_messages);

    GPUThread::RunOnThread(
      [title = std::move(title), summary = std::move(summary), badge_path = std::move(badge_path)]() mutable {
        if (!FullscreenUI::Initialize())
          return;

        ImGuiFullscreen::AddNotification("achievements_login", LOGIN_NOTIFICATION_TIME, std::move(title),
                                         std::move(summary), std::move(badge_path));
      });
  }
}

const char* Achievements::GetLoggedInUserName()
{
  const rc_client_user_t* user = rc_client_get_user_info(s_state.client);
  if (!user) [[unlikely]]
    return nullptr;

  return user->username;
}

std::string Achievements::GetLoggedInUserBadgePath()
{
  std::string badge_path;

  const rc_client_user_t* user = rc_client_get_user_info(s_state.client);
  if (!user) [[unlikely]]
    return badge_path;

  badge_path = GetLocalImagePath(user->username, RC_IMAGE_TYPE_USER);
  if (!badge_path.empty() && !FileSystem::FileExists(badge_path.c_str())) [[unlikely]]
  {
    std::string url;
    if (IsUsingRAIntegration() || !user->avatar_url)
      url = GetImageURL(user->username, RC_IMAGE_TYPE_USER);
    else
      url = user->avatar_url;

    DownloadImage(std::move(url), badge_path);
  }

  return badge_path;
}

u32 Achievements::GetPauseThrottleFrames()
{
  if (!IsActive() || !IsHardcoreModeActive())
    return 0;

  u32 frames_remaining = 0;
  return rc_client_can_pause(s_state.client, &frames_remaining) ? 0 : frames_remaining;
}

void Achievements::Logout()
{
  if (IsActive())
  {
    const auto lock = GetLock();

    if (HasActiveGame())
    {
      ClearGameInfo();
      DisableHardcoreMode(false, false);
    }

    CancelHashDatabaseRequests();

    INFO_LOG("Logging out...");
    rc_client_logout(s_state.client);
  }

  INFO_LOG("Clearing credentials...");
  Host::DeleteBaseSettingValue("Cheevos", "Username");
  Host::DeleteBaseSettingValue("Cheevos", "Token");
  Host::DeleteBaseSettingValue("Cheevos", "LoginTimestamp");
  Host::CommitBaseSettingChanges();
  ClearProgressDatabase();
}

bool Achievements::ConfirmHardcoreModeDisable(const char* trigger)
{
  // I really hope this doesn't deadlock :/
  const bool confirmed = Host::ConfirmMessage(
    TRANSLATE("Achievements", "Confirm Hardcore Mode Disable"),
    fmt::format(TRANSLATE_FS("Achievements", "{0} cannot be performed while hardcore mode is active. Do you "
                                             "want to disable hardcore mode? {0} will be cancelled if you select No."),
                trigger));
  if (!confirmed)
    return false;

  DisableHardcoreMode(true, true);
  return true;
}

void Achievements::ConfirmHardcoreModeDisableAsync(const char* trigger, std::function<void(bool)> callback)
{
  Host::ConfirmMessageAsync(
    TRANSLATE_STR("Achievements", "Confirm Hardcore Mode Disable"),
    fmt::format(TRANSLATE_FS("Achievements", "{0} cannot be performed while hardcore mode is active. Do you want to "
                                             "disable hardcore mode? {0} will be cancelled if you select No."),
                trigger),
    [callback = std::move(callback)](bool res) mutable {
      // don't run the callback in the middle of rendering the UI
      Host::RunOnCPUThread([callback = std::move(callback), res]() {
        if (res)
          DisableHardcoreMode(true, true);
        callback(res);
      });
    });
}

void Achievements::ClearUIState()
{
  if (!FullscreenUI::IsInitialized())
    return;

  CloseLeaderboard();

  s_state.achievement_badge_paths = {};

  s_state.leaderboard_user_icon_paths = {};
  s_state.leaderboard_entry_lists = {};
  if (s_state.leaderboard_list)
  {
    rc_client_destroy_leaderboard_list(s_state.leaderboard_list);
    s_state.leaderboard_list = nullptr;
  }

  if (s_state.achievement_list)
  {
    rc_client_destroy_achievement_list(s_state.achievement_list);
    s_state.achievement_list = nullptr;
  }

  s_state.most_recent_unlock.reset();
  s_state.achievement_nearest_completion.reset();
}

template<typename T>
static float IndicatorOpacity(float delta_time, T& i)
{
  float target, rate;
  if (i.active)
  {
    target = 1.0f;
    rate = Achievements::INDICATOR_FADE_IN_TIME;
  }
  else
  {
    target = 0.0f;
    rate = -Achievements::INDICATOR_FADE_OUT_TIME;
  }

  if (i.opacity != target)
    i.opacity = ImSaturate(i.opacity + (delta_time / rate));

  return i.opacity;
}

void Achievements::DrawGameOverlays()
{
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::ModAlpha;
  using ImGuiFullscreen::RenderShadowedTextClipped;
  using ImGuiFullscreen::UIStyle;

  if (!HasActiveGame() || !g_settings.achievements_overlays)
    return;

  const auto lock = GetLock();

  constexpr float bg_opacity = 0.8f;

  const float margin =
    std::max(ImCeil(ImGuiManager::GetScreenMargin() * ImGuiManager::GetGlobalScale()), LayoutScale(10.0f));
  const float spacing = LayoutScale(10.0f);
  const float padding = LayoutScale(10.0f);
  const float rounding = LayoutScale(10.0f);
  const ImVec2 image_size =
    LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT);
  const ImGuiIO& io = ImGui::GetIO();
  ImVec2 position = ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin);
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  if (!s_state.active_challenge_indicators.empty())
  {
    const float x_advance = image_size.x + spacing;
    ImVec2 current_position = ImVec2(position.x - image_size.x, position.y - image_size.y);

    for (auto it = s_state.active_challenge_indicators.begin(); it != s_state.active_challenge_indicators.end();)
    {
      AchievementChallengeIndicator& indicator = *it;
      const float opacity = IndicatorOpacity(io.DeltaTime, indicator);

      GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(indicator.badge_path);
      if (badge)
      {
        dl->AddImage(badge, current_position, current_position + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity)));
        current_position.x -= x_advance;
      }

      if (!indicator.active && opacity <= 0.01f)
      {
        DEV_LOG("Remove challenge indicator");
        it = s_state.active_challenge_indicators.erase(it);
      }
      else
      {
        ++it;
      }
    }

    position.y -= image_size.y + padding;
  }

  if (s_state.active_progress_indicator.has_value())
  {
    AchievementProgressIndicator& indicator = s_state.active_progress_indicator.value();
    const float opacity = IndicatorOpacity(io.DeltaTime, indicator);

    const std::string_view text = s_state.active_progress_indicator->achievement->measured_progress;
    const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                         0.0f, IMSTR_START_END(text));

    const ImVec2 box_min = ImVec2(position.x - image_size.x - text_size.x - spacing - padding * 2.0f,
                                  position.y - image_size.y - padding * 2.0f);
    const ImVec2 box_max = position;

    dl->AddRectFilled(box_min, box_max,
                      ImGui::GetColorU32(ModAlpha(UIStyle.ToastBackgroundColor, opacity * bg_opacity)), rounding);

    GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(indicator.badge_path);
    if (badge)
    {
      const ImVec2 badge_pos = box_min + ImVec2(padding, padding);
      dl->AddImage(badge, badge_pos, badge_pos + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                   ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity)));
    }

    const ImVec2 text_pos =
      box_min + ImVec2(padding + image_size.x + spacing, (box_max.y - box_min.y - text_size.y) * 0.5f);
    const ImRect text_clip_rect(text_pos, box_max);
    RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos, box_max,
                              ImGui::GetColorU32(ModAlpha(UIStyle.ToastTextColor, opacity)), text, &text_size,
                              ImVec2(0.0f, 0.0f), 0.0f, &text_clip_rect);

    if (!indicator.active && opacity <= 0.01f)
    {
      DEV_LOG("Remove progress indicator");
      s_state.active_progress_indicator.reset();
    }

    position.y -= image_size.y + padding * 3.0f;
  }

  if (!s_state.active_leaderboard_trackers.empty())
  {
    for (auto it = s_state.active_leaderboard_trackers.begin(); it != s_state.active_leaderboard_trackers.end();)
    {
      LeaderboardTrackerIndicator& indicator = *it;
      const float opacity = IndicatorOpacity(io.DeltaTime, indicator);

      TinyString width_string;
      width_string.append(ICON_FA_STOPWATCH);
      for (u32 i = 0; i < indicator.text.length(); i++)
        width_string.append('0');
      const ImVec2 size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX, 0.0f,
                                                      IMSTR_START_END(width_string));

      const ImRect box(ImVec2(position.x - size.x - padding * 2.0f, position.y - size.y - padding * 2.0f), position);
      dl->AddRectFilled(box.Min, box.Max,
                        ImGui::GetColorU32(ModAlpha(UIStyle.ToastBackgroundColor, opacity * bg_opacity)), rounding);

      const u32 text_col = ImGui::GetColorU32(ModAlpha(UIStyle.ToastTextColor, opacity));
      const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                           0.0f, IMSTR_START_END(indicator.text));
      const ImVec2 text_pos = ImVec2(box.Max.x - padding - text_size.x, box.Min.y + padding);
      RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos, box.Max,
                                text_col, indicator.text, &text_size, ImVec2(0.0f, 0.0f), 0.0f, &box);

      const ImVec2 icon_pos = ImVec2(box.Min.x + padding, box.Min.y + padding);
      RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, icon_pos, box.Max,
                                text_col, ICON_FA_STOPWATCH, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &box);

      if (!indicator.active && opacity <= 0.01f)
      {
        DEV_LOG("Remove tracker indicator");
        it = s_state.active_leaderboard_trackers.erase(it);
      }
      else
      {
        ++it;
      }

      position.x = box.Min.x - padding;
    }

    // Uncomment if there are any other overlays above this one.
    // position.y -= image_size.y - padding * 3.0f;
  }
}

#ifndef __ANDROID__

void Achievements::DrawPauseMenuOverlays(float start_pos_y)
{
  using ImGuiFullscreen::DarkerColor;
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::ModAlpha;
  using ImGuiFullscreen::UIStyle;

  if (!HasActiveGame() || !HasAchievements())
    return;

  const auto lock = GetLock();

  const ImVec2& display_size = ImGui::GetIO().DisplaySize;
  const float box_margin = LayoutScale(20.0f);
  const float box_width = LayoutScale(450.0f);
  const float box_padding = LayoutScale(15.0f);
  const float box_content_width = box_width - box_padding - box_padding;
  const float box_rounding = LayoutScale(20.0f);
  const u32 box_background_color = ImGui::GetColorU32(ModAlpha(UIStyle.BackgroundColor, 0.8f));
  const ImU32 title_text_color = ImGui::GetColorU32(UIStyle.BackgroundTextColor) | IM_COL32_A_MASK;
  const ImU32 text_color = ImGui::GetColorU32(DarkerColor(UIStyle.BackgroundTextColor)) | IM_COL32_A_MASK;
  const float paragraph_spacing = LayoutScale(10.0f);
  const float text_spacing = LayoutScale(2.0f);

  const float progress_height = LayoutScale(20.0f);
  const float progress_rounding = LayoutScale(5.0f);
  const float badge_size = LayoutScale(40.0f);
  const float badge_text_width = box_content_width - badge_size - (text_spacing * 3.0f);
  const bool disconnected = rc_client_is_disconnected(s_state.client);
  const int pending_count = disconnected ? rc_client_get_award_achievement_pending_count(s_state.client) : 0;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  const auto get_achievement_height = [&badge_size, &badge_text_width, &text_spacing](std::string_view description,
                                                                                      bool show_measured) {
    const ImVec2 description_size =
      description.empty() ? ImVec2(0.0f, 0.0f) :
                            UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                        badge_text_width, IMSTR_START_END(description));
    const float text_height = UIStyle.MediumFontSize + text_spacing + description_size.y;
    return std::max(text_height, badge_size);
  };

  float box_height =
    box_padding + box_padding + UIStyle.MediumFontSize + paragraph_spacing + progress_height + paragraph_spacing;
  if (pending_count > 0)
  {
    box_height += UIStyle.MediumFontSize + paragraph_spacing;
  }
  if (s_state.most_recent_unlock.has_value())
  {
    box_height += UIStyle.MediumFontSize + paragraph_spacing +
                  get_achievement_height(s_state.most_recent_unlock->description, false) +
                  (s_state.achievement_nearest_completion ? (paragraph_spacing + paragraph_spacing) : 0.0f);
  }
  if (s_state.achievement_nearest_completion.has_value())
  {
    box_height += UIStyle.MediumFontSize + paragraph_spacing +
                  get_achievement_height(s_state.achievement_nearest_completion->description, true);
  }

  ImVec2 box_min = ImVec2(display_size.x - box_width - box_margin, start_pos_y + box_margin);
  ImVec2 box_max = ImVec2(box_min.x + box_width, box_min.y + box_height);
  ImVec2 text_pos = ImVec2(box_min.x + box_padding, box_min.y + box_padding);
  ImVec2 text_size;

  dl->AddRectFilled(box_min, box_max, box_background_color, box_rounding);

  const auto draw_achievement_with_summary = [&box_max, &badge_text_width, &dl, &title_text_color, &text_color,
                                              &text_spacing, &text_pos,
                                              &badge_size](std::string_view title, std::string_view description,
                                                           const std::string& badge_path, bool show_measured) {
    const ImVec2 image_max = ImVec2(text_pos.x + badge_size, text_pos.y + badge_size);
    ImVec2 badge_text_pos = ImVec2(image_max.x + (text_spacing * 3.0f), text_pos.y);
    const ImVec4 clip_rect = ImVec4(badge_text_pos.x, badge_text_pos.y, badge_text_pos.x + badge_text_width, box_max.y);
    const ImVec2 description_size =
      description.empty() ? ImVec2(0.0f, 0.0f) :
                            UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                        badge_text_width, IMSTR_START_END(description));

    GPUTexture* badge_tex = ImGuiFullscreen::GetCachedTextureAsync(badge_path);
    dl->AddImage(badge_tex, text_pos, image_max);

    if (!title.empty())
    {
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, badge_text_pos, title_text_color,
                  IMSTR_START_END(title), 0.0f, &clip_rect);
      badge_text_pos.y += UIStyle.MediumFontSize + text_spacing;
    }

    if (!description.empty())
    {
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, badge_text_pos, text_color,
                  IMSTR_START_END(description), badge_text_width, &clip_rect);
      badge_text_pos.y += description_size.y;
    }

    text_pos.y = badge_text_pos.y;
  };

  TinyString buffer;

  // title
  {
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, text_color,
                TRANSLATE_DISAMBIG("Achievements", "Achievements Unlocked", "Pause Menu"));
    const float unlocked_fraction = static_cast<float>(s_state.game_summary.num_unlocked_achievements) /
                                    static_cast<float>(s_state.game_summary.num_core_achievements);
    buffer.format("{}%", static_cast<u32>(std::round(unlocked_fraction * 100.0f)));
    text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                            IMSTR_START_END(buffer));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight,
                ImVec2(text_pos.x + (box_content_width - text_size.x), text_pos.y), text_color,
                IMSTR_START_END(buffer));
    text_pos.y += UIStyle.MediumFontSize + paragraph_spacing;

    const ImRect progress_bb(text_pos, text_pos + ImVec2(box_content_width, progress_height));
    const u32 progress_color = ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryColor));
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(UIStyle.PrimaryDarkColor),
                      progress_rounding);
    if (s_state.game_summary.num_unlocked_achievements > 0)
    {
      dl->AddRectFilled(progress_bb.Min,
                        ImVec2(progress_bb.Min.x + unlocked_fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                        progress_color, progress_rounding);
    }

    buffer.format("{}/{}", s_state.game_summary.num_unlocked_achievements, s_state.game_summary.num_core_achievements);
    text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                            IMSTR_START_END(buffer));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight,
                ImVec2(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                       progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f)),
                ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(buffer));
    text_pos.y += progress_height + paragraph_spacing;

    if (pending_count > 0)
    {
      buffer.format(ICON_EMOJI_WARNING " {}",
                    TRANSLATE_PLURAL_SSTR("Achievements", "%n unlocks have not been confirmed by the server.",
                                          "Pause Menu", pending_count));
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, title_text_color,
                  IMSTR_START_END(buffer));
      text_pos.y += UIStyle.MediumFontSize + paragraph_spacing;
    }
  }

  if (s_state.most_recent_unlock.has_value())
  {
    buffer.format(ICON_FA_LOCK_OPEN " {}", TRANSLATE_DISAMBIG_SV("Achievements", "Most Recent", "Pause Menu"));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, text_color,
                IMSTR_START_END(buffer));
    text_pos.y += UIStyle.MediumFontSize + paragraph_spacing;

    draw_achievement_with_summary(s_state.most_recent_unlock->title, s_state.most_recent_unlock->description,
                                  s_state.most_recent_unlock->badge_path, false);

    // extra spacing if we have two
    text_pos.y += s_state.achievement_nearest_completion ? (paragraph_spacing + paragraph_spacing) : 0.0f;
  }

  if (s_state.achievement_nearest_completion.has_value())
  {
    buffer.format(ICON_FA_LOCK " {}", TRANSLATE_DISAMBIG_SV("Achievements", "Nearest Completion", "Pause Menu"));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, text_color,
                IMSTR_START_END(buffer));
    text_pos.y += UIStyle.MediumFontSize + paragraph_spacing;

    draw_achievement_with_summary(s_state.achievement_nearest_completion->title,
                                  s_state.achievement_nearest_completion->description,
                                  s_state.achievement_nearest_completion->badge_path, true);
    text_pos.y += paragraph_spacing;
  }

  // Challenge indicators

  if (!s_state.active_challenge_indicators.empty())
  {
    box_height = box_padding + box_padding + UIStyle.MediumFontSize;
    for (size_t i = 0; i < s_state.active_challenge_indicators.size(); i++)
    {
      const AchievementChallengeIndicator& indicator = s_state.active_challenge_indicators[i];
      box_height += paragraph_spacing + get_achievement_height(indicator.achievement->description, false) +
                    ((i == s_state.active_challenge_indicators.size() - 1) ? paragraph_spacing : 0.0f);
    }

    box_min = ImVec2(box_min.x, box_max.y + box_margin);
    box_max = ImVec2(box_min.x + box_width, box_min.y + box_height);
    text_pos = ImVec2(box_min.x + box_padding, box_min.y + box_padding);

    dl->AddRectFilled(box_min, box_max, box_background_color, box_rounding);

    buffer.format(ICON_FA_STOPWATCH " {}",
                  TRANSLATE_DISAMBIG_SV("Achievements", "Active Challenge Achievements", "Pause Menu"));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, text_color,
                IMSTR_START_END(buffer));
    text_pos.y += UIStyle.MediumFontSize;

    for (const AchievementChallengeIndicator& indicator : s_state.active_challenge_indicators)
    {
      text_pos.y += paragraph_spacing;
      draw_achievement_with_summary(indicator.achievement->title, indicator.achievement->description,
                                    indicator.badge_path, false);
      text_pos.y += paragraph_spacing;
    }
  }
}

bool Achievements::PrepareAchievementsWindow()
{
  auto lock = Achievements::GetLock();

  s_state.achievement_badge_paths = {};

  if (s_state.achievement_list)
    rc_client_destroy_achievement_list(s_state.achievement_list);
  s_state.achievement_list = rc_client_create_achievement_list(
    s_state.client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS /*RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE*/);
  if (!s_state.achievement_list)
  {
    ERROR_LOG("rc_client_create_achievement_list() returned null");
    return false;
  }

  return true;
}

void Achievements::DrawAchievementsWindow()
{
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::RenderShadowedTextClipped;
  using ImGuiFullscreen::UIStyle;

  const auto lock = Achievements::GetLock();

  // achievements can get turned off via the main UI
  if (!s_state.achievement_list)
  {
    FullscreenUI::ReturnToPreviousWindow();
    return;
  }

  static constexpr float alpha = 0.8f;
  static constexpr float heading_alpha = 0.95f;
  static constexpr float heading_height_unscaled = 110.0f;

  const ImVec4 background = ImGuiFullscreen::ModAlpha(UIStyle.BackgroundColor, alpha);
  const ImVec4 heading_background = ImGuiFullscreen::ModAlpha(UIStyle.BackgroundColor, heading_alpha);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const float heading_height = LayoutScale(heading_height_unscaled);
  bool close_window = false;

  if (ImGuiFullscreen::BeginFullscreenWindow(
        ImVec2(), ImVec2(display_size.x, heading_height), "achievements_heading", heading_background, 0.0f, ImVec2(),
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse))
  {
    ImRect bb;
    bool visible, hovered;
    ImGuiFullscreen::MenuButtonFrame("achievements_heading", false, heading_height_unscaled, &visible, &hovered,
                                     &bb.Min, &bb.Max, 0, heading_alpha);
    if (visible)
    {
      const float padding = LayoutScale(10.0f);
      const float spacing = LayoutScale(10.0f);
      const float image_height = LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      if (!s_state.game_icon.empty())
      {
        GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(s_state.game_icon);
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge, icon_min, icon_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                               IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      SmallString text;
      ImVec2 text_size;

      close_window =
        (ImGuiFullscreen::FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true) ||
         ImGuiFullscreen::WantsToCloseMenu());

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFontSize));
      text.assign(s_state.game_title);

      if (rc_client_get_hardcore_enabled(s_state.client))
        text.append(TRANSLATE_SV("Achievements", " (Hardcore Mode)"));

      top += UIStyle.LargeFontSize + spacing;

      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min, title_bb.Max,
                                ImGui::GetColorU32(ImGuiCol_Text), text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &title_bb);

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
      if (s_state.game_summary.num_core_achievements > 0)
      {
        if (s_state.game_summary.num_unlocked_achievements == s_state.game_summary.num_core_achievements)
        {
          text = TRANSLATE_PLURAL_SSTR("Achievements", "You have unlocked all achievements and earned %n points!",
                                       "Point count", s_state.game_summary.points_unlocked);
        }
        else
        {
          text.format(TRANSLATE_FS("Achievements",
                                   "You have unlocked {0} of {1} achievements, earning {2} of {3} possible points."),
                      s_state.game_summary.num_unlocked_achievements, s_state.game_summary.num_core_achievements,
                      s_state.game_summary.points_unlocked, s_state.game_summary.points_core);
        }
      }
      else
      {
        text.assign(TRANSLATE_SV("Achievements", "This game has no achievements."));
      }

      top += UIStyle.MediumFontSize + spacing;

      RenderShadowedTextClipped(
        UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, summary_bb.Min, summary_bb.Max,
        ImGui::GetColorU32(ImGuiFullscreen::DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])), text, nullptr,
        ImVec2(0.0f, 0.0f), 0.0f, &summary_bb);

      if (s_state.game_summary.num_core_achievements > 0)
      {
        const float progress_height = LayoutScale(20.0f);
        const float progress_rounding = LayoutScale(5.0f);
        const ImRect progress_bb(ImVec2(left, top), ImVec2(right, top + progress_height));
        const float fraction = static_cast<float>(s_state.game_summary.num_unlocked_achievements) /
                               static_cast<float>(s_state.game_summary.num_core_achievements);
        dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(UIStyle.PrimaryDarkColor),
                          progress_rounding);
        if (s_state.game_summary.num_unlocked_achievements > 0)
        {
          dl->AddRectFilled(progress_bb.Min,
                            ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                            ImGui::GetColorU32(UIStyle.SecondaryColor), progress_rounding);
        }

        text.format("{}%", static_cast<u32>(std::round(fraction * 100.0f)));
        text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                                IMSTR_START_END(text));
        const ImVec2 text_pos(
          progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
        dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos,
                    ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(text));
        // top += progress_height + spacing;
      }
    }
  }
  ImGuiFullscreen::EndFullscreenWindow();

  ImGui::SetNextWindowBgAlpha(alpha);

  // See note in FullscreenUI::DrawSettingsWindow().
  if (ImGuiFullscreen::IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (ImGuiFullscreen::BeginFullscreenWindow(
        ImVec2(0.0f, heading_height),
        ImVec2(display_size.x, display_size.y - heading_height - LayoutScale(ImGuiFullscreen::LAYOUT_FOOTER_HEIGHT)),
        "achievements", background, 0.0f,
        ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, ImGuiFullscreen::LAYOUT_MENU_WINDOW_Y_PADDING), 0))
  {
    static bool buckets_collapsed[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS] = {};
    static constexpr std::pair<const char*, const char*> bucket_names[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS] = {
      {ICON_FA_EXCLAMATION_TRIANGLE, TRANSLATE_NOOP("Achievements", "Unknown")},
      {ICON_FA_LOCK, TRANSLATE_NOOP("Achievements", "Locked")},
      {ICON_FA_UNLOCK, TRANSLATE_NOOP("Achievements", "Unlocked")},
      {ICON_FA_EXCLAMATION_TRIANGLE, TRANSLATE_NOOP("Achievements", "Unsupported")},
      {ICON_FA_QUESTION_CIRCLE, TRANSLATE_NOOP("Achievements", "Unofficial")},
      {ICON_EMOJI_UNLOCKED, TRANSLATE_NOOP("Achievements", "Recently Unlocked")},
      {ICON_FA_STOPWATCH, TRANSLATE_NOOP("Achievements", "Active Challenges")},
      {ICON_FA_RULER_HORIZONTAL, TRANSLATE_NOOP("Achievements", "Almost There")},
    };

    ImGuiFullscreen::ResetFocusHere();
    ImGuiFullscreen::BeginMenuButtons();

    for (u32 bucket_type : {RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED, RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED, RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED})
    {
      for (u32 bucket_idx = 0; bucket_idx < s_state.achievement_list->num_buckets; bucket_idx++)
      {
        const rc_client_achievement_bucket_t& bucket = s_state.achievement_list->buckets[bucket_idx];
        if (bucket.bucket_type != bucket_type)
          continue;

        DebugAssert(bucket.bucket_type < NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS);

        // TODO: Once subsets are supported, this will need to change.
        bool& bucket_collapsed = buckets_collapsed[bucket.bucket_type];
        bucket_collapsed ^= ImGuiFullscreen::MenuHeadingButton(
          TinyString::from_format("{} {}", bucket_names[bucket.bucket_type].first,
                                  Host::TranslateToStringView("Achievements", bucket_names[bucket.bucket_type].second)),
          bucket_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
        if (!bucket_collapsed)
        {
          for (u32 i = 0; i < bucket.num_achievements; i++)
            DrawAchievement(bucket.achievements[i]);
        }
      }
    }

    ImGuiFullscreen::EndMenuButtons();
  }
  ImGuiFullscreen::EndFullscreenWindow();

  ImGuiFullscreen::SetFullscreenStatusText(std::array{
    std::make_pair(ICON_PF_ACHIEVEMENTS_MISSABLE, TRANSLATE_SV("Achievements", "Missable")),
    std::make_pair(ICON_PF_ACHIEVEMENTS_PROGRESSION, TRANSLATE_SV("Achievements", "Progression")),
    std::make_pair(ICON_PF_ACHIEVEMENTS_WIN, TRANSLATE_SV("Achievements", "Win Condition")),
    std::make_pair(ICON_FA_LOCK, TRANSLATE_SV("Achievements", "Locked")),
    std::make_pair(ICON_EMOJI_UNLOCKED, TRANSLATE_SV("Achievements", "Unlocked")),
  });
  ImGuiFullscreen::SetFullscreenFooterText(
    std::array{std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_XBOX_DPAD_UP_DOWN :
                                                                        ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN,
                              TRANSLATE_SV("Achievements", "Change Selection")),
               std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_BUTTON_A : ICON_PF_ENTER,
                              TRANSLATE_SV("Achievements", "View Details")),
               std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_BUTTON_B : ICON_PF_ESC,
                              TRANSLATE_SV("Achievements", "Back"))},
    FullscreenUI::GetBackgroundAlpha());

  if (close_window)
    FullscreenUI::ReturnToPreviousWindow();
}

void Achievements::DrawAchievement(const rc_client_achievement_t* cheevo)
{
  using ImGuiFullscreen::DarkerColor;
  using ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT;
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::LayoutUnscale;
  using ImGuiFullscreen::RenderShadowedTextClipped;
  using ImGuiFullscreen::UIStyle;

  static constexpr float alpha = 0.8f;
  static constexpr float progress_height_unscaled = 20.0f;
  static constexpr float progress_spacing_unscaled = 5.0f;
  static constexpr float progress_rounding_unscaled = 5.0f;
  static constexpr float spacing_unscaled = 4.0f;

  const float spacing = ImGuiFullscreen::LayoutScale(spacing_unscaled);
  const u32 text_color = ImGui::GetColorU32(UIStyle.SecondaryTextColor);
  const u32 summary_color = ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryTextColor));
  const u32 rarity_color = ImGui::GetColorU32(DarkerColor(DarkerColor(UIStyle.SecondaryTextColor)));

  const bool is_unlocked = (cheevo->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  const std::string_view measured_progress(cheevo->measured_progress);
  const bool is_measured = !is_unlocked && !measured_progress.empty();
  const float unlock_rarity_height = spacing_unscaled + ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE;
  const ImVec2 points_template_size = UIStyle.Font->CalcTextSizeA(
    UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX, 0.0f, TRANSLATE("Achievements", "XXX points"));
  const size_t summary_length = std::strlen(cheevo->description);
  const float summary_wrap_width =
    (ImGui::GetCurrentWindow()->WorkRect.GetWidth() - (ImGui::GetStyle().FramePadding.x * 2.0f) -
     LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT + 30.0f) - points_template_size.x);
  const ImVec2 summary_text_size =
    UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX, summary_wrap_width,
                                cheevo->description, cheevo->description + summary_length);

  // Messy, but need to undo LayoutScale in MenuButtonFrame()...
  const float extra_summary_height = std::max(LayoutUnscale(summary_text_size.y) - LAYOUT_MENU_BUTTON_HEIGHT, 0.0f);

  ImRect bb;
  bool visible, hovered;
  const bool clicked = ImGuiFullscreen::MenuButtonFrame(
    TinyString::from_format("chv_{}", cheevo->id), true,
    !is_measured ? (LAYOUT_MENU_BUTTON_HEIGHT + extra_summary_height + unlock_rarity_height) :
                   (LAYOUT_MENU_BUTTON_HEIGHT + extra_summary_height + unlock_rarity_height + progress_height_unscaled +
                    progress_spacing_unscaled),
    &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const std::string& badge_path =
    GetCachedAchievementBadgePath(cheevo, cheevo->state != RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);

  const ImVec2 image_size(
    LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT));
  if (!badge_path.empty())
  {
    GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(badge_path);
    if (badge)
    {
      const ImRect image_bb = ImGuiFullscreen::CenterImage(ImRect(bb.Min, bb.Min + image_size), badge);
      ImGui::GetWindowDrawList()->AddImage(badge, image_bb.Min, image_bb.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                           IM_COL32(255, 255, 255, 255));
    }
  }

  SmallString text;

  const float midpoint = bb.Min.y + UIStyle.LargeFontSize + spacing;
  text = TRANSLATE_PLURAL_SSTR("Achievements", "%n points", "Achievement points", cheevo->points);
  const ImVec2 points_size =
    UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX, 0.0f, IMSTR_START_END(text));
  const float points_template_start = bb.Max.x - points_template_size.x;
  const float points_start = points_template_start + ((points_template_size.x - points_size.x) * 0.5f);

  std::string_view right_icon_text;
  switch (cheevo->type)
  {
    case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE:
      right_icon_text = ICON_PF_ACHIEVEMENTS_MISSABLE; // Missable
      break;

    case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION:
      right_icon_text = ICON_PF_ACHIEVEMENTS_PROGRESSION; // Progression
      break;

    case RC_CLIENT_ACHIEVEMENT_TYPE_WIN:
      right_icon_text = ICON_PF_ACHIEVEMENTS_WIN; // Win Condition
      break;

      // Just use the lock for standard achievements.
    case RC_CLIENT_ACHIEVEMENT_TYPE_STANDARD:
    default:
      right_icon_text = is_unlocked ? ICON_EMOJI_UNLOCKED : ICON_FA_LOCK;
      break;
  }

  const ImVec2 right_icon_size = UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, FLT_MAX,
                                                             0.0f, IMSTR_START_END(right_icon_text));

  const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(points_start, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), ImVec2(points_start, midpoint + summary_text_size.y));
  const ImRect unlock_rarity_bb(summary_bb.Min.x, summary_bb.Max.y + spacing, summary_bb.Max.x,
                                summary_bb.Max.y +
                                  LayoutScale(spacing_unscaled + ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE));
  const ImRect points_bb(ImVec2(points_start, midpoint), bb.Max);
  const ImRect lock_bb(ImVec2(points_template_start + ((points_template_size.x - right_icon_size.x) * 0.5f), bb.Min.y),
                       ImVec2(bb.Max.x, midpoint));

  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min, title_bb.Max,
                            text_color, cheevo->title, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &title_bb);
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, lock_bb.Min, lock_bb.Max,
                            text_color, right_icon_text, &right_icon_size, ImVec2(0.0f, 0.0f), 0.0f, &lock_bb);
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, points_bb.Min,
                            points_bb.Max, summary_color, text, &points_size, ImVec2(0.0f, 0.0f), 0.0f, &points_bb);

  if (cheevo->description && summary_length > 0)
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, summary_bb.Min,
                              summary_bb.Max, summary_color, std::string_view(cheevo->description, summary_length),
                              &summary_text_size, ImVec2(0.0f, 0.0f), summary_wrap_width, &summary_bb);
  }

  // display hc if hc is active
  const float rarity_to_display = IsHardcoreModeActive() ? cheevo->rarity_hardcore : cheevo->rarity;

  if (is_unlocked)
  {
    TinyString date;
    FullscreenUI::TimeToPrintableString(&date, cheevo->unlock_time);
    text.format(TRANSLATE_FS("Achievements", "Unlocked: {} | {:.1f}% of players have this achievement"), date,
                rarity_to_display);

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, unlock_rarity_bb.Min,
                              unlock_rarity_bb.Max, rarity_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f,
                              &unlock_rarity_bb);
  }
  else
  {
    text.format(TRANSLATE_FS("Achievements", "{:.1f}% of players have this achievement"), rarity_to_display);
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, unlock_rarity_bb.Min,
                              unlock_rarity_bb.Max, rarity_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f,
                              &unlock_rarity_bb);
  }

  if (!is_unlocked && is_measured)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float progress_height = LayoutScale(progress_height_unscaled);
    const float progress_spacing = LayoutScale(progress_spacing_unscaled);
    const float progress_rounding = LayoutScale(progress_rounding_unscaled);
    const ImRect progress_bb(summary_bb.Min.x, unlock_rarity_bb.Max.y + progress_spacing,
                             summary_bb.Max.x - progress_spacing,
                             unlock_rarity_bb.Max.y + progress_spacing + progress_height);
    const float fraction = cheevo->measured_percent * 0.01f;
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIStyle.PrimaryDarkColor),
                      progress_rounding);
    dl->AddRectFilled(progress_bb.Min, ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                      ImGui::GetColorU32(ImGuiFullscreen::UIStyle.SecondaryColor), progress_rounding);

    const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                         0.0f, IMSTR_START_END(measured_progress));
    const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos,
                ImGui::GetColorU32(ImGuiFullscreen::UIStyle.PrimaryTextColor), IMSTR_START_END(measured_progress));
  }

  if (clicked)
  {
    const SmallString url = SmallString::from_format(fmt::runtime(ACHEIVEMENT_DETAILS_URL_TEMPLATE), cheevo->id);
    INFO_LOG("Opening achievement details: {}", url);
    Host::OpenURL(url);
  }
}

bool Achievements::PrepareLeaderboardsWindow()
{
  auto lock = Achievements::GetLock();
  rc_client_t* const client = s_state.client;

  s_state.achievement_badge_paths = {};
  CloseLeaderboard();
  if (s_state.leaderboard_list)
    rc_client_destroy_leaderboard_list(s_state.leaderboard_list);
  s_state.leaderboard_list = rc_client_create_leaderboard_list(client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
  if (!s_state.leaderboard_list)
  {
    ERROR_LOG("rc_client_create_leaderboard_list() returned null");
    return false;
  }

  return true;
}

void Achievements::DrawLeaderboardsWindow()
{
  using ImGuiFullscreen::DarkerColor;
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::RenderShadowedTextClipped;
  using ImGuiFullscreen::UIStyle;

  static constexpr float alpha = 0.8f;
  static constexpr float heading_alpha = 0.95f;
  static constexpr float heading_height_unscaled = 110.0f;
  static constexpr float tab_height_unscaled = 50.0f;

  const auto lock = Achievements::GetLock();
  if (!s_state.leaderboard_list)
  {
    FullscreenUI::ReturnToPreviousWindow();
    return;
  }

  const bool is_leaderboard_open = (s_state.open_leaderboard != nullptr);
  bool close_leaderboard_on_exit = false;

  ImRect bb;
  SmallString text;

  const ImVec4 background = ImGuiFullscreen::ModAlpha(ImGuiFullscreen::UIStyle.BackgroundColor, alpha);
  const ImVec4 heading_background = ImGuiFullscreen::ModAlpha(ImGuiFullscreen::UIStyle.BackgroundColor, heading_alpha);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const u32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
  const float padding = LayoutScale(10.0f);
  const float spacing = LayoutScale(10.0f);
  const float spacing_small = spacing / 2.0f;
  float heading_height = LayoutScale(heading_height_unscaled);
  if (is_leaderboard_open)
  {
    // tabs
    heading_height += spacing_small + LayoutScale(tab_height_unscaled) + spacing;

    // Add space for a legend - spacing + 1 line of text + spacing + line
    heading_height += LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + spacing;
  }

  const float rank_column_width =
    UIStyle.Font
      ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, std::numeric_limits<float>::max(), -1.0f, "99999")
      .x;
  const float name_column_width = UIStyle.Font
                                    ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                    std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWWWWWWWWWWWWW")
                                    .x;
  const float time_column_width = UIStyle.Font
                                    ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                    std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWW")
                                    .x;
  const float column_spacing = spacing * 2.0f;

  if (ImGuiFullscreen::BeginFullscreenWindow(
        ImVec2(), ImVec2(display_size.x, heading_height), "leaderboards_heading", heading_background, 0.0f, ImVec2(),
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse))
  {
    bool visible, hovered;
    bool pressed = ImGuiFullscreen::MenuButtonFrame("leaderboards_heading", false, heading_height_unscaled, &visible,
                                                    &hovered, &bb.Min, &bb.Max, 0, alpha);
    UNREFERENCED_VARIABLE(pressed);

    if (visible)
    {
      const float image_height = LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      if (!s_state.game_icon.empty())
      {
        GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(s_state.game_icon);
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge, icon_min, icon_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                               IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;

      if (!is_leaderboard_open)
      {
        if (ImGuiFullscreen::FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true) ||
            ImGuiFullscreen::WantsToCloseMenu())
        {
          FullscreenUI::ReturnToPreviousWindow();
        }
      }
      else
      {
        if (ImGuiFullscreen::FloatingButton(ICON_FA_CARET_SQUARE_LEFT, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true) ||
            ImGuiFullscreen::WantsToCloseMenu())
        {
          close_leaderboard_on_exit = true;
        }
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFontSize));
      text.assign(Achievements::GetGameTitle());

      top += UIStyle.LargeFontSize + spacing_small;

      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min, title_bb.Max,
                                text_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &title_bb);

      u32 summary_color;
      if (is_leaderboard_open)
      {
        const ImRect subtitle_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFontSize));
        text.assign(s_state.open_leaderboard->title);

        top += UIStyle.LargeFontSize + spacing_small;

        RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, subtitle_bb.Min,
                                  subtitle_bb.Max,
                                  ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])), text,
                                  nullptr, ImVec2(0.0f, 0.0f), 0.0f, &subtitle_bb);

        text.assign(s_state.open_leaderboard->description);
        summary_color = ImGui::GetColorU32(DarkerColor(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])));
      }
      else
      {
        u32 count = 0;
        for (u32 i = 0; i < s_state.leaderboard_list->num_buckets; i++)
          count += s_state.leaderboard_list->buckets[i].num_leaderboards;
        text = TRANSLATE_PLURAL_SSTR("Achievements", "This game has %n leaderboards.", "Leaderboard count", count);
        summary_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));
      }

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
      top += UIStyle.MediumFontSize + spacing_small;

      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, summary_bb.Min,
                                summary_bb.Max, summary_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &summary_bb);

      if (!is_leaderboard_open && !Achievements::IsHardcoreModeActive())
      {
        const ImRect hardcore_warning_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
        top += UIStyle.MediumFontSize + spacing_small;

        text.format(
          ICON_EMOJI_WARNING " {}",
          TRANSLATE_SV("Achievements",
                       "Submitting scores is disabled because hardcore mode is off. Leaderboards are read-only."));

        RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, hardcore_warning_bb.Min,
                                  hardcore_warning_bb.Max,
                                  ImGui::GetColorU32(DarkerColor(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]))),
                                  text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &hardcore_warning_bb);
      }

      if (is_leaderboard_open)
      {
        const float tab_width = (ImGui::GetWindowWidth() / ImGuiFullscreen::UIStyle.LayoutScale) * 0.5f;
        ImGui::SetCursorPos(ImVec2(0.0f, top + spacing_small));

        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ||
            ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakSlow, false) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakFast, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
        {
          s_state.is_showing_all_leaderboard_entries = !s_state.is_showing_all_leaderboard_entries;
          ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::ViewChanged);
        }

        for (const bool show_all : {false, true})
        {
          const char* title =
            show_all ? TRANSLATE("Achievements", "Show Best") : TRANSLATE("Achievements", "Show Nearby");
          if (ImGuiFullscreen::NavTab(title, s_state.is_showing_all_leaderboard_entries == show_all, true, tab_width,
                                      tab_height_unscaled, heading_background))
          {
            s_state.is_showing_all_leaderboard_entries = show_all;
            ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::ViewChanged);
          }
        }

        const ImVec2 bg_pos =
          ImVec2(0.0f, ImGui::GetCurrentWindow()->DC.CursorPos.y + LayoutScale(tab_height_unscaled));
        const ImVec2 bg_size =
          ImVec2(ImGui::GetWindowWidth(),
                 spacing + LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + spacing);
        ImGui::GetWindowDrawList()->AddRectFilled(bg_pos, bg_pos + bg_size, ImGui::GetColorU32(heading_background));

        ImGui::SetCursorPos(ImVec2(0.0f, ImGui::GetCursorPosY() + LayoutScale(tab_height_unscaled) + spacing));

        pressed =
          ImGuiFullscreen::MenuButtonFrame("legend", false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                                           &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
        UNREFERENCED_VARIABLE(pressed);

        // add padding from the window below, don't want the menu items butted up against the edge
        bb.Min.x += LayoutScale(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING);
        bb.Max.x -= LayoutScale(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING);

        const u32 heading_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));

        const float midpoint = bb.Min.y + UIStyle.LargeFontSize + LayoutScale(4.0f);
        float text_start_x = bb.Min.x + LayoutScale(15.0f) + padding;

        const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, rank_bb.Min, rank_bb.Max,
                                  heading_color, TRANSLATE_SV("Achievements", "Rank"), nullptr, ImVec2(0.0f, 0.0f),
                                  0.0f, &rank_bb);
        text_start_x += rank_column_width + column_spacing;

        const ImRect user_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, user_bb.Min, user_bb.Max,
                                  heading_color, TRANSLATE_SV("Achievements", "Name"), nullptr, ImVec2(0.0f, 0.0f),
                                  0.0f, &user_bb);
        text_start_x += name_column_width + column_spacing;

        static const char* value_headings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
          TRANSLATE_NOOP("Achievements", "Time"),
          TRANSLATE_NOOP("Achievements", "Score"),
          TRANSLATE_NOOP("Achievements", "Value"),
        };

        const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        RenderShadowedTextClipped(
          UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, score_bb.Min, score_bb.Max, heading_color,
          Host::TranslateToStringView(
            "Achievements",
            value_headings[std::min<u8>(s_state.open_leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)]),
          nullptr, ImVec2(0.0f, 0.0f), 0.0f, &score_bb);
        text_start_x += time_column_width + column_spacing;

        const ImRect date_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, date_bb.Min, date_bb.Max,
                                  heading_color, TRANSLATE_SV("Achievements", "Date Submitted"), nullptr,
                                  ImVec2(0.0f, 0.0f), 0.0f, &date_bb);

        const float line_thickness = LayoutScale(1.0f);
        const float line_padding = LayoutScale(5.0f);
        const ImVec2 line_start(bb.Min.x, bb.Min.y + UIStyle.LargeFontSize + line_padding);
        const ImVec2 line_end(bb.Max.x, line_start.y);
        ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            line_thickness);
      }
    }
  }
  ImGuiFullscreen::EndFullscreenWindow();

  // See note in FullscreenUI::DrawSettingsWindow().
  if (ImGuiFullscreen::IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (!is_leaderboard_open)
  {
    if (ImGuiFullscreen::BeginFullscreenWindow(
          ImVec2(0.0f, heading_height),
          ImVec2(display_size.x, display_size.y - heading_height - LayoutScale(ImGuiFullscreen::LAYOUT_FOOTER_HEIGHT)),
          "leaderboards", background, 0.0f,
          ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, ImGuiFullscreen::LAYOUT_MENU_WINDOW_Y_PADDING), 0))
    {
      ImGuiFullscreen::ResetFocusHere();
      ImGuiFullscreen::BeginMenuButtons();

      for (u32 bucket_index = 0; bucket_index < s_state.leaderboard_list->num_buckets; bucket_index++)
      {
        const rc_client_leaderboard_bucket_t& bucket = s_state.leaderboard_list->buckets[bucket_index];
        for (u32 i = 0; i < bucket.num_leaderboards; i++)
          DrawLeaderboardListEntry(bucket.leaderboards[i]);
      }

      ImGuiFullscreen::EndMenuButtons();
    }
    ImGuiFullscreen::EndFullscreenWindow();

    ImGuiFullscreen::SetFullscreenFooterText(
      std::array{std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_XBOX_DPAD_UP_DOWN :
                                                                          ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN,
                                TRANSLATE_SV("Achievements", "Change Selection")),
                 std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_BUTTON_A : ICON_PF_ENTER,
                                TRANSLATE_SV("Achievements", "Open Leaderboard")),
                 std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_BUTTON_B : ICON_PF_ESC,
                                TRANSLATE_SV("Achievements", "Back"))},
      FullscreenUI::GetBackgroundAlpha());
  }
  else
  {
    if (ImGuiFullscreen::BeginFullscreenWindow(
          ImVec2(0.0f, heading_height),
          ImVec2(display_size.x, display_size.y - heading_height - LayoutScale(ImGuiFullscreen::LAYOUT_FOOTER_HEIGHT)),
          "leaderboard", background, 0.0f,
          ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, ImGuiFullscreen::LAYOUT_MENU_WINDOW_Y_PADDING), 0))
    {
      ImGuiFullscreen::BeginMenuButtons();
      ImGuiFullscreen::ResetFocusHere();

      if (!s_state.is_showing_all_leaderboard_entries)
      {
        if (s_state.leaderboard_nearby_entries)
        {
          for (u32 i = 0; i < s_state.leaderboard_nearby_entries->num_entries; i++)
          {
            DrawLeaderboardEntry(s_state.leaderboard_nearby_entries->entries[i], i,
                                 static_cast<s32>(i) == s_state.leaderboard_nearby_entries->user_index,
                                 rank_column_width, name_column_width, time_column_width, column_spacing);
          }
        }
        else
        {
          const ImVec2 pos_min(0.0f, heading_height);
          const ImVec2 pos_max(display_size.x, display_size.y);
          RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, pos_min, pos_max,
                                    text_color,
                                    TRANSLATE_SV("Achievements", "Downloading leaderboard data, please wait..."),
                                    nullptr, ImVec2(0.5f, 0.5f), 0.0f);
        }
      }
      else
      {
        for (const rc_client_leaderboard_entry_list_t* list : s_state.leaderboard_entry_lists)
        {
          for (u32 i = 0; i < list->num_entries; i++)
          {
            DrawLeaderboardEntry(list->entries[i], i, static_cast<s32>(i) == list->user_index, rank_column_width,
                                 name_column_width, time_column_width, column_spacing);
          }
        }

        // Fetch next chunk if the loading indicator becomes visible (i.e. we scrolled enough).
        bool visible, hovered;
        text.format(ICON_FA_HOURGLASS_HALF " {}", TRANSLATE_SV("Achievements", "Loading..."));
        ImGuiFullscreen::MenuButtonFrame(text, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible,
                                         &hovered, &bb.Min, &bb.Max);
        if (visible)
        {
          const float midpoint = bb.Min.y + UIStyle.LargeFontSize + LayoutScale(4.0f);
          const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));

          RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min,
                                    title_bb.Max, text_color, text, nullptr, ImVec2(0, 0), 0.0f, &title_bb);

          if (!s_state.leaderboard_fetch_handle)
            FetchNextLeaderboardEntries();
        }
      }

      ImGuiFullscreen::EndMenuButtons();
    }
    ImGuiFullscreen::EndFullscreenWindow();

    ImGuiFullscreen::SetFullscreenFooterText(
      std::array{std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_XBOX_DPAD_LEFT_RIGHT :
                                                                          ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                                TRANSLATE_SV("Achievements", "Change Page")),
                 std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_XBOX_DPAD_UP_DOWN :
                                                                          ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN,
                                TRANSLATE_SV("Achievements", "Change Selection")),
                 std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_BUTTON_A : ICON_PF_ENTER,
                                TRANSLATE_SV("Achievements", "View Profile")),
                 std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_BUTTON_B : ICON_PF_ESC,
                                TRANSLATE_SV("Achievements", "Back"))},
      FullscreenUI::GetBackgroundAlpha());
  }

  if (close_leaderboard_on_exit)
    FullscreenUI::BeginTransition(&CloseLeaderboard);
}

void Achievements::DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, u32 index, bool is_self,
                                        float rank_column_width, float name_column_width, float time_column_width,
                                        float column_spacing)
{
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::RenderShadowedTextClipped;
  using ImGuiFullscreen::UIStyle;

  static constexpr float alpha = 0.8f;

  ImRect bb;
  bool visible, hovered;
  bool pressed =
    ImGuiFullscreen::MenuButtonFrame(entry.user, true, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible,
                                     &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + UIStyle.LargeFontSize + LayoutScale(4.0f);
  float text_start_x = bb.Min.x + LayoutScale(15.0f);
  SmallString text;

  text.format("{}", entry.rank);

  const u32 text_color =
    is_self ?
      IM_COL32(255, 242, 0, 255) :
      ImGui::GetColorU32(((index % 2) == 0) ? ImGuiFullscreen::DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]) :
                                              ImGui::GetStyle().Colors[ImGuiCol_Text]);

  const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, rank_bb.Min, rank_bb.Max,
                            text_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &rank_bb);
  text_start_x += rank_column_width + column_spacing;

  const float icon_size = bb.Max.y - bb.Min.y;
  const ImRect icon_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  GPUTexture* icon_tex = nullptr;
  if (auto it = std::find_if(s_state.leaderboard_user_icon_paths.begin(), s_state.leaderboard_user_icon_paths.end(),
                             [&entry](const auto& it) { return it.first == &entry; });
      it != s_state.leaderboard_user_icon_paths.end())
  {
    if (!it->second.empty())
      icon_tex = ImGuiFullscreen::GetCachedTextureAsync(it->second);
  }
  else
  {
    std::string path = Achievements::GetLeaderboardUserBadgePath(&entry);
    if (!path.empty())
    {
      icon_tex = ImGuiFullscreen::GetCachedTextureAsync(path);
      s_state.leaderboard_user_icon_paths.emplace_back(&entry, std::move(path));
    }
  }
  if (icon_tex)
  {
    const ImRect fit_icon_bb =
      ImGuiFullscreen::CenterImage(ImRect(icon_bb.Min, icon_bb.Min + ImVec2(icon_size, icon_size)), icon_tex);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(icon_tex), fit_icon_bb.Min, fit_icon_bb.Max);
  }

  const ImRect user_bb(ImVec2(text_start_x + column_spacing + icon_size, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, user_bb.Min, user_bb.Max,
                            text_color, entry.user, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &user_bb);
  text_start_x += name_column_width + column_spacing;

  const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, score_bb.Min, score_bb.Max,
                            text_color, entry.display, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &score_bb);
  text_start_x += time_column_width + column_spacing;

  const ImRect time_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  SmallString submit_time;
  FullscreenUI::TimeToPrintableString(&submit_time, entry.submitted);
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, time_bb.Min, time_bb.Max,
                            text_color, submit_time, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &time_bb);

  if (pressed)
  {
    const SmallString url = SmallString::from_format(fmt::runtime(PROFILE_DETAILS_URL_TEMPLATE), entry.user);
    INFO_LOG("Opening profile details: {}", url);
    Host::OpenURL(url);
  }
}
void Achievements::DrawLeaderboardListEntry(const rc_client_leaderboard_t* lboard)
{
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::RenderShadowedTextClipped;
  using ImGuiFullscreen::UIStyle;

  static constexpr float alpha = 0.8f;

  TinyString id_str;
  id_str.format("{}", lboard->id);

  ImRect bb;
  bool visible, hovered;
  bool pressed = ImGuiFullscreen::MenuButtonFrame(id_str, true, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, &visible,
                                                  &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + UIStyle.LargeFontSize + LayoutScale(4.0f);
  const float text_start_x = bb.Min.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min, title_bb.Max,
                            ImGui::GetColorU32(ImGuiCol_Text), lboard->title, nullptr, ImVec2(0.0f, 0.0f), 0.0f,
                            &title_bb);

  if (lboard->description && lboard->description[0] != '\0')
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, summary_bb.Min,
                              summary_bb.Max,
                              ImGui::GetColorU32(ImGuiFullscreen::DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])),
                              lboard->description, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &summary_bb);
  }

  if (pressed)
    FullscreenUI::BeginTransition([id = lboard->id]() { OpenLeaderboardById(id); });
}

#endif // __ANDROID__

void Achievements::OpenLeaderboard(const rc_client_leaderboard_t* lboard)
{
  DEV_LOG("Opening leaderboard '{}' ({})", lboard->title, lboard->id);

  CloseLeaderboard();

  s_state.open_leaderboard = lboard;
  s_state.is_showing_all_leaderboard_entries = false;
  s_state.leaderboard_fetch_handle = rc_client_begin_fetch_leaderboard_entries_around_user(
    s_state.client, lboard->id, LEADERBOARD_NEARBY_ENTRIES_TO_FETCH, LeaderboardFetchNearbyCallback, nullptr);
  ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::Other);
}

bool Achievements::OpenLeaderboardById(u32 leaderboard_id)
{
  const rc_client_leaderboard_t* lb = rc_client_get_leaderboard_info(s_state.client, leaderboard_id);
  if (!lb)
    return false;

  OpenLeaderboard(lb);
  return true;
}

u32 Achievements::GetOpenLeaderboardId()
{
  return s_state.open_leaderboard ? s_state.open_leaderboard->id : 0;
}

bool Achievements::IsShowingAllLeaderboardEntries()
{
  return s_state.is_showing_all_leaderboard_entries;
}

const std::vector<rc_client_leaderboard_entry_list_t*>& Achievements::GetLeaderboardEntryLists()
{
  return s_state.leaderboard_entry_lists;
}

const rc_client_leaderboard_entry_list_t* Achievements::GetLeaderboardNearbyEntries()
{
  return s_state.leaderboard_nearby_entries;
}

void Achievements::LeaderboardFetchNearbyCallback(int result, const char* error_message,
                                                  rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                                  void* callback_userdata)
{
  const auto lock = GetLock();

  s_state.leaderboard_fetch_handle = nullptr;

  if (result != RC_OK)
  {
    ImGuiFullscreen::ShowToast(TRANSLATE("Achievements", "Leaderboard download failed"), error_message);
    CloseLeaderboard();
    return;
  }

  if (s_state.leaderboard_nearby_entries)
    rc_client_destroy_leaderboard_entry_list(s_state.leaderboard_nearby_entries);
  s_state.leaderboard_nearby_entries = list;
  ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::Other);
}

void Achievements::LeaderboardFetchAllCallback(int result, const char* error_message,
                                               rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                               void* callback_userdata)
{
  const auto lock = GetLock();

  s_state.leaderboard_fetch_handle = nullptr;

  if (result != RC_OK)
  {
    ImGuiFullscreen::ShowToast(TRANSLATE("Achievements", "Leaderboard download failed"), error_message);
    CloseLeaderboard();
    return;
  }

  if (s_state.leaderboard_entry_lists.empty())
    ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::Other);

  s_state.leaderboard_entry_lists.push_back(list);
}

void Achievements::FetchNextLeaderboardEntries()
{
  u32 start = 1;
  for (rc_client_leaderboard_entry_list_t* list : s_state.leaderboard_entry_lists)
    start += list->num_entries;

  DEV_LOG("Fetching entries {} to {}", start, start + LEADERBOARD_ALL_FETCH_SIZE);

  if (s_state.leaderboard_fetch_handle)
    rc_client_abort_async(s_state.client, s_state.leaderboard_fetch_handle);
  s_state.leaderboard_fetch_handle =
    rc_client_begin_fetch_leaderboard_entries(s_state.client, s_state.open_leaderboard->id, start,
                                              LEADERBOARD_ALL_FETCH_SIZE, LeaderboardFetchAllCallback, nullptr);
}

void Achievements::CloseLeaderboard()
{
  s_state.leaderboard_user_icon_paths.clear();

  for (auto iter = s_state.leaderboard_entry_lists.rbegin(); iter != s_state.leaderboard_entry_lists.rend(); ++iter)
    rc_client_destroy_leaderboard_entry_list(*iter);
  s_state.leaderboard_entry_lists.clear();

  if (s_state.leaderboard_nearby_entries)
  {
    rc_client_destroy_leaderboard_entry_list(s_state.leaderboard_nearby_entries);
    s_state.leaderboard_nearby_entries = nullptr;
  }

  if (s_state.leaderboard_fetch_handle)
  {
    rc_client_abort_async(s_state.client, s_state.leaderboard_fetch_handle);
    s_state.leaderboard_fetch_handle = nullptr;
  }

  s_state.open_leaderboard = nullptr;
  ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::ViewChanged);
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
    return 0;
  }

  ret.resize(machine_guid_length);
  if ((error = RegGetValueA(hKey, NULL, "MachineGuid", RRF_RT_REG_SZ, NULL, ret.data(), &machine_guid_length)) !=
        ERROR_SUCCESS ||
      machine_guid_length <= 1)
  {
    WARNING_LOG("Read MachineGuid failed: {}", error);
    ret = {};
    RegCloseKey(hKey);
    return 0;
  }

  ret.resize(machine_guid_length);
  RegCloseKey(hKey);
#elif !defined(__ANDROID__)
#ifdef __linux__
  // use /etc/machine-id on Linux
  std::optional<std::string> machine_id = FileSystem::ReadFileToString("/etc/machine-id");
  if (machine_id.has_value())
    ret = std::string_view(machine_id.value());
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

std::string Achievements::GetHashDatabasePath()
{
  return Path::Combine(EmuFolders::Cache, "achievement_gamedb.cache");
}

std::string Achievements::GetProgressDatabasePath()
{
  return Path::Combine(EmuFolders::Cache, "achievement_progress.cache");
}

void Achievements::BeginRefreshHashDatabase()
{
  INFO_LOG("Starting hash database refresh...");

  // kick off both requests
  CancelHashDatabaseRequests();
  s_state.fetch_hash_library_request =
    rc_client_begin_fetch_hash_library(s_state.client, RC_CONSOLE_PLAYSTATION, FetchHashLibraryCallback, nullptr);
  s_state.fetch_all_progress_request =
    rc_client_begin_fetch_all_user_progress(s_state.client, RC_CONSOLE_PLAYSTATION, FetchAllProgressCallback, nullptr);
  if (!s_state.fetch_hash_library_request || !s_state.fetch_hash_library_request)
  {
    ERROR_LOG("Failed to create hash database refresh requests.");
    CancelHashDatabaseRequests();
  }
}

void Achievements::FetchHashLibraryCallback(int result, const char* error_message, rc_client_hash_library_t* list,
                                            rc_client_t* client, void* callback_userdata)
{
  s_state.fetch_hash_library_request = nullptr;

  if (result != RC_OK)
  {
    ERROR_LOG("Fetch hash library failed: {}: {}", rc_error_str(result), error_message);
    CancelHashDatabaseRequests();
    return;
  }

  s_state.fetch_hash_library_result = list;
  FinishRefreshHashDatabase();
}

void Achievements::FetchAllProgressCallback(int result, const char* error_message, rc_client_all_user_progress_t* list,
                                            rc_client_t* client, void* callback_userdata)
{
  s_state.fetch_all_progress_request = nullptr;

  if (result != RC_OK)
  {
    ERROR_LOG("Fetch all progress failed: {}: {}", rc_error_str(result), error_message);
    CancelHashDatabaseRequests();
    return;
  }

  s_state.fetch_all_progress_result = list;
  FinishRefreshHashDatabase();
}

void Achievements::CancelHashDatabaseRequests()
{
  if (s_state.fetch_all_progress_result)
  {
    rc_client_destroy_all_user_progress(s_state.fetch_all_progress_result);
    s_state.fetch_all_progress_result = nullptr;
  }
  if (s_state.fetch_all_progress_request)
  {
    rc_client_abort_async(s_state.client, s_state.fetch_all_progress_request);
    s_state.fetch_all_progress_request = nullptr;
  }

  if (s_state.fetch_hash_library_result)
  {
    rc_client_destroy_hash_library(s_state.fetch_hash_library_result);
    s_state.fetch_hash_library_result = nullptr;
  }
  if (s_state.fetch_hash_library_request)
  {
    rc_client_abort_async(s_state.client, s_state.fetch_hash_library_request);
    s_state.fetch_hash_library_request = nullptr;
  }
}

void Achievements::FinishRefreshHashDatabase()
{
  if (!s_state.fetch_hash_library_result || !s_state.fetch_all_progress_result)
  {
    // not done yet
    return;
  }

  // build mapping of hashes to game ids and achievement counts
  BuildHashDatabase(s_state.fetch_hash_library_result, s_state.fetch_all_progress_result);

  // update the progress tracking while we're at it
  BuildProgressDatabase(s_state.fetch_all_progress_result);

  // tidy up
  rc_client_destroy_all_user_progress(s_state.fetch_all_progress_result);
  s_state.fetch_all_progress_result = nullptr;
  rc_client_destroy_hash_library(s_state.fetch_hash_library_result);
  s_state.fetch_hash_library_result = nullptr;

  // update game list, we might have some new games that weren't in the seed database
  GameList::UpdateAllAchievementData();

  Host::OnAchievementsAllProgressRefreshed();
}

bool Achievements::RefreshAllProgressDatabase(Error* error)
{
  if (!IsLoggedIn())
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "User is not logged in."));
    return false;
  }

  if (s_state.fetch_hash_library_request || s_state.fetch_all_progress_request || s_state.refresh_all_progress_request)
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "Progress is already being updated."));
    return false;
  }

  // refresh in progress
  s_state.refresh_all_progress_request = rc_client_begin_fetch_all_user_progress(s_state.client, RC_CONSOLE_PLAYSTATION,
                                                                                 RefreshAllProgressCallback, nullptr);

  return true;
}

void Achievements::RefreshAllProgressCallback(int result, const char* error_message,
                                              rc_client_all_user_progress_t* list, rc_client_t* client,
                                              void* callback_userdata)
{
  s_state.refresh_all_progress_request = nullptr;

  if (result != RC_OK)
  {
    Host::ReportErrorAsync(TRANSLATE_SV("Achievements", "Error"),
                           fmt::format("{}: {}\n{}", TRANSLATE_SV("Achievements", "Refresh all progress failed"),
                                       rc_error_str(result), error_message));
    return;
  }

  BuildProgressDatabase(list);
  rc_client_destroy_all_user_progress(list);

  GameList::UpdateAllAchievementData();

  Host::OnAchievementsAllProgressRefreshed();

  if (FullscreenUI::IsInitialized())
  {
    GPUThread::RunOnThread([]() {
      if (!FullscreenUI::IsInitialized())
        return;

      ImGuiFullscreen::ShowToast({}, TRANSLATE_STR("Achievements", "Updated achievement progress database."),
                                 Host::OSD_INFO_DURATION);
    });
  }
}

void Achievements::BuildHashDatabase(const rc_client_hash_library_t* hashlib,
                                     const rc_client_all_user_progress_t* allprog)
{
  std::vector<HashDatabaseEntry> dbentries;
  dbentries.reserve(hashlib->num_entries);

  for (const rc_client_hash_library_entry_t& entry :
       std::span<const rc_client_hash_library_entry_t>(hashlib->entries, hashlib->num_entries))
  {
    HashDatabaseEntry dbentry;
    dbentry.game_id = entry.game_id;
    dbentry.num_achievements = 0;
    if (StringUtil::DecodeHex(dbentry.hash, entry.hash) != GAME_HASH_LENGTH)
    {
      WARNING_LOG("Invalid hash '{}' in game ID {}", entry.hash, entry.game_id);
      continue;
    }

    // Just in case...
    if (std::any_of(dbentries.begin(), dbentries.end(),
                    [&dbentry](const HashDatabaseEntry& e) { return (e.hash == dbentry.hash); }))
    {
      WARNING_LOG("Duplicate hash {}", entry.hash);
      continue;
    }

    dbentries.push_back(dbentry);
  }

  // fill in achievement counts
  for (const rc_client_all_user_progress_entry_t& entry :
       std::span<const rc_client_all_user_progress_entry_t>(allprog->entries, allprog->num_entries))
  {
    // can have multiple hashes with the same game id, update count on all of them
    bool found_one = false;
    for (HashDatabaseEntry& dbentry : dbentries)
    {
      if (dbentry.game_id == entry.game_id)
      {
        dbentry.num_achievements = entry.num_achievements;
        found_one = true;
      }
    }

    if (!found_one)
      WARNING_LOG("All progress contained game ID {} without hash", entry.game_id);
  }

  s_state.hashdb_entries = std::move(dbentries);
  s_state.hashdb_loaded = true;

  Error error;
  if (!SortAndSaveHashDatabase(&error))
    ERROR_LOG("Failed to sort/save hash database from server: {}", error.GetDescription());
}

bool Achievements::CreateHashDatabaseFromSeedDatabase(const std::string& path, Error* error)
{
  std::optional<std::string> yaml_data = Host::ReadResourceFileToString("achievement_hashlib.yaml", false, error);
  if (!yaml_data.has_value())
  {
    Error::SetStringView(error, "Seed database is missing.");
    return false;
  }

  const ryml::Tree yaml =
    ryml::parse_in_place(to_csubstr(path), c4::substr(reinterpret_cast<char*>(yaml_data->data()), yaml_data->size()));
  const ryml::ConstNodeRef root = yaml.rootref();
  if (root.empty())
  {
    Error::SetStringView(error, "Seed database is empty.");
    return false;
  }

  std::vector<HashDatabaseEntry> dbentries;

  if (const ryml::ConstNodeRef hashes = root.find_child(to_csubstr("hashes")); hashes.valid())
  {
    dbentries.reserve(hashes.num_children());
    for (const ryml::ConstNodeRef& current : hashes.cchildren())
    {
      const std::string_view hash = to_stringview(current.key());
      const std::optional<u32> game_id = StringUtil::FromChars<u32>(to_stringview(current.val()));
      if (!game_id.has_value())
      {
        WARNING_LOG("Invalid game ID {} in hash {}", to_stringview(current.val()), hash);
        continue;
      }

      HashDatabaseEntry dbentry;
      dbentry.game_id = game_id.value();
      dbentry.num_achievements = 0;
      if (StringUtil::DecodeHex(dbentry.hash, hash) != GAME_HASH_LENGTH)
      {
        WARNING_LOG("Invalid hash '{}' in game ID {}", hash, game_id.value());
        continue;
      }

      dbentries.push_back(dbentry);
    }
  }

  if (const ryml::ConstNodeRef achievements = root.find_child(to_csubstr("achievements")); achievements.valid())
  {
    for (const ryml::ConstNodeRef& current : achievements.cchildren())
    {
      const std::optional<u32> game_id = StringUtil::FromChars<u32>(to_stringview(current.key()));
      const std::optional<u32> num_achievements = StringUtil::FromChars<u32>(to_stringview(current.val()));
      if (!game_id.has_value() || !num_achievements.has_value())
      {
        WARNING_LOG("Invalid achievements entry in game ID {}", to_stringview(current.key()));
        continue;
      }

      // can have multiple hashes with the same game id, update count on all of them
      bool found_one = false;
      for (HashDatabaseEntry& dbentry : dbentries)
      {
        if (dbentry.game_id == game_id.value())
        {
          dbentry.num_achievements = num_achievements.value();
          found_one = true;
        }
      }

      if (!found_one)
        WARNING_LOG("Seed database contained game ID {} without hash", game_id.value());
    }
  }

  if (dbentries.empty())
  {
    Error::SetStringView(error, "Parsed seed database was empty");
    return false;
  }

  s_state.hashdb_entries = std::move(dbentries);
  s_state.hashdb_loaded = true;

  Error save_error;
  if (!SortAndSaveHashDatabase(&save_error))
    ERROR_LOG("Failed to sort/save hash database from server: {}", save_error.GetDescription());

  return true;
}

bool Achievements::SortAndSaveHashDatabase(Error* error)
{
  // sort hashes for quick lookup
  s_state.hashdb_entries.shrink_to_fit();
  std::sort(s_state.hashdb_entries.begin(), s_state.hashdb_entries.end(),
            [](const HashDatabaseEntry& lhs, const HashDatabaseEntry& rhs) {
              return std::memcmp(lhs.hash.data(), rhs.hash.data(), GAME_HASH_LENGTH) < 0;
            });

  FileSystem::AtomicRenamedFile fp = FileSystem::CreateAtomicRenamedFile(GetHashDatabasePath().c_str(), error);
  if (!fp)
  {
    Error::AddPrefix(error, "Failed to open cache for writing: ");
    return false;
  }

  BinaryFileWriter writer(fp.get());
  writer.WriteU32(static_cast<u32>(s_state.hashdb_entries.size()));
  for (const HashDatabaseEntry& entry : s_state.hashdb_entries)
  {
    writer.Write(entry.hash.data(), GAME_HASH_LENGTH);
    writer.WriteU32(entry.game_id);
    writer.WriteU32(entry.num_achievements);
  }

  if (!writer.Flush(error) || !FileSystem::CommitAtomicRenamedFile(fp, error))
  {
    Error::AddPrefix(error, "Failed to write cache: ");
    return false;
  }

  INFO_LOG("Wrote {} games to hash database", s_state.hashdb_entries.size());
  return true;
}

bool Achievements::LoadHashDatabase(const std::string& path, Error* error)
{
  FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(path.c_str(), "rb", error);
  if (!fp)
  {
    Error::AddPrefix(error, "Failed to open cache for reading: ");
    return false;
  }

  BinaryFileReader reader(fp.get());
  const u32 count = reader.ReadU32();

  // simple sanity check on file size
  constexpr size_t entry_size = (GAME_HASH_LENGTH + sizeof(u32) + sizeof(u32));
  if (static_cast<s64>((count * entry_size) + sizeof(u32)) > FileSystem::FSize64(fp.get()))
  {
    Error::SetStringFmt(error, "Invalid entry count: {}", count);
    return false;
  }

  s_state.hashdb_entries.resize(count);
  for (HashDatabaseEntry& entry : s_state.hashdb_entries)
  {
    reader.Read(entry.hash.data(), entry.hash.size());
    reader.ReadU32(&entry.game_id);
    reader.ReadU32(&entry.num_achievements);
  }
  if (reader.HasError())
  {
    Error::SetStringView(error, "Error while reading cache");
    s_state.hashdb_entries = {};
    return false;
  }

  VERBOSE_LOG("Loaded {} entries from cached hash database", s_state.hashdb_entries.size());
  return true;
}

const Achievements::HashDatabaseEntry* Achievements::LookupGameHash(const GameHash& hash)
{
  if (!s_state.hashdb_loaded) [[unlikely]]
  {
    // loaded by another thread?
    std::unique_lock lock(s_state.mutex);
    if (!s_state.hashdb_loaded)
    {
      Error error;
      std::string path = GetHashDatabasePath();
      const bool hashdb_exists = FileSystem::FileExists(path.c_str());
      if (!hashdb_exists || !LoadHashDatabase(path, &error))
      {
        if (hashdb_exists)
          WARNING_LOG("Failed to load hash database: {}", error.GetDescription());

        if (!CreateHashDatabaseFromSeedDatabase(path, &error))
          ERROR_LOG("Failed to create hash database from seed database: {}", error.GetDescription());
      }
    }

    s_state.hashdb_loaded = true;
  }

  const auto iter = std::lower_bound(s_state.hashdb_entries.begin(), s_state.hashdb_entries.end(), hash,
                                     [](const HashDatabaseEntry& entry, const GameHash& search) {
                                       return (std::memcmp(entry.hash.data(), search.data(), GAME_HASH_LENGTH) < 0);
                                     });
  return (iter != s_state.hashdb_entries.end() && std::memcmp(iter->hash.data(), hash.data(), GAME_HASH_LENGTH) == 0) ?
           &(*iter) :
           nullptr;
}

void Achievements::PreloadHashDatabase()
{
  const std::string hash_database_path = GetHashDatabasePath();
  const std::string progress_database_path = GetProgressDatabasePath();

  bool has_hash_database = (s_state.hashdb_loaded && !s_state.hashdb_entries.empty());
  const bool has_progress_database = FileSystem::FileExists(progress_database_path.c_str());

  // if we don't have a progress database, just redownload everything, it's probably our first login
  if (!has_hash_database && has_progress_database && FileSystem::FileExists(hash_database_path.c_str()))
  {
    // try loading binary cache
    VERBOSE_LOG("Trying to load hash database from {}", hash_database_path);

    Error error;
    has_hash_database = LoadHashDatabase(hash_database_path, &error);
    if (!has_hash_database)
      ERROR_LOG("Failed to load hash database: {}", error.GetDescription());
  }

  // don't try to load the hash database from the game list now
  s_state.hashdb_loaded = true;

  // got everything?
  if (has_hash_database && has_progress_database)
    return;

  // kick off a new download, game list will be notified when it's done
  BeginRefreshHashDatabase();
}

FileSystem::ManagedCFilePtr Achievements::OpenProgressDatabase(bool for_write, bool truncate, Error* error)
{
  const std::string path = GetProgressDatabasePath();
  const FileSystem::FileShareMode share_mode =
    for_write ? FileSystem::FileShareMode::DenyReadWrite : FileSystem::FileShareMode::DenyWrite;
#ifdef _WIN32
  const char* mode = for_write ? (truncate ? "w+b" : "r+b") : "rb";
#else
  // Always open read/write on Linux, since we need it for flock().
  const char* mode = truncate ? "w+b" : "r+b";
#endif

  FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedSharedCFile(path.c_str(), mode, share_mode, error);
  if (fp)
    return fp;

  // Doesn't exist? Create it.
  if (errno == ENOENT)
  {
    if (!for_write)
      return nullptr;

    mode = "w+b";
    fp = FileSystem::OpenManagedSharedCFile(path.c_str(), mode, share_mode, error);
    if (fp)
      return fp;
  }

  // If there's a sharing violation, try again for 100ms.
  if (errno != EACCES)
    return nullptr;

  Timer timer;
  while (timer.GetTimeMilliseconds() <= 100.0f)
  {
    fp = FileSystem::OpenManagedSharedCFile(path.c_str(), mode, share_mode, error);
    if (fp)
      return fp;

    if (errno != EACCES)
      return nullptr;
  }

  Error::SetStringView(error, "Timed out while trying to open progress database.");
  return nullptr;
}

void Achievements::BuildProgressDatabase(const rc_client_all_user_progress_t* allprog)
{
  // no point storing it in memory, just write directly to the file
  Error error;
  FileSystem::ManagedCFilePtr fp = OpenProgressDatabase(true, true, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to build progress database: {}", error.GetDescription());
    return;
  }

#ifdef HAS_POSIX_FILE_LOCK
  FileSystem::POSIXLock lock(fp.get());
#endif

  // save a rewrite at the beginning
  u32 games_with_unlocks = 0;
  for (u32 i = 0; i < allprog->num_entries; i++)
  {
    games_with_unlocks += BoolToUInt32(
      (allprog->entries[i].num_unlocked_achievements + allprog->entries[i].num_unlocked_achievements_hardcore) > 0);
  }

  BinaryFileWriter writer(fp.get());
  writer.WriteU32(games_with_unlocks);
  if (games_with_unlocks > 0)
  {
    for (const rc_client_all_user_progress_entry_t& entry :
         std::span<const rc_client_all_user_progress_entry_t>(allprog->entries, allprog->num_entries))
    {
      if ((entry.num_unlocked_achievements + entry.num_unlocked_achievements_hardcore) == 0)
        continue;

      writer.WriteU32(entry.game_id);
      writer.WriteU16(Truncate16(entry.num_unlocked_achievements));
      writer.WriteU16(Truncate16(entry.num_unlocked_achievements_hardcore));
    }
  }

  if (!writer.Flush(&error))
    ERROR_LOG("Failed to write progress database: {}", error.GetDescription());
}

void Achievements::UpdateProgressDatabase()
{
  // don't write updates in spectator mode
  if (rc_client_get_spectator_mode_enabled(s_state.client))
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
    GameList::UpdateAchievementData(s_state.game_hash.value(), s_state.game_id, num_achievements, achievements_unlocked,
                                    achievements_unlocked_hardcore);
  }

  // done asynchronously so we don't hitch on disk I/O
  System::QueueAsyncTask([game_id = s_state.game_id, achievements_unlocked, achievements_unlocked_hardcore]() {
    // no point storing it in memory, just write directly to the file
    Error error;
    FileSystem::ManagedCFilePtr fp = OpenProgressDatabase(true, false, &error);
    const s64 size = fp ? FileSystem::FSize64(fp.get(), &error) : -1;
    if (!fp || size < 0)
    {
      ERROR_LOG("Failed to update progress database: {}", error.GetDescription());
      return;
    }

#ifdef HAS_POSIX_FILE_LOCK
    FileSystem::POSIXLock lock(fp.get());
#endif

    BinaryFileReader reader(fp.get());
    const u32 game_count = (size > 0) ? reader.ReadU32() : 0;

    // entry exists?
    s64 found_offset = -1;
    for (u32 i = 0; i < game_count; i++)
    {
      const u32 check_game_id = reader.ReadU32();
      if (check_game_id == game_id)
      {
        // do we even need to change it?
        const u16 current_achievements_unlocked = reader.ReadU16();
        const u16 current_achievements_unlocked_hardcore = reader.ReadU16();
        if (current_achievements_unlocked == achievements_unlocked &&
            current_achievements_unlocked_hardcore == achievements_unlocked_hardcore)
        {
          VERBOSE_LOG("No update to progress database needed for game {}", game_id);
          return;
        }

        found_offset = FileSystem::FTell64(fp.get()) - sizeof(u16) - sizeof(u16);
        break;
      }

      if (!FileSystem::FSeek64(fp.get(), sizeof(u16) + sizeof(u16), SEEK_CUR, &error)) [[unlikely]]
      {
        ERROR_LOG("Failed to seek in progress database: {}", error.GetDescription());
        return;
      }
    }

    // make sure we had no read errors, don't want to make corrupted files
    if (reader.HasError())
    {
      ERROR_LOG("Failed to read in progress database: {}", error.GetDescription());
      return;
    }

    BinaryFileWriter writer(fp.get());

    // append/update the entry
    if (found_offset > 0)
    {
      INFO_LOG("Updating game {} with {}/{} unlocked", game_id, achievements_unlocked, achievements_unlocked_hardcore);

      // need to seek when switching read->write
      if (!FileSystem::FSeek64(fp.get(), found_offset, SEEK_SET, &error))
      {
        ERROR_LOG("Failed to write seek in progress database: {}", error.GetDescription());
        return;
      }

      writer.WriteU16(Truncate16(achievements_unlocked));
      writer.WriteU16(Truncate16(achievements_unlocked_hardcore));
    }
    else
    {
      // don't write zeros to the file. we could still end up with zeros here after reset, but that's rare
      if (achievements_unlocked == 0 && achievements_unlocked_hardcore == 0)
        return;

      INFO_LOG("Appending game {} with {}/{} unlocked", game_id, achievements_unlocked, achievements_unlocked_hardcore);

      if (size == 0)
      {
        // if the file is empty, need to write the header
        writer.WriteU32(1);
      }
      else
      {
        // update the count
        if (!FileSystem::FSeek64(fp.get(), 0, SEEK_SET, &error) || !writer.WriteU32(game_count + 1) ||
            !FileSystem::FSeek64(fp.get(), 0, SEEK_END, &error))
        {
          ERROR_LOG("Failed to write seek/update header in progress database: {}", error.GetDescription());
          return;
        }
      }

      writer.WriteU32(game_id);
      writer.WriteU16(Truncate16(achievements_unlocked));
      writer.WriteU16(Truncate16(achievements_unlocked_hardcore));
    }

    if (!writer.Flush(&error))
    {
      ERROR_LOG("Failed to write count in progress database: {}", error.GetDescription());
      return;
    }
  });
}

void Achievements::ClearProgressDatabase()
{
  std::string path = GetProgressDatabasePath();
  if (FileSystem::FileExists(path.c_str()))
  {
    INFO_LOG("Deleting progress database {}", path);

    Error error;
    if (!FileSystem::DeleteFile(path.c_str(), &error))
      ERROR_LOG("Failed to delete progress database: {}", error.GetDescription());
  }

  GameList::UpdateAllAchievementData();
}

Achievements::ProgressDatabase::ProgressDatabase() = default;

Achievements::ProgressDatabase::~ProgressDatabase() = default;

bool Achievements::ProgressDatabase::Load(Error* error)
{
  FileSystem::ManagedCFilePtr fp = OpenProgressDatabase(false, false, error);
  if (!fp)
    return false;

#ifdef HAS_POSIX_FILE_LOCK
  FileSystem::POSIXLock lock(fp.get());
#endif

  BinaryFileReader reader(fp.get());
  const u32 count = reader.ReadU32();

  // simple sanity check on file size
  constexpr size_t entry_size = (sizeof(u32) + sizeof(u16) + sizeof(u16));
  if (static_cast<s64>((count * entry_size) + sizeof(u32)) > FileSystem::FSize64(fp.get()))
  {
    Error::SetStringFmt(error, "Invalid entry count: {}", count);
    return false;
  }

  m_entries.reserve(count);
  for (u32 i = 0; i < count; i++)
  {
    const Entry entry = {.game_id = reader.ReadU32(),
                         .num_achievements_unlocked = reader.ReadU16(),
                         .num_hc_achievements_unlocked = reader.ReadU16()};

    // Just in case...
    if (std::any_of(m_entries.begin(), m_entries.end(),
                    [id = entry.game_id](const Entry& e) { return (e.game_id == id); }))
    {
      WARNING_LOG("Duplicate game ID {}", entry.game_id);
      continue;
    }

    m_entries.push_back(entry);
  }

  // sort for quick lookup
  m_entries.shrink_to_fit();
  std::sort(m_entries.begin(), m_entries.end(),
            [](const Entry& lhs, const Entry& rhs) { return (lhs.game_id < rhs.game_id); });

  return true;
}

const Achievements::ProgressDatabase::Entry* Achievements::ProgressDatabase::LookupGame(u32 game_id) const
{
  const auto iter = std::lower_bound(m_entries.begin(), m_entries.end(), game_id,
                                     [](const Entry& entry, u32 search) { return (entry.game_id < search); });
  return (iter != m_entries.end() && iter->game_id == game_id) ? &(*iter) : nullptr;
}

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

#include "common/windows_headers.h"

#include "rc_client_raintegration.h"

namespace Achievements {

static void FinishLoadRAIntegration();
static void FinishLoadRAIntegrationOnCPUThread();

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
    s_state.client,
    (wi.has_value() && wi->type == WindowInfo::Type::Win32) ? static_cast<HWND>(wi->window_handle) : NULL,
    "DuckStation", g_scm_tag_str, &error_message);
  if (res != RC_OK)
  {
    std::string message = fmt::format("Failed to initialize RAIntegration:\n{}", error_message ? error_message : "");
    Host::ReportErrorAsync("RAIntegration Error", message);
    s_state.using_raintegration = false;
    Host::RunOnCPUThread(&Achievements::FinishLoadRAIntegrationOnCPUThread);
    return;
  }

  rc_client_raintegration_set_write_memory_function(s_state.client, RAIntegrationWriteMemoryCallback);
  rc_client_raintegration_set_console_id(s_state.client, RC_CONSOLE_PLAYSTATION);
  rc_client_raintegration_set_get_game_name_function(s_state.client, RAIntegrationGetGameNameCallback);
  rc_client_raintegration_set_event_handler(s_state.client, RAIntegrationEventHandler);

  Host::OnRAIntegrationMenuChanged();

  Host::RunOnCPUThread(&Achievements::FinishLoadRAIntegrationOnCPUThread);
}

void Achievements::FinishLoadRAIntegrationOnCPUThread()
{
  // note: this is executed even for the failure case.
  // we want to finish initializing with internal client if RAIntegration didn't load.
  const auto lock = GetLock();
  s_state.raintegration_loading = false;
  FinishInitialize();
}

void Achievements::UnloadRAIntegration()
{
  DebugAssert(s_state.using_raintegration && s_state.client);

  if (s_state.load_raintegration_request)
  {
    rc_client_abort_async(s_state.client, s_state.load_raintegration_request);
    s_state.load_raintegration_request = nullptr;
  }

  // Have to unload it on the UI thread, otherwise the DLL unload races the UI thread message processing.
  s_state.http_downloader->WaitForAllRequests();
  s_state.http_downloader.reset();
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
      Host::RunOnCPUThread([]() {
        const auto lock = GetLock();
        OnHardcoreModeChanged(rc_client_get_hardcore_enabled(s_state.client) != 0, false, false);
      });
    }
    break;

    case RC_CLIENT_RAINTEGRATION_EVENT_PAUSE:
    {
      Host::RunOnCPUThread([]() { System::PauseSystem(true); });
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
  Host::RunOnCPUThread([address, data = std::move(data)]() {
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

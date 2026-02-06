// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// TODO: Don't poll when booting the game, e.g. Crash Warped freaks out.

#include "achievements.h"
#include "achievements_private.h"
#include "bios.h"
#include "bus.h"
#include "cheats.h"
#include "core.h"
#include "cpu_core.h"
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
#include "common/md5_digest.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/ryml_helpers.h"
#include "common/scoped_guard.h"
#include "common/sha256_digest.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "util/cd_image.h"
#include "util/http_downloader.h"
#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "rc_api_info.h"
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
static constexpr const char* CACHE_SUBDIRECTORY_NAME = "achievement_images";
constexpr const char* const RA_LOGO_ICON_NAME = "images/ra-icon.webp";

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

struct FetchGameTitlesParameters
{
  Error* error;
  rc_client_async_handle_t* request;
  rc_client_game_title_list_t* list;
  bool success;
};

} // namespace

static void ReportError(std::string_view sv);
template<typename... T>
static void ReportFmtError(fmt::format_string<T...> fmt, T&&... args);
template<typename... T>
static void ReportRCError(int err, fmt::format_string<T...> fmt, T&&... args);
static void ClearGameInfo();
static void ClearGameHash();
static bool HasSavedCredentials();
static bool TryLoggingInWithToken();
static void EnableHardcoreMode(bool display_message, bool display_game_summary);
static void OnHardcoreModeChanged(bool enabled, bool display_message, bool display_game_summary);
static bool IsRAIntegrationInitializing();
static void FinishInitialize();
static void FinishLogin();
static bool IdentifyGame(CDImage* image);
static bool IdentifyCurrentGame();
static void BeginLoadGame();
static void UpdateGameSummary(bool update_progress_database);
static DynamicHeapArray<u8> SaveStateToBuffer();
static void LoadStateFromBuffer(std::span<const u8> data, std::unique_lock<std::recursive_mutex>& lock);
static bool SaveStateToBuffer(std::span<u8> data);
static std::string GetImageURL(const char* image_name, u32 type);
static std::string GetLocalImagePath(const std::string_view image_name, u32 type);
static void DownloadImage(std::string url, std::string cache_path);

static TinyString DecryptLoginToken(std::string_view encrypted_token, std::string_view username);
static TinyString EncryptLoginToken(std::string_view token, std::string_view username);

static bool CreateClient(rc_client_t** client, std::unique_ptr<HTTPDownloader>* http);
static void DestroyClient(rc_client_t** client, std::unique_ptr<HTTPDownloader>* http);
static void ClientMessageCallback(const char* message, const rc_client_t* client);
static uint32_t ClientReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client);
static void ClientServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data,
                             rc_client_t* client);
static rc_api_server_response_t MakeRCAPIServerResponse(s32 status_code, const std::vector<u8>& data);
static void WaitForHTTPRequestsWithYield(std::unique_lock<std::recursive_mutex>& lock);

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
static void FetchGameTitlesCallback(int result, const char* error_message, rc_client_game_title_list_t* list,
                                    rc_client_t* client, void* userdata);
static void ClientLoadGameCallback(int result, const char* error_message, rc_client_t* client, void* userdata);

static void DisplayHardcoreDeferredMessage();
static void DisplayAchievementSummary();
static void UpdateRichPresence(std::unique_lock<std::recursive_mutex>& lock);

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

struct State
{
  rc_client_t* client = nullptr;
  bool has_achievements = false;
  bool has_leaderboards = false;
  bool has_rich_presence = false;
  bool reload_game_on_reset = false;
  bool hashdb_loaded = false;

  std::recursive_mutex mutex; // large

  std::string user_badge_path;

  std::string rich_presence_string;
  Timer::Value rich_presence_poll_time = 0;

  std::vector<LeaderboardTrackerIndicator> active_leaderboard_trackers;
  std::vector<ActiveChallengeIndicator> active_challenge_indicators;
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

std::optional<Achievements::GameHash> Achievements::GetGameHash(CDImage* image)
{
  std::optional<GameHash> ret;

  std::string executable_name;
  std::vector<u8> executable_data;
  if (!System::ReadExecutableFromImage(image, &executable_name, &executable_data))
    return ret;

  return GetGameHash(executable_name, executable_data);
}

std::optional<Achievements::GameHash> Achievements::GetGameHash(const std::string_view executable_name,
                                                                std::span<const u8> executable_data)
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

  ret.emplace();
  digest.Final(ret.value());

  INFO_COLOR_LOG(StrongOrange, "RA Hash for '{}': {} ({} bytes hashed)", executable_name, GameHashToString(ret),
                 hash_size);

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

    VideoThread::RunOnThread(
      [cache_path = std::move(cache_path)]() { FullscreenUI::InvalidateCachedTexture(cache_path); });
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
  rc_client_set_allow_background_memory_reads(s_state.client, true);

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
  *http = HTTPDownloader::Create(fmt::format("{} {}", Core::GetHTTPUserAgent(), rc_client_user_agent));
  if (!*http)
  {
    Host::ReportErrorAsync("Achievements Error", "Failed to create HTTPDownloader, cannot use achievements");
    rc_client_destroy(new_client);
    return false;
  }

  (*http)->SetTimeout(SERVER_CALL_TIMEOUT);
  (*http)->SetMaxActiveRequests(MAX_CONCURRENT_SERVER_CALLS);

  rc_client_set_userdata(new_client, http->get());

  // Allow custom host to be overridden through config.
  if (std::string host = Core::GetBaseStringSettingValue("Cheevos", "Host"); !host.empty())
  {
    // drop trailing slash, rc_client appends its own
    while (!host.empty() && host.back() == '/')
      host.pop_back();
    if (!host.empty())
    {
      INFO_COLOR_LOG(StrongOrange, "Using alternative host for achievements: {}", host);
      rc_client_set_host(new_client, host.c_str());
    }
  }

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
  const TinyString username = Core::GetTinyStringSettingValue("Cheevos", "Username");
  const TinyString api_token = Core::GetTinyStringSettingValue("Cheevos", "Token");
  return (!username.empty() && !api_token.empty());
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
    if (!g_settings.achievements_hardcore_mode)
      DisableHardcoreMode(true, true);
  }

  auto lock = GetLock();
  const bool encore_mode_changed = (g_settings.achievements_encore_mode != old_config.achievements_encore_mode);
  const bool spectator_mode_changed =
    (g_settings.achievements_spectator_mode != old_config.achievements_spectator_mode);
  const bool unofficial_test_mode_changed =
    (g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode);

  if (encore_mode_changed)
    rc_client_set_encore_mode_enabled(s_state.client, g_settings.achievements_encore_mode);
  if (spectator_mode_changed)
    rc_client_set_spectator_mode_enabled(s_state.client, g_settings.achievements_spectator_mode);
  if (unofficial_test_mode_changed)
    rc_client_set_unofficial_enabled(s_state.client, g_settings.achievements_unofficial_test_mode);

  // If a game is active and these settings changed, reload the game to apply them.
  // Just unload and reload without destroying the client to preserve hardcore mode.
  if (HasActiveGame() && (encore_mode_changed || spectator_mode_changed || unofficial_test_mode_changed))
  {
    // Save and restore state to preserve progress.
    const DynamicHeapArray<u8> state_data = SaveStateToBuffer();
    ClearGameInfo();
    BeginLoadGame();
    LoadStateFromBuffer(state_data.cspan(), lock);
    return;
  }

  if (!g_settings.achievements_leaderboard_trackers)
    s_state.active_leaderboard_trackers.clear();

  if (!g_settings.achievements_progress_indicators)
    s_state.active_progress_indicator.reset();
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

    const rc_api_server_response_t rr = MakeRCAPIServerResponse(status_code, data);
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

rc_api_server_response_t Achievements::MakeRCAPIServerResponse(s32 status_code, const std::vector<u8>& data)
{
  return rc_api_server_response_t{
    .body = data.empty() ? nullptr : reinterpret_cast<const char*>(data.data()),
    .body_length = data.size(),
    .http_status_code = (status_code <= 0) ? (status_code == HTTPDownloader::HTTP_STATUS_CANCELLED ?
                                                RC_API_SERVER_RESPONSE_CLIENT_ERROR :
                                                RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR) :
                                             status_code,
  };
}

void Achievements::WaitForHTTPRequestsWithYield(std::unique_lock<std::recursive_mutex>& lock)
{
  DebugAssert(s_state.http_downloader);
  s_state.http_downloader->WaitForAllRequestsWithYield([&lock]() { lock.unlock(); }, [&lock]() { lock.lock(); });
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
    if (image && !disable_hardcore_mode && g_settings.achievements_hardcore_mode && IsLoggedInOrLoggingIn())
      EnableHardcoreMode(false, false);
  }

  // now we can finally identify the game
  IdentifyGame(image);
  BeginLoadGame();
}

void Achievements::OnSystemDestroyed()
{
  const auto lock = GetLock();
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

  s_state.load_game_request =
    rc_client_begin_change_media_from_hash(s_state.client, GameHashToString(s_state.game_hash).c_str(),
                                           ClientLoadGameCallback, reinterpret_cast<void*>(static_cast<uintptr_t>(1)));

  // Flag the disc change. That way we reload the game on reset instead of treating it as a swap.
  s_state.reload_game_on_reset = true;
}

bool Achievements::IdentifyGame(CDImage* image)
{
  std::optional<GameHash> game_hash;
  if (image)
  {
    game_hash = GetGameHash(image);

    if (!game_hash.has_value() && !rc_client_is_game_loaded(s_state.client))
    {
      // If we are starting with this game and it's bad, notify the user that this is why.
      Host::AddIconOSDMessage(OSDMessageType::Error, "AchievementsHashFailed", ICON_EMOJI_WARNING,
                              TRANSLATE_STR("Achievements", "Failed to read executable from disc."),
                              TRANSLATE_STR("Achievements", "Achievements have been disabled."));
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
  if (const std::string& disc_path = System::GetGamePath(); !disc_path.empty())
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
  s_state.game_icon_url =
    info->badge_url ? std::string(info->badge_url) : GetImageURL(info->badge_name, RC_IMAGE_TYPE_GAME);
  s_state.game_icon = GetLocalImagePath(info->badge_name, RC_IMAGE_TYPE_GAME);
  if (!s_state.game_icon.empty() && !s_state.game_icon_url.empty())
  {
    if (!FileSystem::FileExists(s_state.game_icon.c_str()))
      DownloadImage(s_state.game_icon_url, s_state.game_icon);

    GameList::UpdateAchievementBadgeName(info->id, info->badge_name);
  }

  // update progress database on first load, in case it was played on another PC
  UpdateGameSummary(true);

  // needed for notifications
  SoundEffectManager::EnsureInitialized();

  if (display_summary)
    DisplayAchievementSummary();
}

void Achievements::ClearGameInfo()
{
  FullscreenUI::ClearAchievementsState();

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
  s_state.reload_game_on_reset = false;
  s_state.has_achievements = false;
  s_state.has_leaderboards = false;
  s_state.has_rich_presence = false;
  s_state.rich_presence_string = {};
  s_state.game_summary = {};
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

    FullscreenUI::AddAchievementNotification("AchievementsSummary",
                                             IsHardcoreModeActive() ? ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME_HC :
                                                                      ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME,
                                             s_state.game_icon, s_state.game_title, std::string(summary),
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
  const rc_client_achievement_t* cheevo = event->achievement;
  DebugAssert(cheevo);

  INFO_LOG("Achievement {} ({}) for game {} unlocked", cheevo->id, cheevo->title, s_state.game_id);
  UpdateGameSummary(true);

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

    FullscreenUI::AddAchievementNotification(
      fmt::format("achievement_unlock_{}", cheevo->id),
      static_cast<float>(g_settings.achievements_notification_duration), GetAchievementBadgePath(cheevo, false),
      std::move(title), std::string(cheevo->description), std::move(note),
      (cheevo->points > 0) ? FullscreenUI::AchievementNotificationNoteType::Text :
                             FullscreenUI::AchievementNotificationNoteType::None);
  }

  if (g_settings.achievements_sound_effects)
    SoundEffectManager::EnqueueSoundEffect(UNLOCK_SOUND_NAME);
}

void Achievements::HandleGameCompleteEvent(const rc_client_event_t* event)
{
  INFO_LOG("Game {} ({}) complete", s_state.game_id, s_state.game_title);
  UpdateGameSummary(false);

  if (g_settings.achievements_notifications)
  {
    std::string message = fmt::format(
      TRANSLATE_FS("Achievements", "Game complete.\n{0} and {1}."),
      TRANSLATE_PLURAL_STR("Achievements", "%n achievements", "Mastery popup",
                           s_state.game_summary.num_unlocked_achievements),
      TRANSLATE_PLURAL_STR("Achievements", "%n points", "Achievement points", s_state.game_summary.points_unlocked));

    FullscreenUI::AddAchievementNotification("achievement_mastery", GAME_COMPLETE_NOTIFICATION_TIME, s_state.game_icon,
                                             s_state.game_title, std::move(message), ICON_EMOJI_TROPHY,
                                             FullscreenUI::AchievementNotificationNoteType::IconText);
  }
}

void Achievements::HandleSubsetCompleteEvent(const rc_client_event_t* event)
{
  INFO_LOG("Subset {} ({}) complete", event->subset->id, event->subset->title);
  UpdateGameSummary(false);

  if (g_settings.achievements_notifications && event->subset->badge_name[0] != '\0')
  {
    // Need to grab the icon for the subset.
    std::string badge_path = GetSubsetBadgePath(event->subset);

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
      fmt::format("leaderboard_{}", event->leaderboard->id), LEADERBOARD_STARTED_NOTIFICATION_TIME, s_state.game_icon,
      std::string(event->leaderboard->title), TRANSLATE_STR("Achievements", "Leaderboard attempt started."),
      ICON_EMOJI_RED_FLAG, FullscreenUI::AchievementNotificationNoteType::IconText);
  }
}

void Achievements::HandleLeaderboardFailedEvent(const rc_client_event_t* event)
{
  DEV_LOG("Leaderboard {} ({}) failed", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications)
  {
    FullscreenUI::AddAchievementNotification(
      fmt::format("leaderboard_{}", event->leaderboard->id), LEADERBOARD_FAILED_NOTIFICATION_TIME, s_state.game_icon,
      std::string(event->leaderboard->title), TRANSLATE_STR("Achievements", "Leaderboard attempt failed."),
      ICON_EMOJI_CROSS_MARK_BUTTON, FullscreenUI::AchievementNotificationNoteType::IconText);
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
      static_cast<float>(g_settings.achievements_leaderboard_duration), s_state.game_icon,
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
      static_cast<float>(g_settings.achievements_leaderboard_duration), s_state.game_icon,
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

  std::string badge_path = GetAchievementBadgePath(event->achievement, false);

  // we still track these even if the option is disabled, so that they can be displayed in the pause menu
  if (g_settings.achievements_challenge_indicator_mode == AchievementChallengeIndicatorMode::Notification)
  {
    FullscreenUI::AddAchievementNotification(
      fmt::format("AchievementChallenge{}", event->achievement->id), CHALLENGE_STARTED_NOTIFICATION_TIME, badge_path,
      fmt::format(TRANSLATE_FS("Achievements", "Challenge Started: {}"),
                  event->achievement->title ? event->achievement->title : ""),
      fmt::format(ICON_EMOJI_DIRECT_HIT " {}", event->achievement->description ? event->achievement->description : ""),
      {}, FullscreenUI::AchievementNotificationNoteType::None, 0, true);
  }

  s_state.active_challenge_indicators.push_back(
    ActiveChallengeIndicator{.achievement = event->achievement,
                             .badge_path = std::move(badge_path),
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
      fmt::format("AchievementChallenge{}", event->achievement->id), CHALLENGE_FAILED_NOTIFICATION_TIME, it->badge_path,
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

  if (!g_settings.achievements_progress_indicators)
    return;

  if (!s_state.active_progress_indicator.has_value())
    s_state.active_progress_indicator.emplace();

  s_state.active_progress_indicator->achievement = event->achievement;
  s_state.active_progress_indicator->badge_path = GetAchievementBadgePath(event->achievement, false);
  s_state.active_progress_indicator->time = 0.0f;
  s_state.active_progress_indicator->active = true;
  FullscreenUI::UpdateAchievementsLastProgressUpdate(event->achievement);
}

void Achievements::HandleAchievementProgressIndicatorHideEvent(const rc_client_event_t* event)
{
  if (!s_state.active_progress_indicator.has_value())
    return;

  DEV_LOG("Hiding progress indicator");

  if (!g_settings.achievements_progress_indicators)
  {
    s_state.active_progress_indicator.reset();
    return;
  }

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

  Host::AddIconOSDMessage(OSDMessageType::Warning, "AchievementsDisconnected", RA_LOGO_ICON_NAME,
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
    Host::AddIconOSDMessage(OSDMessageType::Info, "AchievementsHardcoreModeChanged", RA_LOGO_ICON_NAME,
                            enabled ? TRANSLATE_STR("Achievements", "Hardcore mode enabled.") :
                                      TRANSLATE_STR("Achievements", "Hardcore mode disabled."),
                            enabled ? TRANSLATE_STR("Achievements", "Restrictions are now active.") :
                                      TRANSLATE_STR("Achievements", "Restrictions are no longer active."));
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
    Host::RunOnCoreThread([]() { System::ApplySettings(false); });
  }
  else if (System::GetState() == System::State::Starting)
  {
    // Initial HC enable, activate restrictions.
    System::ApplySettings(false);
  }

  // Toss away UI state, because it's invalid now
  FullscreenUI::ClearAchievementsState();

  Host::OnAchievementsHardcoreModeChanged(enabled);
}

void Achievements::LoadStateFromBuffer(std::span<const u8> data, std::unique_lock<std::recursive_mutex>& lock)
{
  // if we're active, make sure we've downloaded and activated all the achievements
  // before deserializing, otherwise that state's going to get lost.
  if (s_state.load_game_request)
  {
    FullscreenUI::OpenOrUpdateLoadingScreen(System::GetImageForLoadingScreen(System::GetGamePath()),
                                            TRANSLATE_SV("Achievements", "Downloading achievements data..."));

    WaitForHTTPRequestsWithYield(lock);

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

std::string Achievements::GetSubsetBadgePath(const rc_client_subset_t* subset)
{
  std::string badge_path = GetLocalImagePath(subset->badge_name, RC_IMAGE_TYPE_GAME);
  if (!FileSystem::FileExists(badge_path.c_str()))
  {
    std::string url;
    if (IsUsingRAIntegration() || !subset->badge_url)
      url = GetImageURL(subset->badge_name, RC_IMAGE_TYPE_GAME);
    else
      url = subset->badge_url;
    DownloadImage(std::move(url), badge_path);
  }

  return badge_path;
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
  http->WaitForAllRequestsWithYield([&lock]() { lock.unlock(); }, [&lock]() { lock.lock(); });
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
  Core::SetBaseStringSettingValue("Cheevos", "Username", params->username);
  Core::SetBaseStringSettingValue("Cheevos", "Token", EncryptLoginToken(user->token, params->username));
  Core::SetBaseStringSettingValue("Cheevos", "LoginTimestamp", fmt::format("{}", std::time(nullptr)).c_str());
  Host::CommitBaseSettingChanges();

  // Will be using temporary client if achievements are not enabled.
  if (client == s_state.client)
    FinishLogin();
}

void Achievements::FetchGameTitlesCallback(int result, const char* error_message, rc_client_game_title_list_t* list,
                                           rc_client_t* client, void* userdata)
{
  FetchGameTitlesParameters* params = static_cast<FetchGameTitlesParameters*>(userdata);
  params->request = nullptr;

  if (result != RC_OK || !list)
  {
    if (error_message)
      Error::SetString(params->error, error_message);
    else
      Error::SetStringFmt(params->error, TRANSLATE_FS("Achievements", "Failed to fetch game titles (code {})."),
                          result);
    params->success = false;
    return;
  }

  params->list = list;
  params->success = true;
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
}

void Achievements::FinishLogin()
{
  const rc_client_user_t* const user = rc_client_get_user_info(s_state.client);
  if (!user)
    return;

  s_state.user_badge_path = GetLocalImagePath(user->username, RC_IMAGE_TYPE_USER);
  if (!s_state.user_badge_path.empty() && !FileSystem::FileExists(s_state.user_badge_path.c_str()))
  {
    std::string url;
    if (IsUsingRAIntegration() || !user->avatar_url)
      url = GetImageURL(user->username, RC_IMAGE_TYPE_USER);
    else
      url = user->avatar_url;

    DownloadImage(std::move(url), s_state.user_badge_path);
  }

  PreloadHashDatabase();

  Host::OnAchievementsLoginSuccess(user->username, user->score, user->score_softcore, user->num_unread_messages);

  if (g_settings.achievements_notifications)
  {
    //: Summary for login notification.
    std::string summary = fmt::format(TRANSLATE_FS("Achievements", "Score: {} ({} softcore)\nUnread messages: {}"),
                                      user->score, user->score_softcore, user->num_unread_messages);

    FullscreenUI::AddAchievementNotification("achievements_login", LOGIN_NOTIFICATION_TIME, s_state.user_badge_path,
                                             user->display_name, std::move(summary), RA_LOGO_ICON_NAME,
                                             FullscreenUI::AchievementNotificationNoteType::Image);
  }
}

const char* Achievements::GetLoggedInUserName()
{
  const rc_client_user_t* user = rc_client_get_user_info(s_state.client);
  if (!user) [[unlikely]]
    return nullptr;

  return user->username;
}

const std::string& Achievements::GetLoggedInUserBadgePath()
{
  return s_state.user_badge_path;
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

std::string Achievements::GetGameBadgePath(std::string_view badge_name)
{
  return GetLocalImagePath(badge_name, RC_IMAGE_TYPE_GAME);
}

bool Achievements::DownloadGameIcons(ProgressCallback* progress, Error* error)
{
  // Collect all unique game IDs that don't have icons yet
  std::vector<u32> game_ids;
  {
    const auto lock = GameList::GetLock();
    for (const GameList::Entry& entry : GameList::GetEntries())
    {
      if (entry.achievements_game_id != 0)
      {
        // Check if we already have this badge
        const std::string existing_badge = GameList::GetAchievementGameBadgePath(entry.achievements_game_id);
        if (existing_badge.empty() &&
            std::find(game_ids.begin(), game_ids.end(), entry.achievements_game_id) == game_ids.end())
        {
          game_ids.push_back(entry.achievements_game_id);
        }
      }
    }
  }

  if (game_ids.empty())
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "All available icons have already been downloaded."));
    return false;
  }

  progress->FormatStatusText(TRANSLATE_FS("Achievements", "Fetching icon info for {} games..."), game_ids.size());

  auto lock = GetLock();
  if (!IsActive())
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "Achievements are not enabled."));
    return false;
  }

  // Fetch game titles (includes badge names) from RetroAchievements
  FetchGameTitlesParameters params = {error, nullptr, nullptr, false};
  params.request = rc_client_begin_fetch_game_titles(s_state.client, game_ids.data(), static_cast<u32>(game_ids.size()),
                                                     FetchGameTitlesCallback, &params);
  if (!params.request)
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "Failed to create game titles request."));
    return false;
  }

  WaitForHTTPRequestsWithYield(lock);

  if (!params.success || !params.list)
    return false;

  const ScopedGuard list_guard([&params]() { rc_client_destroy_game_title_list(params.list); });
  if (params.list->num_entries == 0)
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "No image names returned."));
    return false;
  }

  // Create all download requests in parallel
  u32 badges_to_download = 0;
  for (u32 i = 0; i < params.list->num_entries; i++)
  {
    const rc_client_game_title_entry_t& entry = params.list->entries[i];

    if (entry.badge_name[0] == '\0')
      continue;

    std::string path = GetLocalImagePath(entry.badge_name, RC_IMAGE_TYPE_GAME);
    if (FileSystem::FileExists(path.c_str()))
    {
      // Already have this icon, just update the cache
      GameList::UpdateAchievementBadgeName(entry.game_id, entry.badge_name);
      continue;
    }

    std::string url =
      entry.badge_url ? std::string(entry.badge_url) : GetImageURL(entry.badge_name, RC_IMAGE_TYPE_GAME);
    if (url.empty())
      continue;

    badges_to_download++;
    s_state.http_downloader->CreateRequest(
      std::move(url), [path = std::move(path), progress](s32 status_code, const Error& http_error, const std::string&,
                                                         HTTPDownloader::Request::Data data) {
        if (status_code == HTTPDownloader::HTTP_STATUS_OK)
        {
          INFO_LOG("Writing badge to {}...", Path::GetFileName(path));

          Error write_error;
          if (!FileSystem::FileExists(path.c_str()) && !FileSystem::WriteBinaryFile(path.c_str(), data, &write_error))
          {
            ERROR_LOG("Failed to write badge to {}: {}", Path::GetFileName(path), write_error.GetDescription());
            FileSystem::DeleteFile(path.c_str());
          }
        }
        else
        {
          ERROR_LOG("Failed to download badge: HTTP {}: {}", status_code, http_error.GetDescription());
        }

        progress->IncrementProgressValue();
      });
  }

  if (badges_to_download == 0)
  {
    Error::SetStringView(error, TRANSLATE_SV("Achievements", "All icons have already been downloaded."));
    return false;
  }

  progress->SetProgressRange(badges_to_download);
  progress->FormatStatusText(TRANSLATE_FS("Achievements", "Downloading {} game icons..."), badges_to_download);
  WaitForHTTPRequestsWithYield(lock);
  return true;
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
  Core::DeleteBaseSettingValue("Cheevos", "Username");
  Core::DeleteBaseSettingValue("Cheevos", "Token");
  Core::DeleteBaseSettingValue("Cheevos", "LoginTimestamp");
  Host::CommitBaseSettingChanges();
  ClearProgressDatabase();
}

void Achievements::ConfirmHardcoreModeDisableAsync(std::string_view trigger, std::function<void(bool)> callback)
{
  Host::ConfirmMessageAsync(
    TRANSLATE_STR("Achievements", "Confirm Hardcore Mode Disable"),
    fmt::format(TRANSLATE_FS("Achievements", "{0} cannot be performed while hardcore mode is active. Do you want to "
                                             "disable hardcore mode? {0} will be cancelled if you select No."),
                trigger),
    [callback = std::move(callback)](bool res) mutable {
      // don't run the callback in the middle of rendering the UI
      Host::RunOnCoreThread([callback = std::move(callback), res]() {
        if (res)
          DisableHardcoreMode(true, true);
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
}

bool Achievements::RefreshAllProgressDatabase(ProgressCallback* progress, Error* error)
{
  auto lock = GetLock();
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
  progress->SetStatusText(TRANSLATE_SV("Achievements", "Refreshing achievement progress..."));
  progress->SetProgressRange(0);
  progress->SetProgressValue(0);

  std::pair<bool, Error*> result = {false, error};
  s_state.refresh_all_progress_request = rc_client_begin_fetch_all_user_progress(s_state.client, RC_CONSOLE_PLAYSTATION,
                                                                                 RefreshAllProgressCallback, &result);
  while (s_state.refresh_all_progress_request)
    WaitForHTTPRequestsWithYield(lock);

  return true;
}

void Achievements::RefreshAllProgressCallback(int result, const char* error_message,
                                              rc_client_all_user_progress_t* list, rc_client_t* client,
                                              void* callback_userdata)
{
  s_state.refresh_all_progress_request = nullptr;

  std::pair<bool, Error*>* result_ud = static_cast<std::pair<bool, Error*>*>(callback_userdata);
  if (result != RC_OK)
  {
    if (result_ud)
    {
      result_ud->first = false;
      result_ud->second->SetStringFmt("{}: {}\n{}", TRANSLATE_SV("Achievements", "Refresh all progress failed"),
                                      rc_error_str(result), error_message);
    }

    return;
  }

  BuildProgressDatabase(list);
  rc_client_destroy_all_user_progress(list);

  GameList::UpdateAllAchievementData();

  if (result_ud)
    result_ud->first = true;
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
  Host::QueueAsyncTask([game_id = s_state.game_id, achievements_unlocked, achievements_unlocked_hardcore]() {
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

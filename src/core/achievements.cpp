// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// TODO: Don't poll when booting the game, e.g. Crash Warped freaks out.

#include "achievements.h"
#include "achievements_private.h"
#include "bios.h"
#include "bus.h"
#include "cpu_core.h"
#include "fullscreen_ui.h"
#include "gpu_thread.h"
#include "host.h"
#include "imgui_overlays.h"
#include "system.h"

#include "scmversion/scmversion.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"
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

#ifdef ENABLE_RAINTEGRATION
// RA_Interface ends up including windows.h, with its silly macros.
#ifdef _WIN32
#include "common/windows_headers.h"
#endif
#include "RA_Interface.h"
#endif
namespace Achievements {

static constexpr const char* INFO_SOUND_NAME = "sounds/achievements/message.wav";
static constexpr const char* UNLOCK_SOUND_NAME = "sounds/achievements/unlock.wav";
static constexpr const char* LBSUBMIT_SOUND_NAME = "sounds/achievements/lbsubmit.wav";
static constexpr const char* ACHEIVEMENT_DETAILS_URL_TEMPLATE = "https://retroachievements.org/achievement/{}";
static constexpr const char* PROFILE_DETAILS_URL_TEMPLATE = "https://retroachievements.org/user/{}";
static constexpr const char* CACHE_SUBDIRECTORY_NAME = "achievement_images";

static constexpr size_t URL_BUFFER_SIZE = 256;

static constexpr u32 LEADERBOARD_NEARBY_ENTRIES_TO_FETCH = 10;
static constexpr u32 LEADERBOARD_ALL_FETCH_SIZE = 20;

static constexpr float LOGIN_NOTIFICATION_TIME = 5.0f;
static constexpr float ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME = 5.0f;
static constexpr float GAME_COMPLETE_NOTIFICATION_TIME = 20.0f;
static constexpr float LEADERBOARD_STARTED_NOTIFICATION_TIME = 3.0f;
static constexpr float LEADERBOARD_FAILED_NOTIFICATION_TIME = 3.0f;

static constexpr float INDICATOR_FADE_IN_TIME = 0.1f;
static constexpr float INDICATOR_FADE_OUT_TIME = 0.5f;

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
  Timer show_hide_time;
  bool active;
};

struct AchievementChallengeIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_path;
  Timer show_hide_time;
  bool active;
};

struct AchievementProgressIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_path;
  Timer show_hide_time;
  bool active;
};
} // namespace

static void ReportError(std::string_view sv);
template<typename... T>
static void ReportFmtError(fmt::format_string<T...> fmt, T&&... args);
template<typename... T>
static void ReportRCError(int err, fmt::format_string<T...> fmt, T&&... args);
static void ClearGameInfo();
static void ClearGameHash();
static std::string GetGameHash(CDImage* image);
static void SetHardcoreMode(bool enabled, bool force_display_message);
static bool IsLoggedInOrLoggingIn();
static bool CanEnableHardcoreMode();
static void ShowLoginSuccess(const rc_client_t* client);
static void ShowLoginNotification();
static void IdentifyGame(const std::string& path, CDImage* image);
static void BeginLoadGame();
static void BeginChangeDisc();
static void UpdateGameSummary();
static std::string GetLocalImagePath(const std::string_view image_name, int type);
static void DownloadImage(std::string url, std::string cache_filename);
static void UpdateGlyphRanges();

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
static void DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, bool is_self, float rank_column_width,
                                 float name_column_width, float time_column_width, float column_spacing);
#endif

struct State
{
  rc_client_t* client = nullptr;
  bool hardcore_mode = false;
  bool has_achievements = false;
  bool has_leaderboards = false;
  bool has_rich_presence = false;

#ifdef ENABLE_RAINTEGRATION
  bool using_raintegration = false;
#endif

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
  std::string game_hash;
  std::string game_title;
  std::string game_icon;
  std::string game_icon_url;

  DynamicHeapArray<u8> state_buffer;

  rc_client_async_handle_t* login_request = nullptr;
  rc_client_async_handle_t* load_game_request = nullptr;

  rc_client_achievement_list_t* achievement_list = nullptr;
  std::vector<std::pair<const void*, std::string>> achievement_badge_paths;

  rc_client_leaderboard_list_t* leaderboard_list = nullptr;
  const rc_client_leaderboard_t* open_leaderboard = nullptr;
  rc_client_async_handle_t* leaderboard_fetch_handle = nullptr;
  std::vector<rc_client_leaderboard_entry_list_t*> leaderboard_entry_lists;
  std::vector<std::pair<const rc_client_leaderboard_entry_t*, std::string>> leaderboard_user_icon_paths;
  rc_client_leaderboard_entry_list_t* leaderboard_nearby_entries;
  bool is_showing_all_leaderboard_entries = false;
};

ALIGN_TO_CACHE_LINE static State s_state;

} // namespace Achievements

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
  std::string error = fmt::format("Achievements error: {}", sv);
  ERROR_LOG(error.c_str());
  Host::AddOSDMessage(std::move(error), Host::OSD_CRITICAL_ERROR_DURATION);
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

std::string Achievements::GetGameHash(CDImage* image)
{
  std::string executable_name;
  std::vector<u8> executable_data;
  if (!System::ReadExecutableFromImage(image, &executable_name, &executable_data))
    return {};

  BIOS::PSEXEHeader header = {};
  if (executable_data.size() >= sizeof(header))
    std::memcpy(&header, executable_data.data(), sizeof(header));
  if (!BIOS::IsValidPSExeHeader(header, executable_data.size()))
  {
    ERROR_LOG("PS-EXE header is invalid in '{}' ({} bytes)", executable_name, executable_data.size());
    return {};
  }

  // This is absolutely bonkers silly. Someone decided to hash the file size specified in the executable, plus 2048,
  // instead of adding the size of the header. It _should_ be "header.file_size + sizeof(header)". But we have to hack
  // around it because who knows how many games are affected by this.
  // https://github.com/RetroAchievements/rcheevos/blob/b8dd5747a4ed38f556fd776e6f41b131ea16178f/src/rhash/hash.c#L2824
  const u32 hash_size = std::min(header.file_size + 2048, static_cast<u32>(executable_data.size()));

  MD5Digest digest;
  digest.Update(executable_name.c_str(), static_cast<u32>(executable_name.size()));
  if (hash_size > 0)
    digest.Update(executable_data.data(), hash_size);

  u8 hash[16];
  digest.Final(hash);

  const std::string hash_str =
    fmt::format("{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], hash[8], hash[9], hash[10],
                hash[11], hash[12], hash[13], hash[14], hash[15]);

  INFO_LOG("Hash for '{}' ({} bytes, {} bytes hashed): {}", executable_name, executable_data.size(), hash_size,
           hash_str);
  return hash_str;
}

std::string Achievements::GetLocalImagePath(const std::string_view image_name, int type)
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

void Achievements::DownloadImage(std::string url, std::string cache_filename)
{
  auto callback = [cache_filename](s32 status_code, const Error& error, const std::string& content_type,
                                   HTTPDownloader::Request::Data data) {
    if (status_code != HTTPDownloader::HTTP_STATUS_OK)
    {
      ERROR_LOG("Failed to download badge '{}': {}", Path::GetFileName(cache_filename), error.GetDescription());
      return;
    }

    if (!FileSystem::WriteBinaryFile(cache_filename.c_str(), data.data(), data.size()))
    {
      ERROR_LOG("Failed to write badge image to '{}'", cache_filename);
      return;
    }

    ImGuiFullscreen::InvalidateCachedTexture(cache_filename);
  };

  s_state.http_downloader->CreateRequest(std::move(url), std::move(callback));
}

void Achievements::UpdateGlyphRanges()
{
  // To avoid rasterizing all emoji fonts, we get the set of used glyphs in the emoji range for all strings in the
  // current game's achievement data.
  using CodepointSet = std::unordered_set<ImGuiManager::WCharType>;
  CodepointSet codepoints;

  static constexpr auto add_string = [](const std::string_view str, CodepointSet& codepoints) {
    char32_t codepoint;
    for (size_t offset = 0; offset < str.length();)
    {
      offset += StringUtil::DecodeUTF8(str, offset, &codepoint);

      // Basic Latin + Latin Supplement always included.
      if (codepoint != StringUtil::UNICODE_REPLACEMENT_CHARACTER && codepoint >= 0x2000)
        codepoints.insert(static_cast<ImGuiManager::WCharType>(codepoint));
    }
  };

  if (rc_client_has_rich_presence(s_state.client))
  {
    std::vector<const char*> rp_strings;
    for (;;)
    {
      rp_strings.resize(std::max<size_t>(rp_strings.size() * 2, 512));

      size_t count;
      const int err = rc_client_get_rich_presence_strings(s_state.client, rp_strings.data(), rp_strings.size(), &count);
      if (err == RC_INSUFFICIENT_BUFFER)
        continue;
      else if (err != RC_OK)
        rp_strings.clear();
      else
        rp_strings.resize(count);

      break;
    }

    for (const char* str : rp_strings)
      add_string(str, codepoints);
  }

  if (rc_client_has_achievements(s_state.client))
  {
    rc_client_achievement_list_t* const achievements =
      rc_client_create_achievement_list(s_state.client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL, 0);
    if (achievements)
    {
      for (u32 i = 0; i < achievements->num_buckets; i++)
      {
        const rc_client_achievement_bucket_t& bucket = achievements->buckets[i];
        for (u32 j = 0; j < bucket.num_achievements; j++)
        {
          const rc_client_achievement_t* achievement = bucket.achievements[j];
          if (achievement->title)
            add_string(achievement->title, codepoints);
          if (achievement->description)
            add_string(achievement->description, codepoints);
        }
      }
      rc_client_destroy_achievement_list(achievements);
    }
  }

  if (rc_client_has_leaderboards(s_state.client))
  {
    rc_client_leaderboard_list_t* const leaderboards =
      rc_client_create_leaderboard_list(s_state.client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
    if (leaderboards)
    {
      for (u32 i = 0; i < leaderboards->num_buckets; i++)
      {
        const rc_client_leaderboard_bucket_t& bucket = leaderboards->buckets[i];
        for (u32 j = 0; j < bucket.num_leaderboards; j++)
        {
          const rc_client_leaderboard_t* leaderboard = bucket.leaderboards[j];
          if (leaderboard->title)
            add_string(leaderboard->title, codepoints);
          if (leaderboard->description)
            add_string(leaderboard->description, codepoints);
        }
      }
      rc_client_destroy_leaderboard_list(leaderboards);
    }
  }

  std::vector<ImGuiManager::WCharType> sorted_codepoints;
  sorted_codepoints.reserve(codepoints.size());
  sorted_codepoints.insert(sorted_codepoints.begin(), codepoints.begin(), codepoints.end());
  std::sort(sorted_codepoints.begin(), sorted_codepoints.end());

  // Compact codepoints to ranges.
  GPUThread::RunOnThread([sorted_codepoints = std::move(sorted_codepoints)]() {
    ImGuiManager::SetEmojiFontRange(ImGuiManager::CompactFontRange(sorted_codepoints));
  });
}

bool Achievements::IsActive()
{
#ifdef ENABLE_RAINTEGRATION
  return (s_state.client != nullptr) || s_state.using_raintegration;
#else
  return (s_state.client != nullptr);
#endif
}

bool Achievements::IsHardcoreModeActive()
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    return RA_HardcoreModeIsActive() != 0;
#endif

  return s_state.hardcore_mode;
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
  if (IsUsingRAIntegration())
    return true;

  auto lock = GetLock();
  AssertMsg(g_settings.achievements_enabled, "Achievements are enabled");
  Assert(!s_state.client && !s_state.http_downloader);

  if (!CreateClient(&s_state.client, &s_state.http_downloader))
    return false;

  // Hardcore starts off. We enable it on first boot.
  s_state.hardcore_mode = false;

  rc_client_set_event_handler(s_state.client, ClientEventHandler);

  rc_client_set_hardcore_enabled(s_state.client, s_state.hardcore_mode);
  rc_client_set_encore_mode_enabled(s_state.client, g_settings.achievements_encore_mode);
  rc_client_set_unofficial_enabled(s_state.client, g_settings.achievements_unofficial_test_mode);
  rc_client_set_spectator_mode_enabled(s_state.client, g_settings.achievements_spectator_mode);

  // Begin disc identification early, before the login finishes.
  if (System::IsValid())
    IdentifyGame(System::GetDiscPath(), nullptr);

  std::string username = Host::GetBaseStringSettingValue("Cheevos", "Username");
  std::string api_token = Host::GetBaseStringSettingValue("Cheevos", "Token");
  if (!username.empty() && !api_token.empty())
  {
    INFO_LOG("Attempting login with user '{}'...", username);

    // If we can't decrypt the token, it was an old config and we need to re-login.
    if (const TinyString decrypted_api_token = DecryptLoginToken(api_token, username); !decrypted_api_token.empty())
    {
      s_state.login_request = rc_client_begin_login_with_token(
        s_state.client, username.c_str(), decrypted_api_token.c_str(), ClientLoginWithTokenCallback, nullptr);
    }
    else
    {
      WARNING_LOG("Invalid encrypted login token, requesitng a new one.");
      Host::OnAchievementsLoginRequested(LoginRequestReason::TokenInvalid);
    }
  }

  // Hardcore mode isn't enabled when achievements first starts, if a game is already running.
  if (System::IsValid() && IsLoggedInOrLoggingIn() && g_settings.achievements_hardcore_mode)
    DisplayHardcoreDeferredMessage();

  return true;
}

bool Achievements::CreateClient(rc_client_t** client, std::unique_ptr<HTTPDownloader>* http)
{
  *http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
  if (!*http)
  {
    Host::ReportErrorAsync("Achievements Error", "Failed to create HTTPDownloader, cannot use achievements");
    return false;
  }

  (*http)->SetTimeout(SERVER_CALL_TIMEOUT);
  (*http)->SetMaxActiveRequests(MAX_CONCURRENT_SERVER_CALLS);

  rc_client_t* new_client = rc_client_create(ClientReadMemory, ClientServerCall);
  if (!new_client)
  {
    Host::ReportErrorAsync("Achievements Error", "rc_client_create() failed, cannot use achievements");
    http->reset();
    return false;
  }

#if defined(_DEBUG) || defined(_DEVEL)
  rc_client_enable_logging(new_client, RC_CLIENT_LOG_LEVEL_VERBOSE, ClientMessageCallback);
#else
  rc_client_enable_logging(new_client, RC_CLIENT_LOG_LEVEL_INFO, ClientMessageCallback);
#endif

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

void Achievements::UpdateSettings(const Settings& old_config)
{
  if (IsUsingRAIntegration())
    return;

  if (!g_settings.achievements_enabled)
  {
    // we're done here
    Shutdown(false);
    return;
  }

  if (!IsActive())
  {
    // we just got enabled
    Initialize();
    return;
  }

  if (g_settings.achievements_hardcore_mode != old_config.achievements_hardcore_mode)
  {
    // Hardcore mode can only be enabled through reset (ResetChallengeMode()).
    if (s_state.hardcore_mode && !g_settings.achievements_hardcore_mode)
    {
      ResetHardcoreMode(false);
    }
    else if (!s_state.hardcore_mode && g_settings.achievements_hardcore_mode)
    {
      if (HasActiveGame())
        DisplayHardcoreDeferredMessage();
    }
  }

  // These cannot be modified while a game is loaded, so just toss state and reload.
  if (HasActiveGame())
  {
    if (g_settings.achievements_encore_mode != old_config.achievements_encore_mode ||
        g_settings.achievements_spectator_mode != old_config.achievements_spectator_mode ||
        g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode)
    {
      Shutdown(false);
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

bool Achievements::Shutdown(bool allow_cancel)
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    if (System::IsValid() && allow_cancel && !RA_ConfirmLoadNewRom(true))
      return false;

    RA_SetPaused(false);
    RA_ActivateGame(0);
    return true;
  }
#endif

  if (!IsActive())
    return true;

  auto lock = GetLock();
  Assert(s_state.client && s_state.http_downloader);

  ClearGameInfo();
  ClearGameHash();
  DisableHardcoreMode();
  UpdateGlyphRanges();

  if (s_state.load_game_request)
  {
    rc_client_abort_async(s_state.client, s_state.load_game_request);
    s_state.load_game_request = nullptr;
  }
  if (s_state.login_request)
  {
    rc_client_abort_async(s_state.client, s_state.login_request);
    s_state.login_request = nullptr;
  }

  s_state.hardcore_mode = false;
  DestroyClient(&s_state.client, &s_state.http_downloader);

  Host::OnAchievementsRefreshed();
  return true;
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
    rr.body = reinterpret_cast<const char*>(data.data());

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

#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    return;
#endif

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

#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RA_DoAchievementsFrame();
    return;
  }
#endif

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

void Achievements::GameChanged(const std::string& path, CDImage* image)
{
  std::unique_lock lock(s_state.mutex);

  if (!IsActive())
    return;

  IdentifyGame(path, image);
}

void Achievements::IdentifyGame(const std::string& path, CDImage* image)
{
  if (s_state.game_path == path)
  {
    WARNING_LOG("Game path is unchanged.");
    return;
  }

  std::unique_ptr<CDImage> temp_image;
  if (!path.empty() && !image)
  {
    temp_image = CDImage::Open(path.c_str(), g_settings.cdrom_load_image_patches, nullptr);
    image = temp_image.get();
    if (!temp_image)
      ERROR_LOG("Failed to open temporary CD image '{}'", path);
  }

  std::string game_hash;
  if (image)
    game_hash = GetGameHash(image);

  if (s_state.game_hash == game_hash)
  {
    // only the path has changed - different format/save state/etc.
    INFO_LOG("Detected path change from '{}' to '{}'", s_state.game_path, path);
    s_state.game_path = path;
    return;
  }

  ClearGameHash();
  s_state.game_path = path;
  s_state.game_hash = std::move(game_hash);
  s_state.state_buffer.deallocate();

#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RAIntegration::GameChanged();
    return;
  }
#endif

  // shouldn't have a load game request when we're not logged in.
  Assert(IsLoggedInOrLoggingIn() || !s_state.load_game_request);

  // bail out if we're not logged in, just save the hash
  if (!IsLoggedInOrLoggingIn())
  {
    INFO_LOG("Skipping load game because we're not logged in.");
    DisableHardcoreMode();
    return;
  }

  if (!rc_client_is_game_loaded(s_state.client))
    BeginLoadGame();
  else
    BeginChangeDisc();
}

void Achievements::BeginLoadGame()
{
  ClearGameInfo();

  if (s_state.game_hash.empty())
  {
    // when we're booting the bios, this will fail
    if (!s_state.game_path.empty())
    {
      Host::AddKeyedOSDMessage(
        "retroachievements_disc_read_failed",
        TRANSLATE_STR("Achievements", "Failed to read executable from disc. Achievements disabled."),
        Host::OSD_ERROR_DURATION);
    }

    DisableHardcoreMode();
    UpdateGlyphRanges();
    return;
  }

  s_state.load_game_request =
    rc_client_begin_load_game(s_state.client, s_state.game_hash.c_str(), ClientLoadGameCallback, nullptr);
}

void Achievements::BeginChangeDisc()
{
  // cancel previous requests
  if (s_state.load_game_request)
  {
    rc_client_abort_async(s_state.client, s_state.load_game_request);
    s_state.load_game_request = nullptr;
  }

  if (s_state.game_hash.empty())
  {
    // when we're booting the bios, this will fail
    if (!s_state.game_path.empty())
    {
      Host::AddKeyedOSDMessage(
        "retroachievements_disc_read_failed",
        TRANSLATE_STR("Achievements", "Failed to read executable from disc. Achievements disabled."),
        Host::OSD_ERROR_DURATION);
    }

    ClearGameInfo();
    DisableHardcoreMode();
    UpdateGlyphRanges();
    return;
  }

  s_state.load_game_request =
    rc_client_begin_change_media_from_hash(s_state.client, s_state.game_hash.c_str(), ClientLoadGameCallback,
                                           reinterpret_cast<void*>(static_cast<uintptr_t>(1)));
}

void Achievements::ClientLoadGameCallback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  const bool was_disc_change = (userdata != nullptr);

  s_state.load_game_request = nullptr;
  s_state.state_buffer.deallocate();

  if (result == RC_NO_GAME_LOADED)
  {
    // Unknown game.
    INFO_LOG("Unknown game '{}', disabling achievements.", s_state.game_hash);
    if (was_disc_change)
    {
      ClearGameInfo();
      UpdateGlyphRanges();
    }

    DisableHardcoreMode();
    return;
  }
  else if (result == RC_LOGIN_REQUIRED)
  {
    // We would've asked to re-authenticate, so leave HC on for now.
    // Once we've done so, we'll reload the game.
    return;
  }
  else if (result != RC_OK)
  {
    ReportFmtError("Loading game failed: {}", error_message);
    if (was_disc_change)
    {
      ClearGameInfo();
      UpdateGlyphRanges();
    }

    DisableHardcoreMode();
    return;
  }
  else if (result == RC_HARDCORE_DISABLED)
  {
    if (error_message)
      ReportError(error_message);

    DisableHardcoreMode();
  }

  const rc_client_game_t* info = rc_client_get_game_info(s_state.client);
  if (!info)
  {
    ReportError("rc_client_get_game_info() returned NULL");
    if (was_disc_change)
    {
      ClearGameInfo();
      UpdateGlyphRanges();
    }

    DisableHardcoreMode();
    return;
  }

  const bool has_achievements = rc_client_has_achievements(client);
  const bool has_leaderboards = rc_client_has_leaderboards(client);

  // Only display summary if the game title has changed across discs.
  const bool display_summary = (s_state.game_id != info->id || s_state.game_title != info->title);

  // If the game has a RetroAchievements entry but no achievements or leaderboards,
  // enforcing hardcore mode is pointless.
  if (!has_achievements && !has_leaderboards)
    DisableHardcoreMode();

  // We should have matched hardcore mode state.
  Assert(s_state.hardcore_mode == (rc_client_get_hardcore_enabled(client) != 0));

  s_state.game_id = info->id;
  s_state.game_title = info->title;
  s_state.has_achievements = has_achievements;
  s_state.has_leaderboards = has_leaderboards;
  s_state.has_rich_presence = rc_client_has_rich_presence(client);

  // update ranges before initializing fsui
  UpdateGlyphRanges();

  // ensure fullscreen UI is ready for notifications
  if (display_summary)
    GPUThread::RunOnThread(&FullscreenUI::Initialize);

  char url_buf[URL_BUFFER_SIZE];
  if (int err = rc_client_game_get_image_url(info, url_buf, std::size(url_buf)); err == RC_OK)
    s_state.game_icon_url = url_buf;
  else
    ReportRCError(err, "rc_client_game_get_image_url() failed: ");

  s_state.game_icon = GetLocalImagePath(info->badge_name, RC_IMAGE_TYPE_GAME);
  if (!s_state.game_icon.empty() && !s_state.game_icon_url.empty() &&
      !FileSystem::FileExists(s_state.game_icon.c_str()))
    DownloadImage(s_state.game_icon_url, s_state.game_icon);

  UpdateGameSummary();
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
  s_state.state_buffer.deallocate();
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
  std::string().swap(s_state.game_hash);
}

void Achievements::DisplayAchievementSummary()
{
  if (g_settings.achievements_notifications)
  {
    std::string title;
    if (IsHardcoreModeActive())
      title = fmt::format(TRANSLATE_FS("Achievements", "{} (Hardcore Mode)"), s_state.game_title);
    else
      title = s_state.game_title;

    std::string summary;
    if (s_state.game_summary.num_core_achievements > 0)
    {
      summary = fmt::format(
        TRANSLATE_FS("Achievements", "{0}, {1}."),
        SmallString::from_format(TRANSLATE_PLURAL_FS("Achievements", "You have unlocked {} of %n achievements",
                                                     "Achievement popup", s_state.game_summary.num_core_achievements),
                                 s_state.game_summary.num_unlocked_achievements),
        SmallString::from_format(TRANSLATE_PLURAL_FS("Achievements", "and earned {} of %n points", "Achievement popup",
                                                     s_state.game_summary.points_core),
                                 s_state.game_summary.points_unlocked));
    }
    else
    {
      summary = TRANSLATE_STR("Achievements", "This game has no achievements.");
    }

    GPUThread::RunOnThread(
      [title = std::move(title), summary = std::move(summary), icon = s_state.game_icon]() mutable {
        if (!FullscreenUI::Initialize())
          return;

        ImGuiFullscreen::AddNotification("achievement_summary", ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME, std::move(title),
                                         std::move(summary), std::move(icon));
      });
  }

  // Technically not going through the resource API, but since we're passing this to something else, we can't.
  if (g_settings.achievements_sound_effects)
    PlatformMisc::PlaySoundAsync(EmuFolders::GetOverridableResourcePath(INFO_SOUND_NAME).c_str());
}

void Achievements::DisplayHardcoreDeferredMessage()
{
  if (g_settings.achievements_hardcore_mode && !s_state.hardcore_mode && System::IsValid())
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
  // We handle system resets ourselves, but still need to reset the client's state.
  INFO_LOG("Resetting runtime due to reset event");
  rc_client_reset(s_state.client);

  if (HasActiveGame())
    UpdateGameSummary();
}

void Achievements::HandleUnlockEvent(const rc_client_event_t* event)
{
  const rc_client_achievement_t* cheevo = event->achievement;
  DebugAssert(cheevo);

  INFO_LOG("Achievement {} ({}) for game {} unlocked", cheevo->title, cheevo->id, s_state.game_id);
  UpdateGameSummary();

  if (g_settings.achievements_notifications)
  {
    std::string title;
    if (cheevo->category == RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL)
      title = fmt::format(TRANSLATE_FS("Achievements", "{} (Unofficial)"), cheevo->title);
    else
      title = cheevo->title;

    std::string badge_path = GetAchievementBadgePath(cheevo, cheevo->state);

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
  UpdateGameSummary();

  if (g_settings.achievements_notifications)
  {
    std::string title = fmt::format(TRANSLATE_FS("Achievements", "Mastered {}"), s_state.game_title);
    std::string message = fmt::format(
      TRANSLATE_FS("Achievements", "{0}, {1}"),
      TRANSLATE_PLURAL_STR("Achievements", "%n achievements", "Mastery popup",
                           s_state.game_summary.num_unlocked_achievements),
      TRANSLATE_PLURAL_STR("Achievements", "%n points", "Achievement points", s_state.game_summary.points_unlocked));

    GPUThread::RunOnThread(
      [title = std::move(title), message = std::move(message), icon = s_state.game_icon]() mutable {
        if (!FullscreenUI::Initialize())
          return;

        ImGuiFullscreen::AddNotification("achievement_mastery", GAME_COMPLETE_NOTIFICATION_TIME, std::move(title),
                                         std::move(message), std::move(icon));
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

  TinyString width_string;
  width_string.append(ICON_FA_STOPWATCH);
  const u32 display_len = static_cast<u32>(std::strlen(event->leaderboard_tracker->display));
  for (u32 i = 0; i < display_len; i++)
    width_string.append('0');

  LeaderboardTrackerIndicator indicator;
  indicator.tracker_id = event->leaderboard_tracker->id;
  indicator.text = event->leaderboard_tracker->display;
  indicator.active = true;
  s_state.active_leaderboard_trackers.push_back(std::move(indicator));
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
  it->show_hide_time.Reset();
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
  if (auto it =
        std::find_if(s_state.active_challenge_indicators.begin(), s_state.active_challenge_indicators.end(),
                     [event](const AchievementChallengeIndicator& it) { return it.achievement == event->achievement; });
      it != s_state.active_challenge_indicators.end())
  {
    it->show_hide_time.Reset();
    it->active = true;
    return;
  }

  AchievementChallengeIndicator indicator;
  indicator.achievement = event->achievement;
  indicator.badge_path = GetAchievementBadgePath(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  indicator.active = true;
  s_state.active_challenge_indicators.push_back(std::move(indicator));

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
  it->show_hide_time.Reset();
  it->active = false;
}

void Achievements::HandleAchievementProgressIndicatorShowEvent(const rc_client_event_t* event)
{
  DEV_LOG("Showing progress indicator: {} ({}): {}", event->achievement->id, event->achievement->title,
          event->achievement->measured_progress);

  if (!s_state.active_progress_indicator.has_value())
    s_state.active_progress_indicator.emplace();
  else
    s_state.active_progress_indicator->show_hide_time.Reset();

  s_state.active_progress_indicator->achievement = event->achievement;
  s_state.active_progress_indicator->badge_path =
    GetAchievementBadgePath(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  s_state.active_progress_indicator->active = true;
}

void Achievements::HandleAchievementProgressIndicatorHideEvent(const rc_client_event_t* event)
{
  if (!s_state.active_progress_indicator.has_value())
    return;

  DEV_LOG("Hiding progress indicator");
  s_state.active_progress_indicator->show_hide_time.Reset();
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

void Achievements::ResetClient()
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RA_OnReset();
    return;
  }
#endif

  if (!IsActive())
    return;

  DEV_LOG("Reset client");
  rc_client_reset(s_state.client);
}

void Achievements::OnSystemPaused(bool paused)
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    RA_SetPaused(paused);
#endif
}

void Achievements::DisableHardcoreMode()
{
  if (!IsActive())
    return;

#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    if (RA_HardcoreModeIsActive())
      RA_DisableHardcore();

    return;
  }
#endif

  if (!s_state.hardcore_mode)
    return;

  SetHardcoreMode(false, true);
}

bool Achievements::ResetHardcoreMode(bool is_booting)
{
  if (!IsActive())
    return false;

  const auto lock = GetLock();

  // If we're not logged in, don't apply hardcore mode restrictions.
  // If we later log in, we'll start with it off anyway.
  const bool wanted_hardcore_mode =
    (IsLoggedInOrLoggingIn() || s_state.load_game_request) && g_settings.achievements_hardcore_mode;
  if (s_state.hardcore_mode == wanted_hardcore_mode)
    return false;

  if (!is_booting && wanted_hardcore_mode && !CanEnableHardcoreMode())
    return false;

  SetHardcoreMode(wanted_hardcore_mode, false);
  return true;
}

void Achievements::SetHardcoreMode(bool enabled, bool force_display_message)
{
  if (enabled == s_state.hardcore_mode)
    return;

  // new mode
  s_state.hardcore_mode = enabled;

  if (System::IsValid() && (HasActiveGame() || force_display_message))
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

  rc_client_set_hardcore_enabled(s_state.client, enabled);
  DebugAssert((rc_client_get_hardcore_enabled(s_state.client) != 0) == enabled);
  if (HasActiveGame())
  {
    UpdateGameSummary();
    DisplayAchievementSummary();
  }

  // Reload setting to permit cheating-like things if we were just disabled.
  if (!enabled)
    Host::RunOnCPUThread([]() { System::ApplySettings(false); });

  // Toss away UI state, because it's invalid now
  ClearUIState();

  Host::OnAchievementsHardcoreModeChanged(enabled);
}

bool Achievements::DoState(StateWrapper& sw)
{
  // if we're inactive, we still need to skip the data (if any)
  if (!IsActive())
  {
    u32 data_size = 0;
    sw.Do(&data_size);
    if (data_size > 0)
      sw.SkipBytes(data_size);

    return !sw.HasError();
  }

  std::unique_lock lock(s_state.mutex);

  if (sw.IsReading())
  {
    // if we're active, make sure we've downloaded and activated all the achievements
    // before deserializing, otherwise that state's going to get lost.
    if (!IsUsingRAIntegration() && s_state.load_game_request)
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
    sw.Do(&data_size);
    if (data_size == 0)
    {
      // reset runtime, no data (state might've been created without cheevos)
      DEV_LOG("State is missing cheevos data, resetting runtime");
#ifdef ENABLE_RAINTEGRATION
      if (IsUsingRAIntegration())
        RA_OnReset();
      else
        rc_client_reset(s_state.client);
#else
      rc_client_reset(s_state.client);
#endif

      return !sw.HasError();
    }

    if (data_size > s_state.state_buffer.size())
      s_state.state_buffer.resize(data_size);
    if (data_size > 0)
      sw.DoBytes(s_state.state_buffer.data(), data_size);
    if (sw.HasError())
      return false;

#ifdef ENABLE_RAINTEGRATION
    if (IsUsingRAIntegration())
    {
      RA_RestoreState(reinterpret_cast<const char*>(s_state.state_buffer.data()));
    }
    else
    {
      const int result = rc_client_deserialize_progress_sized(s_state.client, s_state.state_buffer.data(), data_size);
      if (result != RC_OK)
      {
        WARNING_LOG("Failed to deserialize cheevos state ({}), resetting", result);
        rc_client_reset(s_state.client);
      }
    }
#endif

    return true;
  }
  else
  {
    size_t data_size;

#ifdef ENABLE_RAINTEGRATION
    if (IsUsingRAIntegration())
    {
      const int size = RA_CaptureState(nullptr, 0);

      data_size = (size >= 0) ? static_cast<u32>(size) : 0;
      s_state.state_buffer.resize(data_size);

      if (data_size > 0)
      {
        const int result =
          RA_CaptureState(reinterpret_cast<char*>(s_state.state_buffer.data()), static_cast<int>(data_size));
        if (result != static_cast<int>(data_size))
        {
          WARNING_LOG("Failed to serialize cheevos state from RAIntegration.");
          data_size = 0;
        }
      }
    }
    else
#endif
    {
      data_size = rc_client_progress_size(s_state.client);
      if (data_size > 0)
      {
        if (s_state.state_buffer.size() < data_size)
          s_state.state_buffer.resize(data_size);

        const int result = rc_client_serialize_progress_sized(s_state.client, s_state.state_buffer.data(), data_size);
        if (result != RC_OK)
        {
          // set data to zero, effectively serializing nothing
          WARNING_LOG("Failed to serialize cheevos state ({})", result);
          data_size = 0;
        }
      }
    }

    sw.Do(&data_size);
    if (data_size > 0)
      sw.DoBytes(s_state.state_buffer.data(), data_size);

    return !sw.HasError();
  }
}

std::string Achievements::GetAchievementBadgePath(const rc_client_achievement_t* achievement, int state,
                                                  bool download_if_missing)
{
  const std::string path = GetLocalImagePath(achievement->badge_name, (state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED) ?
                                                                        RC_IMAGE_TYPE_ACHIEVEMENT :
                                                                        RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED);
  if (download_if_missing && !path.empty() && !FileSystem::FileExists(path.c_str()))
  {
    char buf[URL_BUFFER_SIZE];
    const int res = rc_client_achievement_get_image_url(achievement, state, buf, std::size(buf));
    if (res == RC_OK)
      DownloadImage(buf, path);
    else
      ReportRCError(res, "rc_client_achievement_get_image_url() for {} failed", achievement->title);
  }

  return path;
}

std::string Achievements::GetLeaderboardUserBadgePath(const rc_client_leaderboard_entry_t* entry)
{
  // TODO: maybe we should just cache these in memory...
  const std::string path = GetLocalImagePath(entry->user, RC_IMAGE_TYPE_USER);

  if (!FileSystem::FileExists(path.c_str()))
  {
    char buf[URL_BUFFER_SIZE];
    const int res = rc_client_leaderboard_entry_get_user_image_url(entry, buf, std::size(buf));
    if (res == RC_OK)
      DownloadImage(buf, path);
    else
      ReportRCError(res, "rc_client_leaderboard_entry_get_user_image_url() for {} failed", entry->user);
  }

  return path;
}

bool Achievements::IsLoggedInOrLoggingIn()
{
  return (rc_client_get_user_info(s_state.client) != nullptr || s_state.login_request);
}

bool Achievements::CanEnableHardcoreMode()
{
  return (s_state.load_game_request || s_state.has_achievements || s_state.has_leaderboards);
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
    BeginLoadGame();

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

  ShowLoginSuccess(client);
}

void Achievements::ClientLoginWithTokenCallback(int result, const char* error_message, rc_client_t* client,
                                                void* userdata)
{
  s_state.login_request = nullptr;

  if (result != RC_OK)
  {
    ReportFmtError("Login failed: {}", error_message);
    Host::OnAchievementsLoginRequested(LoginRequestReason::TokenInvalid);
    return;
  }

  ShowLoginSuccess(client);

  if (System::IsValid())
    BeginLoadGame();
}

void Achievements::ShowLoginSuccess(const rc_client_t* client)
{
  const rc_client_user_t* user = rc_client_get_user_info(client);
  if (!user)
    return;

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
    char url[URL_BUFFER_SIZE];
    const int res = rc_client_user_get_image_url(user, url, std::size(url));
    if (res == RC_OK)
      DownloadImage(url, badge_path);
    else
      ReportRCError(res, "rc_client_user_get_image_url() failed: ");
  }

  return badge_path;
}

u32 Achievements::GetPauseThrottleFrames()
{
  if (!IsActive() || !IsHardcoreModeActive() || IsUsingRAIntegration())
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
      UpdateGlyphRanges();
    }

    INFO_LOG("Logging out...");
    rc_client_logout(s_state.client);
  }

  INFO_LOG("Clearing credentials...");
  Host::DeleteBaseSettingValue("Cheevos", "Username");
  Host::DeleteBaseSettingValue("Cheevos", "Token");
  Host::DeleteBaseSettingValue("Cheevos", "LoginTimestamp");
  Host::CommitBaseSettingChanges();
}

bool Achievements::ConfirmSystemReset()
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    return RA_ConfirmLoadNewRom(false);
#endif

  return true;
}

bool Achievements::ConfirmHardcoreModeDisable(const char* trigger)
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    return (RA_WarnDisableHardcore(trigger) != 0);
#endif

  // I really hope this doesn't deadlock :/
  const bool confirmed = Host::ConfirmMessage(
    TRANSLATE("Achievements", "Confirm Hardcore Mode"),
    fmt::format(TRANSLATE_FS("Achievements", "{0} cannot be performed while hardcore mode is active. Do you "
                                             "want to disable hardcore mode? {0} will be cancelled if you select No."),
                trigger));
  if (!confirmed)
    return false;

  DisableHardcoreMode();
  return true;
}

void Achievements::ConfirmHardcoreModeDisableAsync(const char* trigger, std::function<void(bool)> callback)
{
  auto real_callback = [callback = std::move(callback)](bool res) mutable {
    // don't run the callback in the middle of rendering the UI
    Host::RunOnCPUThread([callback = std::move(callback), res]() {
      if (res)
        DisableHardcoreMode();
      callback(res);
    });
  };

#ifndef __ANDROID__
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    const bool result = (RA_WarnDisableHardcore(trigger) != 0);
    callback(result);
    return;
  }
#endif

  GPUThread::RunOnThread([trigger = std::string(trigger), real_callback = std::move(real_callback)]() mutable {
    if (!FullscreenUI::Initialize())
    {
      Host::AddOSDMessage(
        fmt::format(TRANSLATE_FS("Achievements", "Cannot {} while hardcode mode is active."), trigger),
        Host::OSD_WARNING_DURATION);
      real_callback(false);
      return;
    }

    ImGuiFullscreen::OpenConfirmMessageDialog(
      TRANSLATE_STR("Achievements", "Confirm Hardcore Mode"),
      fmt::format(TRANSLATE_FS("Achievements",
                               "{0} cannot be performed while hardcore mode is active. Do you "
                               "want to disable hardcore mode? {0} will be cancelled if you select No."),
                  trigger),
      std::move(real_callback), fmt::format(ICON_FA_CHECK " {}", TRANSLATE_SV("Achievements", "Yes")),
      fmt::format(ICON_FA_TIMES " {}", TRANSLATE_SV("Achievements", "No")));
  });
#else
  Host::ConfirmMessageAsync(
    TRANSLATE_STR("Achievements", "Confirm Hardcore Mode"),
    fmt::format(TRANSLATE_FS("Achievements", "{0} cannot be performed while hardcore mode is active. Do you want to "
                                             "disable hardcore mode? {0} will be cancelled if you select No."),
                trigger),
    std::move(real_callback));
#endif
}

void Achievements::ClearUIState()
{
#ifndef __ANDROID__
  if (FullscreenUI::IsAchievementsWindowOpen() || FullscreenUI::IsLeaderboardsWindowOpen())
    FullscreenUI::ReturnToPreviousWindow();

  CloseLeaderboard();
#endif

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
}

template<typename T>
static float IndicatorOpacity(const T& i)
{
  const float elapsed = static_cast<float>(i.show_hide_time.GetTimeSeconds());
  const float time = i.active ? Achievements::INDICATOR_FADE_IN_TIME : Achievements::INDICATOR_FADE_OUT_TIME;
  const float opacity = (elapsed >= time) ? 1.0f : (elapsed / time);
  return (i.active) ? opacity : (1.0f - opacity);
}

void Achievements::DrawGameOverlays()
{
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::UIStyle;

  if (!HasActiveGame() || !g_settings.achievements_overlays)
    return;

  const auto lock = GetLock();

  const float spacing = LayoutScale(10.0f);
  const float padding = LayoutScale(10.0f);
  const ImVec2 image_size =
    LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT);
  const ImGuiIO& io = ImGui::GetIO();
  ImVec2 position = ImVec2(io.DisplaySize.x - padding, io.DisplaySize.y - padding);
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  if (!s_state.active_challenge_indicators.empty())
  {
    const float x_advance = image_size.x + spacing;
    ImVec2 current_position = ImVec2(position.x - image_size.x, position.y - image_size.y);

    for (auto it = s_state.active_challenge_indicators.begin(); it != s_state.active_challenge_indicators.end();)
    {
      const AchievementChallengeIndicator& indicator = *it;
      const float opacity = IndicatorOpacity(indicator);
      const u32 col = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity));

      GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(indicator.badge_path);
      if (badge)
      {
        dl->AddImage(badge, current_position, current_position + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     col);
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
    const AchievementProgressIndicator& indicator = s_state.active_progress_indicator.value();
    const float opacity = IndicatorOpacity(indicator);
    const u32 col = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity));

    const char* text_start = s_state.active_progress_indicator->achievement->measured_progress;
    const char* text_end = text_start + std::strlen(text_start);
    const ImVec2 text_size =
      UIStyle.MediumFont->CalcTextSizeA(UIStyle.MediumFont->FontSize, FLT_MAX, 0.0f, text_start, text_end);

    const ImVec2 box_min = ImVec2(position.x - image_size.x - text_size.x - spacing - padding * 2.0f,
                                  position.y - image_size.y - padding * 2.0f);
    const ImVec2 box_max = position;
    const float box_rounding = LayoutScale(1.0f);

    dl->AddRectFilled(box_min, box_max, ImGui::GetColorU32(ImVec4(0.13f, 0.13f, 0.13f, opacity * 0.5f)), box_rounding);
    dl->AddRect(box_min, box_max, ImGui::GetColorU32(ImVec4(0.8f, 0.8f, 0.8f, opacity)), box_rounding);

    GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(indicator.badge_path);
    if (badge)
    {
      const ImVec2 badge_pos = box_min + ImVec2(padding, padding);
      dl->AddImage(badge, badge_pos, badge_pos + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), col);
    }

    const ImVec2 text_pos =
      box_min + ImVec2(padding + image_size.x + spacing, (box_max.y - box_min.y - text_size.y) * 0.5f);
    const ImVec4 text_clip_rect(text_pos.x, text_pos.y, box_max.x, box_max.y);
    dl->AddText(UIStyle.MediumFont, UIStyle.MediumFont->FontSize, text_pos, col, text_start, text_end, 0.0f,
                &text_clip_rect);

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
      const LeaderboardTrackerIndicator& indicator = *it;
      const float opacity = IndicatorOpacity(indicator);

      TinyString width_string;
      width_string.append(ICON_FA_STOPWATCH);
      for (u32 i = 0; i < indicator.text.length(); i++)
        width_string.append('0');
      const ImVec2 size = ImGuiFullscreen::UIStyle.MediumFont->CalcTextSizeA(
        ImGuiFullscreen::UIStyle.MediumFont->FontSize, FLT_MAX, 0.0f, width_string.c_str(), width_string.end_ptr());

      const ImVec2 box_min = ImVec2(position.x - size.x - padding * 2.0f, position.y - size.y - padding * 2.0f);
      const ImVec2 box_max = position;
      const float box_rounding = LayoutScale(1.0f);
      dl->AddRectFilled(box_min, box_max, ImGui::GetColorU32(ImVec4(0.13f, 0.13f, 0.13f, opacity * 0.5f)),
                        box_rounding);
      dl->AddRect(box_min, box_max, ImGui::GetColorU32(ImVec4(0.8f, 0.8f, 0.8f, opacity)), box_rounding);

      const u32 text_col = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity));
      const ImVec2 text_size = ImGuiFullscreen::UIStyle.MediumFont->CalcTextSizeA(
        ImGuiFullscreen::UIStyle.MediumFont->FontSize, FLT_MAX, 0.0f, indicator.text.c_str(),
        indicator.text.c_str() + indicator.text.length());
      const ImVec2 text_pos = ImVec2(box_max.x - padding - text_size.x, box_min.y + padding);
      const ImVec4 text_clip_rect(box_min.x, box_min.y, box_max.x, box_max.y);
      dl->AddText(UIStyle.MediumFont, UIStyle.MediumFont->FontSize, text_pos, text_col, indicator.text.c_str(),
                  indicator.text.c_str() + indicator.text.length(), 0.0f, &text_clip_rect);

      const ImVec2 icon_pos = ImVec2(box_min.x + padding, box_min.y + padding);
      dl->AddText(UIStyle.MediumFont, UIStyle.MediumFont->FontSize, icon_pos, text_col, ICON_FA_STOPWATCH, nullptr,
                  0.0f, &text_clip_rect);

      if (!indicator.active && opacity <= 0.01f)
      {
        DEV_LOG("Remove tracker indicator");
        it = s_state.active_leaderboard_trackers.erase(it);
      }
      else
      {
        ++it;
      }

      position.x = box_min.x - padding;
    }

    // Uncomment if there are any other overlays above this one.
    // position.y -= image_size.y - padding * 3.0f;
  }
}

#ifndef __ANDROID__

void Achievements::DrawPauseMenuOverlays()
{
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::UIStyle;

  if (!HasActiveGame())
    return;

  const auto lock = GetLock();

  if (s_state.active_challenge_indicators.empty() && !s_state.active_progress_indicator.has_value())
    return;

  const ImGuiIO& io = ImGui::GetIO();
  ImFont* font = UIStyle.MediumFont;

  const ImVec2 image_size(LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                                      ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY));
  const float start_y =
    LayoutScale(10.0f + 4.0f + 4.0f) + UIStyle.LargeFont->FontSize + (UIStyle.MediumFont->FontSize * 2.0f);
  const float margin = LayoutScale(10.0f);
  const float spacing = LayoutScale(10.0f);
  const float padding = LayoutScale(10.0f);

  const float max_text_width = ImGuiFullscreen::LayoutScale(300.0f);
  const float row_width = max_text_width + padding + padding + image_size.x + spacing;
  const float title_height = padding + font->FontSize + padding;

  if (!s_state.active_challenge_indicators.empty())
  {
    const ImVec2 box_min(io.DisplaySize.x - row_width - margin, start_y + margin);
    const ImVec2 box_max(box_min.x + row_width,
                         box_min.y + title_height +
                           (static_cast<float>(s_state.active_challenge_indicators.size()) * (image_size.y + padding)));

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(box_min, box_max, IM_COL32(0x21, 0x21, 0x21, 200), LayoutScale(10.0f));
    dl->AddText(font, font->FontSize, ImVec2(box_min.x + padding, box_min.y + padding), IM_COL32(255, 255, 255, 255),
                TRANSLATE("Achievements", "Active Challenge Achievements"));

    const float y_advance = image_size.y + spacing;
    const float acheivement_name_offset = (image_size.y - font->FontSize) / 2.0f;
    const float max_non_ellipised_text_width = max_text_width - LayoutScale(10.0f);
    ImVec2 position(box_min.x + padding, box_min.y + title_height);

    for (const AchievementChallengeIndicator& indicator : s_state.active_challenge_indicators)
    {
      GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(indicator.badge_path);
      if (!badge)
        continue;

      dl->AddImage(badge, position, position + image_size);

      const char* achievement_title = indicator.achievement->title;
      const char* achievement_title_end = achievement_title + std::strlen(indicator.achievement->title);
      const char* remaining_text = nullptr;
      const ImVec2 text_width(font->CalcTextSizeA(font->FontSize, max_non_ellipised_text_width, 0.0f, achievement_title,
                                                  achievement_title_end, &remaining_text));
      const ImVec2 text_position(position.x + image_size.x + spacing, position.y + acheivement_name_offset);
      const ImVec4 text_bbox(text_position.x, text_position.y, text_position.x + max_text_width,
                             text_position.y + image_size.y);
      const u32 text_color = IM_COL32(255, 255, 255, 255);

      if (remaining_text < achievement_title_end)
      {
        dl->AddText(font, font->FontSize, text_position, text_color, achievement_title, remaining_text, 0.0f,
                    &text_bbox);
        dl->AddText(font, font->FontSize, ImVec2(text_position.x + text_width.x, text_position.y), text_color, "...",
                    nullptr, 0.0f, &text_bbox);
      }
      else
      {
        dl->AddText(font, font->FontSize, text_position, text_color, achievement_title, achievement_title_end, 0.0f,
                    &text_bbox);
      }

      position.y += y_advance;
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
  using ImGuiFullscreen::UIStyle;

  if (!s_state.achievement_list)
    return;

  auto lock = Achievements::GetLock();

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
        GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(s_state.game_icon.c_str());
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

      close_window = (ImGuiFullscreen::FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f,
                                                      true, UIStyle.LargeFont) ||
                      ImGuiFullscreen::WantsToCloseMenu());

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFont->FontSize));
      text.assign(s_state.game_title);

      if (s_state.hardcore_mode)
        text.append(TRANSLATE_SV("Achievements", " (Hardcore Mode)"));

      top += UIStyle.LargeFont->FontSize + spacing;

      ImGui::PushFont(UIStyle.LargeFont);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.c_str(), text.end_ptr(), nullptr, ImVec2(0.0f, 0.0f),
                               &title_bb);
      ImGui::PopFont();

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFont->FontSize));
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

      top += UIStyle.MediumFont->FontSize + spacing;

      ImGui::PushFont(UIStyle.MediumFont);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.c_str(), text.end_ptr(), nullptr,
                               ImVec2(0.0f, 0.0f), &summary_bb);
      ImGui::PopFont();

      if (s_state.game_summary.num_core_achievements > 0)
      {
        const float progress_height = LayoutScale(20.0f);
        const ImRect progress_bb(ImVec2(left, top), ImVec2(right, top + progress_height));
        const float fraction = static_cast<float>(s_state.game_summary.num_unlocked_achievements) /
                               static_cast<float>(s_state.game_summary.num_core_achievements);
        dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(UIStyle.PrimaryDarkColor));
        dl->AddRectFilled(progress_bb.Min,
                          ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                          ImGui::GetColorU32(UIStyle.SecondaryColor));

        text.format("{}%", static_cast<int>(std::round(fraction * 100.0f)));
        text_size = ImGui::CalcTextSize(text.c_str(), text.end_ptr());
        const ImVec2 text_pos(
          progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
        dl->AddText(UIStyle.MediumFont, UIStyle.MediumFont->FontSize, text_pos,
                    ImGui::GetColorU32(UIStyle.PrimaryTextColor), text.c_str(), text.end_ptr());
        top += progress_height + spacing;
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
        "achievements", background, 0.0f, ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, 0.0f), 0))
  {
    static bool buckets_collapsed[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS] = {};
    static const char* bucket_names[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS] = {
      TRANSLATE_NOOP("Achievements", "Unknown"),           TRANSLATE_NOOP("Achievements", "Locked"),
      TRANSLATE_NOOP("Achievements", "Unlocked"),          TRANSLATE_NOOP("Achievements", "Unsupported"),
      TRANSLATE_NOOP("Achievements", "Unofficial"),        TRANSLATE_NOOP("Achievements", "Recently Unlocked"),
      TRANSLATE_NOOP("Achievements", "Active Challenges"), TRANSLATE_NOOP("Achievements", "Almost There"),
    };

    ImGuiFullscreen::ResetFocusHere();
    ImGuiFullscreen::BeginMenuButtons();

    for (u32 bucket_type : {RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED, RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE, RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED,
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
        bucket_collapsed ^=
          ImGuiFullscreen::MenuHeadingButton(Host::TranslateToCString("Achievements", bucket_names[bucket.bucket_type]),
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

  ImGuiFullscreen::SetFullscreenFooterText(
    std::array{std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_XBOX_DPAD_UP_DOWN :
                                                                        ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN,
                              TRANSLATE_SV("Achievements", "Change Selection")),
               std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_BUTTON_A : ICON_PF_ENTER,
                              TRANSLATE_SV("Achievements", "View Details")),
               std::make_pair(ImGuiFullscreen::IsGamepadInputSource() ? ICON_PF_BUTTON_B : ICON_PF_ESC,
                              TRANSLATE_SV("Achievements", "Back"))});

  if (close_window)
    FullscreenUI::ReturnToPreviousWindow();
}

void Achievements::DrawAchievement(const rc_client_achievement_t* cheevo)
{
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::LayoutUnscale;
  using ImGuiFullscreen::UIStyle;

  static constexpr float alpha = 0.8f;
  static constexpr float progress_height_unscaled = 20.0f;
  static constexpr float progress_spacing_unscaled = 5.0f;

  const float spacing = ImGuiFullscreen::LayoutScale(4.0f);

  const bool is_unlocked = (cheevo->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  const std::string_view measured_progress(cheevo->measured_progress);
  const bool is_measured = !is_unlocked && !measured_progress.empty();
  const float unlock_size = is_unlocked ? (spacing + ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE) : 0.0f;
  const ImVec2 points_template_size(UIStyle.MediumFont->CalcTextSizeA(UIStyle.MediumFont->FontSize, FLT_MAX, 0.0f,
                                                                      TRANSLATE("Achievements", "XXX points")));

  const size_t summary_length = std::strlen(cheevo->description);
  const float summary_wrap_width =
    (ImGui::GetCurrentWindow()->WorkRect.GetWidth() - (ImGui::GetStyle().FramePadding.x * 2.0f) -
     LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT + 30.0f) - points_template_size.x);
  const ImVec2 summary_text_size(UIStyle.MediumFont->CalcTextSizeA(UIStyle.MediumFont->FontSize, FLT_MAX,
                                                                   summary_wrap_width, cheevo->description,
                                                                   cheevo->description + summary_length));

  // Messy, but need to undo LayoutScale in MenuButtonFrame()...
  const float extra_summary_height = LayoutUnscale(std::max(summary_text_size.y - UIStyle.MediumFont->FontSize, 0.0f));

  ImRect bb;
  bool visible, hovered;
  const bool clicked = ImGuiFullscreen::MenuButtonFrame(
    TinyString::from_format("chv_{}", cheevo->id), true,
    !is_measured ? ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT + extra_summary_height + unlock_size :
                   ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT + extra_summary_height + progress_height_unscaled +
                     progress_spacing_unscaled,
    &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  std::string* badge_path;
  if (const auto badge_it = std::find_if(s_state.achievement_badge_paths.begin(), s_state.achievement_badge_paths.end(),
                                         [cheevo](const auto& it) { return (it.first == cheevo); });
      badge_it != s_state.achievement_badge_paths.end())
  {
    badge_path = &badge_it->second;
  }
  else
  {
    std::string new_badge_path = Achievements::GetAchievementBadgePath(cheevo, cheevo->state);
    badge_path = &s_state.achievement_badge_paths.emplace_back(cheevo, std::move(new_badge_path)).second;
  }

  const ImVec2 image_size(
    LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT));
  if (!badge_path->empty())
  {
    GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(*badge_path);
    if (badge)
    {
      ImGui::GetWindowDrawList()->AddImage(badge, bb.Min, bb.Min + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                           IM_COL32(255, 255, 255, 255));
    }
  }

  SmallString text;

  const float midpoint = bb.Min.y + UIStyle.LargeFont->FontSize + spacing;
  text = TRANSLATE_PLURAL_SSTR("Achievements", "%n points", "Achievement points", cheevo->points);
  const ImVec2 points_size(
    UIStyle.MediumFont->CalcTextSizeA(UIStyle.MediumFont->FontSize, FLT_MAX, 0.0f, text.c_str(), text.end_ptr()));
  const float points_template_start = bb.Max.x - points_template_size.x;
  const float points_start = points_template_start + ((points_template_size.x - points_size.x) * 0.5f);

  const char* right_icon_text;
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

  const ImVec2 right_icon_size(
    UIStyle.LargeFont->CalcTextSizeA(UIStyle.LargeFont->FontSize, FLT_MAX, 0.0f, right_icon_text));

  const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(points_start, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint),
                          ImVec2(points_start, midpoint + UIStyle.MediumFont->FontSize + extra_summary_height));
  const ImRect points_bb(ImVec2(points_start, midpoint), bb.Max);
  const ImRect lock_bb(ImVec2(points_template_start + ((points_template_size.x - right_icon_size.x) * 0.5f), bb.Min.y),
                       ImVec2(bb.Max.x, midpoint));

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, cheevo->title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::RenderTextClipped(lock_bb.Min, lock_bb.Max, right_icon_text, nullptr, &right_icon_size, ImVec2(0.0f, 0.0f),
                           &lock_bb);
  ImGui::PopFont();

  ImGui::PushFont(UIStyle.MediumFont);
  if (cheevo->description && summary_length > 0)
  {
    ImGui::RenderTextWrapped(summary_bb.Min, cheevo->description, cheevo->description + summary_length,
                             summary_wrap_width);
  }
  ImGui::RenderTextClipped(points_bb.Min, points_bb.Max, text.c_str(), text.end_ptr(), &points_size, ImVec2(0.0f, 0.0f),
                           &points_bb);

  if (is_unlocked)
  {
    TinyString date;
    FullscreenUI::TimeToPrintableString(&date, cheevo->unlock_time);
    text.format(TRANSLATE_FS("Achievements", "Unlocked: {}"), date);

    const ImRect unlock_bb(summary_bb.Min.x, summary_bb.Max.y + spacing, summary_bb.Max.x, bb.Max.y);
    ImGui::RenderTextClipped(unlock_bb.Min, unlock_bb.Max, text.c_str(), text.end_ptr(), nullptr, ImVec2(0.0f, 0.0f),
                             &unlock_bb);
  }
  else if (is_measured)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float progress_height = LayoutScale(progress_height_unscaled);
    const float progress_spacing = LayoutScale(progress_spacing_unscaled);
    const float top = midpoint + UIStyle.MediumFont->FontSize + progress_spacing;
    const ImRect progress_bb(ImVec2(text_start_x, top), ImVec2(bb.Max.x, top + progress_height));
    const float fraction = cheevo->measured_percent * 0.01f;
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIStyle.PrimaryDarkColor));
    dl->AddRectFilled(progress_bb.Min, ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                      ImGui::GetColorU32(ImGuiFullscreen::UIStyle.SecondaryColor));

    const ImVec2 text_size =
      ImGui::CalcTextSize(measured_progress.data(), measured_progress.data() + measured_progress.size());
    const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
    dl->AddText(UIStyle.MediumFont, UIStyle.MediumFont->FontSize, text_pos,
                ImGui::GetColorU32(ImGuiFullscreen::UIStyle.PrimaryTextColor), measured_progress.data(),
                measured_progress.data() + measured_progress.size());
  }

  if (clicked)
  {
    const SmallString url = SmallString::from_format(fmt::runtime(ACHEIVEMENT_DETAILS_URL_TEMPLATE), cheevo->id);
    INFO_LOG("Opening achievement details: {}", url);
    Host::OpenURL(url);
  }

  ImGui::PopFont();
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
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::UIStyle;

  static constexpr float alpha = 0.8f;
  static constexpr float heading_alpha = 0.95f;
  static constexpr float heading_height_unscaled = 110.0f;
  static constexpr float tab_height_unscaled = 50.0f;

  auto lock = Achievements::GetLock();

  const bool is_leaderboard_open = (s_state.open_leaderboard != nullptr);
  bool close_leaderboard_on_exit = false;

  ImRect bb;

  const ImVec4 background = ImGuiFullscreen::ModAlpha(ImGuiFullscreen::UIStyle.BackgroundColor, alpha);
  const ImVec4 heading_background = ImGuiFullscreen::ModAlpha(ImGuiFullscreen::UIStyle.BackgroundColor, heading_alpha);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
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
    UIStyle.LargeFont->CalcTextSizeA(UIStyle.LargeFont->FontSize, std::numeric_limits<float>::max(), -1.0f, "99999").x;
  const float name_column_width =
    UIStyle.LargeFont
      ->CalcTextSizeA(UIStyle.LargeFont->FontSize, std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWWWWWWWWWWWWW")
      .x;
  const float time_column_width =
    UIStyle.LargeFont
      ->CalcTextSizeA(UIStyle.LargeFont->FontSize, std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWW")
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
        GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(s_state.game_icon.c_str());
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge, icon_min, icon_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                               IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      SmallString text;

      if (!is_leaderboard_open)
      {
        if (ImGuiFullscreen::FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true,
                                            UIStyle.LargeFont) ||
            ImGuiFullscreen::WantsToCloseMenu())
        {
          FullscreenUI::ReturnToPreviousWindow();
        }
      }
      else
      {
        if (ImGuiFullscreen::FloatingButton(ICON_FA_CARET_SQUARE_LEFT, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true,
                                            UIStyle.LargeFont) ||
            ImGuiFullscreen::WantsToCloseMenu())
        {
          close_leaderboard_on_exit = true;
        }
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFont->FontSize));
      text.assign(Achievements::GetGameTitle());

      top += UIStyle.LargeFont->FontSize + spacing;

      ImGui::PushFont(UIStyle.LargeFont);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.c_str(), text.end_ptr(), nullptr, ImVec2(0.0f, 0.0f),
                               &title_bb);
      ImGui::PopFont();

      if (is_leaderboard_open)
      {
        const ImRect subtitle_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFont->FontSize));
        text.assign(s_state.open_leaderboard->title);

        top += UIStyle.LargeFont->FontSize + spacing_small;

        ImGui::PushFont(UIStyle.LargeFont);
        ImGui::RenderTextClipped(subtitle_bb.Min, subtitle_bb.Max, text.c_str(), text.end_ptr(), nullptr,
                                 ImVec2(0.0f, 0.0f), &subtitle_bb);
        ImGui::PopFont();

        text.assign(s_state.open_leaderboard->description);
      }
      else
      {
        u32 count = 0;
        for (u32 i = 0; i < s_state.leaderboard_list->num_buckets; i++)
          count += s_state.leaderboard_list->buckets[i].num_leaderboards;
        text = TRANSLATE_PLURAL_SSTR("Achievements", "This game has %n leaderboards.", "Leaderboard count", count);
      }

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFont->FontSize));
      top += UIStyle.MediumFont->FontSize + spacing_small;

      ImGui::PushFont(UIStyle.MediumFont);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.c_str(), text.end_ptr(), nullptr,
                               ImVec2(0.0f, 0.0f), &summary_bb);

      if (!is_leaderboard_open && !Achievements::IsHardcoreModeActive())
      {
        const ImRect hardcore_warning_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFont->FontSize));
        top += UIStyle.MediumFont->FontSize + spacing_small;

        ImGui::RenderTextClipped(
          hardcore_warning_bb.Min, hardcore_warning_bb.Max,
          TRANSLATE("Achievements",
                    "Submitting scores is disabled because hardcore mode is off. Leaderboards are read-only."),
          nullptr, nullptr, ImVec2(0.0f, 0.0f), &hardcore_warning_bb);
      }

      ImGui::PopFont();

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
          ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::Other);
        }

        for (const bool show_all : {false, true})
        {
          const char* title =
            show_all ? TRANSLATE("Achievements", "Show Best") : TRANSLATE("Achievements", "Show Nearby");
          if (ImGuiFullscreen::NavTab(title, s_state.is_showing_all_leaderboard_entries == show_all, true, tab_width,
                                      tab_height_unscaled, heading_background))
          {
            s_state.is_showing_all_leaderboard_entries = show_all;
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

        const float midpoint = bb.Min.y + UIStyle.LargeFont->FontSize + LayoutScale(4.0f);
        float text_start_x = bb.Min.x + LayoutScale(15.0f) + padding;

        ImGui::PushFont(UIStyle.LargeFont);

        const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(rank_bb.Min, rank_bb.Max, TRANSLATE("Achievements", "Rank"), nullptr, nullptr,
                                 ImVec2(0.0f, 0.0f), &rank_bb);
        text_start_x += rank_column_width + column_spacing;

        const ImRect user_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(user_bb.Min, user_bb.Max, TRANSLATE("Achievements", "Name"), nullptr, nullptr,
                                 ImVec2(0.0f, 0.0f), &user_bb);
        text_start_x += name_column_width + column_spacing;

        static const char* value_headings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
          TRANSLATE_NOOP("Achievements", "Time"),
          TRANSLATE_NOOP("Achievements", "Score"),
          TRANSLATE_NOOP("Achievements", "Value"),
        };

        const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(
          score_bb.Min, score_bb.Max,
          Host::TranslateToCString(
            "Achievements",
            value_headings[std::min<u8>(s_state.open_leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)]),
          nullptr, nullptr, ImVec2(0.0f, 0.0f), &score_bb);
        text_start_x += time_column_width + column_spacing;

        const ImRect date_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(date_bb.Min, date_bb.Max, TRANSLATE("Achievements", "Date Submitted"), nullptr,
                                 nullptr, ImVec2(0.0f, 0.0f), &date_bb);

        ImGui::PopFont();

        const float line_thickness = LayoutScale(1.0f);
        const float line_padding = LayoutScale(5.0f);
        const ImVec2 line_start(bb.Min.x, bb.Min.y + UIStyle.LargeFont->FontSize + line_padding);
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
          "leaderboards", background, 0.0f, ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, 0.0f), 0))
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
                                TRANSLATE_SV("Achievements", "Back"))});
  }
  else
  {
    if (ImGuiFullscreen::BeginFullscreenWindow(
          ImVec2(0.0f, heading_height),
          ImVec2(display_size.x, display_size.y - heading_height - LayoutScale(ImGuiFullscreen::LAYOUT_FOOTER_HEIGHT)),
          "leaderboard", background, 0.0f, ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, 0.0f), 0))
    {
      // Defer focus reset until loading finishes.
      if (!s_state.is_showing_all_leaderboard_entries ||
          (ImGuiFullscreen::IsFocusResetFromWindowChange() && !s_state.leaderboard_entry_lists.empty()))
      {
        ImGuiFullscreen::ResetFocusHere();
      }

      ImGuiFullscreen::BeginMenuButtons();

      if (!s_state.is_showing_all_leaderboard_entries)
      {
        if (s_state.leaderboard_nearby_entries)
        {
          for (u32 i = 0; i < s_state.leaderboard_nearby_entries->num_entries; i++)
          {
            DrawLeaderboardEntry(s_state.leaderboard_nearby_entries->entries[i],
                                 static_cast<s32>(i) == s_state.leaderboard_nearby_entries->user_index,
                                 rank_column_width, name_column_width, time_column_width, column_spacing);
          }
        }
        else
        {
          ImGui::PushFont(UIStyle.LargeFont);

          const ImVec2 pos_min(0.0f, heading_height);
          const ImVec2 pos_max(display_size.x, display_size.y);
          ImGui::RenderTextClipped(pos_min, pos_max,
                                   TRANSLATE("Achievements", "Downloading leaderboard data, please wait..."), nullptr,
                                   nullptr, ImVec2(0.5f, 0.5f));

          ImGui::PopFont();
        }
      }
      else
      {
        for (const rc_client_leaderboard_entry_list_t* list : s_state.leaderboard_entry_lists)
        {
          for (u32 i = 0; i < list->num_entries; i++)
          {
            DrawLeaderboardEntry(list->entries[i], static_cast<s32>(i) == list->user_index, rank_column_width,
                                 name_column_width, time_column_width, column_spacing);
          }
        }

        // Fetch next chunk if the loading indicator becomes visible (i.e. we scrolled enough).
        bool visible, hovered;
        ImGuiFullscreen::MenuButtonFrame(TRANSLATE("Achievements", "Loading..."), false,
                                         ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible, &hovered,
                                         &bb.Min, &bb.Max);
        if (visible)
        {
          const float midpoint = bb.Min.y + UIStyle.LargeFont->FontSize + LayoutScale(4.0f);
          const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));

          ImGui::PushFont(UIStyle.LargeFont);
          ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, TRANSLATE("Achievements", "Loading..."), nullptr,
                                   nullptr, ImVec2(0, 0), &title_bb);
          ImGui::PopFont();

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
                                TRANSLATE_SV("Achievements", "Back"))});
  }

  if (close_leaderboard_on_exit)
    CloseLeaderboard();
}

void Achievements::DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, bool is_self,
                                        float rank_column_width, float name_column_width, float time_column_width,
                                        float column_spacing)
{
  using ImGuiFullscreen::LayoutScale;
  using ImGuiFullscreen::UIStyle;

  static constexpr float alpha = 0.8f;

  ImRect bb;
  bool visible, hovered;
  bool pressed =
    ImGuiFullscreen::MenuButtonFrame(entry.user, true, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible,
                                     &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + UIStyle.LargeFont->FontSize + LayoutScale(4.0f);
  float text_start_x = bb.Min.x + LayoutScale(15.0f);
  SmallString text;

  text.format("{}", entry.rank);

  ImGui::PushFont(UIStyle.LargeFont);

  if (is_self)
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 242, 0, 255));

  const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(rank_bb.Min, rank_bb.Max, text.c_str(), text.end_ptr(), nullptr, ImVec2(0.0f, 0.0f),
                           &rank_bb);
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
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(icon_tex), icon_bb.Min,
                                         icon_bb.Min + ImVec2(icon_size, icon_size));
  }

  const ImRect user_bb(ImVec2(text_start_x + column_spacing + icon_size, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(user_bb.Min, user_bb.Max, entry.user, nullptr, nullptr, ImVec2(0.0f, 0.0f), &user_bb);
  text_start_x += name_column_width + column_spacing;

  const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(score_bb.Min, score_bb.Max, entry.display, nullptr, nullptr, ImVec2(0.0f, 0.0f), &score_bb);
  text_start_x += time_column_width + column_spacing;

  const ImRect time_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  SmallString submit_time;
  FullscreenUI::TimeToPrintableString(&submit_time, entry.submitted);
  ImGui::RenderTextClipped(time_bb.Min, time_bb.Max, submit_time.c_str(), submit_time.end_ptr(), nullptr,
                           ImVec2(0.0f, 0.0f), &time_bb);

  if (is_self)
    ImGui::PopStyleColor();

  ImGui::PopFont();

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

  const float midpoint = bb.Min.y + UIStyle.LargeFont->FontSize + LayoutScale(4.0f);
  const float text_start_x = bb.Min.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

  ImGui::PushFont(UIStyle.LargeFont);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, lboard->title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (lboard->description && lboard->description[0] != '\0')
  {
    ImGui::PushFont(UIStyle.MediumFont);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, lboard->description, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (pressed)
    OpenLeaderboard(lboard);
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
  ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::Other);
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

#ifdef ENABLE_RAINTEGRATION

#include "RA_Consoles.h"

bool Achievements::IsUsingRAIntegration()
{
  return s_state.using_raintegration;
}

namespace Achievements::RAIntegration {
static void InitializeRAIntegration(void* main_window_handle);

static int RACallbackIsActive();
static void RACallbackCauseUnpause();
static void RACallbackCausePause();
static void RACallbackRebuildMenu();
static void RACallbackEstimateTitle(char* buf);
static void RACallbackResetEmulator();
static void RACallbackLoadROM(const char* unused);
static unsigned char RACallbackReadRAM(unsigned int address);
static unsigned int RACallbackReadRAMBlock(unsigned int nAddress, unsigned char* pBuffer, unsigned int nBytes);
static void RACallbackWriteRAM(unsigned int address, unsigned char value);
static unsigned char RACallbackReadScratchpad(unsigned int address);
static unsigned int RACallbackReadScratchpadBlock(unsigned int nAddress, unsigned char* pBuffer, unsigned int nBytes);
static void RACallbackWriteScratchpad(unsigned int address, unsigned char value);

static bool s_raintegration_initialized = false;
} // namespace Achievements::RAIntegration

void Achievements::SwitchToRAIntegration()
{
  s_state.using_raintegration = true;
}

void Achievements::RAIntegration::InitializeRAIntegration(void* main_window_handle)
{
  RA_InitClient((HWND)main_window_handle, "DuckStation", g_scm_tag_str);
  RA_SetUserAgentDetail(Host::GetHTTPUserAgent().c_str());

  RA_InstallSharedFunctions(RACallbackIsActive, RACallbackCauseUnpause, RACallbackCausePause, RACallbackRebuildMenu,
                            RACallbackEstimateTitle, RACallbackResetEmulator, RACallbackLoadROM);
  RA_SetConsoleID(PlayStation);

  // Apparently this has to be done early, or the memory inspector doesn't work.
  // That's a bit unfortunate, because the RAM size can vary between games, and depending on the option.
  RA_InstallMemoryBank(0, RACallbackReadRAM, RACallbackWriteRAM, Bus::RAM_2MB_SIZE);
  RA_InstallMemoryBankBlockReader(0, RACallbackReadRAMBlock);
  RA_InstallMemoryBank(1, RACallbackReadScratchpad, RACallbackWriteScratchpad, CPU::SCRATCHPAD_SIZE);
  RA_InstallMemoryBankBlockReader(1, RACallbackReadScratchpadBlock);

  // Fire off a login anyway. Saves going into the menu and doing it.
  RA_AttemptLogin(0);

  s_raintegration_initialized = true;

  // this is pretty lame, but we may as well persist until we exit anyway
  std::atexit(RA_Shutdown);
}

void Achievements::RAIntegration::MainWindowChanged(void* new_handle)
{
  if (s_raintegration_initialized)
  {
    RA_UpdateHWnd((HWND)new_handle);
    return;
  }

  InitializeRAIntegration(new_handle);
}

void Achievements::RAIntegration::GameChanged()
{
  s_state.game_id = s_state.game_hash.empty() ? 0 : RA_IdentifyHash(s_state.game_hash.c_str());
  RA_ActivateGame(s_state.game_id);
}

std::vector<std::tuple<int, std::string, bool>> Achievements::RAIntegration::GetMenuItems()
{
  std::array<RA_MenuItem, 64> items;
  const int num_items = RA_GetPopupMenuItems(items.data());

  std::vector<std::tuple<int, std::string, bool>> ret;
  ret.reserve(static_cast<u32>(num_items));

  for (int i = 0; i < num_items; i++)
  {
    const RA_MenuItem& it = items[i];
    if (!it.sLabel)
      ret.emplace_back(0, std::string(), false);
    else
      ret.emplace_back(static_cast<int>(it.nID), StringUtil::WideStringToUTF8String(it.sLabel), it.bChecked);
  }

  return ret;
}

void Achievements::RAIntegration::ActivateMenuItem(int item)
{
  RA_InvokeDialog(item);
}

int Achievements::RAIntegration::RACallbackIsActive()
{
  return static_cast<int>(HasActiveGame());
}

void Achievements::RAIntegration::RACallbackCauseUnpause()
{
  Host::RunOnCPUThread([]() { System::PauseSystem(false); });
}

void Achievements::RAIntegration::RACallbackCausePause()
{
  Host::RunOnCPUThread([]() { System::PauseSystem(true); });
}

void Achievements::RAIntegration::RACallbackRebuildMenu()
{
  // unused, we build the menu on demand
}

void Achievements::RAIntegration::RACallbackEstimateTitle(char* buf)
{
  StringUtil::Strlcpy(buf, System::GetGameTitle(), 256);
}

void Achievements::RAIntegration::RACallbackResetEmulator()
{
  if (System::IsValid())
    System::ResetSystem();
}

void Achievements::RAIntegration::RACallbackLoadROM(const char* unused)
{
  // unused
  UNREFERENCED_PARAMETER(unused);
}

unsigned char Achievements::RAIntegration::RACallbackReadRAM(unsigned int address)
{
  if (!System::IsValid())
    return 0;

  u8 value = 0;
  CPU::SafeReadMemoryByte(address, &value);
  return value;
}

void Achievements::RAIntegration::RACallbackWriteRAM(unsigned int address, unsigned char value)
{
  CPU::SafeWriteMemoryByte(address, value);
}

unsigned int Achievements::RAIntegration::RACallbackReadRAMBlock(unsigned int nAddress, unsigned char* pBuffer,
                                                                 unsigned int nBytes)
{
  if (nAddress >= Bus::g_ram_size)
    return 0;

  const u32 copy_size = std::min<u32>(Bus::g_ram_size - nAddress, nBytes);
  std::memcpy(pBuffer, Bus::g_unprotected_ram + nAddress, copy_size);
  return copy_size;
}

unsigned char Achievements::RAIntegration::RACallbackReadScratchpad(unsigned int address)
{
  if (!System::IsValid() || address >= CPU::SCRATCHPAD_SIZE)
    return 0;

  return CPU::g_state.scratchpad[address];
}

void Achievements::RAIntegration::RACallbackWriteScratchpad(unsigned int address, unsigned char value)
{
  if (address >= CPU::SCRATCHPAD_SIZE)
    return;

  CPU::g_state.scratchpad[address] = value;
}

unsigned int Achievements::RAIntegration::RACallbackReadScratchpadBlock(unsigned int nAddress, unsigned char* pBuffer,
                                                                        unsigned int nBytes)
{
  if (nAddress >= CPU::SCRATCHPAD_SIZE)
    return 0;

  const u32 copy_size = std::min<u32>(CPU::SCRATCHPAD_SIZE - nAddress, nBytes);
  std::memcpy(pBuffer, &CPU::g_state.scratchpad[nAddress], copy_size);
  return copy_size;
}

#else

bool Achievements::IsUsingRAIntegration()
{
  return false;
}

#endif

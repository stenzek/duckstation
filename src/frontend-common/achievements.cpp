#include "achievements.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/http_downloader.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"
#include "common/platform.h"
#include "common/string_util.h"
#include "core/bios.h"
#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/host.h"
#include "core/host_display.h"
#include "core/host_settings.h"
#include "core/system.h"
#include "fullscreen_ui.h"
#include "imgui_fullscreen.h"
#include "rapidjson/document.h"
#include "rc_api_info.h"
#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_url.h"
#include "rcheevos.h"
#include "scmversion/scmversion.h"
#include "util/cd_image.h"
#include "util/state_wrapper.h"
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
Log_SetChannel(Achievements);

#ifdef WITH_RAINTEGRATION
// RA_Interface ends up including windows.h, with its silly macros.
#ifdef _WIN32
#include "common/windows_headers.h"
#endif
#include "RA_Interface.h"
#endif
namespace Achievements {
enum : s32
{
  HTTP_OK = Common::HTTPDownloader::HTTP_OK,

  // Number of seconds between rich presence pings. RAIntegration uses 2 minutes.
  RICH_PRESENCE_PING_FREQUENCY = 2 * 60,
  NO_RICH_PRESENCE_PING_FREQUENCY = RICH_PRESENCE_PING_FREQUENCY * 2,
};

static void FormattedError(const char* format, ...);
static void LogFailedResponseJSON(const Common::HTTPDownloader::Request::Data& data);
static void EnsureCacheDirectoriesExist();
static void CheevosEventHandler(const rc_runtime_event_t* runtime_event);
static unsigned PeekMemory(unsigned address, unsigned num_bytes, void* ud);
static void ActivateLockedAchievements();
static bool ActivateAchievement(Achievement* achievement);
static void DeactivateAchievement(Achievement* achievement);
static void SendPing();
static void SendPlaying();
static void UpdateRichPresence();
static Achievement* GetAchievementByID(u32 id);
static void ClearGameInfo(bool clear_achievements = true, bool clear_leaderboards = true);
static void ClearGameHash();
static std::string GetUserAgent();
static void LoginCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void LoginASyncCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void SendLogin(const char* username, const char* password, Common::HTTPDownloader* http_downloader,
                      Common::HTTPDownloader::Request::Callback callback);
static void DownloadImage(std::string url, std::string cache_filename);
static void DisplayAchievementSummary();
static void GetUserUnlocksCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void GetUserUnlocks();
static void GetPatchesCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void GetLbInfoCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void GetPatches(u32 game_id);
static std::string GetGameHash(CDImage* image);
static void SetChallengeMode(bool enabled);
static void GetGameIdCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void SendPlayingCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void UpdateRichPresence();
static void SendPingCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void UnlockAchievementCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);
static void SubmitLeaderboardCallback(s32 status_code, Common::HTTPDownloader::Request::Data data);

static bool s_active = false;
static bool s_logged_in = false;
static bool s_challenge_mode = false;
static u32 s_game_id = 0;

#ifdef WITH_RAINTEGRATION
static bool s_using_raintegration = false;
#endif

static std::recursive_mutex s_achievements_mutex;
static rc_runtime_t s_rcheevos_runtime;
static std::string s_game_icon_cache_directory;
static std::string s_achievement_icon_cache_directory;
static std::unique_ptr<Common::HTTPDownloader> s_http_downloader;

static std::string s_username;
static std::string s_api_token;

static std::string s_game_path;
static std::string s_game_hash;
static std::string s_game_title;
static std::string s_game_icon;
static std::vector<Achievements::Achievement> s_achievements;
static std::vector<Achievements::Leaderboard> s_leaderboards;

static bool s_has_rich_presence = false;
static std::string s_rich_presence_string;
static Common::Timer s_last_ping_time;

static u32 s_last_queried_lboard = 0;
static u32 s_submitting_lboard_id = 0;
static std::optional<std::vector<Achievements::LeaderboardEntry>> s_lboard_entries;

template<typename T>
static const char* RAPIStructName();

#define RAPI_STRUCT_NAME(x)                                                                                            \
  template<>                                                                                                           \
  const char* RAPIStructName<x>()                                                                                      \
  {                                                                                                                    \
    return #x;                                                                                                         \
  }

RAPI_STRUCT_NAME(rc_api_login_request_t);
RAPI_STRUCT_NAME(rc_api_fetch_image_request_t);
RAPI_STRUCT_NAME(rc_api_resolve_hash_request_t);
RAPI_STRUCT_NAME(rc_api_fetch_game_data_request_t);
RAPI_STRUCT_NAME(rc_api_fetch_user_unlocks_request_t);
RAPI_STRUCT_NAME(rc_api_start_session_request_t);
RAPI_STRUCT_NAME(rc_api_ping_request_t);
RAPI_STRUCT_NAME(rc_api_award_achievement_request_t);
RAPI_STRUCT_NAME(rc_api_submit_lboard_entry_request_t);
RAPI_STRUCT_NAME(rc_api_fetch_leaderboard_info_request_t);

RAPI_STRUCT_NAME(rc_api_login_response_t);
RAPI_STRUCT_NAME(rc_api_resolve_hash_response_t);
RAPI_STRUCT_NAME(rc_api_fetch_game_data_response_t);
RAPI_STRUCT_NAME(rc_api_ping_response_t);
RAPI_STRUCT_NAME(rc_api_award_achievement_response_t);
RAPI_STRUCT_NAME(rc_api_submit_lboard_entry_response_t);
RAPI_STRUCT_NAME(rc_api_start_session_response_t);
RAPI_STRUCT_NAME(rc_api_fetch_user_unlocks_response_t);
RAPI_STRUCT_NAME(rc_api_fetch_leaderboard_info_response_t);

// Unused for now.
// RAPI_STRUCT_NAME(rc_api_fetch_achievement_info_response_t);
// RAPI_STRUCT_NAME(rc_api_fetch_games_list_response_t);

#undef RAPI_STRUCT_NAME

template<typename T, int (*InitFunc)(rc_api_request_t*, const T*)>
struct RAPIRequest : public T
{
private:
  rc_api_request_t api_request;

public:
  RAPIRequest() { std::memset(this, 0, sizeof(*this)); }

  ~RAPIRequest() { rc_api_destroy_request(&api_request); }

  void Send(Common::HTTPDownloader::Request::Callback callback) { Send(s_http_downloader.get(), std::move(callback)); }

  void Send(Common::HTTPDownloader* http_downloader, Common::HTTPDownloader::Request::Callback callback)
  {
    const int error = InitFunc(&api_request, this);
    if (error != RC_OK)
    {
      FormattedError("%s failed: error %d (%s)", RAPIStructName<T>(), error, rc_error_str(error));
      callback(-1, Common::HTTPDownloader::Request::Data());
      return;
    }

    if (api_request.post_data)
    {
      // needs to be a post
      http_downloader->CreatePostRequest(api_request.url, api_request.post_data, std::move(callback));
    }
    else
    {
      // get is fine
      http_downloader->CreateRequest(api_request.url, std::move(callback));
    }
  }

  bool DownloadImage(std::string cache_filename)
  {
    const int error = InitFunc(&api_request, this);
    if (error != RC_OK)
    {
      FormattedError("%s failed: error %d (%s)", RAPIStructName<T>(), error, rc_error_str(error));
      return false;
    }

    DebugAssertMsg(!api_request.post_data, "Download request does not have POST data");
    Achievements::DownloadImage(api_request.url, std::move(cache_filename));
    return true;
  }
};

template<typename T, int (*ParseFunc)(T*, const char*), void (*DestroyFunc)(T*)>
struct RAPIResponse : public T
{
private:
  bool initialized = false;

public:
  RAPIResponse(s32 status_code, Common::HTTPDownloader::Request::Data& data)
  {
    if (status_code != Common::HTTPDownloader::HTTP_OK || data.empty())
    {
      FormattedError("%s failed: empty response and/or status code %d", RAPIStructName<T>(), status_code);
      LogFailedResponseJSON(data);
      return;
    }

    // ensure null termination, rapi needs it
    data.push_back(0);

    const int error = ParseFunc(this, reinterpret_cast<const char*>(data.data()));
    initialized = true;

    const rc_api_response_t& response = static_cast<T*>(this)->response;
    if (error != RC_OK)
    {
      FormattedError("%s failed: parse function returned %d (%s)", RAPIStructName<T>(), error, rc_error_str(error));
      LogFailedResponseJSON(data);
    }
    else if (!response.succeeded)
    {
      FormattedError("%s failed: %s", RAPIStructName<T>(),
                     response.error_message ? response.error_message : "<no error>");
      LogFailedResponseJSON(data);
    }
  }

  ~RAPIResponse()
  {
    if (initialized)
      DestroyFunc(this);
  }

  operator bool() const { return initialized && static_cast<const T*>(this)->response.succeeded; }
};

} // namespace Achievements

#ifdef WITH_RAINTEGRATION
bool Achievements::IsUsingRAIntegration()
{
  return s_using_raintegration;
}
#endif

void Achievements::FormattedError(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string error(fmt::format("Achievements error: {}", StringUtil::StdStringFromFormatV(format, ap)));
  va_end(ap);

  Log_ErrorPrint(error.c_str());
  Host::AddOSDMessage(std::move(error), 10.0f);
}

void Achievements::LogFailedResponseJSON(const Common::HTTPDownloader::Request::Data& data)
{
  const std::string str_data(reinterpret_cast<const char*>(data.data()), data.size());
  Log_ErrorPrintf("API call failed. Response JSON was:\n%s", str_data.c_str());
}

static Achievements::Achievement* Achievements::GetAchievementByID(u32 id)
{
  for (Achievement& ach : s_achievements)
  {
    if (ach.id == id)
      return &ach;
  }

  return nullptr;
}

void Achievements::ClearGameInfo(bool clear_achievements, bool clear_leaderboards)
{
  const bool had_game = (s_game_id != 0);

  if (clear_achievements)
  {
    while (!s_achievements.empty())
    {
      Achievement& ach = s_achievements.back();
      DeactivateAchievement(&ach);
      s_achievements.pop_back();
    }
  }
  if (clear_leaderboards)
  {
    while (!s_leaderboards.empty())
    {
      Leaderboard& lb = s_leaderboards.back();
      rc_runtime_deactivate_lboard(&s_rcheevos_runtime, lb.id);
      s_leaderboards.pop_back();
    }

    s_last_queried_lboard = 0;
    s_submitting_lboard_id = 0;
    s_lboard_entries.reset();
  }

  if (s_achievements.empty() && s_leaderboards.empty())
  {
    // Ready to tear down cheevos completely
    s_game_title = {};
    s_game_icon = {};
    s_rich_presence_string = {};
    s_has_rich_presence = false;
    s_game_id = 0;
  }

  if (had_game)
    Host::OnAchievementsRefreshed();
}

void Achievements::ClearGameHash()
{
  s_game_path = {};
  std::string().swap(s_game_hash);
}

std::string Achievements::GetUserAgent()
{
#if 0
  return fmt::format("DuckStation for {} ({}) {}", SYSTEM_STR, CPU_ARCH_STR, g_scm_tag_str);
#else
  return "DuckStation";
#endif
}

bool Achievements::IsActive()
{
  return s_active;
}

bool Achievements::IsLoggedIn()
{
  return s_logged_in;
}

bool Achievements::ChallengeModeActive()
{
#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
    return RA_HardcoreModeIsActive() != 0;
#endif

  return s_challenge_mode;
}

bool Achievements::IsTestModeActive()
{
  return g_settings.achievements_test_mode;
}

bool Achievements::IsUnofficialTestModeActive()
{
  return g_settings.achievements_unofficial_test_mode;
}

bool Achievements::IsRichPresenceEnabled()
{
  return g_settings.achievements_rich_presence;
}

bool Achievements::HasActiveGame()
{
  return s_game_id != 0;
}

u32 Achievements::GetGameID()
{
  return s_game_id;
}

std::unique_lock<std::recursive_mutex> Achievements::GetLock()
{
  return std::unique_lock(s_achievements_mutex);
}

void Achievements::Initialize()
{
  if (IsUsingRAIntegration())
    return;

  std::unique_lock lock(s_achievements_mutex);
  AssertMsg(g_settings.achievements_enabled, "Achievements are enabled");

  s_http_downloader = Common::HTTPDownloader::Create(GetUserAgent().c_str());
  if (!s_http_downloader)
  {
    Host::ReportErrorAsync("Achievements Error", "Failed to create HTTPDownloader, cannot use achievements");
    return;
  }

  s_active = true;
  s_challenge_mode = false;
  rc_runtime_init(&s_rcheevos_runtime);
  EnsureCacheDirectoriesExist();

  s_last_ping_time.Reset();
  s_username = Host::GetBaseStringSettingValue("Cheevos", "Username");
  s_api_token = Host::GetBaseStringSettingValue("Cheevos", "Token");
  s_logged_in = (!s_username.empty() && !s_api_token.empty());

  if (IsLoggedIn() && System::IsValid())
    GameChanged(System::GetRunningPath(), nullptr);
}

void Achievements::UpdateSettings(const Settings& old_config)
{
  if (IsUsingRAIntegration())
    return;

  if (!g_settings.achievements_enabled)
  {
    // we're done here
    Shutdown();
    return;
  }

  if (!s_active)
  {
    // we just got enabled
    Initialize();
    return;
  }

  if (g_settings.achievements_challenge_mode != old_config.achievements_challenge_mode)
  {
    // Hardcore mode can only be enabled through reset (ResetChallengeMode()).
    if (s_challenge_mode && !old_config.achievements_challenge_mode)
    {
      ResetChallengeMode();
    }
    else if (g_settings.achievements_challenge_mode)
    {
      Host::AddKeyedOSDMessage(
        "challenge_mode_reset",
        Host::TranslateStdString("Achievements", "Hardcore mode will be enabled on system reset."), 10.0f);
    }
  }

  // FIXME: Handle changes to various settings individually
  if (g_settings.achievements_test_mode != old_config.achievements_test_mode ||
      g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode ||
      g_settings.achievements_use_first_disc_from_playlist != old_config.achievements_use_first_disc_from_playlist ||
      g_settings.achievements_rich_presence != old_config.achievements_rich_presence)
  {
    Shutdown();
    Initialize();
    return;
  }

  // in case cache directory changed
  EnsureCacheDirectoriesExist();
}

bool Achievements::ConfirmChallengeModeDisable(const char* trigger)
{
#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
    return (RA_WarnDisableHardcore(trigger) != 0);
#endif

  // I really hope this doesn't deadlock :/
  const bool confirmed = Host::ConfirmMessage(
    Host::TranslateString("Achievements", "Confirm Hardcore Mode"),
    fmt::format(Host::TranslateString("Achievements",
                                      "{0} cannot be performed while hardcore mode is active. Do you "
                                      "want to disable hardcore mode? {0} will be cancelled if you select No.")
                  .GetCharArray(),
                trigger));
  if (!confirmed)
    return false;

  DisableChallengeMode();
  return true;
}

void Achievements::DisableChallengeMode()
{
  if (!s_active)
    return;

#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    if (RA_HardcoreModeIsActive())
      RA_DisableHardcore();

    return;
  }
#endif

  if (s_challenge_mode)
    SetChallengeMode(false);
}

void Achievements::ResetChallengeMode()
{
  if (!s_active)
    return;

  if (s_challenge_mode != g_settings.achievements_challenge_mode)
    SetChallengeMode(g_settings.achievements_challenge_mode);
}

void Achievements::SetChallengeMode(bool enabled)
{
  if (enabled == s_challenge_mode)
    return;

  // new mode
  s_challenge_mode = enabled;

  if (HasActiveGame())
  {
    Host::AddKeyedOSDMessage("achievements_set_challenge_mode",
                             Host::TranslateStdString("Achievements", enabled ? "Hardcore mode is now enabled." :
                                                                                "Hardcore mode is now disabled."),
                             10.0f);
  }

  if (HasActiveGame() && !g_settings.achievements_test_mode)
  {
    // deactivate, but don't clear all achievements (getting unlocks will reactivate them)
    std::unique_lock lock(s_achievements_mutex);
    for (Achievement& achievement : s_achievements)
    {
      DeactivateAchievement(&achievement);
      achievement.locked = true;
    }
    for (Leaderboard& leaderboard : s_leaderboards)
      rc_runtime_deactivate_lboard(&s_rcheevos_runtime, leaderboard.id);
  }

  // re-grab unlocks, this will reactivate what's locked in non-hardcore mode later on
  if (!s_achievements.empty())
    GetUserUnlocks();
}

bool Achievements::Shutdown()
{
#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    if (!RA_ConfirmLoadNewRom(true))
      return false;

    RA_SetPaused(false);
    RA_ActivateGame(0);
    return true;
  }
#endif

  if (!s_active)
    return true;

  std::unique_lock lock(s_achievements_mutex);
  s_http_downloader->WaitForAllRequests();

  ClearGameInfo();
  ClearGameHash();
  std::string().swap(s_username);
  std::string().swap(s_api_token);
  s_logged_in = false;
  Host::OnAchievementsRefreshed();

  s_active = false;
  s_challenge_mode = false;
  rc_runtime_destroy(&s_rcheevos_runtime);

  s_http_downloader.reset();
  return true;
}

bool Achievements::Reset()
{
#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    if (!RA_ConfirmLoadNewRom(false))
      return false;

    RA_OnReset();
    return true;
  }
#endif

  if (!s_active)
    return true;

  std::unique_lock lock(s_achievements_mutex);
  Log_DevPrint("Resetting rcheevos state...");
  rc_runtime_reset(&s_rcheevos_runtime);
  return true;
}

void Achievements::OnPaused(bool paused)
{
#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
    RA_SetPaused(paused);
#endif
}

void Achievements::FrameUpdate()
{
  if (!IsActive())
    return;

#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RA_DoAchievementsFrame();
    return;
  }
#endif

  s_http_downloader->PollRequests();

  if (HasActiveGame())
  {
    std::unique_lock lock(s_achievements_mutex);
    rc_runtime_do_frame(&s_rcheevos_runtime, &CheevosEventHandler, &PeekMemory, nullptr, nullptr);
    UpdateRichPresence();

    if (!g_settings.achievements_test_mode)
    {
      const s32 ping_frequency =
        g_settings.achievements_rich_presence ? RICH_PRESENCE_PING_FREQUENCY : NO_RICH_PRESENCE_PING_FREQUENCY;
      if (static_cast<s32>(s_last_ping_time.GetTimeSeconds()) >= ping_frequency)
        SendPing();
    }
  }
}

void Achievements::ProcessPendingHTTPRequests()
{
#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
    return;
#endif

  s_http_downloader->PollRequests();
}

bool Achievements::DoState(StateWrapper& sw)
{
  // if we're inactive, we still need to skip the data (if any)
  if (!s_active)
  {
    u32 data_size = 0;
    sw.Do(&data_size);
    if (data_size > 0)
      sw.SkipBytes(data_size);

    return !sw.HasError();
  }

  std::unique_lock lock(s_achievements_mutex);

  if (sw.IsReading())
  {
    u32 data_size = 0;
    sw.Do(&data_size);
    if (data_size == 0)
    {
      // reset runtime, no data (state might've been created without cheevos)
      Log_DevPrintf("State is missing cheevos data, resetting runtime");
#ifdef WITH_RAINTEGRATION
      if (IsUsingRAIntegration())
        RA_OnReset();
      else
        rc_runtime_reset(&s_rcheevos_runtime);
#else
      rc_runtime_reset(&s_rcheevos_runtime);
#endif

      return !sw.HasError();
    }

    const std::unique_ptr<u8[]> data(new u8[data_size]);
    sw.DoBytes(data.get(), data_size);
    if (sw.HasError())
      return false;

#ifdef WITH_RAINTEGRATION
    if (IsUsingRAIntegration())
    {
      RA_RestoreState(reinterpret_cast<const char*>(data.get()));
    }
    else
    {
      const int result = rc_runtime_deserialize_progress(&s_rcheevos_runtime, data.get(), nullptr);
      if (result != RC_OK)
      {
        Log_WarningPrintf("Failed to deserialize cheevos state (%d), resetting", result);
        rc_runtime_reset(&s_rcheevos_runtime);
      }
    }
#endif

    return true;
  }
  else
  {
    u32 data_size;
    std::unique_ptr<u8[]> data;

#ifdef WITH_RAINTEGRATION
    if (IsUsingRAIntegration())
    {
      const int size = RA_CaptureState(nullptr, 0);

      data_size = (size >= 0) ? static_cast<u32>(size) : 0;
      data = std::unique_ptr<u8[]>(new u8[data_size]);

      const int result = RA_CaptureState(reinterpret_cast<char*>(data.get()), static_cast<int>(data_size));
      if (result != static_cast<int>(data_size))
      {
        Log_WarningPrint("Failed to serialize cheevos state from RAIntegration.");
        data_size = 0;
      }
    }
    else
    {
      // internally this happens twice.. not great.
      const int size = rc_runtime_progress_size(&s_rcheevos_runtime, nullptr);

      data_size = (size >= 0) ? static_cast<u32>(size) : 0;
      data = std::unique_ptr<u8[]>(new u8[data_size]);

      const int result = rc_runtime_serialize_progress(data.get(), &s_rcheevos_runtime, nullptr);
      if (result != RC_OK)
      {
        // set data to zero, effectively serializing nothing
        Log_WarningPrintf("Failed to serialize cheevos state (%d)", result);
        data_size = 0;
      }
    }
#endif

    sw.Do(&data_size);
    if (data_size > 0)
      sw.DoBytes(data.get(), data_size);

    return !sw.HasError();
  }
}

bool Achievements::SafeHasAchievementsOrLeaderboards()
{
  std::unique_lock lock(s_achievements_mutex);
  return !s_achievements.empty() || s_leaderboards.empty();
}

const std::string& Achievements::GetUsername()
{
  return s_username;
}

const std::string& Achievements::GetRichPresenceString()
{
  return s_rich_presence_string;
}

void Achievements::EnsureCacheDirectoriesExist()
{
  s_game_icon_cache_directory = Path::Combine(EmuFolders::Cache, "achievement_gameicon");
  s_achievement_icon_cache_directory = Path::Combine(EmuFolders::Cache, "achievement_badge");

  if (!FileSystem::DirectoryExists(s_game_icon_cache_directory.c_str()) &&
      !FileSystem::CreateDirectory(s_game_icon_cache_directory.c_str(), false))
  {
    FormattedError("Failed to create cache directory '%s'", s_game_icon_cache_directory.c_str());
  }

  if (!FileSystem::DirectoryExists(s_achievement_icon_cache_directory.c_str()) &&
      !FileSystem::CreateDirectory(s_achievement_icon_cache_directory.c_str(), false))
  {
    FormattedError("Failed to create cache directory '%s'", s_achievement_icon_cache_directory.c_str());
  }
}

void Achievements::LoginCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  std::unique_lock lock(s_achievements_mutex);

  RAPIResponse<rc_api_login_response_t, rc_api_process_login_response, rc_api_destroy_login_response> response(
    status_code, data);
  if (!response)
  {
    FormattedError("Login failed. Please check your user name and password, and try again.");
    return;
  }

  std::string username(response.username);
  std::string api_token(response.api_token);

  // save to config
  Host::SetBaseStringSettingValue("Cheevos", "Username", username.c_str());
  Host::SetBaseStringSettingValue("Cheevos", "Token", api_token.c_str());
  Host::SetBaseStringSettingValue("Cheevos", "LoginTimestamp", fmt::format("{}", std::time(nullptr)).c_str());
  Host::CommitBaseSettingChanges();

  if (s_active)
  {
    s_username = std::move(username);
    s_api_token = std::move(api_token);
    s_logged_in = true;

    // If we have a game running, set it up.
    if (System::IsValid())
      GameChanged(System::GetRunningPath(), nullptr);
  }
}

void Achievements::LoginASyncCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  ImGuiFullscreen::CloseBackgroundProgressDialog("cheevos_async_login");

  LoginCallback(status_code, std::move(data));
}

void Achievements::SendLogin(const char* username, const char* password, Common::HTTPDownloader* http_downloader,
                             Common::HTTPDownloader::Request::Callback callback)
{
  RAPIRequest<rc_api_login_request_t, rc_api_init_login_request> request;
  request.username = username;
  request.password = password;
  request.api_token = nullptr;
  request.Send(http_downloader, std::move(callback));
}

bool Achievements::LoginAsync(const char* username, const char* password)
{
  s_http_downloader->WaitForAllRequests();

  if (s_logged_in || std::strlen(username) == 0 || std::strlen(password) == 0 || IsUsingRAIntegration())
    return false;

  if (FullscreenUI::IsInitialized())
  {
    ImGuiFullscreen::OpenBackgroundProgressDialog("cheevos_async_login", "Logging in to RetroAchivements...", 0, 1, 0);
  }

  SendLogin(username, password, s_http_downloader.get(), LoginASyncCallback);
  return true;
}

bool Achievements::Login(const char* username, const char* password)
{
  if (s_active)
    s_http_downloader->WaitForAllRequests();

  if (s_logged_in || std::strlen(username) == 0 || std::strlen(password) == 0 || IsUsingRAIntegration())
    return false;

  if (s_active)
  {
    SendLogin(username, password, s_http_downloader.get(), LoginCallback);
    s_http_downloader->WaitForAllRequests();
    return IsLoggedIn();
  }

  // create a temporary downloader if we're not initialized
  AssertMsg(!s_active, "RetroAchievements is not active on login");
  std::unique_ptr<Common::HTTPDownloader> http_downloader = Common::HTTPDownloader::Create(GetUserAgent().c_str());
  if (!http_downloader)
    return false;

  SendLogin(username, password, http_downloader.get(), LoginCallback);
  http_downloader->WaitForAllRequests();

  return !Host::GetBaseStringSettingValue("Cheevos", "Token").empty();
}

void Achievements::Logout()
{
  if (s_active)
  {
    std::unique_lock lock(s_achievements_mutex);
    s_http_downloader->WaitForAllRequests();
    if (s_logged_in)
    {
      ClearGameInfo();
      std::string().swap(s_username);
      std::string().swap(s_api_token);
      s_logged_in = false;
      Host::OnAchievementsRefreshed();
    }
  }

  // remove from config
  Host::DeleteBaseSettingValue("Cheevos", "Username");
  Host::DeleteBaseSettingValue("Cheevos", "Token");
  Host::DeleteBaseSettingValue("Cheevos", "LoginTimestamp");
  Host::CommitBaseSettingChanges();
}

void Achievements::DownloadImage(std::string url, std::string cache_filename)
{
  auto callback = [cache_filename](s32 status_code, Common::HTTPDownloader::Request::Data data) {
    if (status_code != HTTP_OK)
      return;

    if (!FileSystem::WriteBinaryFile(cache_filename.c_str(), data.data(), data.size()))
    {
      Log_ErrorPrintf("Failed to write badge image to '%s'", cache_filename.c_str());
      return;
    }

    ImGuiFullscreen::InvalidateCachedTexture(cache_filename);
  };

  s_http_downloader->CreateRequest(std::move(url), std::move(callback));
}

void Achievements::DisplayAchievementSummary()
{
  std::string title;
  if (ChallengeModeActive())
    title = fmt::format(Host::TranslateString("Achievements", "{} (Hardcore Mode)").GetCharArray(), s_game_title);
  else
    title = s_game_title;

  std::string summary;
  if (GetAchievementCount() > 0)
  {
    summary = fmt::format(
      Host::TranslateString("Achievements", "You have earned {} of {} achievements, and {} of {} points.")
        .GetCharArray(),
      GetUnlockedAchiementCount(), GetAchievementCount(), GetCurrentPointsForGame(), GetMaximumPointsForGame());
  }
  else
  {
    summary = Host::TranslateStdString("Achievements", "This game has no achievements.");
  }
  if (GetLeaderboardCount() > 0)
  {
    summary.push_back('\n');
    if (ChallengeModeActive())
    {
      summary.append(Host::TranslateString("Achievements", "Leaderboards are enabled."));
    }
    else
    {
      summary.append(Host::TranslateString("Achievements", "Leaderboards are disabled because hardcore mode is off."));
    }
  }

  ImGuiFullscreen::AddNotification(10.0f, std::move(title), std::move(summary), s_game_icon);
}

void Achievements::GetUserUnlocksCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  if (!System::IsValid())
    return;

  RAPIResponse<rc_api_fetch_user_unlocks_response_t, rc_api_process_fetch_user_unlocks_response,
               rc_api_destroy_fetch_user_unlocks_response>
    response(status_code, data);

  std::unique_lock lock(s_achievements_mutex);
  if (!response)
  {
    ClearGameInfo(true, false);
    return;
  }

  // flag achievements as unlocked
  for (u32 i = 0; i < response.num_achievement_ids; i++)
  {
    Achievement* cheevo = GetAchievementByID(response.achievement_ids[i]);
    if (!cheevo)
    {
      Log_ErrorPrintf("Server returned unknown achievement %u", response.achievement_ids[i]);
      continue;
    }

    cheevo->locked = false;
  }

  // start scanning for locked achievements
  ActivateLockedAchievements();
  DisplayAchievementSummary();
  SendPlaying();
  UpdateRichPresence();
  SendPing();
  Host::OnAchievementsRefreshed();
}

void Achievements::GetUserUnlocks()
{
  RAPIRequest<rc_api_fetch_user_unlocks_request_t, rc_api_init_fetch_user_unlocks_request> request;
  request.username = s_username.c_str();
  request.api_token = s_api_token.c_str();
  request.game_id = s_game_id;
  request.hardcore = static_cast<int>(ChallengeModeActive());
  request.Send(GetUserUnlocksCallback);
}

void Achievements::GetPatchesCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  if (!System::IsValid())
    return;

  RAPIResponse<rc_api_fetch_game_data_response_t, rc_api_process_fetch_game_data_response,
               rc_api_destroy_fetch_game_data_response>
    response(status_code, data);

  std::unique_lock lock(s_achievements_mutex);
  ClearGameInfo();
  if (!response)
  {
    DisableChallengeMode();
    return;
  }

  // ensure fullscreen UI is ready
  Host::RunOnCPUThread(FullscreenUI::Initialize);

  s_game_id = response.id;
  s_game_title = response.title;

  // try for a icon
  if (std::strlen(response.image_name) > 0)
  {
    s_game_icon = Path::Combine(s_game_icon_cache_directory, fmt::format("{}.png", s_game_id));
    if (!FileSystem::FileExists(s_game_icon.c_str()))
    {
      RAPIRequest<rc_api_fetch_image_request_t, rc_api_init_fetch_image_request> request;
      request.image_name = response.image_name;
      request.image_type = RC_IMAGE_TYPE_GAME;
      request.DownloadImage(s_game_icon);
    }
  }

  // parse achievements
  for (u32 i = 0; i < response.num_achievements; i++)
  {
    const rc_api_achievement_definition_t& defn = response.achievements[i];

    // Skip local and unofficial achievements for now, unless "Test Unofficial Achievements" is enabled
    if (defn.category == RC_ACHIEVEMENT_CATEGORY_UNOFFICIAL)
    {
      if (!g_settings.achievements_unofficial_test_mode)
      {
        Log_WarningPrintf("Skipping unofficial achievement %u (%s)", defn.id, defn.title);
        continue;
      }
    }
    // local achievements shouldn't be in this list, but just in case?
    else if (defn.category != RC_ACHIEVEMENT_CATEGORY_CORE)
    {
      continue;
    }

    if (GetAchievementByID(defn.id))
    {
      Log_ErrorPrintf("Achievement %u already exists", defn.id);
      continue;
    }

    Achievement cheevo;
    cheevo.id = defn.id;
    cheevo.memaddr = defn.definition;
    cheevo.title = defn.title;
    cheevo.description = defn.description;
    cheevo.badge_name = defn.badge_name;
    cheevo.locked = true;
    cheevo.active = false;
    cheevo.points = defn.points;
    cheevo.category = static_cast<AchievementCategory>(defn.category);
    s_achievements.push_back(std::move(cheevo));
  }

  for (u32 i = 0; i < response.num_leaderboards; i++)
  {
    const rc_api_leaderboard_definition_t& defn = response.leaderboards[i];

    Leaderboard lboard;
    lboard.id = defn.id;
    lboard.title = defn.title;
    lboard.description = defn.description;
    lboard.format = defn.format;
    s_leaderboards.push_back(std::move(lboard));

    const int err = rc_runtime_activate_lboard(&s_rcheevos_runtime, defn.id, defn.definition, nullptr, 0);
    if (err != RC_OK)
    {
      Log_ErrorPrintf("Leaderboard %u memaddr parse error: %s", defn.id, rc_error_str(err));
    }
    else
    {
      Log_DevPrintf("Activated leaderboard %s (%u)", defn.title, defn.id);
    }
  }

  // parse rich presence
  if (std::strlen(response.rich_presence_script) > 0)
  {
    int res = rc_runtime_activate_richpresence(&s_rcheevos_runtime, response.rich_presence_script, nullptr, 0);
    if (res == RC_OK)
      s_has_rich_presence = true;
    else
      Log_WarningPrintf("Failed to activate rich presence: %s", rc_error_str(res));
  }

  Log_InfoPrintf("Game Title: %s", s_game_title.c_str());
  Log_InfoPrintf("Achievements: %zu", s_achievements.size());
  Log_InfoPrintf("Leaderboards: %zu", s_leaderboards.size());

  // We don't want to block saving/loading states when there's no achievements.
  if (s_achievements.empty() && s_leaderboards.empty())
    DisableChallengeMode();

  if (!s_achievements.empty() || s_has_rich_presence)
  {
    if (!g_settings.achievements_test_mode)
    {
      GetUserUnlocks();
    }
    else
    {
      ActivateLockedAchievements();
      DisplayAchievementSummary();
      Host::OnAchievementsRefreshed();
    }
  }
  else
  {
    DisplayAchievementSummary();
  }

  if (s_achievements.empty() && s_leaderboards.empty() && !s_has_rich_presence)
  {
    ClearGameInfo();
  }
}

void Achievements::GetLbInfoCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  if (!System::IsValid())
    return;

  RAPIResponse<rc_api_fetch_leaderboard_info_response_t, rc_api_process_fetch_leaderboard_info_response,
               rc_api_destroy_fetch_leaderboard_info_response>
    response(status_code, data);
  if (!response)
    return;

  std::unique_lock lock(s_achievements_mutex);
  if (response.id != s_last_queried_lboard)
  {
    // User has already requested another leaderboard, drop this data
    return;
  }

  const Leaderboard* leaderboard = GetLeaderboardByID(response.id);
  if (!leaderboard)
  {
    Log_ErrorPrintf("Attempting to list unknown leaderboard %u", response.id);
    return;
  }

  s_lboard_entries = std::vector<Achievements::LeaderboardEntry>();
  for (u32 i = 0; i < response.num_entries; i++)
  {
    const rc_api_lboard_info_entry_t& entry = response.entries[i];

    char score[128];
    rc_runtime_format_lboard_value(score, sizeof(score), entry.score, leaderboard->format);

    LeaderboardEntry lbe;
    lbe.user = entry.username;
    lbe.rank = entry.rank;
    lbe.formatted_score = score;
    lbe.is_self = lbe.user == s_username;

    s_lboard_entries->push_back(std::move(lbe));
  }
}

void Achievements::GetPatches(u32 game_id)
{
  RAPIRequest<rc_api_fetch_game_data_request_t, rc_api_init_fetch_game_data_request> request;
  request.username = s_username.c_str();
  request.api_token = s_api_token.c_str();
  request.game_id = game_id;
  request.Send(GetPatchesCallback);
}

std::string Achievements::GetGameHash(CDImage* image)
{
  std::string executable_name;
  std::vector<u8> executable_data;
  if (!System::ReadExecutableFromImage(image, &executable_name, &executable_data))
    return {};

  BIOS::PSEXEHeader header;
  if (executable_data.size() >= sizeof(header))
    std::memcpy(&header, executable_data.data(), sizeof(header));
  if (!BIOS::IsValidPSExeHeader(header, static_cast<u32>(executable_data.size())))
  {
    Log_ErrorPrintf("PS-EXE header is invalid in '%s' (%zu bytes)", executable_name.c_str(), executable_data.size());
    return {};
  }

  // See rcheevos hash.c - rc_hash_psx().
  const u32 MAX_HASH_SIZE = 64 * 1024 * 1024;
  const u32 hash_size = std::min<u32>(sizeof(header) + header.file_size, MAX_HASH_SIZE);
  Assert(hash_size <= executable_data.size());

  MD5Digest digest;
  digest.Update(executable_name.c_str(), static_cast<u32>(executable_name.size()));
  if (hash_size > 0)
    digest.Update(executable_data.data(), hash_size);

  u8 hash[16];
  digest.Final(hash);

  std::string hash_str(StringUtil::StdStringFromFormat(
    "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", hash[0], hash[1], hash[2], hash[3], hash[4],
    hash[5], hash[6], hash[7], hash[8], hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]));

  Log_InfoPrintf("Hash for '%s' (%zu bytes, %u bytes hashed): %s", executable_name.c_str(), executable_data.size(),
                 hash_size, hash_str.c_str());
  return hash_str;
}

void Achievements::GetGameIdCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  if (!System::IsValid())
    return;

  RAPIResponse<rc_api_resolve_hash_response_t, rc_api_process_resolve_hash_response,
               rc_api_destroy_resolve_hash_response>
    response(status_code, data);
  if (!response)
    return;

  const u32 game_id = response.game_id;
  Log_VerbosePrintf("Server returned GameID %u", game_id);
  if (game_id == 0)
  {
    // We don't want to block saving/loading states when there's no achievements.
    DisableChallengeMode();
    return;
  }

  GetPatches(game_id);
}

void Achievements::GameChanged(const std::string& path, CDImage* image)
{
  if (!IsActive() || s_game_path == path)
    return;

  std::unique_ptr<CDImage> temp_image;
  if (!path.empty() && (!image || (g_settings.achievements_use_first_disc_from_playlist && image->HasSubImages() &&
                                   image->GetCurrentSubImage() != 0)))
  {
    temp_image = CDImage::Open(path.c_str(), nullptr);
    image = temp_image.get();
    if (!temp_image)
    {
      Log_ErrorPrintf("Failed to open temporary CD image '%s'", path.c_str());
      DisableChallengeMode();
      ClearGameInfo();
      return;
    }
  }

  std::string game_hash;
  if (image)
  {
    game_hash = GetGameHash(image);
    if (s_game_hash == game_hash)
    {
      // only the path has changed - different format/save state/etc.
      Log_InfoPrintf("Detected path change from '%s' to '%s'", s_game_path.c_str(), path.c_str());
      s_game_path = path;
      return;
    }
  }

  s_http_downloader->WaitForAllRequests();

  if (image && image->HasSubImages() && image->GetCurrentSubImage() != 0)
  {
    std::unique_ptr<CDImage> image_copy(CDImage::Open(image->GetFileName().c_str(), nullptr));
    if (!image_copy)
    {
      Log_ErrorPrintf("Failed to reopen image '%s'", image->GetFileName().c_str());
      return;
    }

    // this will go to subimage zero automatically
    Assert(image_copy->GetCurrentSubImage() == 0);
    GameChanged(path, image_copy.get());
    return;
  }

  std::unique_lock lock(s_achievements_mutex);
  if (!IsUsingRAIntegration())
    s_http_downloader->WaitForAllRequests();

  ClearGameInfo();
  ClearGameHash();
  s_game_path = path;
  s_game_hash = std::move(game_hash);

#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RAIntegration::GameChanged();
    return;
  }
#endif

  if (s_game_hash.empty())
  {
    // when we're booting the bios, this will fail
    if (!s_game_path.empty())
    {
      Host::AddKeyedOSDMessage("retroachievements_disc_read_failed",
                               "Failed to read executable from disc. Achievements disabled.", 10.0f);
    }

    DisableChallengeMode();
    return;
  }

  RAPIRequest<rc_api_resolve_hash_request_t, rc_api_init_resolve_hash_request> request;
  request.username = s_username.c_str();
  request.api_token = s_api_token.c_str();
  request.game_hash = s_game_hash.c_str();
  request.Send(GetGameIdCallback);
}

void Achievements::SendPlayingCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  if (!System::IsValid())
    return;

  RAPIResponse<rc_api_start_session_response_t, rc_api_process_start_session_response,
               rc_api_destroy_start_session_response>
    response(status_code, data);
  if (!response)
    return;

  Log_InfoPrintf("Playing game updated to %u (%s)", s_game_id, s_game_title.c_str());
}

void Achievements::SendPlaying()
{
  if (!HasActiveGame())
    return;

  RAPIRequest<rc_api_start_session_request_t, rc_api_init_start_session_request> request;
  request.username = s_username.c_str();
  request.api_token = s_api_token.c_str();
  request.game_id = s_game_id;
  request.Send(SendPlayingCallback);
}

void Achievements::UpdateRichPresence()
{
  if (!s_has_rich_presence)
    return;

  char buffer[512];
  int res = rc_runtime_get_richpresence(&s_rcheevos_runtime, buffer, sizeof(buffer), PeekMemory, nullptr, nullptr);
  if (res <= 0)
  {
    const bool had_rich_presence = !s_rich_presence_string.empty();
    s_rich_presence_string.clear();
    if (had_rich_presence)
      Host::OnAchievementsRefreshed();

    return;
  }

  std::unique_lock lock(s_achievements_mutex);
  if (s_rich_presence_string == buffer)
    return;

  s_rich_presence_string.assign(buffer);
  Host::OnAchievementsRefreshed();
}

void Achievements::SendPingCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  if (!System::IsValid())
    return;

  RAPIResponse<rc_api_ping_response_t, rc_api_process_ping_response, rc_api_destroy_ping_response> response(status_code,
                                                                                                            data);
}

void Achievements::SendPing()
{
  if (!HasActiveGame())
    return;

  s_last_ping_time.Reset();

  RAPIRequest<rc_api_ping_request_t, rc_api_init_ping_request> request;
  request.api_token = s_api_token.c_str();
  request.username = s_username.c_str();
  request.game_id = s_game_id;
  request.rich_presence = s_rich_presence_string.c_str();
  request.Send(SendPingCallback);
}

const std::string& Achievements::GetGameTitle()
{
  return s_game_title;
}

const std::string& Achievements::GetGameIcon()
{
  return s_game_icon;
}

bool Achievements::EnumerateAchievements(std::function<bool(const Achievement&)> callback)
{
  for (const Achievement& cheevo : s_achievements)
  {
    if (!callback(cheevo))
      return false;
  }

  return true;
}

u32 Achievements::GetUnlockedAchiementCount()
{
  u32 count = 0;
  for (const Achievement& cheevo : s_achievements)
  {
    if (!cheevo.locked)
      count++;
  }

  return count;
}

u32 Achievements::GetAchievementCount()
{
  return static_cast<u32>(s_achievements.size());
}

u32 Achievements::GetMaximumPointsForGame()
{
  u32 points = 0;
  for (const Achievement& cheevo : s_achievements)
    points += cheevo.points;

  return points;
}

u32 Achievements::GetCurrentPointsForGame()
{
  u32 points = 0;
  for (const Achievement& cheevo : s_achievements)
  {
    if (!cheevo.locked)
      points += cheevo.points;
  }

  return points;
}

bool Achievements::EnumerateLeaderboards(std::function<bool(const Leaderboard&)> callback)
{
  for (const Leaderboard& lboard : s_leaderboards)
  {
    if (!callback(lboard))
      return false;
  }

  return true;
}

std::optional<bool> Achievements::TryEnumerateLeaderboardEntries(u32 id,
                                                                 std::function<bool(const LeaderboardEntry&)> callback)
{
  if (id == s_last_queried_lboard)
  {
    if (s_lboard_entries)
    {
      for (const LeaderboardEntry& entry : *s_lboard_entries)
      {
        if (!callback(entry))
          return false;
      }
      return true;
    }
  }
  else
  {
    s_last_queried_lboard = id;
    s_lboard_entries.reset();

    // TODO: Add paging? For now, stick to defaults
    RAPIRequest<rc_api_fetch_leaderboard_info_request_t, rc_api_init_fetch_leaderboard_info_request> request;
    request.username = s_username.c_str();
    request.leaderboard_id = id;
    request.first_entry = 0;

    // Just over what a single page can store, should be a reasonable amount for now
    request.count = 15;

    request.Send(GetLbInfoCallback);
  }

  return std::nullopt;
}

const Achievements::Leaderboard* Achievements::GetLeaderboardByID(u32 id)
{
  for (const Leaderboard& lb : s_leaderboards)
  {
    if (lb.id == id)
      return &lb;
  }

  return nullptr;
}

u32 Achievements::GetLeaderboardCount()
{
  return static_cast<u32>(s_leaderboards.size());
}

bool Achievements::IsLeaderboardTimeType(const Leaderboard& leaderboard)
{
  return leaderboard.format != RC_FORMAT_SCORE && leaderboard.format != RC_FORMAT_VALUE;
}

void Achievements::ActivateLockedAchievements()
{
  for (Achievement& cheevo : s_achievements)
  {
    if (cheevo.locked)
      ActivateAchievement(&cheevo);
  }
}

bool Achievements::ActivateAchievement(Achievement* achievement)
{
  if (achievement->active)
    return true;

  const int err =
    rc_runtime_activate_achievement(&s_rcheevos_runtime, achievement->id, achievement->memaddr.c_str(), nullptr, 0);
  if (err != RC_OK)
  {
    Log_ErrorPrintf("Achievement %u memaddr parse error: %s", achievement->id, rc_error_str(err));
    return false;
  }

  achievement->active = true;

  Log_DevPrintf("Activated achievement %s (%u)", achievement->title.c_str(), achievement->id);
  return true;
}

void Achievements::DeactivateAchievement(Achievement* achievement)
{
  if (!achievement->active)
    return;

  rc_runtime_deactivate_achievement(&s_rcheevos_runtime, achievement->id);
  achievement->active = false;

  Log_DevPrintf("Deactivated achievement %s (%u)", achievement->title.c_str(), achievement->id);
}

void Achievements::UnlockAchievementCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  RAPIResponse<rc_api_award_achievement_response_t, rc_api_process_award_achievement_response,
               rc_api_destroy_award_achievement_response>
    response(status_code, data);
  if (!response)
    return;

  Log_InfoPrintf("Successfully unlocked achievement %u, new score %u", response.awarded_achievement_id,
                 response.new_player_score);
}

void Achievements::SubmitLeaderboardCallback(s32 status_code, Common::HTTPDownloader::Request::Data data)
{
  RAPIResponse<rc_api_submit_lboard_entry_response_t, rc_api_process_submit_lboard_entry_response,
               rc_api_destroy_submit_lboard_entry_response>
    response(status_code, data);
  if (!response)
    return;

  // Force the next leaderboard query to repopulate everything, just in case the user wants to see their new score
  s_last_queried_lboard = 0;

  // RA API doesn't send us the leaderboard ID back.. hopefully we don't submit two at once :/
  if (s_submitting_lboard_id == 0)
    return;

  const Leaderboard* lb = GetLeaderboardByID(std::exchange(s_submitting_lboard_id, 0u));
  if (!lb)
    return;

  char submitted_score[128];
  char best_score[128];
  rc_runtime_format_lboard_value(submitted_score, sizeof(submitted_score), response.submitted_score, lb->format);
  rc_runtime_format_lboard_value(best_score, sizeof(best_score), response.best_score, lb->format);

  std::string summary = fmt::format(
    Host::TranslateString("Achievements", "Your Score: {} (Best: {})\nLeaderboard Position: {} of {}").GetCharArray(),
    submitted_score, best_score, response.new_rank, response.num_entries);

  ImGuiFullscreen::AddNotification(10.0f, lb->title, std::move(summary), s_game_icon);
}

void Achievements::UnlockAchievement(u32 achievement_id, bool add_notification /* = true*/)
{
  Achievement* achievement = GetAchievementByID(achievement_id);
  if (!achievement)
  {
    Log_ErrorPrintf("Attempting to unlock unknown achievement %u", achievement_id);
    return;
  }
  else if (!achievement->locked)
  {
    Log_WarningPrintf("Achievement %u for game %u is already unlocked", achievement_id, s_game_id);
    return;
  }

  achievement->locked = false;
  DeactivateAchievement(achievement);

  Log_InfoPrintf("Achievement %s (%u) for game %u unlocked", achievement->title.c_str(), achievement_id, s_game_id);

  std::string title;
  switch (achievement->category)
  {
    case AchievementCategory::Local:
      title = fmt::format("{} (Local)", achievement->title);
      break;
    case AchievementCategory::Unofficial:
      title = fmt::format("{} (Unofficial)", achievement->title);
      break;
    case AchievementCategory::Core:
    default:
      title = achievement->title;
      break;
  }

  ImGuiFullscreen::AddNotification(15.0f, std::move(title), achievement->description,
                                   GetAchievementBadgePath(*achievement));

  if (g_settings.achievements_test_mode)
  {
    Log_WarningPrintf("Skipping sending achievement %u unlock to server because of test mode.", achievement_id);
    return;
  }

  if (achievement->category != AchievementCategory::Core)
  {
    Log_WarningPrintf("Skipping sending achievement %u unlock to server because it's not from the core set.",
                      achievement_id);
    return;
  }

  RAPIRequest<rc_api_award_achievement_request_t, rc_api_init_award_achievement_request> request;
  request.username = s_username.c_str();
  request.api_token = s_api_token.c_str();
  request.game_hash = s_game_hash.c_str();
  request.achievement_id = achievement_id;
  request.hardcore = static_cast<int>(ChallengeModeActive());
  request.Send(UnlockAchievementCallback);
}

void Achievements::SubmitLeaderboard(u32 leaderboard_id, int value)
{
  if (g_settings.achievements_test_mode)
  {
    Log_WarningPrintf("Skipping sending leaderboard %u result to server because of test mode.", leaderboard_id);
    return;
  }

  if (!ChallengeModeActive())
  {
    Log_WarningPrintf("Skipping sending leaderboard %u result to server because Challenge mode is off.",
                      leaderboard_id);
    return;
  }

  s_submitting_lboard_id = leaderboard_id;

  RAPIRequest<rc_api_submit_lboard_entry_request_t, rc_api_init_submit_lboard_entry_request> request;
  request.username = s_username.c_str();
  request.api_token = s_api_token.c_str();
  request.game_hash = s_game_hash.c_str();
  request.leaderboard_id = leaderboard_id;
  request.score = value;
  request.Send(SubmitLeaderboardCallback);
}

std::pair<u32, u32> Achievements::GetAchievementProgress(const Achievement& achievement)
{
  std::pair<u32, u32> result;
  rc_runtime_get_achievement_measured(&s_rcheevos_runtime, achievement.id, &result.first, &result.second);
  return result;
}

std::string Achievements::GetAchievementProgressText(const Achievement& achievement)
{
  char buf[256];
  rc_runtime_format_achievement_measured(&s_rcheevos_runtime, achievement.id, buf, std::size(buf));
  return buf;
}

const std::string& Achievements::GetAchievementBadgePath(const Achievement& achievement)
{
  std::string& badge_path = achievement.locked ? achievement.locked_badge_path : achievement.unlocked_badge_path;
  if (!badge_path.empty() || achievement.badge_name.empty())
    return badge_path;

  // well, this comes from the internet.... :)
  std::string clean_name(achievement.badge_name);
  Path::SanitizeFileName(clean_name);
  badge_path = Path::Combine(s_achievement_icon_cache_directory,
                             fmt::format("{}{}.png", clean_name, achievement.locked ? "_lock" : ""));
  if (FileSystem::FileExists(badge_path.c_str()))
    return badge_path;

  // need to download it
  RAPIRequest<rc_api_fetch_image_request_t, rc_api_init_fetch_image_request> request;
  request.image_name = achievement.badge_name.c_str();
  request.image_type = achievement.locked ? RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED : RC_IMAGE_TYPE_ACHIEVEMENT;
  request.DownloadImage(badge_path);
  return badge_path;
}

void Achievements::CheevosEventHandler(const rc_runtime_event_t* runtime_event)
{
  static const char* events[] = {"RC_RUNTIME_EVENT_ACHIEVEMENT_ACTIVATED", "RC_RUNTIME_EVENT_ACHIEVEMENT_PAUSED",
                                 "RC_RUNTIME_EVENT_ACHIEVEMENT_RESET",     "RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED",
                                 "RC_RUNTIME_EVENT_ACHIEVEMENT_PRIMED",    "RC_RUNTIME_EVENT_LBOARD_STARTED",
                                 "RC_RUNTIME_EVENT_LBOARD_CANCELED",       "RC_RUNTIME_EVENT_LBOARD_UPDATED",
                                 "RC_RUNTIME_EVENT_LBOARD_TRIGGERED",      "RC_RUNTIME_EVENT_ACHIEVEMENT_DISABLED",
                                 "RC_RUNTIME_EVENT_LBOARD_DISABLED"};
  const char* event_text =
    ((unsigned)runtime_event->type >= countof(events)) ? "unknown" : events[(unsigned)runtime_event->type];
  Log_DevPrintf("Cheevos Event %s for %u", event_text, runtime_event->id);

  if (runtime_event->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED)
    UnlockAchievement(runtime_event->id);
  else if (runtime_event->type == RC_RUNTIME_EVENT_LBOARD_TRIGGERED)
    SubmitLeaderboard(runtime_event->id, runtime_event->value);
}

unsigned Achievements::PeekMemory(unsigned address, unsigned num_bytes, void* ud)
{
  switch (num_bytes)
  {
    case 1:
    {
      u8 value = 0;
      CPU::SafeReadMemoryByte(address, &value);
      return value;
    }

    case 2:
    {
      u16 value;
      CPU::SafeReadMemoryHalfWord(address, &value);
      return value;
    }

    case 4:
    {
      u32 value;
      CPU::SafeReadMemoryWord(address, &value);
      return value;
    }

    default:
      return 0;
  }
}

#ifdef WITH_RAINTEGRATION

#include "RA_Consoles.h"

namespace Achievements::RAIntegration {
static void InitializeRAIntegration(void* main_window_handle);

static int RACallbackIsActive();
static void RACallbackCauseUnpause();
static void RACallbackCausePause();
static void RACallbackRebuildMenu();
static void RACallbackEstimateTitle(char* buf);
static void RACallbackResetEmulator();
static void RACallbackLoadROM(const char* unused);
static unsigned char RACallbackReadMemory(unsigned int address);
static void RACallbackWriteMemory(unsigned int address, unsigned char value);

static bool s_raintegration_initialized = false;
} // namespace Achievements::RAIntegration

void Achievements::SwitchToRAIntegration()
{
  s_using_raintegration = true;
  s_active = true;

  // Not strictly the case, but just in case we gate anything by IsLoggedIn().
  s_logged_in = true;
}

void Achievements::RAIntegration::InitializeRAIntegration(void* main_window_handle)
{
  RA_InitClient((HWND)main_window_handle, "DuckStation", g_scm_tag_str);
  RA_SetUserAgentDetail(Achievements::GetUserAgent().c_str());

  RA_InstallSharedFunctions(RACallbackIsActive, RACallbackCauseUnpause, RACallbackCausePause, RACallbackRebuildMenu,
                            RACallbackEstimateTitle, RACallbackResetEmulator, RACallbackLoadROM);
  RA_SetConsoleID(PlayStation);

  // Apparently this has to be done early, or the memory inspector doesn't work.
  // That's a bit unfortunate, because the RAM size can vary between games, and depending on the option.
  RA_InstallMemoryBank(0, RACallbackReadMemory, RACallbackWriteMemory, Bus::RAM_2MB_SIZE);

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
  s_game_id = s_game_hash.empty() ? 0 : RA_IdentifyHash(s_game_hash.c_str());
  RA_ActivateGame(s_game_id);
}

std::vector<std::pair<int, const char*>> Achievements::RAIntegration::GetMenuItems()
{
  // NOTE: I *really* don't like doing this. But sadly it's the only way we can integrate with Qt.
  static constexpr int IDM_RA_RETROACHIEVEMENTS = 1700;
  static constexpr int IDM_RA_OVERLAYSETTINGS = 1701;
  static constexpr int IDM_RA_FILES_MEMORYBOOKMARKS = 1703;
  static constexpr int IDM_RA_FILES_ACHIEVEMENTS = 1704;
  static constexpr int IDM_RA_FILES_MEMORYFINDER = 1705;
  static constexpr int IDM_RA_FILES_LOGIN = 1706;
  static constexpr int IDM_RA_FILES_LOGOUT = 1707;
  static constexpr int IDM_RA_FILES_ACHIEVEMENTEDITOR = 1708;
  static constexpr int IDM_RA_HARDCORE_MODE = 1710;
  static constexpr int IDM_RA_REPORTBROKENACHIEVEMENTS = 1711;
  static constexpr int IDM_RA_GETROMCHECKSUM = 1712;
  static constexpr int IDM_RA_OPENUSERPAGE = 1713;
  static constexpr int IDM_RA_OPENGAMEPAGE = 1714;
  static constexpr int IDM_RA_PARSERICHPRESENCE = 1716;
  static constexpr int IDM_RA_TOGGLELEADERBOARDS = 1717;
  static constexpr int IDM_RA_NON_HARDCORE_WARNING = 1718;

  std::vector<std::pair<int, const char*>> ret;

  const char* username = RA_UserName();
  if (!username || std::strlen(username) == 0)
  {
    ret.emplace_back(IDM_RA_FILES_LOGIN, "&Login");
  }
  else
  {
    ret.emplace_back(IDM_RA_FILES_LOGOUT, "Log&out");
    ret.emplace_back(0, nullptr);
    ret.emplace_back(IDM_RA_OPENUSERPAGE, "Open my &User Page");
    ret.emplace_back(IDM_RA_OPENGAMEPAGE, "Open this &Game's Page");
    ret.emplace_back(0, nullptr);
    ret.emplace_back(IDM_RA_HARDCORE_MODE, "&Hardcore Mode");
    ret.emplace_back(IDM_RA_NON_HARDCORE_WARNING, "Non-Hardcore &Warning");
    ret.emplace_back(0, nullptr);
    ret.emplace_back(IDM_RA_TOGGLELEADERBOARDS, "Enable &Leaderboards");
    ret.emplace_back(IDM_RA_OVERLAYSETTINGS, "O&verlay Settings");
    ret.emplace_back(0, nullptr);
    ret.emplace_back(IDM_RA_FILES_ACHIEVEMENTS, "Assets Li&st");
    ret.emplace_back(IDM_RA_FILES_ACHIEVEMENTEDITOR, "Assets &Editor");
    ret.emplace_back(IDM_RA_FILES_MEMORYFINDER, "&Memory Inspector");
    ret.emplace_back(IDM_RA_FILES_MEMORYBOOKMARKS, "Memory &Bookmarks");
    ret.emplace_back(IDM_RA_PARSERICHPRESENCE, "Rich &Presence Monitor");
    ret.emplace_back(0, nullptr);
    ret.emplace_back(IDM_RA_REPORTBROKENACHIEVEMENTS, "&Report Achievement Problem");
    ret.emplace_back(IDM_RA_GETROMCHECKSUM, "View Game H&ash");
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
  System::PauseSystem(false);
}

void Achievements::RAIntegration::RACallbackCausePause()
{
  System::PauseSystem(true);
}

void Achievements::RAIntegration::RACallbackRebuildMenu()
{
  // unused, we build the menu on demand
}

void Achievements::RAIntegration::RACallbackEstimateTitle(char* buf)
{
  StringUtil::Strlcpy(buf, System::GetRunningTitle(), 256);
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

unsigned char Achievements::RAIntegration::RACallbackReadMemory(unsigned int address)
{
  if (!System::IsValid())
    return 0;

  u8 value = 0;
  CPU::SafeReadMemoryByte(address, &value);
  return value;
}

void Achievements::RAIntegration::RACallbackWriteMemory(unsigned int address, unsigned char value)
{
  CPU::SafeWriteMemoryByte(address, value);
}

#endif

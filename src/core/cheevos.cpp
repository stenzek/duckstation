#include "cheevos.h"
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
#include "core/host_display.h"
#include "core/system.h"
#include "host_interface.h"
#include "imgui_fullscreen.h"
#include "rapidjson/document.h"
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
Log_SetChannel(Cheevos);

#ifdef WITH_RAINTEGRATION
// RA_Interface ends up including windows.h, with its silly macros.
#ifdef _WIN32
#include "common/windows_headers.h"
#endif
#include "RA_Interface.h"
#endif

namespace Cheevos {

enum : s32
{
  HTTP_OK = FrontendCommon::HTTPDownloader::HTTP_OK,

  // Number of seconds between rich presence pings. RAIntegration uses 2 minutes.
  RICH_PRESENCE_PING_FREQUENCY = 2 * 60,
  NO_RICH_PRESENCE_PING_FREQUENCY = RICH_PRESENCE_PING_FREQUENCY * 2
};

static void FormattedError(const char* format, ...) printflike(1, 2);
static void CheevosEventHandler(const rc_runtime_event_t* runtime_event);
static unsigned CheevosPeek(unsigned address, unsigned num_bytes, void* ud);
static void ActivateLockedAchievements();
static bool ActivateAchievement(Achievement* achievement);
static void DeactivateAchievement(Achievement* achievement);
static void SendPing();
static void SendPlaying();
static void UpdateRichPresence();
static void GameChanged();
static std::string GetErrorFromResponseJSON(const rapidjson::Document& doc);
static void LogFailedResponseJSON(const FrontendCommon::HTTPDownloader::Request::Data& data);
static bool ParseResponseJSON(const char* request_type, s32 status_code,
                              const FrontendCommon::HTTPDownloader::Request::Data& data, rapidjson::Document& doc,
                              const char* success_field = "Success");
static Achievement* GetAchievementByID(u32 id);
static void ClearGameInfo(bool clear_achievements = true, bool clear_leaderboards = true);
static void ClearGamePath();
static std::string GetUserAgent();
static void LoginCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void LoginASyncCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void SendLogin(const char* username, const char* password, FrontendCommon::HTTPDownloader* http_downloader,
                      FrontendCommon::HTTPDownloader::Request::Callback callback);
static void UpdateImageDownloadProgress();
static void DownloadImage(std::string url, std::string cache_filename);
static std::string GetBadgeImageFilename(const char* badge_name, bool locked, bool cache_path);
static std::string ResolveBadgePath(const char* badge_name, bool locked);
static void DisplayAchievementSummary();
static void GetUserUnlocksCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void GetUserUnlocks();
static void GetPatchesCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void GetLbInfoCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void GetPatches(u32 game_id);
static std::string GetGameHash(CDImage* cdi);
static void GetGameIdCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void SendPlayingCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void UpdateRichPresence();
static void SendPingCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void UnlockAchievementCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);
static void SubmitLeaderboardCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data);

bool g_active = false;
bool g_challenge_mode = false;
u32 g_game_id = 0;

static bool s_logged_in = false;
static bool s_test_mode = false;
static bool s_unofficial_test_mode = false;
static bool s_use_first_disc_from_playlist = true;
static bool s_rich_presence_enabled = false;

#ifdef WITH_RAINTEGRATION
bool g_using_raintegration = false;
bool g_raintegration_initialized = false;
#endif

static rc_runtime_t s_rcheevos_runtime;
static std::unique_ptr<FrontendCommon::HTTPDownloader> s_http_downloader;

static std::string s_username;
static std::string s_login_token;

static std::string s_game_path;
static std::string s_game_hash;
static std::string s_game_title;
static std::string s_game_developer;
static std::string s_game_publisher;
static std::string s_game_release_date;
static std::string s_game_icon;
static std::vector<Cheevos::Achievement> s_achievements;
static std::vector<Cheevos::Leaderboard> s_leaderboards;

static bool s_has_rich_presence = false;
static std::string s_rich_presence_string;
static Common::Timer s_last_ping_time;

static u32 s_last_queried_lboard;
static std::optional<std::vector<Cheevos::LeaderboardEntry>> s_lboard_entries;

static u32 s_total_image_downloads;
static u32 s_completed_image_downloads;
static bool s_image_download_progress_active;

} // namespace Cheevos

template<typename T>
static std::string GetOptionalString(const T& value, const char* key)
{
  if (!value.HasMember(key) || !value[key].IsString())
    return std::string();

  return value[key].GetString();
}

template<typename T>
static u32 GetOptionalUInt(const T& value, const char* key)
{
  if (!value.HasMember(key) || !value[key].IsUint())
    return 0;

  return value[key].GetUint();
}

void Cheevos::FormattedError(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  SmallString str;
  str.AppendString("Cheevos Error: ");
  str.AppendFormattedStringVA(format, ap);

  va_end(ap);

  g_host_interface->AddOSDMessage(str.GetCharArray(), 10.0f);
  Log_ErrorPrint(str.GetCharArray());
}

std::string Cheevos::GetErrorFromResponseJSON(const rapidjson::Document& doc)
{
  if (doc.HasMember("Error") && doc["Error"].IsString())
    return doc["Error"].GetString();

  return "";
}

void Cheevos::LogFailedResponseJSON(const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  const std::string str_data(reinterpret_cast<const char*>(data.data()), data.size());
  Log_ErrorPrintf("API call failed. Response JSON was:\n%s", str_data.c_str());
}

bool Cheevos::ParseResponseJSON(const char* request_type, s32 status_code,
                                const FrontendCommon::HTTPDownloader::Request::Data& data, rapidjson::Document& doc,
                                const char* success_field)
{
  if (status_code != HTTP_OK || data.empty())
  {
    FormattedError("%s failed: empty response", request_type);
    LogFailedResponseJSON(data);
    return false;
  }

  doc.Parse(reinterpret_cast<const char*>(data.data()), data.size());
  if (doc.HasParseError())
  {
    FormattedError("%s failed: parse error at offset %zu: %u", request_type, doc.GetErrorOffset(),
                   static_cast<unsigned>(doc.GetParseError()));
    LogFailedResponseJSON(data);
    return false;
  }

  if (success_field && (!doc.HasMember(success_field) || !doc[success_field].GetBool()))
  {
    const std::string error(GetErrorFromResponseJSON(doc));
    FormattedError("%s failed: Server returned an error: %s", request_type, error.c_str());
    LogFailedResponseJSON(data);
    return false;
  }

  return true;
}

static Cheevos::Achievement* Cheevos::GetAchievementByID(u32 id)
{
  for (Achievement& ach : s_achievements)
  {
    if (ach.id == id)
      return &ach;
  }

  return nullptr;
}

void Cheevos::ClearGameInfo(bool clear_achievements, bool clear_leaderboards)
{
  const bool had_game = (g_game_id != 0);

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
    s_lboard_entries.reset();
  }

  if (s_achievements.empty() && s_leaderboards.empty())
  {
    // Ready to tear down cheevos completely
    s_has_rich_presence = false;

    std::string().swap(s_game_title);
    std::string().swap(s_game_developer);
    std::string().swap(s_game_publisher);
    std::string().swap(s_game_release_date);
    std::string().swap(s_game_icon);
    s_rich_presence_string.clear();
    g_game_id = 0;
  }

  if (had_game)
    g_host_interface->OnAchievementsRefreshed();
}

void Cheevos::ClearGamePath()
{
  std::string().swap(s_game_path);
  std::string().swap(s_game_hash);
}

std::string Cheevos::GetUserAgent()
{
  return StringUtil::StdStringFromFormat("DuckStation for %s (%s) %s", SYSTEM_STR, CPU_ARCH_STR, g_scm_tag_str);
}

bool Cheevos::Initialize(bool test_mode, bool use_first_disc_from_playlist, bool enable_rich_presence,
                         bool challenge_mode, bool include_unofficial)
{
  s_http_downloader = FrontendCommon::HTTPDownloader::Create(GetUserAgent().c_str());
  if (!s_http_downloader)
  {
    Log_ErrorPrint("Failed to create HTTP downloader, cannot use cheevos");
    return false;
  }

  g_active = true;
  g_challenge_mode = challenge_mode;
#ifdef WITH_RAINTEGRATION
  g_using_raintegration = false;
#endif

  s_test_mode = test_mode;
  s_unofficial_test_mode = include_unofficial;
  s_use_first_disc_from_playlist = use_first_disc_from_playlist;
  s_rich_presence_enabled = enable_rich_presence;
  rc_runtime_init(&s_rcheevos_runtime);

  s_last_ping_time.Reset();
  s_username = g_host_interface->GetStringSettingValue("Cheevos", "Username");
  s_login_token = g_host_interface->GetStringSettingValue("Cheevos", "Token");
  s_logged_in = (!s_username.empty() && !s_login_token.empty());

  if (IsLoggedIn() && System::IsValid())
    GameChanged();

  return true;
}

void Cheevos::Reset()
{
  if (!g_active)
    return;

#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RA_OnReset();
    return;
  }
#endif

  Log_DevPrint("Resetting rcheevos state...");
  rc_runtime_reset(&s_rcheevos_runtime);
}

void Cheevos::Shutdown()
{
  if (!g_active)
    return;

  Assert(!s_image_download_progress_active);

  s_http_downloader->WaitForAllRequests();

  ClearGameInfo();
  ClearGamePath();
  std::string().swap(s_username);
  std::string().swap(s_login_token);
  s_logged_in = false;
  g_host_interface->OnAchievementsRefreshed();

  g_active = false;
  rc_runtime_destroy(&s_rcheevos_runtime);

  s_http_downloader.reset();
}

void Cheevos::Update()
{
  s_http_downloader->PollRequests();

  if (HasActiveGame())
  {
#ifdef WITH_RAINTEGRATION
    if (IsUsingRAIntegration())
    {
      if (g_raintegration_initialized)
        RA_DoAchievementsFrame();

      return;
    }
#endif

    rc_runtime_do_frame(&s_rcheevos_runtime, &CheevosEventHandler, &CheevosPeek, nullptr, nullptr);
    UpdateRichPresence();

    if (!s_test_mode)
    {
      const s32 ping_frequency =
        s_rich_presence_enabled ? RICH_PRESENCE_PING_FREQUENCY : NO_RICH_PRESENCE_PING_FREQUENCY;
      if (static_cast<s32>(s_last_ping_time.GetTimeSeconds()) >= ping_frequency)
        SendPing();
    }
  }
}

bool Cheevos::DoState(StateWrapper& sw)
{
  // if we're inactive, we still need to skip the data (if any)
  if (!g_active)
  {
    u32 data_size = 0;
    sw.Do(&data_size);
    if (data_size > 0)
      sw.SkipBytes(data_size);

    return !sw.HasError();
  }

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
      {
        if (g_raintegration_initialized)
          RA_OnReset();
      }
      else
      {
        rc_runtime_reset(&s_rcheevos_runtime);
      }
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
    if (IsUsingRAIntegration() && g_raintegration_initialized)
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
      if (g_raintegration_initialized)
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

bool Cheevos::IsLoggedIn()
{
  return s_logged_in;
}

bool Cheevos::IsTestModeActive()
{
  return s_test_mode;
}

bool Cheevos::IsUnofficialTestModeActive()
{
  return s_unofficial_test_mode;
}

bool Cheevos::IsUsingFirstDiscFromPlaylist()
{
  return s_use_first_disc_from_playlist;
}

bool Cheevos::IsRichPresenceEnabled()
{
  return s_rich_presence_enabled;
}

const std::string& Cheevos::GetUsername()
{
  return s_username;
}

const std::string& Cheevos::GetRichPresenceString()
{
  return s_rich_presence_string;
}

void Cheevos::LoginCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  rapidjson::Document doc;
  if (!ParseResponseJSON("Login", status_code, data, doc))
    return;

  if (!doc["User"].IsString() || !doc["Token"].IsString())
  {
    FormattedError("Login failed. Please check your user name and password, and try again.");
    return;
  }

  std::string username = doc["User"].GetString();
  std::string login_token = doc["Token"].GetString();

  // save to config
  {
    std::lock_guard<std::recursive_mutex> guard(g_host_interface->GetSettingsLock());
    g_host_interface->GetSettingsInterface()->SetStringValue("Cheevos", "Username", username.c_str());
    g_host_interface->GetSettingsInterface()->SetStringValue("Cheevos", "Token", login_token.c_str());
    g_host_interface->GetSettingsInterface()->SetStringValue(
      "Cheevos", "LoginTimestamp", TinyString::FromFormat("%" PRIu64, static_cast<u64>(std::time(nullptr))));
    g_host_interface->GetSettingsInterface()->Save();
  }

  if (g_active)
  {
    s_username = std::move(username);
    s_login_token = std::move(login_token);
    s_logged_in = true;

    // If we have a game running, set it up.
    if (System::IsValid())
      GameChanged();
  }
}

void Cheevos::LoginASyncCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  if (ImGuiFullscreen::IsInitialized())
    ImGuiFullscreen::CloseBackgroundProgressDialog("cheevos_async_login");

  LoginCallback(status_code, data);
}

void Cheevos::SendLogin(const char* username, const char* password, FrontendCommon::HTTPDownloader* http_downloader,
                        FrontendCommon::HTTPDownloader::Request::Callback callback)
{
  char url[768] = {};
  int res = rc_url_login_with_password(url, sizeof(url), username, password);
  Assert(res == 0);

  http_downloader->CreateRequest(url, std::move(callback));
}

bool Cheevos::LoginAsync(const char* username, const char* password)
{
  s_http_downloader->WaitForAllRequests();

  if (s_logged_in || std::strlen(username) == 0 || std::strlen(password) == 0 || IsUsingRAIntegration())
    return false;

  if (ImGuiFullscreen::IsInitialized())
  {
    ImGuiFullscreen::OpenBackgroundProgressDialog(
      "cheevos_async_login", g_host_interface->TranslateStdString("Cheevos", "Logging in to RetroAchivements..."), 0, 1,
      0);
  }

  SendLogin(username, password, s_http_downloader.get(), LoginASyncCallback);
  return true;
}

bool Cheevos::Login(const char* username, const char* password)
{
  if (g_active)
    s_http_downloader->WaitForAllRequests();

  if (s_logged_in || std::strlen(username) == 0 || std::strlen(password) == 0 || IsUsingRAIntegration())
    return false;

  if (g_active)
  {
    SendLogin(username, password, s_http_downloader.get(), LoginCallback);
    s_http_downloader->WaitForAllRequests();
    return IsLoggedIn();
  }

  // create a temporary downloader if we're not initialized
  Assert(!g_active);
  std::unique_ptr<FrontendCommon::HTTPDownloader> http_downloader =
    FrontendCommon::HTTPDownloader::Create(GetUserAgent().c_str());
  if (!http_downloader)
    return false;

  SendLogin(username, password, http_downloader.get(), LoginCallback);
  http_downloader->WaitForAllRequests();

  return !g_host_interface->GetStringSettingValue("Cheevos", "Token").empty();
}

void Cheevos::Logout()
{
  if (g_active)
  {
    s_http_downloader->WaitForAllRequests();
    if (s_logged_in)
    {
      ClearGameInfo();
      std::string().swap(s_username);
      std::string().swap(s_login_token);
      s_logged_in = false;
      g_host_interface->OnAchievementsRefreshed();
    }
  }

  // remove from config
  std::lock_guard<std::recursive_mutex> guard(g_host_interface->GetSettingsLock());
  {
    g_host_interface->GetSettingsInterface()->DeleteValue("Cheevos", "Username");
    g_host_interface->GetSettingsInterface()->DeleteValue("Cheevos", "Token");
    g_host_interface->GetSettingsInterface()->DeleteValue("Cheevos", "LoginTimestamp");
    g_host_interface->GetSettingsInterface()->Save();
  }
}

void Cheevos::UpdateImageDownloadProgress()
{
  static const char* str_id = "cheevo_image_download";

  if (s_completed_image_downloads >= s_total_image_downloads)
  {
    s_completed_image_downloads = 0;
    s_total_image_downloads = 0;

    if (s_image_download_progress_active)
    {
      ImGuiFullscreen::CloseBackgroundProgressDialog(str_id);
      s_image_download_progress_active = false;
    }

    return;
  }

  if (!ImGuiFullscreen::IsInitialized())
    return;

  std::string message(g_host_interface->TranslateStdString("Cheevos", "Downloading achievement resources..."));
  if (!s_image_download_progress_active)
  {
    ImGuiFullscreen::OpenBackgroundProgressDialog(str_id, std::move(message), 0,
                                                  static_cast<s32>(s_total_image_downloads),
                                                  static_cast<s32>(s_completed_image_downloads));
    s_image_download_progress_active = true;
  }
  else
  {
    ImGuiFullscreen::UpdateBackgroundProgressDialog(str_id, std::move(message), 0,
                                                    static_cast<s32>(s_total_image_downloads),
                                                    static_cast<s32>(s_completed_image_downloads));
  }
}

void Cheevos::DownloadImage(std::string url, std::string cache_filename)
{
  auto callback = [cache_filename](s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data) {
    s_completed_image_downloads++;
    UpdateImageDownloadProgress();

    if (status_code != HTTP_OK)
      return;

    if (!FileSystem::WriteBinaryFile(cache_filename.c_str(), data.data(), data.size()))
    {
      Log_ErrorPrintf("Failed to write badge image to '%s'", cache_filename.c_str());
      return;
    }

    ImGuiFullscreen::InvalidateCachedTexture(cache_filename);
    UpdateImageDownloadProgress();
  };

  s_total_image_downloads++;
  UpdateImageDownloadProgress();

  s_http_downloader->CreateRequest(std::move(url), std::move(callback));
}

std::string Cheevos::GetBadgeImageFilename(const char* badge_name, bool locked, bool cache_path)
{
  if (!cache_path)
  {
    return fmt::format("%s%s.png", badge_name, locked ? "_lock" : "");
  }
  else
  {
    // well, this comes from the internet.... :)
    std::string clean_name(badge_name);
    Path::SanitizeFileName(clean_name);
    return g_host_interface->GetUserDirectoryRelativePath("cache" FS_OSPATH_SEPARATOR_STR
                                                          "achievement_badge" FS_OSPATH_SEPARATOR_STR "%s%s.png",
                                                          clean_name.c_str(), locked ? "_lock" : "");
  }
}

std::string Cheevos::ResolveBadgePath(const char* badge_name, bool locked)
{
  char url[256];

  // unlocked image
  std::string cache_path(GetBadgeImageFilename(badge_name, locked, true));
  if (FileSystem::FileExists(cache_path.c_str()))
    return cache_path;

  std::string badge_name_with_extension(GetBadgeImageFilename(badge_name, locked, false));
  int res = rc_url_get_badge_image(url, sizeof(url), badge_name_with_extension.c_str());
  Assert(res == 0);
  DownloadImage(url, cache_path);
  return cache_path;
}

void Cheevos::DisplayAchievementSummary()
{
  std::string title = s_game_title;
  if (g_challenge_mode)
    title += g_host_interface->TranslateString("Cheevos", " (Hardcore Mode)");

  std::string summary;
  if (GetAchievementCount() > 0)
  {
    summary = StringUtil::StdStringFromFormat(
      g_host_interface->TranslateString("Cheevos", "You have earned %u of %u achievements, and %u of %u points."),
      GetUnlockedAchiementCount(), GetAchievementCount(), GetCurrentPointsForGame(), GetMaximumPointsForGame());
  }
  else
  {
    summary = g_host_interface->TranslateString("Cheevos", "This game has no achievements.");
  }
  if (GetLeaderboardCount() > 0)
  {
    summary.push_back('\n');
    if (g_challenge_mode)
    {
      summary.append(g_host_interface->TranslateString("Cheevos", "Leaderboards are enabled."));
    }
    else
    {
      summary.append(
        g_host_interface->TranslateString("Cheevos", "Leaderboards are DISABLED because Hardcore Mode is off."));
    }
  }

  ImGuiFullscreen::AddNotification(10.0f, std::move(title), std::move(summary), s_game_icon);
}

void Cheevos::GetUserUnlocksCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  rapidjson::Document doc;
  if (!ParseResponseJSON("Get User Unlocks", status_code, data, doc))
  {
    ClearGameInfo(true, false);
    return;
  }

  // verify game id for sanity
  const u32 game_id = GetOptionalUInt(doc, "GameID");
  if (game_id != g_game_id)
  {
    FormattedError("GameID from user unlocks doesn't match (got %u expected %u)", game_id, g_game_id);
    ClearGameInfo(true, false);
    return;
  }

  // flag achievements as unlocked
  if (doc.HasMember("UserUnlocks") && doc["UserUnlocks"].IsArray())
  {
    for (const auto& value : doc["UserUnlocks"].GetArray())
    {
      if (!value.IsUint())
        continue;

      const u32 achievement_id = value.GetUint();
      Achievement* cheevo = GetAchievementByID(achievement_id);
      if (!cheevo)
      {
        Log_ErrorPrintf("Server returned unknown achievement %u", achievement_id);
        continue;
      }

      cheevo->locked = false;
    }
  }

  // start scanning for locked achievements
  ActivateLockedAchievements();
  DisplayAchievementSummary();
  SendPlaying();
  UpdateRichPresence();
  SendPing();
  g_host_interface->OnAchievementsRefreshed();
}

void Cheevos::GetUserUnlocks()
{
  char url[512];
  int res = rc_url_get_unlock_list(url, sizeof(url), s_username.c_str(), s_login_token.c_str(), g_game_id,
                                   static_cast<int>(g_challenge_mode));
  Assert(res == 0);

  s_http_downloader->CreateRequest(url, GetUserUnlocksCallback);
}

void Cheevos::GetPatchesCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  ClearGameInfo();

  rapidjson::Document doc;
  if (!ParseResponseJSON("Get Patches", status_code, data, doc))
    return;

  if (!doc.HasMember("PatchData") || !doc["PatchData"].IsObject())
  {
    FormattedError("No patch data returned from server.");
    return;
  }

  // parse info
  const auto patch_data(doc["PatchData"].GetObject());
  if (!patch_data["ID"].IsUint())
  {
    FormattedError("Patch data is missing game ID");
    return;
  }

  g_game_id = GetOptionalUInt(patch_data, "ID");
  s_game_title = GetOptionalString(patch_data, "Title");
  s_game_developer = GetOptionalString(patch_data, "Developer");
  s_game_publisher = GetOptionalString(patch_data, "Publisher");
  s_game_release_date = GetOptionalString(patch_data, "Released");

  // try for a icon
  std::string icon_name(GetOptionalString(patch_data, "ImageIcon"));
  if (!icon_name.empty())
  {
    s_game_icon = g_host_interface->GetUserDirectoryRelativePath(
      "cache" FS_OSPATH_SEPARATOR_STR "achievement_gameicon" FS_OSPATH_SEPARATOR_STR "%u.png", g_game_id);
    if (!FileSystem::FileExists(s_game_icon.c_str()))
    {
      // for some reason rurl doesn't have this :(
      std::string icon_url(StringUtil::StdStringFromFormat("http://i.retroachievements.org%s", icon_name.c_str()));
      DownloadImage(std::move(icon_url), s_game_icon);
    }
  }

  // parse achievements
  if (patch_data.HasMember("Achievements") && patch_data["Achievements"].IsArray())
  {
    const auto achievements(patch_data["Achievements"].GetArray());
    for (const auto& achievement : achievements)
    {
      if (!achievement.HasMember("ID") || !achievement["ID"].IsNumber() || !achievement.HasMember("Flags") ||
          !achievement["Flags"].IsNumber() || !achievement.HasMember("MemAddr") || !achievement["MemAddr"].IsString() ||
          !achievement.HasMember("Title") || !achievement["Title"].IsString())
      {
        continue;
      }

      const u32 id = achievement["ID"].GetUint();
      const AchievementCategory category = static_cast<AchievementCategory>(achievement["Flags"].GetUint());
      const char* memaddr = achievement["MemAddr"].GetString();
      std::string title = achievement["Title"].GetString();
      std::string description = GetOptionalString(achievement, "Description");
      std::string badge_name = GetOptionalString(achievement, "BadgeName");
      const u32 points = GetOptionalUInt(achievement, "Points");

      // Skip local and unofficial achievements for now, unless "Test Unofficial Achievements" is enabled
      if (!s_unofficial_test_mode &&
          (category == AchievementCategory::Local || category == AchievementCategory::Unofficial))
      {
        Log_WarningPrintf("Skipping unofficial achievement %u (%s)", id, title.c_str());
        continue;
      }

      if (GetAchievementByID(id))
      {
        Log_ErrorPrintf("Achievement %u already exists", id);
        continue;
      }

      Achievement cheevo;
      cheevo.id = id;
      cheevo.memaddr = memaddr;
      cheevo.title = std::move(title);
      cheevo.description = std::move(description);
      cheevo.locked = true;
      cheevo.active = false;
      cheevo.points = points;
      cheevo.category = category;

      if (!badge_name.empty())
      {
        cheevo.locked_badge_path = ResolveBadgePath(badge_name.c_str(), true);
        cheevo.unlocked_badge_path = ResolveBadgePath(badge_name.c_str(), false);
      }

      s_achievements.push_back(std::move(cheevo));
    }
  }

  // parse leaderboards
  if (patch_data.HasMember("Leaderboards") && patch_data["Leaderboards"].IsArray())
  {
    const auto leaderboards(patch_data["Leaderboards"].GetArray());
    for (const auto& leaderboard : leaderboards)
    {
      if (!leaderboard.HasMember("ID") || !leaderboard["ID"].IsNumber() || !leaderboard.HasMember("Mem") ||
          !leaderboard["Mem"].IsString() || !leaderboard.HasMember("Title") || !leaderboard["Title"].IsString() ||
          !leaderboard.HasMember("Format") || !leaderboard["Format"].IsString())
      {
        continue;
      }

      const unsigned int id = leaderboard["ID"].GetUint();
      const char* title = leaderboard["Title"].GetString();
      const char* memaddr = leaderboard["Mem"].GetString();
      const char* format = leaderboard["Format"].GetString();
      std::string description = GetOptionalString(leaderboard, "Description");

      Leaderboard lboard;
      lboard.id = id;
      lboard.title = title;
      lboard.description = std::move(description);
      lboard.format = rc_parse_format(format);
      s_leaderboards.push_back(std::move(lboard));

      const int err = rc_runtime_activate_lboard(&s_rcheevos_runtime, id, memaddr, nullptr, 0);
      if (err != RC_OK)
      {
        Log_ErrorPrintf("Leaderboard %u memaddr parse error: %s", id, rc_error_str(err));
      }
      else
      {
        Log_DevPrintf("Activated leaderboard %s (%u)", title, id);
      }
    }
  }

  // parse rich presence
  if (s_rich_presence_enabled && patch_data.HasMember("RichPresencePatch") &&
      patch_data["RichPresencePatch"].IsString())
  {
    const char* patch = patch_data["RichPresencePatch"].GetString();
    int res = rc_runtime_activate_richpresence(&s_rcheevos_runtime, patch, nullptr, 0);
    if (res == RC_OK)
      s_has_rich_presence = true;
    else
      Log_WarningPrintf("Failed to activate rich presence: %s", rc_error_str(res));
  }

  Log_InfoPrintf("Game Title: %s", s_game_title.c_str());
  Log_InfoPrintf("Game Developer: %s", s_game_developer.c_str());
  Log_InfoPrintf("Game Publisher: %s", s_game_publisher.c_str());
  Log_InfoPrintf("Achievements: %zu", s_achievements.size());
  Log_InfoPrintf("Leaderboards: %zu", s_leaderboards.size());

  if (!s_achievements.empty() || s_has_rich_presence)
  {
    if (!s_test_mode)
    {
      GetUserUnlocks();
    }
    else
    {
      ActivateLockedAchievements();
      DisplayAchievementSummary();
      g_host_interface->OnAchievementsRefreshed();
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

void Cheevos::GetLbInfoCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  rapidjson::Document doc;
  if (!ParseResponseJSON("Get Leaderboard Info", status_code, data, doc))
    return;

  if (!doc.HasMember("LeaderboardData") || !doc["LeaderboardData"].IsObject())
  {
    FormattedError("No leaderboard returned from server.");
    return;
  }

  // parse info
  const auto lb_data(doc["LeaderboardData"].GetObject());
  if (!lb_data["LBID"].IsUint())
  {
    FormattedError("Leaderboard data is missing leadeboard ID");
    return;
  }

  const u32 lbid = lb_data["LBID"].GetUint();
  if (lbid != s_last_queried_lboard)
  {
    // User has already requested another leaderboard, drop this data
    return;
  }

  if (lb_data.HasMember("Entries") && lb_data["Entries"].IsArray())
  {
    const Leaderboard* leaderboard = GetLeaderboardByID(lbid);
    if (leaderboard == nullptr)
    {
      Log_ErrorPrintf("Attempting to list unknown leaderboard %u", lbid);
      return;
    }

    std::vector<LeaderboardEntry> entries;

    const auto lb_entries(lb_data["Entries"].GetArray());
    for (const auto& entry : lb_entries)
    {
      if (!entry.HasMember("User") || !entry["User"].IsString() || !entry.HasMember("Score") ||
          !entry["Score"].IsNumber() || !entry.HasMember("Rank") || !entry["Rank"].IsNumber())
      {
        continue;
      }

      char score[128];
      rc_runtime_format_lboard_value(score, sizeof(score), entry["Score"].GetInt(), leaderboard->format);

      LeaderboardEntry lbe;
      lbe.user = entry["User"].GetString();
      lbe.rank = entry["Rank"].GetUint();
      lbe.formatted_score = score;
      lbe.is_self = lbe.user == s_username;

      entries.push_back(std::move(lbe));
    }

    s_lboard_entries = std::move(entries);
  }
}

void Cheevos::GetPatches(u32 game_id)
{
  char url[512];
  int res = rc_url_get_patch(url, sizeof(url), s_username.c_str(), s_login_token.c_str(), game_id);
  Assert(res == 0);

  s_http_downloader->CreateRequest(url, GetPatchesCallback);
}

std::string Cheevos::GetGameHash(CDImage* cdi)
{
  std::string executable_name;
  std::vector<u8> executable_data;
  if (!System::ReadExecutableFromImage(cdi, &executable_name, &executable_data))
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

void Cheevos::GetGameIdCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  rapidjson::Document doc;
  if (!ParseResponseJSON("Get Game ID", status_code, data, doc))
    return;

  const u32 game_id = (doc.HasMember("GameID") && doc["GameID"].IsUint()) ? doc["GameID"].GetUint() : 0;
  Log_InfoPrintf("Server returned GameID %u", game_id);
  if (game_id == 0)
    return;

  GetPatches(game_id);
}

void Cheevos::GameChanged()
{
  Assert(System::IsValid());

  const std::string& path = System::GetRunningPath();
  if (path.empty() || s_game_path == path)
    return;

  std::unique_ptr<CDImage> cdi = CDImage::Open(path.c_str(), nullptr);
  if (!cdi)
  {
    Log_ErrorPrintf("Failed to open temporary CD image '%s'", path.c_str());
    ClearGameInfo();
    return;
  }

  GameChanged(path, cdi.get());
}

void Cheevos::GameChanged(const std::string& path, CDImage* image)
{
  if (s_game_path == path)
    return;

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

  ClearGameInfo();
  ClearGamePath();
  s_game_path = path;
  s_game_hash = std::move(game_hash);
  if (!image)
    return;

  if (s_game_hash.empty())
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("OSDMessage", "Failed to read executable from disc. Achievements disabled."),
      10.0f);
    return;
  }

#ifdef WITH_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RAIntegration::GameChanged();
    return;
  }
#endif

  char url[256];
  int res = rc_url_get_gameid(url, sizeof(url), s_game_hash.c_str());
  Assert(res == 0);

  s_http_downloader->CreateRequest(url, GetGameIdCallback);
}

void Cheevos::SendPlayingCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  rapidjson::Document doc;
  if (!ParseResponseJSON("Post Activity", status_code, data, doc))
    return;

  Log_InfoPrintf("Playing game updated to %u (%s)", g_game_id, s_game_title.c_str());
}

void Cheevos::SendPlaying()
{
  if (!HasActiveGame())
    return;

  char url[512];
  int res = rc_url_post_playing(url, sizeof(url), s_username.c_str(), s_login_token.c_str(), g_game_id);
  Assert(res == 0);

  s_http_downloader->CreateRequest(url, SendPlayingCallback);
}

void Cheevos::UpdateRichPresence()
{
  if (!s_has_rich_presence)
    return;

  char buffer[512];
  int res = rc_runtime_get_richpresence(&s_rcheevos_runtime, buffer, sizeof(buffer), CheevosPeek, nullptr, nullptr);
  if (res <= 0)
  {
    const bool had_rich_presence = !s_rich_presence_string.empty();
    s_rich_presence_string.clear();
    if (had_rich_presence)
      g_host_interface->OnAchievementsRefreshed();

    return;
  }

  if (s_rich_presence_string == buffer)
    return;

  s_rich_presence_string.assign(buffer);
  g_host_interface->OnAchievementsRefreshed();
}

void Cheevos::SendPingCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  rapidjson::Document doc;
  if (!ParseResponseJSON("Ping", status_code, data, doc))
    return;
}

void Cheevos::SendPing()
{
  if (!HasActiveGame())
    return;

  char url[512];
  char post_data[512];
  int res = rc_url_ping(url, sizeof(url), post_data, sizeof(post_data), s_username.c_str(), s_login_token.c_str(),
                        g_game_id, s_rich_presence_string.c_str());
  Assert(res == 0);

  s_http_downloader->CreatePostRequest(url, post_data, SendPingCallback);
  s_last_ping_time.Reset();
}

const std::string& Cheevos::GetGameTitle()
{
  return s_game_title;
}

const std::string& Cheevos::GetGameDeveloper()
{
  return s_game_developer;
}

const std::string& Cheevos::GetGamePublisher()
{
  return s_game_publisher;
}

const std::string& Cheevos::GetGameReleaseDate()
{
  return s_game_release_date;
}

const std::string& Cheevos::GetGameIcon()
{
  return s_game_icon;
}

bool Cheevos::EnumerateAchievements(std::function<bool(const Achievement&)> callback)
{
  for (const Achievement& cheevo : s_achievements)
  {
    if (!callback(cheevo))
      return false;
  }

  return true;
}

u32 Cheevos::GetUnlockedAchiementCount()
{
  u32 count = 0;
  for (const Achievement& cheevo : s_achievements)
  {
    if (!cheevo.locked)
      count++;
  }

  return count;
}

u32 Cheevos::GetAchievementCount()
{
  return static_cast<u32>(s_achievements.size());
}

u32 Cheevos::GetMaximumPointsForGame()
{
  u32 points = 0;
  for (const Achievement& cheevo : s_achievements)
    points += cheevo.points;

  return points;
}

u32 Cheevos::GetCurrentPointsForGame()
{
  u32 points = 0;
  for (const Achievement& cheevo : s_achievements)
  {
    if (!cheevo.locked)
      points += cheevo.points;
  }

  return points;
}

bool Cheevos::EnumerateLeaderboards(std::function<bool(const Leaderboard&)> callback)
{
  for (const Leaderboard& lboard : s_leaderboards)
  {
    if (!callback(lboard))
      return false;
  }

  return true;
}

std::optional<bool> Cheevos::TryEnumerateLeaderboardEntries(u32 id,
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
    char url[512];

    // Just over what a single page can store, should be a reasonable amount for now
    rc_url_get_lboard_entries_near_user(url, sizeof(url), id, s_username.c_str(), 15);
    s_http_downloader->CreateRequest(url, GetLbInfoCallback);
  }

  return std::nullopt;
}

const Cheevos::Leaderboard* Cheevos::GetLeaderboardByID(u32 id)
{
  for (const Leaderboard& lb : s_leaderboards)
  {
    if (lb.id == id)
      return &lb;
  }

  return nullptr;
}

u32 Cheevos::GetLeaderboardCount()
{
  return static_cast<u32>(s_leaderboards.size());
}

bool Cheevos::IsLeaderboardTimeType(const Leaderboard& leaderboard)
{
  return leaderboard.format != RC_FORMAT_SCORE && leaderboard.format != RC_FORMAT_VALUE;
}

void Cheevos::ActivateLockedAchievements()
{
  for (Achievement& cheevo : s_achievements)
  {
    if (cheevo.locked)
      ActivateAchievement(&cheevo);
  }
}

bool Cheevos::ActivateAchievement(Achievement* achievement)
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

void Cheevos::DeactivateAchievement(Achievement* achievement)
{
  if (!achievement->active)
    return;

  rc_runtime_deactivate_achievement(&s_rcheevos_runtime, achievement->id);
  achievement->active = false;

  Log_DevPrintf("Deactivated achievement %s (%u)", achievement->title.c_str(), achievement->id);
}

void Cheevos::UnlockAchievementCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  rapidjson::Document doc;
  if (!ParseResponseJSON("Award Cheevo", status_code, data, doc))
    return;

  // we don't really need to do anything here
}

void Cheevos::SubmitLeaderboardCallback(s32 status_code, const FrontendCommon::HTTPDownloader::Request::Data& data)
{
  // Force the next leaderboard query to repopulate everything, just in case the user wants to see their new score
  s_last_queried_lboard = 0;
}

void Cheevos::UnlockAchievement(u32 achievement_id, bool add_notification /* = true*/)
{
  Achievement* achievement = GetAchievementByID(achievement_id);
  if (!achievement)
  {
    Log_ErrorPrintf("Attempting to unlock unknown achievement %u", achievement_id);
    return;
  }
  else if (!achievement->locked)
  {
    Log_WarningPrintf("Achievement %u for game %u is already unlocked", achievement_id, g_game_id);
    return;
  }

  achievement->locked = false;
  DeactivateAchievement(achievement);

  Log_InfoPrintf("Achievement %s (%u) for game %u unlocked", achievement->title.c_str(), achievement_id, g_game_id);

  std::string title;
  switch (achievement->category)
  {
    case AchievementCategory::Local:
      title = StringUtil::StdStringFromFormat("%s (Local)", achievement->title.c_str());
      break;
    case AchievementCategory::Unofficial:
      title = StringUtil::StdStringFromFormat("%s (Unofficial)", achievement->title.c_str());
      break;
    case AchievementCategory::Core:
    default:
      title = achievement->title;
      break;
  }

  ImGuiFullscreen::AddNotification(15.0f, std::move(title), achievement->description, achievement->unlocked_badge_path);

  if (s_test_mode)
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

  char url[512];
  rc_url_award_cheevo(url, sizeof(url), s_username.c_str(), s_login_token.c_str(), achievement_id,
                      static_cast<int>(g_challenge_mode), s_game_hash.c_str());
  s_http_downloader->CreateRequest(url, UnlockAchievementCallback);
}

void Cheevos::SubmitLeaderboard(u32 leaderboard_id, int value)
{
  if (s_test_mode)
  {
    Log_WarningPrintf("Skipping sending leaderboard %u result to server because of test mode.", leaderboard_id);
    return;
  }

  if (!g_challenge_mode)
  {
    Log_WarningPrintf("Skipping sending leaderboard %u result to server because Challenge mode is off.",
                      leaderboard_id);
    return;
  }

  char url[512];
  rc_url_submit_lboard(url, sizeof(url), s_username.c_str(), s_login_token.c_str(), leaderboard_id, value);
  s_http_downloader->CreateRequest(url, SubmitLeaderboardCallback);
}

std::pair<u32, u32> Cheevos::GetAchievementProgress(const Achievement& achievement)
{
  std::pair<u32, u32> result;
  rc_runtime_get_achievement_measured(&s_rcheevos_runtime, achievement.id, &result.first, &result.second);
  return result;
}

TinyString Cheevos::GetAchievementProgressText(const Achievement& achievement)
{
  TinyString result;
  rc_runtime_format_achievement_measured(&s_rcheevos_runtime, achievement.id, result.GetWriteableCharArray(),
                                         result.GetWritableBufferSize());
  result.UpdateSize();
  return result;
}

void Cheevos::CheevosEventHandler(const rc_runtime_event_t* runtime_event)
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

unsigned Cheevos::CheevosPeek(unsigned address, unsigned num_bytes, void* ud)
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

namespace Cheevos::RAIntegration {
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
} // namespace Cheevos::RAIntegration

void Cheevos::SwitchToRAIntegration()
{
  g_using_raintegration = true;
  g_raintegration_initialized = false;
  g_active = true;
  s_logged_in = true;
}

void Cheevos::RAIntegration::InitializeRAIntegration(void* main_window_handle)
{
  RA_InitClient((HWND)main_window_handle, "DuckStation", g_scm_tag_str);
  RA_SetUserAgentDetail(Cheevos::GetUserAgent().c_str());

  RA_InstallSharedFunctions(RACallbackIsActive, RACallbackCauseUnpause, RACallbackCausePause, RACallbackRebuildMenu,
                            RACallbackEstimateTitle, RACallbackResetEmulator, RACallbackLoadROM);
  RA_SetConsoleID(PlayStation);

  // Apparently this has to be done early, or the memory inspector doesn't work.
  // That's a bit unfortunate, because the RAM size can vary between games, and depending on the option.
  RA_InstallMemoryBank(0, RACallbackReadMemory, RACallbackWriteMemory, Bus::RAM_2MB_SIZE);

  // Fire off a login anyway. Saves going into the menu and doing it.
  RA_AttemptLogin(0);

  g_challenge_mode = RA_HardcoreModeIsActive() != 0;
  g_raintegration_initialized = true;

  // this is pretty lame, but we may as well persist until we exit anyway
  std::atexit(RA_Shutdown);
}

void Cheevos::RAIntegration::MainWindowChanged(void* new_handle)
{
  if (g_raintegration_initialized)
  {
    RA_UpdateHWnd((HWND)new_handle);
    return;
  }

  InitializeRAIntegration(new_handle);
}

void Cheevos::RAIntegration::GameChanged()
{
  g_game_id = RA_IdentifyHash(s_game_hash.c_str());
  RA_ActivateGame(g_game_id);
}

std::vector<std::pair<int, const char*>> Cheevos::RAIntegration::GetMenuItems()
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

void Cheevos::RAIntegration::ActivateMenuItem(int item)
{
  RA_InvokeDialog(item);
}

int Cheevos::RAIntegration::RACallbackIsActive()
{
  return static_cast<int>(HasActiveGame());
}

void Cheevos::RAIntegration::RACallbackCauseUnpause()
{
  if (System::IsValid())
    g_host_interface->PauseSystem(false);
}

void Cheevos::RAIntegration::RACallbackCausePause()
{
  if (System::IsValid())
    g_host_interface->PauseSystem(true);
}

void Cheevos::RAIntegration::RACallbackRebuildMenu()
{
  // unused, we build the menu on demand
}

void Cheevos::RAIntegration::RACallbackEstimateTitle(char* buf)
{
  StringUtil::Strlcpy(buf, System::GetRunningTitle(), 256);
}

void Cheevos::RAIntegration::RACallbackResetEmulator()
{
  g_challenge_mode = RA_HardcoreModeIsActive() != 0;
  if (System::IsValid())
    g_host_interface->ResetSystem();
}

void Cheevos::RAIntegration::RACallbackLoadROM(const char* unused)
{
  // unused
  UNREFERENCED_PARAMETER(unused);
}

unsigned char Cheevos::RAIntegration::RACallbackReadMemory(unsigned int address)
{
  if (!System::IsValid())
    return 0;

  u8 value = 0;
  CPU::SafeReadMemoryByte(address, &value);
  return value;
}

void Cheevos::RAIntegration::RACallbackWriteMemory(unsigned int address, unsigned char value)
{
  if (!System::IsValid())
    return;

  CPU::SafeWriteMemoryByte(address, value);
}

#endif

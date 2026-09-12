// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/achievements_private.h"
#include "core/core.h"
#include "core/core_private.h"
#include "core/cpu_core_private.h"
#include "core/fullscreenui.h"
#include "core/fullscreenui_widgets.h"
#include "core/game_list.h"
#include "core/gdb_server.h"
#include "core/gpu_backend.h"
#include "core/host.h"
#include "core/performance_counters.h"
#include "core/system_private.h"
#include "core/video_thread_private.h"

#include "util/http_downloader.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/translation.h"
#include "util/window_info.h"

#include "common/log.h"
#include "common/time_helpers.h"

LOG_CHANNEL(Host);

void Host::OnSettingsReloaded()
{
}

void Host::CommitBaseSettingChanges()
{
}

bool Host::ResourceFileExists(std::string_view filename, bool allow_override)
{
  return false;
}

std::optional<DynamicHeapArray<u8>> Host::ReadResourceFile(std::string_view filename, bool allow_override, Error* error)
{
  return std::nullopt;
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool allow_override, Error* error)
{
  return std::nullopt;
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(std::string_view filename, bool allow_override)
{
  return std::nullopt;
}

void Host::ReportFatalError(std::string_view title, std::string_view message)
{
  ERROR_LOG("ReportFatalError: {}", message);
  abort();
}

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::ReportStatusMessage(std::string_view message)
{
  INFO_LOG("ReportStatusMessage: {}", message);
}

void Host::ConfirmMessageAsync(std::string_view icon, std::string_view title, std::string_view message,
                               ConfirmMessageAsyncCallback callback, std::string_view yes_text,
                               std::string_view no_text)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ConfirmMessage: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ConfirmMessage: {}", message);

  callback(true);
}

void Host::ReportDebuggerEvent(CPU::DebuggerEvent event, std::string_view message)
{
  ERROR_LOG("ReportDebuggerEvent: {}", message);
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  return {};
}

const char* Host::GetLanguageName(std::string_view language_code)
{
  return "";
}

bool Host::ChangeLanguage(const char* new_language)
{
  return false;
}

s32 Host::Internal::GetTranslatedStringImpl(std::string_view context, std::string_view msg,
                                            std::string_view disambiguation, char* tbuf, size_t tbuf_space)
{
  if (msg.size() > tbuf_space)
    return -1;
  else if (msg.empty())
    return 0;

  std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
  TinyString count_str = TinyString::from_format("{}", count);

  std::string ret(msg);
  for (;;)
  {
    std::string::size_type pos = ret.find("%n");
    if (pos == std::string::npos)
      break;

    ret.replace(pos, pos + 2, count_str.view());
  }

  return ret;
}

TinyString Host::TranslatePluralToTinyString(const char* context, const char* msg, const char* disambiguation,
                                             int count)
{
  TinyString ret(msg);
  ret.replace("%n", TinyString::from_format("{}", count));
  return ret;
}

SmallString Host::TranslatePluralToSmallString(const char* context, const char* msg, const char* disambiguation,
                                               int count)
{
  SmallString ret(msg);
  ret.replace("%n", TinyString::from_format("{}", count));
  return ret;
}

static TinyString FormatDateOrTime(const char* format, std::time_t timestamp)
{
  TinyString ret;
  if (const std::optional<std::tm> ltime = Common::LocalTime(timestamp))
    ret.resize(static_cast<u32>(std::strftime(ret.data(), ret.buffer_size(), format, &ltime.value())));
  else
    ret = "Invalid";

  return ret;
}

TinyString Host::FormatDate(std::time_t timestamp, bool long_format)
{
  return FormatDateOrTime(long_format ? "%A %B %e %Y" : "%x", timestamp);
}

TinyString Host::FormatTime(std::time_t timestamp, bool long_format)
{
  return FormatDateOrTime("%X", timestamp);
}

TinyString Host::FormatDateTime(std::time_t timestamp, bool long_format)
{
  return FormatDateOrTime(long_format ? "%c" : "%X %x", timestamp);
}

void Host::OnSystemStarting()
{
}

void Host::OnSystemStarted()
{
}

void Host::OnSystemStopping()
{
}

void Host::OnSystemDestroyed()
{
}

void Host::OnSystemPaused()
{
}

void Host::OnSystemResumed()
{
}

void Host::OnSystemAbnormalShutdown(const std::string_view reason)
{
}

void Host::OnVideoThreadRunIdleChanged(bool is_active)
{
}

bool Host::SetScreensaverInhibit(bool inhibit, Error* error)
{
  return false;
}

void Host::OnPerformanceCountersUpdated(const GPUBackend* gpu_backend)
{
}

void Host::OnSystemGameChanged(const std::string& disc_path, const std::string& game_serial,
                               const std::string& game_name, GameHash hash)
{
}

void Host::OnSystemUndoStateAvailabilityChanged(bool available, u64 timestamp)
{
}

void Host::OnMediaCaptureStarted(MediaCaptureMode mode)
{
}

void Host::OnMediaCaptureStopped()
{
}

void Host::OnHTTPDownloaderActiveChanged(bool active)
{
}

void Host::OnGDBServerActiveClientsChanged(bool has_clients)
{
}

void Host::PumpMessagesOnCoreThread()
{
}

void Host::RunOnCoreThread(std::function<void()> function, bool block /* = false */)
{
  function();
}

void Host::RunOnUIThread(std::function<void()> function, bool block /* = false */)
{
  function();
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::SetDefaultSettings(SettingsInterface& si)
{
}

void Host::OnSettingsResetToDefault(bool host, bool system, bool controller)
{
}

void Host::RequestExitApplication(bool save_state_if_running)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestSystemShutdown(bool allow_confirm, bool save_state, bool check_memcard_busy)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                                    Error* error)
{
  return std::nullopt;
}

WindowInfoType Host::GetRenderWindowInfoType()
{
  return WindowInfoType::Surfaceless;
}

void Host::ReleaseRenderWindow()
{
}

bool Host::CanChangeFullscreenMode(bool new_fullscreen_state)
{
  return false;
}

bool Host::CreateAuxiliaryRenderWindow(s32 x, s32 y, u32 width, u32 height, std::string_view title,
                                       std::string_view icon_name, AuxiliaryRenderWindowUserData userdata,
                                       AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error)
{
  return false;
}

void Host::DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowHandle handle, s32* pos_x /* = nullptr */,
                                        s32* pos_y /* = nullptr */, u32* width /* = nullptr */,
                                        u32* height /* = nullptr */)
{
}

void Host::FrameDoneOnVideoThread(GPUBackend* gpu_backend, u32 frame_number)
{
}

void Host::OpenURL(std::string_view url)
{
}

std::string Host::GetClipboardText()
{
  return {};
}

bool Host::CopyTextToClipboard(std::string_view text)
{
  return false;
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 casual_points, u32 unread_messages)
{
}

void Host::OnAchievementsActiveChanged(bool active)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

void Host::OnRAIntegrationMenuChanged()
{
}

#endif

const char* Host::GetDefaultFullscreenUITheme()
{
  return "";
}

void Host::AddFixedInputBindings(const SettingsInterface& si)
{
}

void Host::OnInputDeviceConnected(InputBindingKey key, std::string_view identifier, std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier)
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
  return std::nullopt;
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

void Host::OnGameListEntriesChanged(std::span<const u32> changed_indices)
{
}

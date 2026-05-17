// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core.h"
#include "achievements.h"
#include "core_private.h"
#include "discord_presence.h"
#include "gdb_server.h"
#include "host.h"
#include "settings.h"
#include "system.h"
#include "system_private.h"
#include "video_thread.h"
#include "video_thread_private.h"

#include "util/gpu_device.h"
#include "util/http_cache.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/layered_settings_interface.h"
#include "common/log.h"
#include "common/memmap.h"
#include "common/path.h"
#include "common/ryml_helpers.h"
#include "common/string_util.h"
#include "common/task_queue.h"
#include "common/threading.h"
#include "common/timer.h"

#include "scmversion/scmversion.h"

#include "cpuinfo.h"
#include "fmt/format.h"

#include <cstdarg>
#include <cstdlib>
#include <limits>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <ShlObj.h>
#endif

LOG_CHANNEL(Core);

namespace Core {

/// Use two async worker threads, should be enough for most tasks.
static constexpr u32 NUM_ASYNC_WORKER_THREADS = 2;

static bool SetAppRootAndResources(const char* resources_subdir, Error* error);
static bool SetDataRoot(Error* error);
static void SetDefaultSettings(SettingsInterface& si, bool host, bool system, bool controller);

static void CheckCacheLineSize();
static void LogStartupInformation();

namespace {
struct CoreLocals
{
  std::mutex settings_mutex;
  LayeredSettingsInterface layered_settings_interface;
  INISettingsInterface base_settings_interface;

  Threading::ThreadHandle core_thread_handle;

  Timer::Value process_start_time = 0;

  TaskQueue async_task_queue;
};
} // namespace

ALIGN_TO_CACHE_LINE static CoreLocals s_locals;

} // namespace Core

bool Core::SetCriticalFolders(const char* resources_subdir, Error* error)
{
  if (!SetAppRootAndResources(resources_subdir, error))
    return false;

  if (!SetDataRoot(error))
    return false;

  // logging of directories in case something goes wrong super early
  DEV_LOG("AppRoot Directory: {}", EmuFolders::AppRoot);
  DEV_LOG("DataRoot Directory: {}", EmuFolders::DataRoot);
  DEV_LOG("Resources Directory: {}", EmuFolders::Resources);

  // Write crash dumps to the data directory, since that'll be accessible for certain.
  CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

  return true;
}

bool Core::SetAppRootAndResources(const char* resources_subdir, Error* error)
{
  const std::string program_path = FileSystem::GetProgramPath(error);
  if (program_path.empty())
    return false;

  INFO_LOG("Program Path: {}", program_path);

  EmuFolders::AppRoot = Path::Canonicalize(Path::GetDirectory(program_path));

  // MacOS resources are inside the app bundle, so canonicalize them.
  EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, resources_subdir);
#ifdef __APPLE__
  EmuFolders::Resources = Path::Canonicalize(EmuFolders::Resources);
#endif

  if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
  {
    Error::SetStringFmt(error,
                        "Resources directory does not exist at expected path:\n\n{}\n\nYour installation is not "
                        "complete. Please delete and re-download the application from https://www.duckstation.org/.",
                        EmuFolders::Resources);
    return false;
  }

  return true;
}

bool Core::SetDataRoot(Error* error)
{
#ifndef __ANDROID__
  // This bullshit here because AppImage mounts in /tmp, so we need to check the "real" appimage location.
  std::string_view real_approot = EmuFolders::AppRoot;

#ifdef __linux__
  if (const char* appimage_path = std::getenv("APPIMAGE"))
    real_approot = Path::GetDirectory(appimage_path);
#endif

  // Check whether portable.ini exists in the program directory.
  if (FileSystem::FileExists(Path::Combine(real_approot, "portable.txt").c_str()) ||
      FileSystem::FileExists(Path::Combine(real_approot, "settings.ini").c_str()))
  {
    // no need to check that it exists, if it's where the executable is it definitely will
    EmuFolders::DataRoot = std::string(real_approot);
    return true;
  }
#endif // __ANDROID__

#if defined(_WIN32)

  // On Windows, we want to use %APPDATA%\DuckStation for data, and %LOCALAPPDATA%\DuckStation for cache.
  // Old installs use Documents\DuckStation for everything. Check this first.
  PWSTR documents_directory;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &documents_directory)))
  {
    if (std::wcslen(documents_directory) > 0)
    {
      std::string path = Path::Combine(StringUtil::WideStringToUTF8String(documents_directory), "DuckStation");
      if (FileSystem::DirectoryExists(path.c_str()))
      {
        WARNING_LOG("Using Documents directory for data root: {}", path);
        EmuFolders::DataRoot = std::move(path);
      }
    }

    CoTaskMemFree(documents_directory);
  }

  if (EmuFolders::DataRoot.empty())
  {
    PWSTR appdata_directory;
    HRESULT hr;
    if (SUCCEEDED((hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata_directory))))
    {
      if (std::wcslen(appdata_directory) > 0)
        EmuFolders::DataRoot = Path::Combine(StringUtil::WideStringToUTF8String(appdata_directory), "DuckStation");

      CoTaskMemFree(appdata_directory);
    }
    else
    {
      Error::SetHResult(error, "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed: ", hr);
    }
  }

#elif (defined(__linux__) || defined(__FreeBSD__)) && !defined(__ANDROID__)

  // Use $XDG_CONFIG_HOME/duckstation if it exists.
  const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
  if (xdg_config_home && Path::IsAbsolute(xdg_config_home))
  {
    EmuFolders::DataRoot = Path::RealPath(Path::Combine(xdg_config_home, "duckstation"));
  }
  else
  {
    // Use ~/.local/share/duckstation otherwise.
    const char* home_dir = getenv("HOME");
    if (home_dir)
    {
      // ~/.local/share should exist, but just in case it doesn't and this is a fresh profile..
      const std::string local_dir = Path::Combine(home_dir, ".local");
      const std::string share_dir = Path::Combine(local_dir, "share");
      FileSystem::EnsureDirectoryExists(local_dir.c_str(), false);
      FileSystem::EnsureDirectoryExists(share_dir.c_str(), false);
      EmuFolders::DataRoot = Path::RealPath(Path::Combine(share_dir, "duckstation"));
    }
    else
    {
      Error::SetStringView(error, "HOME environment variable is not set.");
    }
  }

#elif defined(__APPLE__)

  static constexpr char MAC_DATA_DIR[] = "Library/Application Support/DuckStation";
  const char* home_dir = getenv("HOME");
  if (home_dir)
    EmuFolders::DataRoot = Path::RealPath(Path::Combine(home_dir, MAC_DATA_DIR));
  else
    Error::SetStringView(error, "HOME environment variable is not set.");

#endif

  // Couldn't find anything? Fall back to portable.
  if (EmuFolders::DataRoot.empty())
  {
    Error::AddPrefix(
      error,
      "Failed to set data directory. Please ensure your system is configured correctly. You can also try portable mode "
      "by creating portable.txt in the same directory you installed DuckStation into.\n\nThe error was:\n");
    return false;
  }

  // Ensure the directories exist.
  if (!FileSystem::EnsureDirectoryExists(EmuFolders::DataRoot.c_str(), false, error))
  {
    Error::AddPrefixFmt(error,
                        "Failed to create data directory at path:\n\n{}\n\n.Please ensure this directory is "
                        "writable. You can also try portable mode by creating portable.txt in the same directory you "
                        "installed DuckStation into.\n\nThe error was:\n",
                        EmuFolders::AppRoot);
    return false;
  }

  return true;
}

#ifndef __ANDROID__

std::string Core::GetBaseSettingsPath()
{
  return Path::Combine(EmuFolders::DataRoot, "settings.ini");
}

bool Core::InitializeBaseSettingsLayer(std::string settings_path, Error* error)
{
  INISettingsInterface& si = s_locals.base_settings_interface;
  s_locals.layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_BASE, &si);

  if (!settings_path.empty())
  {
    const bool settings_exists = FileSystem::FileExists(settings_path.c_str());
    INFO_LOG("Loading config from {}.", settings_path);
    si.SetPath(std::move(settings_path));

    if (settings_exists)
    {
      if (!si.Load(error))
      {
        Error::AddPrefix(error, "Failed to load settings: ");
        return false;
      }
    }
    else
    {
      SetDefaultSettings(si, true, true, true);
      if (!si.Save(error, Settings::GetSectionSaveOrder()))
      {
        Error::AddPrefix(error, "Failed to save settings: ");
        return false;
      }
    }
  }
  else
  {
    // Running settings-file-less, use defaults.
    SetDefaultSettings(si, true, true, true);
  }

  EmuFolders::LoadConfig(si);
  EmuFolders::EnsureFoldersExist();

  // We need to create the console window early, otherwise it appears in front of the main window.
  if (!Log::IsConsoleOutputEnabled() && si.GetBoolValue("Logging", "LogToConsole", false))
    Log::SetConsoleOutputParams(true, si.GetBoolValue("Logging", "LogTimestamps", true));

  return true;
}

bool Core::SaveBaseSettingsLayer(Error* error)
{
  INISettingsInterface& si = s_locals.base_settings_interface;
  if (!si.IsDirty())
    return true;

  si.RemoveEmptySections();
  return si.Save(error, Settings::GetSectionSaveOrder());
}

void Core::SetDefaultSettings(bool host, bool system, bool controller)
{
  {
    const auto lock = GetSettingsLock();
    SetDefaultSettings(s_locals.base_settings_interface, host, system, controller);
  }

  Host::OnSettingsResetToDefault(host, system, controller);
}

void Core::SetDefaultSettings(SettingsInterface& si, bool host, bool system, bool controller)
{
  if (host)
    Host::SetDefaultSettings(si);

  if (system)
  {
    System::SetDefaultSettings(si);
    EmuFolders::SetDefaults();
    EmuFolders::Save(si);
  }

  if (controller)
  {
    InputManager::SetDefaultSourceConfig(si);
    Settings::SetDefaultControllerConfig(si);
    Settings::SetDefaultHotkeyConfig(si);
  }
}

#endif // __ANDROID__

std::unique_lock<std::mutex> Core::GetSettingsLock()
{
  return std::unique_lock(s_locals.settings_mutex);
}

SettingsInterface* Core::GetSettingsInterface()
{
  return &s_locals.layered_settings_interface;
}

std::string Core::GetBaseStringSettingValue(const char* section, const char* key, std::string_view default_value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetStringValue(section, key, default_value);
}

SmallString Core::GetBaseSmallStringSettingValue(const char* section, const char* key, std::string_view default_value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetSmallStringValue(section, key, default_value);
}

TinyString Core::GetBaseTinyStringSettingValue(const char* section, const char* key, std::string_view default_value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetTinyStringValue(section, key, default_value);
}

bool Core::GetBaseBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetBoolValue(section, key, default_value);
}

s32 Core::GetBaseIntSettingValue(const char* section, const char* key, s32 default_value /*= 0*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetIntValue(section, key, default_value);
}

u32 Core::GetBaseUIntSettingValue(const char* section, const char* key, u32 default_value /*= 0*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetUIntValue(section, key, default_value);
}

float Core::GetBaseFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetFloatValue(section, key, default_value);
}

double Core::GetBaseDoubleSettingValue(const char* section, const char* key, double default_value /* = 0.0f */)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetDoubleValue(section, key, default_value);
}

std::vector<std::string> Core::GetBaseStringListSetting(const char* section, const char* key)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetStringList(section, key);
}

std::string Core::GetStringSettingValue(const char* section, const char* key, std::string_view default_value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetStringValue(section, key, default_value);
}

SmallString Core::GetSmallStringSettingValue(const char* section, const char* key, std::string_view default_value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetSmallStringValue(section, key, default_value);
}

TinyString Core::GetTinyStringSettingValue(const char* section, const char* key, std::string_view default_value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetTinyStringValue(section, key, default_value);
}

bool Core::GetBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetBoolValue(section, key, default_value);
}

s32 Core::GetIntSettingValue(const char* section, const char* key, s32 default_value /*= 0*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetIntValue(section, key, default_value);
}

u32 Core::GetUIntSettingValue(const char* section, const char* key, u32 default_value /*= 0*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetUIntValue(section, key, default_value);
}

float Core::GetFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetFloatValue(section, key, default_value);
}

double Core::GetDoubleSettingValue(const char* section, const char* key, double default_value /*= 0.0f*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetDoubleValue(section, key, default_value);
}

std::vector<std::string> Core::GetStringListSetting(const char* section, const char* key)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetStringList(section, key);
}

void Core::SetBaseBoolSettingValue(const char* section, const char* key, bool value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetBoolValue(section, key, value);
}

void Core::SetBaseIntSettingValue(const char* section, const char* key, s32 value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetIntValue(section, key, value);
}

void Core::SetBaseUIntSettingValue(const char* section, const char* key, u32 value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetUIntValue(section, key, value);
}

void Core::SetBaseFloatSettingValue(const char* section, const char* key, float value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->SetFloatValue(section, key, value);
}

void Core::SetBaseStringSettingValue(const char* section, const char* key, const char* value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->SetStringValue(section, key, value);
}

void Core::SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values)
{
  std::unique_lock lock(s_locals.settings_mutex);
  s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->SetStringList(section, key, values);
}

bool Core::AddValueToBaseStringListSetting(const char* section, const char* key, const char* value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->AddToStringList(section, key, value);
}

bool Core::RemoveValueFromBaseStringListSetting(const char* section, const char* key, const char* value)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->RemoveFromStringList(section, key, value);
}

bool Core::ContainsBaseSettingValue(const char* section, const char* key)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->ContainsValue(section, key);
}

void Core::DeleteBaseSettingValue(const char* section, const char* key)
{
  std::unique_lock lock(s_locals.settings_mutex);
  s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->DeleteValue(section, key);
}

SettingsInterface* Core::GetBaseSettingsLayer()
{
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE);
}

SettingsInterface* Core::GetGameSettingsLayer()
{
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_GAME);
}

SettingsInterface* Core::GetInputSettingsLayer()
{
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_INPUT);
}

void Core::SetGameSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock)
{
  s_locals.layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_GAME, sif);
}

void Core::SetInputSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock)
{
  s_locals.layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_INPUT, sif);
}

bool Core::PerformEarlyHardwareChecks(Error* error)
{
  // This shouldn't fail... if it does, just hope for the best.
  cpuinfo_initialize();

#ifdef CPU_ARCH_X64
#ifdef CPU_ARCH_SSE41
  if (!cpuinfo_has_x86_sse4_1())
  {
    Error::SetStringFmt(
      error, "<h3>Your CPU does not support the SSE4.1 instruction set.</h3><p>SSE4.1 is required for this version of "
             "DuckStation. Please download and switch to the legacy SSE2 version.</p><p>You can download this from <a "
             "href=\"https://www.duckstation.org/\">www.duckstation.org</a> under \"Other Platforms\".");
    return false;
  }
#else
  if (cpuinfo_has_x86_sse4_1())
  {
    Error::SetStringFmt(
      error, "You are running the <strong>legacy SSE2 DuckStation executable</strong> on a CPU that supports the "
             "SSE4.1 instruction set.\nPlease download and switch to the regular, non-SSE2 version.\nYou can download "
             "this from <a href=\"https://www.duckstation.org/\">www.duckstation.org</a>.");
  }
#endif
#endif

#ifndef DYNAMIC_HOST_PAGE_SIZE
  // Check page size. If it doesn't match, it is a fatal error.
  const size_t runtime_host_page_size = MemMap::GetRuntimePageSize();
  if (runtime_host_page_size == 0)
  {
    Error::SetStringFmt(error, "Cannot determine size of page. Continuing with expectation of {} byte pages.",
                        HOST_PAGE_SIZE);
  }
  else if (HOST_PAGE_SIZE != runtime_host_page_size)
  {
    Error::SetStringFmt(
      error, "Page size mismatch. This build was compiled with {} byte pages, but the system has {} byte pages.",
      HOST_PAGE_SIZE, runtime_host_page_size);
    return false;
  }
#else
  if (HOST_PAGE_SIZE == 0 || HOST_PAGE_SIZE < MIN_HOST_PAGE_SIZE || HOST_PAGE_SIZE > MAX_HOST_PAGE_SIZE)
  {
    Error::SetStringFmt(error, "Page size of {} bytes is out of the range supported by this build: {}-{}.",
                        HOST_PAGE_SIZE, MIN_HOST_PAGE_SIZE, MAX_HOST_PAGE_SIZE);
    return false;
  }
#endif

  return true;
}

void Core::CheckCacheLineSize()
{
  u32 max_line_size = 0;
  if (cpuinfo_initialize())
  {
    const u32 num_l1is = cpuinfo_get_l1i_caches_count();
    const u32 num_l1ds = cpuinfo_get_l1d_caches_count();
    const u32 num_l2s = cpuinfo_get_l2_caches_count();
    for (u32 i = 0; i < num_l1is; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l1i_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
    for (u32 i = 0; i < num_l1ds; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l1d_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
    for (u32 i = 0; i < num_l2s; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l2_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
  }

  if (max_line_size == 0)
  {
    ERROR_LOG("Cannot determine size of cache line. Continuing with expectation of {} byte lines.",
              HOST_CACHE_LINE_SIZE);
  }
  else if (HOST_CACHE_LINE_SIZE != max_line_size)
  {
    // Not fatal, but does have performance implications.
    WARNING_LOG(
      "Cache line size mismatch. This build was compiled with {} byte lines, but the system has {} byte lines.",
      HOST_CACHE_LINE_SIZE, max_line_size);
  }
}

void Core::LogStartupInformation()
{
#if !defined(CPU_ARCH_X64) || defined(CPU_ARCH_SSE41)
  const std::string_view suffix = {};
#else
  const std::string_view suffix = " [Legacy SSE2]";
#endif
  INFO_LOG("DuckStation for {} ({}){}", TARGET_OS_STR, CPU_ARCH_STR, suffix);
  INFO_LOG("Version: {} [{}]", g_scm_tag_str, g_scm_branch_str);
  INFO_LOG("SCM Timestamp: {}", g_scm_date_str);
  if (const cpuinfo_package* package = cpuinfo_initialize() ? cpuinfo_get_package(0) : nullptr) [[likely]]
  {
    INFO_LOG("Host CPU: {}", package->name);
    INFO_LOG("CPU has {} logical processor(s) and {} core(s) across {} cluster(s).", package->processor_count,
             package->core_count, package->cluster_count);
  }

#ifdef DYNAMIC_HOST_PAGE_SIZE
  INFO_LOG("Host Page Size: {} bytes", HOST_PAGE_SIZE);
#endif
}

bool Core::ProcessStartup(Error* error)
{
  s_locals.process_start_time = Timer::GetCurrentValue();

  if (!System::AllocatePersistentMemory(error))
    return false;

  CheckCacheLineSize();

  // Initialize rapidyaml before anything can use it.
  SetRymlCallbacks();

#ifdef __linux__
  // Running DuckStation out of /usr is not supported and makes no sense.
  if (std::memcmp(EmuFolders::AppRoot.data(), "/usr/", 5) == 0)
    return false;
#endif

  return true;
}

void Core::ProcessShutdown()
{
  System::ReleasePersistentMemory();
}

bool Core::CoreThreadInitialize(bool disable_worker_threads, Error* error)
{
#ifdef _WIN32
  // On Win32, we have a bunch of things which use COM (e.g. SDL, Cubeb, etc).
  // We need to initialize COM first, before anything else does, because otherwise they might
  // initialize it in single-threaded/apartment mode, which can't be changed to multithreaded.
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "CoInitializeEx() failed: ", hr);
    return false;
  }
#endif

  s_locals.core_thread_handle = Threading::ThreadHandle::GetForCallingThread();

  if (!disable_worker_threads)
    s_locals.async_task_queue.SetWorkerCount(NUM_ASYNC_WORKER_THREADS);

  System::LoadSettings(false);

  LogStartupInformation();

  VideoThread::ProcessStartup();

  Achievements::Initialize();

#ifdef ENABLE_DISCORD_PRESENCE
  if (g_settings.enable_discord_presence)
    DiscordPresence::Initialize();
#endif

  return true;
}

void Core::CoreThreadShutdown()
{
  s_locals.async_task_queue.SetWorkerCount(0);

#ifdef ENABLE_DISCORD_PRESENCE
  DiscordPresence::Shutdown();
#endif

  Achievements::Shutdown();

  GPUDevice::UnloadDynamicLibraries();

  InputManager::CloseSources();

  HTTPCache::Shutdown();

  VideoThread::ProcessShutdown();

  s_locals.core_thread_handle = {};

#ifdef _WIN32
  CoUninitialize();
#endif
}

const Threading::ThreadHandle& Host::GetCoreThreadHandle()
{
  return Core::s_locals.core_thread_handle;
}

bool Host::IsOnCoreThread()
{
  return Core::s_locals.core_thread_handle.IsCallingThread();
}

void Host::QueueAsyncTask(std::function<void()> function)
{
  Core::s_locals.async_task_queue.SubmitTask(std::move(function));
}

void Host::WaitForAllAsyncTasks()
{
  Core::s_locals.async_task_queue.WaitForAll();
}

float Core::GetProcessUptime()
{
  return static_cast<float>(Timer::ConvertValueToSeconds(Timer::GetCurrentValue() - s_locals.process_start_time));
}

void Core::IdleUpdate()
{
  InputManager::PollSources();

#ifdef ENABLE_DISCORD_PRESENCE
  DiscordPresence::Poll();
#endif

  HTTPCache::PollRequests();

  Achievements::IdleUpdate();

#ifdef ENABLE_GDB_SERVER
  GDBServer::Poll(0);
#endif
}

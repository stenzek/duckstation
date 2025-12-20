// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core.h"
#include "core_private.h"
#include "settings.h"

#include "scmversion/scmversion.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/layered_settings_interface.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <cstdarg>
#include <cstdlib>
#include <limits>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <ShlObj.h>
#endif

LOG_CHANNEL(Host);

namespace Core {

namespace {
struct CoreLocals
{
  std::mutex settings_mutex;
  LayeredSettingsInterface layered_settings_interface;
};
} // namespace

ALIGN_TO_CACHE_LINE static CoreLocals s_locals;

} // namespace Core

std::string Core::ComputeDataDirectory()
{
  std::string ret;

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
    ret = real_approot;
    return ret;
  }
#endif // __ANDROID__

#if defined(_WIN32)
  // On Windows, use My Documents\DuckStation.
  PWSTR documents_directory;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &documents_directory)))
  {
    if (std::wcslen(documents_directory) > 0)
      ret = Path::Combine(StringUtil::WideStringToUTF8String(documents_directory), "DuckStation");
    CoTaskMemFree(documents_directory);
  }
#elif (defined(__linux__) || defined(__FreeBSD__)) && !defined(__ANDROID__)
  // Use $XDG_CONFIG_HOME/duckstation if it exists.
  const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
  if (xdg_config_home && Path::IsAbsolute(xdg_config_home))
  {
    ret = Path::RealPath(Path::Combine(xdg_config_home, "duckstation"));
  }
  else
  {
    // Use ~/.local/share/duckstation otherwise.
    const char* home_dir = getenv("HOME");
    if (home_dir)
    {
      // ~/.local/share should exist, but just in case it doesn't and this is a fresh profile..
      const std::string local_dir(Path::Combine(home_dir, ".local"));
      const std::string share_dir(Path::Combine(local_dir, "share"));
      FileSystem::EnsureDirectoryExists(local_dir.c_str(), false);
      FileSystem::EnsureDirectoryExists(share_dir.c_str(), false);
      ret = Path::RealPath(Path::Combine(share_dir, "duckstation"));
    }
  }
#elif defined(__APPLE__)
  static constexpr char MAC_DATA_DIR[] = "Library/Application Support/DuckStation";
  const char* home_dir = getenv("HOME");
  if (home_dir)
    ret = Path::RealPath(Path::Combine(home_dir, MAC_DATA_DIR));
#endif

  // Couldn't find anything? Fall back to portable.
  if (ret.empty())
    ret = EmuFolders::AppRoot;

  return ret;
}

std::unique_lock<std::mutex> Core::GetSettingsLock()
{
  return std::unique_lock(s_locals.settings_mutex);
}

SettingsInterface* Core::GetSettingsInterface()
{
  return &s_locals.layered_settings_interface;
}

std::string Core::GetBaseStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetStringValue(section, key, default_value);
}

SmallString Core::GetBaseSmallStringSettingValue(const char* section, const char* key,
                                                 const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetSmallStringValue(section, key, default_value);
}

TinyString Core::GetBaseTinyStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
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

std::string Core::GetStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetStringValue(section, key, default_value);
}

SmallString Core::GetSmallStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_locals.settings_mutex);
  return s_locals.layered_settings_interface.GetSmallStringValue(section, key, default_value);
}

TinyString Core::GetTinyStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
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

void Core::SetBaseSettingsLayer(SettingsInterface* sif)
{
  AssertMsg(s_locals.layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE) == nullptr,
            "Base layer has not been set");
  s_locals.layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_BASE, sif);
}

void Core::SetGameSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock)
{
  s_locals.layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_GAME, sif);
}

void Core::SetInputSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock)
{
  s_locals.layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_INPUT, sif);
}

std::string Core::GetHTTPUserAgent()
{
  return fmt::format("DuckStation for {} ({}) {}", TARGET_OS_STR, CPU_ARCH_STR, g_scm_tag_str);
}

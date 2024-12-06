// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "host.h"
#include "fullscreen_ui.h"
#include "gpu.h"
#include "imgui_overlays.h"
#include "shader_cache_version.h"
#include "system.h"
#include "system_private.h"

#include "scmversion/scmversion.h"

#include "util/compress_helpers.h"
#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"

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

namespace Host {
static std::mutex s_settings_mutex;
static LayeredSettingsInterface s_layered_settings_interface;
} // namespace Host

bool Host::Internal::ShouldUsePortableMode()
{
#ifndef __ANDROID__
  // Check whether portable.ini exists in the program directory.
  return (FileSystem::FileExists(Path::Combine(EmuFolders::AppRoot, "portable.txt").c_str()) ||
          FileSystem::FileExists(Path::Combine(EmuFolders::AppRoot, "settings.ini").c_str()));
#else
  return false;
#endif
}

std::string Host::Internal::ComputeDataDirectory()
{
  std::string ret;

  if (ShouldUsePortableMode())
  {
    ret = EmuFolders::AppRoot;
    return ret;
  }

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

std::unique_lock<std::mutex> Host::GetSettingsLock()
{
  return std::unique_lock<std::mutex>(s_settings_mutex);
}

SettingsInterface* Host::GetSettingsInterface()
{
  return &s_layered_settings_interface;
}

std::optional<DynamicHeapArray<u8>> Host::ReadCompressedResourceFile(std::string_view filename, bool allow_override,
                                                                     Error* error)
{
  std::optional<DynamicHeapArray<u8>> ret = Host::ReadResourceFile(filename, allow_override, error);
  if (ret.has_value())
    ret = CompressHelpers::DecompressFile(filename, std::move(ret), std::nullopt, error);

  return ret;
}

std::string Host::GetBaseStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetStringValue(section, key, default_value);
}

SmallString Host::GetBaseSmallStringSettingValue(const char* section, const char* key,
                                                 const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetSmallStringValue(section, key, default_value);
}

TinyString Host::GetBaseTinyStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetTinyStringValue(section, key, default_value);
}

bool Host::GetBaseBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetBoolValue(section, key, default_value);
}

s32 Host::GetBaseIntSettingValue(const char* section, const char* key, s32 default_value /*= 0*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetIntValue(section, key, default_value);
}

u32 Host::GetBaseUIntSettingValue(const char* section, const char* key, u32 default_value /*= 0*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetUIntValue(section, key, default_value);
}

float Host::GetBaseFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetFloatValue(section, key, default_value);
}

double Host::GetBaseDoubleSettingValue(const char* section, const char* key, double default_value /* = 0.0f */)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->GetDoubleValue(section, key, default_value);
}

std::vector<std::string> Host::GetBaseStringListSetting(const char* section, const char* key)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->GetStringList(section, key);
}

std::string Host::GetStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetStringValue(section, key, default_value);
}

SmallString Host::GetSmallStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetSmallStringValue(section, key, default_value);
}

TinyString Host::GetTinyStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetTinyStringValue(section, key, default_value);
}

bool Host::GetBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetBoolValue(section, key, default_value);
}

s32 Host::GetIntSettingValue(const char* section, const char* key, s32 default_value /*= 0*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetIntValue(section, key, default_value);
}

u32 Host::GetUIntSettingValue(const char* section, const char* key, u32 default_value /*= 0*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetUIntValue(section, key, default_value);
}

float Host::GetFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetFloatValue(section, key, default_value);
}

double Host::GetDoubleSettingValue(const char* section, const char* key, double default_value /*= 0.0f*/)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetDoubleValue(section, key, default_value);
}

std::vector<std::string> Host::GetStringListSetting(const char* section, const char* key)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetStringList(section, key);
}

void Host::SetBaseBoolSettingValue(const char* section, const char* key, bool value)
{
  std::unique_lock lock(s_settings_mutex);
  s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetBoolValue(section, key, value);
}

void Host::SetBaseIntSettingValue(const char* section, const char* key, s32 value)
{
  std::unique_lock lock(s_settings_mutex);
  s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetIntValue(section, key, value);
}

void Host::SetBaseUIntSettingValue(const char* section, const char* key, u32 value)
{
  std::unique_lock lock(s_settings_mutex);
  s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetUIntValue(section, key, value);
}

void Host::SetBaseFloatSettingValue(const char* section, const char* key, float value)
{
  std::unique_lock lock(s_settings_mutex);
  s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetFloatValue(section, key, value);
}

void Host::SetBaseStringSettingValue(const char* section, const char* key, const char* value)
{
  std::unique_lock lock(s_settings_mutex);
  s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetStringValue(section, key, value);
}

void Host::SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values)
{
  std::unique_lock lock(s_settings_mutex);
  s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetStringList(section, key, values);
}

bool Host::AddValueToBaseStringListSetting(const char* section, const char* key, const char* value)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->AddToStringList(section, key, value);
}

bool Host::RemoveValueFromBaseStringListSetting(const char* section, const char* key, const char* value)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)
    ->RemoveFromStringList(section, key, value);
}

bool Host::ContainsBaseSettingValue(const char* section, const char* key)
{
  std::unique_lock lock(s_settings_mutex);
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->ContainsValue(section, key);
}

void Host::DeleteBaseSettingValue(const char* section, const char* key)
{
  std::unique_lock lock(s_settings_mutex);
  s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->DeleteValue(section, key);
}

SettingsInterface* Host::Internal::GetBaseSettingsLayer()
{
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE);
}

SettingsInterface* Host::Internal::GetGameSettingsLayer()
{
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_GAME);
}

SettingsInterface* Host::Internal::GetInputSettingsLayer()
{
  return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_INPUT);
}

void Host::Internal::SetBaseSettingsLayer(SettingsInterface* sif)
{
  AssertMsg(s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE) == nullptr,
            "Base layer has not been set");
  s_layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_BASE, sif);
}

void Host::Internal::SetGameSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock)
{
  s_layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_GAME, sif);
}

void Host::Internal::SetInputSettingsLayer(SettingsInterface* sif, std::unique_lock<std::mutex>& lock)
{
  s_layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_INPUT, sif);
}

std::string Host::GetHTTPUserAgent()
{
  return fmt::format("DuckStation for {} ({}) {}", TARGET_OS_STR, CPU_ARCH_STR, g_scm_tag_str);
}

bool Host::CreateGPUDevice(RenderAPI api, bool fullscreen, Error* error)
{
  DebugAssert(!g_gpu_device);

  INFO_LOG("Trying to create a {} GPU device...", GPUDevice::RenderAPIToString(api));
  g_gpu_device = GPUDevice::CreateDeviceForAPI(api);

  std::optional<GPUDevice::ExclusiveFullscreenMode> fullscreen_mode;
  if (fullscreen && g_gpu_device && g_gpu_device->SupportsExclusiveFullscreen())
  {
    fullscreen_mode =
      GPUDevice::ExclusiveFullscreenMode::Parse(Host::GetTinyStringSettingValue("GPU", "FullscreenMode", ""));
  }
  std::optional<bool> exclusive_fullscreen_control;
  if (g_settings.display_exclusive_fullscreen_control != DisplayExclusiveFullscreenControl::Automatic)
  {
    exclusive_fullscreen_control =
      (g_settings.display_exclusive_fullscreen_control == DisplayExclusiveFullscreenControl::Allowed);
  }

  u32 disabled_features = 0;
  if (g_settings.gpu_disable_dual_source_blend)
    disabled_features |= GPUDevice::FEATURE_MASK_DUAL_SOURCE_BLEND;
  if (g_settings.gpu_disable_framebuffer_fetch)
    disabled_features |= GPUDevice::FEATURE_MASK_FRAMEBUFFER_FETCH;
  if (g_settings.gpu_disable_texture_buffers)
    disabled_features |= GPUDevice::FEATURE_MASK_TEXTURE_BUFFERS;
  if (g_settings.gpu_disable_memory_import)
    disabled_features |= GPUDevice::FEATURE_MASK_MEMORY_IMPORT;
  if (g_settings.gpu_disable_raster_order_views)
    disabled_features |= GPUDevice::FEATURE_MASK_RASTER_ORDER_VIEWS;
  if (g_settings.gpu_disable_compute_shaders)
    disabled_features |= GPUDevice::FEATURE_MASK_COMPUTE_SHADERS;
  if (g_settings.gpu_disable_compressed_textures)
    disabled_features |= GPUDevice::FEATURE_MASK_COMPRESSED_TEXTURES;

    // Don't dump shaders on debug builds for Android, users will complain about storage...
#if !defined(__ANDROID__) || defined(_DEBUG)
  const std::string_view shader_dump_directory(EmuFolders::DataRoot);
#else
  const std::string_view shader_dump_directory;
#endif

  Error create_error;
  std::optional<WindowInfo> wi;
  if (!g_gpu_device ||
      !(wi = Host::AcquireRenderWindow(api, fullscreen, fullscreen_mode.has_value(), &create_error)).has_value() ||
      !g_gpu_device->Create(
        g_settings.gpu_adapter, static_cast<GPUDevice::FeatureMask>(disabled_features), shader_dump_directory,
        g_settings.gpu_disable_shader_cache ? std::string_view() : std::string_view(EmuFolders::Cache),
        SHADER_CACHE_VERSION, g_settings.gpu_use_debug_device, wi.value(), System::GetEffectiveVSyncMode(),
        System::ShouldAllowPresentThrottle(), fullscreen_mode.has_value() ? &fullscreen_mode.value() : nullptr,
        exclusive_fullscreen_control, &create_error))
  {
    ERROR_LOG("Failed to create GPU device: {}", create_error.GetDescription());
    if (g_gpu_device)
      g_gpu_device->Destroy();
    g_gpu_device.reset();
    if (wi.has_value())
      Host::ReleaseRenderWindow();

    Error::SetStringFmt(
      error,
      TRANSLATE_FS("System", "Failed to create render device:\n\n{0}\n\nThis may be due to your GPU not supporting the "
                             "chosen renderer ({1}), or because your graphics drivers need to be updated."),
      create_error.GetDescription(), GPUDevice::RenderAPIToString(api));
    return false;
  }

  if (!ImGuiManager::Initialize(g_settings.display_osd_scale / 100.0f, g_settings.display_osd_margin, &create_error))
  {
    ERROR_LOG("Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    Error::SetStringFmt(error, "Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    g_gpu_device->Destroy();
    g_gpu_device.reset();
    Host::ReleaseRenderWindow();
    return false;
  }

  InputManager::SetDisplayWindowSize(ImGuiManager::GetWindowWidth(), ImGuiManager::GetWindowHeight());
  return true;
}

void Host::UpdateDisplayWindow(bool fullscreen)
{
  if (!g_gpu_device)
    return;

  const GPUVSyncMode vsync_mode = System::GetEffectiveVSyncMode();
  const bool allow_present_throttle = System::ShouldAllowPresentThrottle();
  std::optional<GPUDevice::ExclusiveFullscreenMode> fullscreen_mode;
  if (fullscreen && g_gpu_device->SupportsExclusiveFullscreen())
  {
    fullscreen_mode =
      GPUDevice::ExclusiveFullscreenMode::Parse(Host::GetTinyStringSettingValue("GPU", "FullscreenMode", ""));
  }
  std::optional<bool> exclusive_fullscreen_control;
  if (g_settings.display_exclusive_fullscreen_control != DisplayExclusiveFullscreenControl::Automatic)
  {
    exclusive_fullscreen_control =
      (g_settings.display_exclusive_fullscreen_control == DisplayExclusiveFullscreenControl::Allowed);
  }

  g_gpu_device->DestroyMainSwapChain();

  Error error;
  std::optional<WindowInfo> wi =
    Host::AcquireRenderWindow(g_gpu_device->GetRenderAPI(), fullscreen, fullscreen_mode.has_value(), &error);
  if (!wi.has_value())
  {
    Host::ReportFatalError("Failed to get render window after update", error.GetDescription());
    return;
  }

  // if surfaceless, just leave it
  if (wi->IsSurfaceless())
    return;

  if (!g_gpu_device->RecreateMainSwapChain(wi.value(), vsync_mode, allow_present_throttle,
                                           fullscreen_mode.has_value() ? &fullscreen_mode.value() : nullptr,
                                           exclusive_fullscreen_control, &error))
  {
    Host::ReportFatalError("Failed to change window after update", error.GetDescription());
    return;
  }

  const float f_width = static_cast<float>(g_gpu_device->GetMainSwapChain()->GetWidth());
  const float f_height = static_cast<float>(g_gpu_device->GetMainSwapChain()->GetHeight());
  ImGuiManager::WindowResized(f_width, f_height);
  InputManager::SetDisplayWindowSize(f_width, f_height);
  System::DisplayWindowResized();
}

void Host::ResizeDisplayWindow(s32 width, s32 height, float scale)
{
  if (!g_gpu_device || !g_gpu_device->HasMainSwapChain())
    return;

  DEV_LOG("Display window resized to {}x{}", width, height);

  Error error;
  if (!g_gpu_device->GetMainSwapChain()->ResizeBuffers(width, height, scale, &error))
  {
    ERROR_LOG("Failed to resize main swap chain: {}", error.GetDescription());
    UpdateDisplayWindow(Host::IsFullscreen());
    return;
  }

  const float f_width = static_cast<float>(g_gpu_device->GetMainSwapChain()->GetWidth());
  const float f_height = static_cast<float>(g_gpu_device->GetMainSwapChain()->GetHeight());
  ImGuiManager::WindowResized(f_width, f_height);
  InputManager::SetDisplayWindowSize(f_width, f_height);
  System::DisplayWindowResized();
}

void Host::ReleaseGPUDevice()
{
  if (!g_gpu_device)
    return;

  ImGuiManager::DestroyAllDebugWindows();
  ImGuiManager::DestroyOverlayTextures();
  FullscreenUI::Shutdown();
  ImGuiManager::Shutdown();

  INFO_LOG("Destroying {} GPU device...", GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()));
  g_gpu_device->Destroy();
  g_gpu_device.reset();

  Host::ReleaseRenderWindow();
}

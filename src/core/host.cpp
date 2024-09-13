// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "host.h"
#include "fullscreen_ui.h"
#include "gpu.h"
#include "imgui_overlays.h"
#include "shader_cache_version.h"
#include "system.h"

#include "scmversion/scmversion.h"

#include "util/compress_helpers.h"
#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/layered_settings_interface.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <cstdarg>
#include <limits>

Log_SetChannel(Host);

namespace Host {
static std::mutex s_settings_mutex;
static LayeredSettingsInterface s_layered_settings_interface;
} // namespace Host

std::unique_lock<std::mutex> Host::GetSettingsLock()
{
  return std::unique_lock<std::mutex>(s_settings_mutex);
}

SettingsInterface* Host::GetSettingsInterface()
{
  return &s_layered_settings_interface;
}

std::optional<DynamicHeapArray<u8>> Host::ReadCompressedResourceFile(std::string_view filename, bool allow_override)
{
  std::optional<DynamicHeapArray<u8>> ret = Host::ReadResourceFile(filename, allow_override);
  if (ret.has_value())
  {
    Error error;
    ret = CompressHelpers::DecompressFile(filename, std::move(ret), std::nullopt, &error);
    if (!ret.has_value())
      ERROR_LOG("Failed to decompress '{}': {}", Path::GetFileName(filename), error.GetDescription());
  }

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

void Host::SetBaseIntSettingValue(const char* section, const char* key, int value)
{
  std::unique_lock lock(s_settings_mutex);
  s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->SetIntValue(section, key, value);
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

bool Host::CreateGPUDevice(RenderAPI api, Error* error)
{
  DebugAssert(!g_gpu_device);

  INFO_LOG("Trying to create a {} GPU device...", GPUDevice::RenderAPIToString(api));
  g_gpu_device = GPUDevice::CreateDeviceForAPI(api);

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

  Error create_error;
  if (!g_gpu_device || !g_gpu_device->Create(
                         g_settings.gpu_adapter,
                         g_settings.gpu_disable_shader_cache ? std::string_view() : std::string_view(EmuFolders::Cache),
                         SHADER_CACHE_VERSION, g_settings.gpu_use_debug_device, System::GetEffectiveVSyncMode(),
                         System::ShouldAllowPresentThrottle(), exclusive_fullscreen_control,
                         static_cast<GPUDevice::FeatureMask>(disabled_features), &create_error))
  {
    ERROR_LOG("Failed to create GPU device: {}", create_error.GetDescription());
    if (g_gpu_device)
      g_gpu_device->Destroy();
    g_gpu_device.reset();

    Error::SetStringFmt(
      error,
      TRANSLATE_FS("System", "Failed to create render device:\n\n{0}\n\nThis may be due to your GPU not supporting the "
                             "chosen renderer ({1}), or because your graphics drivers need to be updated."),
      create_error.GetDescription(), GPUDevice::RenderAPIToString(api));
    return false;
  }

  if (!ImGuiManager::Initialize(g_settings.display_osd_scale / 100.0f, &create_error))
  {
    ERROR_LOG("Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    Error::SetStringFmt(error, "Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    g_gpu_device->Destroy();
    g_gpu_device.reset();
    return false;
  }

  InputManager::SetDisplayWindowSize(static_cast<float>(g_gpu_device->GetWindowWidth()),
                                     static_cast<float>(g_gpu_device->GetWindowHeight()));
  return true;
}

void Host::UpdateDisplayWindow()
{
  if (!g_gpu_device)
    return;

  if (!g_gpu_device->UpdateWindow())
  {
    Host::ReportErrorAsync("Error", "Failed to change window after update. The log may contain more information.");
    return;
  }

  const float f_width = static_cast<float>(g_gpu_device->GetWindowWidth());
  const float f_height = static_cast<float>(g_gpu_device->GetWindowHeight());
  ImGuiManager::WindowResized(f_width, f_height);
  InputManager::SetDisplayWindowSize(f_width, f_height);

  if (System::IsValid())
  {
    // Fix up vsync etc.
    System::UpdateSpeedLimiterState();

    // If we're paused, re-present the current frame at the new window size.
    if (System::IsPaused())
      System::InvalidateDisplay();
  }
}

void Host::ResizeDisplayWindow(s32 width, s32 height, float scale)
{
  if (!g_gpu_device)
    return;

  DEV_LOG("Display window resized to {}x{}", width, height);

  g_gpu_device->ResizeWindow(width, height, scale);

  const float f_width = static_cast<float>(g_gpu_device->GetWindowWidth());
  const float f_height = static_cast<float>(g_gpu_device->GetWindowHeight());
  ImGuiManager::WindowResized(f_width, f_height);
  InputManager::SetDisplayWindowSize(f_width, f_height);

  // If we're paused, re-present the current frame at the new window size.
  if (System::IsValid())
  {
    if (System::IsPaused())
    {
      // Hackity hack, on some systems, presenting a single frame isn't enough to actually get it
      // displayed. Two seems to be good enough. Maybe something to do with direct scanout.
      System::InvalidateDisplay();
      System::InvalidateDisplay();
    }

    System::HostDisplayResized();
  }
}

void Host::ReleaseGPUDevice()
{
  if (!g_gpu_device)
    return;

  ImGuiManager::DestroyOverlayTextures();
  FullscreenUI::Shutdown();
  ImGuiManager::Shutdown();

  INFO_LOG("Destroying {} GPU device...", GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()));
  g_gpu_device->Destroy();
  g_gpu_device.reset();
}

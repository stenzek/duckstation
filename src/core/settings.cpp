#include "settings.h"
#include "SimpleIni.h"
#include "YBaseLib/Log.h"
#include <array>
#include <string.h>
Log_SetChannel(Settings);

#ifdef _MSC_VER
#define strcasecmp stricmp
#endif

Settings::Settings() = default;

void Settings::SetDefaults()
{
  region = ConsoleRegion::Auto;
  cpu_execution_mode = CPUExecutionMode::Interpreter;

  speed_limiter_enabled = true;
  start_paused = false;

  gpu_renderer = GPURenderer::HardwareOpenGL;
  gpu_resolution_scale = 1;
  gpu_true_color = true;
  gpu_texture_filtering = false;
  gpu_force_progressive_scan = true;
  display_linear_filtering = true;
  video_sync_enabled = true;

  audio_backend = AudioBackend::Default;
  audio_sync_enabled = true;

  bios_path = "scph1001.bin";
  bios_patch_tty_enable = false;
  bios_patch_fast_boot = false;

  controller_types[0] = ControllerType::DigitalController;
  controller_types[1] = ControllerType::None;

  memory_card_paths[0] = "memory_card_1.mcd";
  memory_card_paths[1].clear();
}

void Settings::Load(const char* filename)
{
  CSimpleIniA ini(true);
  SI_Error err = ini.LoadFile(filename);
  if (err != SI_OK)
  {
    Log_WarningPrintf("Settings could not be loaded from '%s', defaults will be used.", filename);
    SetDefaults();
    return;
  }

  region = ParseConsoleRegionName(ini.GetValue("Console", "Region", "NTSC-U")).value_or(ConsoleRegion::NTSC_U);

  speed_limiter_enabled = ini.GetBoolValue("General", "SpeedLimiterEnabled", true);
  start_paused = ini.GetBoolValue("General", "StartPaused", false);

  cpu_execution_mode =
    ParseCPUExecutionMode(ini.GetValue("CPU", "ExecutionMode", "Interpreter")).value_or(CPUExecutionMode::Interpreter);

  gpu_renderer = ParseRendererName(ini.GetValue("GPU", "Renderer", "OpenGL")).value_or(GPURenderer::HardwareOpenGL);
  gpu_resolution_scale = static_cast<u32>(ini.GetLongValue("GPU", "ResolutionScale", 1));
  gpu_true_color = ini.GetBoolValue("GPU", "TrueColor", false);
  gpu_texture_filtering = ini.GetBoolValue("GPU", "TextureFiltering", false);

  display_linear_filtering = ini.GetBoolValue("Display", "LinearFiltering", true);
  video_sync_enabled = ini.GetBoolValue("Display", "VSync", true);

  audio_backend = ParseAudioBackend(ini.GetValue("Audio", "Backend", "Default")).value_or(AudioBackend::Default);
  audio_sync_enabled = ini.GetBoolValue("Audio", "Sync", true);

  bios_path = ini.GetValue("BIOS", "Path", "scph1001.bin");
  bios_patch_tty_enable = ini.GetBoolValue("BIOS", "PatchTTYEnable", true);
  bios_patch_fast_boot = ini.GetBoolValue("BIOS", "PatchFastBoot", false);

  controller_types[0] = ParseControllerTypeName(ini.GetValue("Ports", "Controller1Type", "DigitalController"))
                          .value_or(ControllerType::DigitalController);
  controller_types[1] =
    ParseControllerTypeName(ini.GetValue("Ports", "Controller2Type", "None")).value_or(ControllerType::None);

  memory_card_paths[0] = ini.GetValue("Ports", "MemoryCard1Path", "memory_card_1.mcd");
  memory_card_paths[1] = ini.GetValue("Ports", "MemoryCard2Path", "");
}

bool Settings::Save(const char* filename) const
{
  // Load the file first to preserve the comments.
  CSimpleIniA ini;
  SI_Error err = ini.LoadFile(filename);
  if (err != SI_OK)
    ini.Reset();

  ini.SetValue("Console", "Region", GetConsoleRegionName(region));

  ini.SetBoolValue("General", "SpeedLimiterEnabled", speed_limiter_enabled);
  ini.SetBoolValue("General", "StartPaused", start_paused);

  ini.SetValue("CPU", "ExecutionMode", GetCPUExecutionModeName(cpu_execution_mode));

  ini.SetValue("GPU", "Renderer", GetRendererName(gpu_renderer));
  ini.SetLongValue("GPU", "ResolutionScale", static_cast<long>(gpu_resolution_scale));
  ini.SetBoolValue("GPU", "TrueColor", gpu_true_color);
  ini.SetBoolValue("GPU", "TextureFiltering", gpu_texture_filtering);

  ini.SetBoolValue("Display", "LinearFiltering", display_linear_filtering);
  ini.SetBoolValue("Display", "VSync", video_sync_enabled);

  ini.SetValue("Audio", "Backend", GetAudioBackendName(audio_backend));
  ini.SetBoolValue("Audio", "Sync", audio_sync_enabled);

  ini.SetValue("BIOS", "Path", bios_path.c_str());
  ini.SetBoolValue("BIOS", "PatchTTYEnable", bios_patch_tty_enable);
  ini.SetBoolValue("BIOS", "PatchFastBoot", bios_patch_fast_boot);

  if (controller_types[0] != ControllerType::None)
    ini.SetValue("Ports", "Controller1Type", GetControllerTypeName(controller_types[0]));
  else
    ini.DeleteValue("Ports", "Controller1Type", nullptr);

  if (controller_types[1] != ControllerType::None)
    ini.SetValue("Ports", "Controller2Type", GetControllerTypeName(controller_types[1]));
  else
    ini.DeleteValue("Ports", "Controller2Type", nullptr);

  if (!memory_card_paths[0].empty())
    ini.SetValue("Ports", "MemoryCard1Path", memory_card_paths[0].c_str());
  else
    ini.DeleteValue("Ports", "MemoryCard1Path", nullptr);

  if (!memory_card_paths[1].empty())
    ini.SetValue("Ports", "MemoryCard2Path", memory_card_paths[1].c_str());
  else
    ini.DeleteValue("Ports", "MemoryCard2Path", nullptr);

  err = ini.SaveFile(filename, false);
  if (err != SI_OK)
  {
    Log_WarningPrintf("Failed to save settings to '%s'.", filename);
    return false;
  }

  return true;
}

static std::array<const char*, 4> s_console_region_names = {{"Auto", "NTSC-J", "NTSC-U", "PAL"}};
static std::array<const char*, 4> s_console_region_display_names = {
  {"Auto-Detect", "NTSC-J (Japan)", "NTSC-U (US)", "PAL (Europe, Australia)"}};

std::optional<ConsoleRegion> Settings::ParseConsoleRegionName(const char* str)
{
  int index = 0;
  for (const char* name : s_console_region_names)
  {
    if (strcasecmp(name, str) == 0)
      return static_cast<ConsoleRegion>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetConsoleRegionName(ConsoleRegion region)
{
  return s_console_region_names[static_cast<int>(region)];
}

const char* Settings::GetConsoleRegionDisplayName(ConsoleRegion region)
{
  return s_console_region_display_names[static_cast<int>(region)];
}

static std::array<const char*, 3> s_cpu_execution_mode_names = {{"Interpreter", "CachedInterpreter", "Recompiler"}};
static std::array<const char*, 3> s_cpu_execution_mode_display_names = {
  {"Intepreter (Slowest)", "Cached Interpreter (Faster)", "Recompiler (Fastest)"}};

std::optional<CPUExecutionMode> Settings::ParseCPUExecutionMode(const char* str)
{
  u8 index = 0;
  for (const char* name : s_cpu_execution_mode_names)
  {
    if (strcasecmp(name, str) == 0)
      return static_cast<CPUExecutionMode>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetCPUExecutionModeName(CPUExecutionMode mode)
{
  return s_cpu_execution_mode_names[static_cast<u8>(mode)];
}

const char* Settings::GetCPUExecutionModeDisplayName(CPUExecutionMode mode)
{
  return s_cpu_execution_mode_display_names[static_cast<u8>(mode)];
}

static std::array<const char*, 3> s_gpu_renderer_names = {{"D3D11", "OpenGL", "Software"}};
static std::array<const char*, 3> s_gpu_renderer_display_names = {
  {"Hardware (D3D11)", "Hardware (OpenGL)", "Software"}};

std::optional<GPURenderer> Settings::ParseRendererName(const char* str)
{
  int index = 0;
  for (const char* name : s_gpu_renderer_names)
  {
    if (strcasecmp(name, str) == 0)
      return static_cast<GPURenderer>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetRendererName(GPURenderer renderer)
{
  return s_gpu_renderer_names[static_cast<int>(renderer)];
}

const char* Settings::GetRendererDisplayName(GPURenderer renderer)
{
  return s_gpu_renderer_display_names[static_cast<int>(renderer)];
}

static std::array<const char*, 2> s_audio_backend_names = {{"Null", "Default"}};
static std::array<const char*, 2> s_audio_backend_display_names = {{"Null (No Output)", "Default"}};

std::optional<AudioBackend> Settings::ParseAudioBackend(const char* str)
{
  int index = 0;
  for (const char* name : s_audio_backend_names)
  {
    if (strcasecmp(name, str) == 0)
      return static_cast<AudioBackend>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetAudioBackendName(AudioBackend backend)
{
  return s_audio_backend_names[static_cast<int>(backend)];
}

const char* Settings::GetAudioBackendDisplayName(AudioBackend backend)
{
  return s_audio_backend_display_names[static_cast<int>(backend)];
}

static std::array<const char*, 3> s_controller_type_names = {{"None", "DigitalController", "AnalogController"}};
static std::array<const char*, 3> s_controller_display_names = {
  {"None", "Digital Controller", "Analog Controller (DualShock)"}};

std::optional<ControllerType> Settings::ParseControllerTypeName(const char* str)
{
  int index = 0;
  for (const char* name : s_controller_type_names)
  {
    if (strcasecmp(name, str) == 0)
      return static_cast<ControllerType>(index);

    index++;
  }

  return std::nullopt;
}

const char* Settings::GetControllerTypeName(ControllerType type)
{
  return s_controller_type_names[static_cast<int>(type)];
}

const char* Settings::GetControllerTypeDisplayName(ControllerType type)
{
  return s_controller_display_names[static_cast<int>(type)];
}

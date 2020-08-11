#include "host_interface.h"
#include "bios.h"
#include "cdrom.h"
#include "common/audio_stream.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/image.h"
#include "common/log.h"
#include "common/string_util.h"
#include "controller.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "dma.h"
#include "gpu.h"
#include "gte.h"
#include "host_display.h"
#include "pgxp.h"
#include "save_state_version.h"
#include "system.h"
#include <cmath>
#include <cstring>
#include <cwchar>
#include <imgui.h>
#include <stdlib.h>
Log_SetChannel(HostInterface);

HostInterface* g_host_interface;

HostInterface::HostInterface()
{
  Assert(!g_host_interface);
  g_host_interface = this;

  // we can get the program directory at construction time
  const std::string program_path = FileSystem::GetProgramPath();
  m_program_directory = FileSystem::GetPathDirectory(program_path.c_str());
}

HostInterface::~HostInterface()
{
  // system should be shut down prior to the destructor
  Assert(System::IsShutdown() && !m_audio_stream && !m_display);
  Assert(g_host_interface == this);
  g_host_interface = nullptr;
}

bool HostInterface::Initialize()
{
  return true;
}

void HostInterface::Shutdown() {}

void HostInterface::CreateAudioStream()
{
  Log_InfoPrintf("Creating '%s' audio stream, sample rate = %u, channels = %u, buffer size = %u",
                 Settings::GetAudioBackendName(g_settings.audio_backend), AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                 g_settings.audio_buffer_size);

  m_audio_stream = CreateAudioStream(g_settings.audio_backend);

  if (!m_audio_stream || !m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, g_settings.audio_buffer_size))
  {
    ReportFormattedError("Failed to create or configure audio stream, falling back to null output.");
    m_audio_stream.reset();
    m_audio_stream = AudioStream::CreateNullAudioStream();
    m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, g_settings.audio_buffer_size);
  }

  m_audio_stream->SetOutputVolume(g_settings.audio_output_muted ? 0 : g_settings.audio_output_volume);
}

bool HostInterface::BootSystem(const SystemBootParameters& parameters)
{
  if (!parameters.state_stream)
  {
    if (parameters.filename.empty())
      Log_InfoPrintf("Boot Filename: <BIOS/Shell>");
    else
      Log_InfoPrintf("Boot Filename: %s", parameters.filename.c_str());
  }

  if (!AcquireHostDisplay())
  {
    ReportFormattedError("Failed to acquire host display");
    OnSystemDestroyed();
    return false;
  }

  // set host display settings
  m_display->SetDisplayLinearFiltering(g_settings.display_linear_filtering);
  m_display->SetDisplayIntegerScaling(g_settings.display_integer_scaling);

  // create the audio stream. this will never fail, since we'll just fall back to null
  CreateAudioStream();

  if (!System::Boot(parameters))
  {
    ReportFormattedError("System failed to boot. The log may contain more information.");
    OnSystemDestroyed();
    m_audio_stream.reset();
    ReleaseHostDisplay();
    return false;
  }

  UpdateSoftwareCursor();
  OnSystemCreated();

  m_audio_stream->PauseOutput(false);
  return true;
}

void HostInterface::ResetSystem()
{
  System::Reset();
  System::ResetPerformanceCounters();
  AddOSDMessage("System reset.");
}

void HostInterface::PowerOffSystem()
{
  DestroySystem();
}

void HostInterface::DestroySystem()
{
  if (System::IsShutdown())
    return;

  System::Shutdown();
  m_audio_stream.reset();
  UpdateSoftwareCursor();
  ReleaseHostDisplay();
  OnSystemDestroyed();
  OnRunningGameChanged();
}

void HostInterface::ReportError(const char* message)
{
  Log_ErrorPrint(message);
}

void HostInterface::ReportMessage(const char* message)
{
  Log_InfoPrintf(message);
}

bool HostInterface::ConfirmMessage(const char* message)
{
  Log_WarningPrintf("ConfirmMessage(\"%s\") -> Yes");
  return true;
}

void HostInterface::ReportFormattedError(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  ReportError(message.c_str());
}

void HostInterface::ReportFormattedMessage(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  ReportMessage(message.c_str());
}

bool HostInterface::ConfirmFormattedMessage(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  return ConfirmMessage(message.c_str());
}

void HostInterface::AddOSDMessage(std::string message, float duration /* = 2.0f */)
{
  Log_InfoPrintf("OSD: %s", message.c_str());
}

void HostInterface::AddFormattedOSDMessage(float duration, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  AddOSDMessage(std::move(message), duration);
}

std::optional<std::vector<u8>> HostInterface::GetBIOSImage(ConsoleRegion region)
{
  // Try the other default filenames in the directory of the configured BIOS.
#define TRY_FILENAME(filename)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    String try_filename = filename;                                                                                    \
    std::optional<BIOS::Image> found_image = BIOS::LoadImageFromFile(try_filename.GetCharArray());                     \
    if (found_image)                                                                                                   \
    {                                                                                                                  \
      BIOS::Hash found_hash = BIOS::GetHash(*found_image);                                                             \
      Log_DevPrintf("Hash for BIOS '%s': %s", try_filename.GetCharArray(), found_hash.ToString().c_str());             \
      if (BIOS::IsValidHashForRegion(region, found_hash))                                                              \
      {                                                                                                                \
        Log_InfoPrintf("Using BIOS from '%s' for region '%s'", try_filename.GetCharArray(),                            \
                       Settings::GetConsoleRegionName(region));                                                        \
        return found_image;                                                                                            \
      }                                                                                                                \
    }                                                                                                                  \
  } while (0)

  // Try the configured image.
  TRY_FILENAME(g_settings.bios_path.c_str());

  // Try searching in the same folder for other region's images.
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph3000.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-11j.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph1000.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-10j.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph5500.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-30j.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph7000.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph7500.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph9000.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-40j.bin", false, false));
      break;

    case ConsoleRegion::NTSC_U:
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph1001.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-22a.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph5501.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph5503.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph7003.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-30a.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph7001.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph7501.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph7503.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph9001.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph9003.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph9903.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-41a.bin", false, false));
      break;

    case ConsoleRegion::PAL:
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph1002.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-21e.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph5502.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph5552.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-30e.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph7002.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph7502.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "scph9002.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(g_settings.bios_path.c_str(), "ps-41e.bin", false, false));
      break;

    default:
      break;
  }

#undef RELATIVE_PATH
#undef TRY_FILENAME

  // Fall back to the default image.
  Log_WarningPrintf("No suitable BIOS image for region %s could be located, using configured image '%s'. This may "
                    "result in instability.",
                    Settings::GetConsoleRegionName(region), g_settings.bios_path.c_str());
  return BIOS::LoadImageFromFile(g_settings.bios_path);
}

bool HostInterface::LoadState(const char* filename)
{
  std::unique_ptr<ByteStream> stream = FileSystem::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  AddFormattedOSDMessage(2.0f, "Loading state from '%s'...", filename);

  if (!System::IsShutdown())
  {
    if (!System::LoadState(stream.get()))
    {
      ReportFormattedError("Loading state from '%s' failed. Resetting.", filename);
      ResetSystem();
      return false;
    }
  }
  else
  {
    SystemBootParameters boot_params;
    boot_params.state_stream = std::move(stream);
    if (!BootSystem(boot_params))
      return false;
  }

  System::ResetPerformanceCounters();
  return true;
}

bool HostInterface::SaveState(const char* filename)
{
  std::unique_ptr<ByteStream> stream =
    FileSystem::OpenFile(filename, BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_TRUNCATE |
                                     BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  const bool result = System::SaveState(stream.get());
  if (!result)
  {
    ReportFormattedError("Saving state to '%s' failed.", filename);
    stream->Discard();
  }
  else
  {
    AddFormattedOSDMessage(2.0f, "State saved to '%s'.", filename);
    stream->Commit();
  }

  return result;
}

void HostInterface::OnSystemCreated() {}

void HostInterface::OnSystemDestroyed() {}

void HostInterface::OnSystemPerformanceCountersUpdated() {}

void HostInterface::OnSystemStateSaved(bool global, s32 slot) {}

void HostInterface::OnRunningGameChanged() {}

void HostInterface::OnControllerTypeChanged(u32 slot) {}

std::string HostInterface::GetShaderCacheBasePath() const
{
  return GetUserDirectoryRelativePath("cache/");
}

void HostInterface::SetDefaultSettings(SettingsInterface& si)
{
  si.SetStringValue("Console", "Region", Settings::GetConsoleRegionName(Settings::DEFAULT_CONSOLE_REGION));

  si.SetFloatValue("Main", "EmulationSpeed", 1.0f);
  si.SetBoolValue("Main", "SpeedLimiterEnabled", true);
  si.SetBoolValue("Main", "IncreaseTimerResolution", true);
  si.SetBoolValue("Main", "StartPaused", false);
  si.SetBoolValue("Main", "SaveStateOnExit", true);
  si.SetBoolValue("Main", "ConfirmPowerOff", true);
  si.SetBoolValue("Main", "LoadDevicesFromSaveStates", false);

  si.SetStringValue("CPU", "ExecutionMode", Settings::GetCPUExecutionModeName(Settings::DEFAULT_CPU_EXECUTION_MODE));
  si.SetBoolValue("CPU", "RecompilerMemoryExceptions", false);

  si.SetStringValue("GPU", "Renderer", Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER));
  si.SetIntValue("GPU", "ResolutionScale", 1);
  si.SetBoolValue("GPU", "UseDebugDevice", false);
  si.SetBoolValue("GPU", "TrueColor", false);
  si.SetBoolValue("GPU", "ScaledDithering", true);
  si.SetBoolValue("GPU", "TextureFiltering", false);
  si.SetBoolValue("GPU", "DisableInterlacing", false);
  si.SetBoolValue("GPU", "ForceNTSCTimings", false);
  si.SetBoolValue("GPU", "WidescreenHack", false);
  si.SetBoolValue("GPU", "PGXPEnable", false);
  si.SetBoolValue("GPU", "PGXPCulling", true);
  si.SetBoolValue("GPU", "PGXPTextureCorrection", true);
  si.SetBoolValue("GPU", "PGXPVertexCache", false);

  si.SetStringValue("Display", "CropMode", Settings::GetDisplayCropModeName(Settings::DEFAULT_DISPLAY_CROP_MODE));
  si.SetStringValue("Display", "AspectRatio",
                    Settings::GetDisplayAspectRatioName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO));
  si.SetBoolValue("Display", "LinearFiltering", true);
  si.SetBoolValue("Display", "IntegerScaling", false);
  si.SetBoolValue("Display", "ShowOSDMessages", true);
  si.SetBoolValue("Display", "ShowFPS", false);
  si.SetBoolValue("Display", "ShowVPS", false);
  si.SetBoolValue("Display", "ShowSpeed", false);
  si.SetBoolValue("Display", "Fullscreen", false);
  si.SetBoolValue("Display", "VSync", true);

  si.SetBoolValue("CDROM", "ReadThread", true);
  si.SetBoolValue("CDROM", "RegionCheck", true);
  si.SetBoolValue("CDROM", "LoadImageToRAM", false);

  si.SetStringValue("Audio", "Backend", Settings::GetAudioBackendName(Settings::DEFAULT_AUDIO_BACKEND));
  si.SetIntValue("Audio", "OutputVolume", 100);
  si.SetIntValue("Audio", "BufferSize", DEFAULT_AUDIO_BUFFER_SIZE);
  si.SetIntValue("Audio", "OutputMuted", false);
  si.SetBoolValue("Audio", "Sync", true);
  si.SetBoolValue("Audio", "DumpOnBoot", false);

  si.SetStringValue("BIOS", "Path", "bios/scph1001.bin");
  si.SetBoolValue("BIOS", "PatchTTYEnable", false);
  si.SetBoolValue("BIOS", "PatchFastBoot", false);

  si.SetStringValue("Controller1", "Type", Settings::GetControllerTypeName(Settings::DEFAULT_CONTROLLER_1_TYPE));
  si.SetStringValue("Controller2", "Type", Settings::GetControllerTypeName(Settings::DEFAULT_CONTROLLER_2_TYPE));

  si.SetStringValue("MemoryCards", "Card1Type", Settings::GetMemoryCardTypeName(Settings::DEFAULT_MEMORY_CARD_1_TYPE));
  si.SetStringValue("MemoryCards", "Card1Path", "memcards/shared_card_1.mcd");
  si.SetStringValue("MemoryCards", "Card2Type", Settings::GetMemoryCardTypeName(Settings::DEFAULT_MEMORY_CARD_2_TYPE));
  si.SetStringValue("MemoryCards", "Card2Path", "memcards/shared_card_2.mcd");

  si.SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(Settings::DEFAULT_LOG_LEVEL));
  si.SetStringValue("Logging", "LogFilter", "");
  si.SetBoolValue("Logging", "LogToConsole", false);
  si.SetBoolValue("Logging", "LogToDebug", false);
  si.SetBoolValue("Logging", "LogToWindow", false);
  si.SetBoolValue("Logging", "LogToFile", false);

  si.SetBoolValue("Debug", "ShowVRAM", false);
  si.SetBoolValue("Debug", "DumpCPUToVRAMCopies", false);
  si.SetBoolValue("Debug", "DumpVRAMToCPUCopies", false);
  si.SetBoolValue("Debug", "ShowGPUState", false);
  si.SetBoolValue("Debug", "ShowCDROMState", false);
  si.SetBoolValue("Debug", "ShowSPUState", false);
  si.SetBoolValue("Debug", "ShowTimersState", false);
  si.SetBoolValue("Debug", "ShowMDECState", false);

  si.SetIntValue("Hacks", "DMAMaxSliceTicks", static_cast<int>(Settings::DEFAULT_DMA_MAX_SLICE_TICKS));
  si.SetIntValue("Hacks", "DMAHaltTicks", static_cast<int>(Settings::DEFAULT_DMA_HALT_TICKS));
  si.SetIntValue("Hacks", "GPUFIFOSize", static_cast<int>(Settings::DEFAULT_GPU_FIFO_SIZE));
  si.SetIntValue("Hacks", "GPUMaxRunAhead", static_cast<int>(Settings::DEFAULT_GPU_MAX_RUN_AHEAD));
}

void HostInterface::LoadSettings(SettingsInterface& si)
{
  g_settings.Load(si);
}

void HostInterface::SaveSettings(SettingsInterface& si)
{
  g_settings.Save(si);
}

void HostInterface::CheckForSettingsChanges(const Settings& old_settings)
{
  if (!System::IsShutdown())
  {
    if (g_settings.gpu_renderer != old_settings.gpu_renderer ||
        g_settings.gpu_use_debug_device != old_settings.gpu_use_debug_device)
    {
      ReportFormattedMessage("Switching to %s%s GPU renderer.", Settings::GetRendererName(g_settings.gpu_renderer),
                             g_settings.gpu_use_debug_device ? " (debug)" : "");
      RecreateSystem();
    }

    if (g_settings.audio_backend != old_settings.audio_backend ||
        g_settings.audio_buffer_size != old_settings.audio_buffer_size)
    {
      if (g_settings.audio_backend != old_settings.audio_backend)
        ReportFormattedMessage("Switching to %s audio backend.",
                               Settings::GetAudioBackendName(g_settings.audio_backend));
      DebugAssert(m_audio_stream);
      m_audio_stream.reset();
      CreateAudioStream();
      m_audio_stream->PauseOutput(System::IsPaused());
    }

    if (g_settings.emulation_speed != old_settings.emulation_speed)
      System::UpdateThrottlePeriod();

    if (g_settings.cpu_execution_mode != old_settings.cpu_execution_mode)
    {
      ReportFormattedMessage("Switching to %s CPU execution mode.",
                             Settings::GetCPUExecutionModeName(g_settings.cpu_execution_mode));
      CPU::CodeCache::SetUseRecompiler(g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler);
    }

    if (g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler &&
        g_settings.cpu_recompiler_memory_exceptions != old_settings.cpu_recompiler_memory_exceptions)
    {
      ReportFormattedMessage("CPU memory exceptions %s, flushing all blocks.",
                             g_settings.cpu_recompiler_memory_exceptions ? "enabled" : "disabled");
      CPU::CodeCache::Flush();
    }

    m_audio_stream->SetOutputVolume(g_settings.audio_output_muted ? 0 : g_settings.audio_output_volume);

    if (g_settings.gpu_resolution_scale != old_settings.gpu_resolution_scale ||
        g_settings.gpu_fifo_size != old_settings.gpu_fifo_size ||
        g_settings.gpu_max_run_ahead != old_settings.gpu_max_run_ahead ||
        g_settings.gpu_true_color != old_settings.gpu_true_color ||
        g_settings.gpu_scaled_dithering != old_settings.gpu_scaled_dithering ||
        g_settings.gpu_texture_filtering != old_settings.gpu_texture_filtering ||
        g_settings.gpu_disable_interlacing != old_settings.gpu_disable_interlacing ||
        g_settings.gpu_force_ntsc_timings != old_settings.gpu_force_ntsc_timings ||
        g_settings.display_crop_mode != old_settings.display_crop_mode ||
        g_settings.display_aspect_ratio != old_settings.display_aspect_ratio ||
        g_settings.gpu_pgxp_enable != old_settings.gpu_pgxp_enable)
    {
      g_gpu->UpdateSettings();
    }

    if (g_settings.gpu_pgxp_enable != old_settings.gpu_pgxp_enable ||
        (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling != old_settings.gpu_pgxp_culling))
    {
      if (g_settings.IsUsingCodeCache())
      {
        ReportFormattedMessage("PGXP %s, recompiling all blocks.", g_settings.gpu_pgxp_enable ? "enabled" : "disabled");
        CPU::CodeCache::Flush();
      }

      if (g_settings.gpu_pgxp_enable)
        PGXP::Initialize();
    }

    if (g_settings.cdrom_read_thread != old_settings.cdrom_read_thread)
      g_cdrom.SetUseReadThread(g_settings.cdrom_read_thread);

    if (g_settings.memory_card_types != old_settings.memory_card_types ||
        g_settings.memory_card_paths != old_settings.memory_card_paths)
    {
      System::UpdateMemoryCards();
    }

    g_dma.SetMaxSliceTicks(g_settings.dma_max_slice_ticks);
    g_dma.SetHaltTicks(g_settings.dma_halt_ticks);
  }

  bool controllers_updated = false;
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (g_settings.controller_types[i] != old_settings.controller_types[i])
    {
      if (!System::IsShutdown() && !controllers_updated)
      {
        System::UpdateControllers();
        System::ResetControllers();
        UpdateSoftwareCursor();
        controllers_updated = true;
      }

      OnControllerTypeChanged(i);
    }

    if (!System::IsShutdown() && !controllers_updated)
    {
      System::UpdateControllerSettings();
      UpdateSoftwareCursor();
    }
  }

  if (m_display && g_settings.display_linear_filtering != old_settings.display_linear_filtering)
    m_display->SetDisplayLinearFiltering(g_settings.display_linear_filtering);

  if (m_display && g_settings.display_integer_scaling != old_settings.display_integer_scaling)
    m_display->SetDisplayIntegerScaling(g_settings.display_integer_scaling);
}

void HostInterface::SetUserDirectoryToProgramDirectory()
{
  const std::string program_path = FileSystem::GetProgramPath();
  const std::string program_directory = FileSystem::GetPathDirectory(program_path.c_str());
  m_user_directory = program_directory;
}

std::string HostInterface::GetUserDirectoryRelativePath(const char* format, ...) const
{
  std::va_list ap;
  va_start(ap, format);
  std::string formatted_path = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  if (m_user_directory.empty())
  {
    return formatted_path;
  }
  else
  {
    return StringUtil::StdStringFromFormat("%s%c%s", m_user_directory.c_str(), FS_OSPATH_SEPERATOR_CHARACTER,
                                           formatted_path.c_str());
  }
}

std::string HostInterface::GetProgramDirectoryRelativePath(const char* format, ...) const
{
  std::va_list ap;
  va_start(ap, format);
  std::string formatted_path = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  if (m_program_directory.empty())
  {
    return formatted_path;
  }
  else
  {
    return StringUtil::StdStringFromFormat("%s%c%s", m_program_directory.c_str(), FS_OSPATH_SEPERATOR_CHARACTER,
                                           formatted_path.c_str());
  }
}

TinyString HostInterface::GetTimestampStringForFileName()
{
  const Timestamp ts(Timestamp::Now());

  TinyString str;
  ts.ToString(str, "%Y-%m-%d_%H-%M-%S");
  return str;
}

std::string HostInterface::GetSharedMemoryCardPath(u32 slot) const
{
  return GetUserDirectoryRelativePath("memcards/shared_card_%d.mcd", slot + 1);
}

std::string HostInterface::GetGameMemoryCardPath(const char* game_code, u32 slot) const
{
  return GetUserDirectoryRelativePath("memcards/%s_%d.mcd", game_code, slot + 1);
}

bool HostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
  std::string value = GetStringSettingValue(section, key, "");
  if (value.empty())
    return default_value;

  std::optional<bool> bool_value = StringUtil::FromChars<bool>(value);
  return bool_value.value_or(default_value);
}

int HostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /*= 0*/)
{
  std::string value = GetStringSettingValue(section, key, "");
  if (value.empty())
    return default_value;

  std::optional<int> int_value = StringUtil::FromChars<int>(value);
  return int_value.value_or(default_value);
}

float HostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  std::string value = GetStringSettingValue(section, key, "");
  if (value.empty())
    return default_value;

  std::optional<float> float_value = StringUtil::FromChars<float>(value);
  return float_value.value_or(default_value);
}

void HostInterface::ToggleSoftwareRendering()
{
  if (System::IsShutdown() || g_settings.gpu_renderer == GPURenderer::Software)
    return;

  const GPURenderer new_renderer = g_gpu->IsHardwareRenderer() ? GPURenderer::Software : g_settings.gpu_renderer;

  AddFormattedOSDMessage(2.0f, "Switching to %s renderer...", Settings::GetRendererDisplayName(new_renderer));
  System::RecreateGPU(new_renderer);
}

void HostInterface::ModifyResolutionScale(s32 increment)
{
  const u32 new_resolution_scale = std::clamp<u32>(
    static_cast<u32>(static_cast<s32>(g_settings.gpu_resolution_scale) + increment), 1, GPU::MAX_RESOLUTION_SCALE);
  if (new_resolution_scale == g_settings.gpu_resolution_scale)
    return;

  g_settings.gpu_resolution_scale = new_resolution_scale;
  AddFormattedOSDMessage(2.0f, "Resolution scale set to %ux (%ux%u)", g_settings.gpu_resolution_scale,
                         GPU::VRAM_WIDTH * g_settings.gpu_resolution_scale,
                         GPU::VRAM_HEIGHT * g_settings.gpu_resolution_scale);

  if (!System::IsShutdown())
  {
    g_gpu->RestoreGraphicsAPIState();
    g_gpu->UpdateSettings();
    g_gpu->ResetGraphicsAPIState();
  }
}

void HostInterface::UpdateSoftwareCursor()
{
  if (System::IsShutdown())
  {
    m_display->ClearSoftwareCursor();
    return;
  }

  const Common::RGBA8Image* image = nullptr;
  float image_scale = 1.0f;

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (controller && controller->GetSoftwareCursor(&image, &image_scale))
      break;
  }

  if (image && image->IsValid())
  {
    m_display->SetSoftwareCursor(image->GetPixels(), image->GetWidth(), image->GetHeight(), image->GetByteStride(),
                                 image_scale);
  }
  else
  {
    m_display->ClearSoftwareCursor();
  }
}

void HostInterface::RecreateSystem()
{
  Assert(!System::IsShutdown());

  std::unique_ptr<ByteStream> stream = ByteStream_CreateGrowableMemoryStream(nullptr, 8 * 1024);
  if (!System::SaveState(stream.get()) || !stream->SeekAbsolute(0))
  {
    ReportError("Failed to save state before system recreation. Shutting down.");
    DestroySystem();
    return;
  }

  DestroySystem();

  SystemBootParameters boot_params;
  boot_params.state_stream = std::move(stream);
  if (!BootSystem(boot_params))
  {
    ReportError("Failed to boot system after recreation.");
    return;
  }

  System::ResetPerformanceCounters();
}

void HostInterface::DisplayLoadingScreen(const char* message, int progress_min /*= -1*/, int progress_max /*= -1*/,
                                         int progress_value /*= -1*/)
{
  Log_InfoPrintf("Loading: %s %d of %d-%d", message, progress_value, progress_min, progress_max);
}

void HostInterface::GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title) {}

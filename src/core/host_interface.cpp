#include "host_interface.h"
#include "bios.h"
#include "cdrom.h"
#include "common/audio_stream.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "dma.h"
#include "gpu.h"
#include "host_display.h"
#include "save_state_version.h"
#include "system.h"
#include <cmath>
#include <cstring>
#include <cwchar>
#include <imgui.h>
#include <stdlib.h>
Log_SetChannel(HostInterface);

HostInterface::HostInterface()
{
  // we can get the program directory at construction time
  const std::string program_path = FileSystem::GetProgramPath();
  m_program_directory = FileSystem::GetPathDirectory(program_path.c_str());
}

HostInterface::~HostInterface()
{
  // system should be shut down prior to the destructor
  Assert(!m_system && !m_audio_stream && !m_display);
}

bool HostInterface::Initialize()
{
  return true;
}

void HostInterface::Shutdown() {}

void HostInterface::CreateAudioStream()
{
  Log_InfoPrintf("Creating '%s' audio stream, sample rate = %u, channels = %u, buffer size = %u",
                 Settings::GetAudioBackendName(m_settings.audio_backend), AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                 m_settings.audio_buffer_size);

  m_audio_stream = CreateAudioStream(m_settings.audio_backend);

  if (!m_audio_stream || !m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, m_settings.audio_buffer_size))
  {
    ReportFormattedError("Failed to create or configure audio stream, falling back to null output.");
    m_audio_stream.reset();
    m_audio_stream = AudioStream::CreateNullAudioStream();
    m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, m_settings.audio_buffer_size);
  }

  m_audio_stream->SetOutputVolume(m_settings.audio_output_muted ? 0 : m_settings.audio_output_volume);
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
  m_display->SetDisplayLinearFiltering(m_settings.display_linear_filtering);
  m_display->SetDisplayIntegerScaling(m_settings.display_integer_scaling);

  // create the audio stream. this will never fail, since we'll just fall back to null
  CreateAudioStream();

  m_system = System::Create(this);
  if (!m_system->Boot(parameters))
  {
    ReportFormattedError("System failed to boot. The log may contain more information.");
    DestroySystem();
    return false;
  }

  OnSystemCreated();

  m_audio_stream->PauseOutput(false);
  return true;
}

void HostInterface::ResetSystem()
{
  m_system->Reset();
  m_system->ResetPerformanceCounters();
  AddOSDMessage("System reset.");
}

void HostInterface::PowerOffSystem()
{
  if (!m_system)
    return;

  DestroySystem();
}

void HostInterface::DestroySystem()
{
  if (!m_system)
    return;

  m_system.reset();
  m_audio_stream.reset();
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
  TRY_FILENAME(m_settings.bios_path.c_str());

  // Try searching in the same folder for other region's images.
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "scph3000.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "ps-11j.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "scph1000.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "ps-10j.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "scph5500.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "ps-30j.bin", false, false));
      break;

    case ConsoleRegion::NTSC_U:
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "scph1001.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "ps-22a.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "scph5501.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "ps-30a.bin", false, false));
      break;

    case ConsoleRegion::PAL:
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "scph1002.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "ps-21e.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "scph5502.bin", false, false));
      TRY_FILENAME(FileSystem::BuildPathRelativeToFile(m_settings.bios_path.c_str(), "ps-30e.bin", false, false));
      break;

    default:
      break;
  }

#undef RELATIVE_PATH
#undef TRY_FILENAME

  // Fall back to the default image.
  Log_WarningPrintf("No suitable BIOS image for region %s could be located, using configured image '%s'. This may "
                    "result in instability.",
                    Settings::GetConsoleRegionName(region), m_settings.bios_path.c_str());
  return BIOS::LoadImageFromFile(m_settings.bios_path);
}

bool HostInterface::LoadState(const char* filename)
{
  std::unique_ptr<ByteStream> stream = FileSystem::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  AddFormattedOSDMessage(2.0f, "Loading state from '%s'...", filename);

  if (m_system)
  {
    if (!m_system->LoadState(stream.get()))
    {
      ReportFormattedError("Loading state from '%s' failed. Resetting.", filename);
      m_system->Reset();
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

  m_system->ResetPerformanceCounters();
  return true;
}

bool HostInterface::SaveState(const char* filename)
{
  std::unique_ptr<ByteStream> stream =
    FileSystem::OpenFile(filename, BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_TRUNCATE |
                                     BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  const bool result = m_system->SaveState(stream.get());
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

void HostInterface::OnSystemDestroyed()
{
  ReportFormattedMessage("System shut down.");
}

void HostInterface::OnSystemPerformanceCountersUpdated() {}

void HostInterface::OnSystemStateSaved(bool global, s32 slot) {}

void HostInterface::OnRunningGameChanged() {}

void HostInterface::OnControllerTypeChanged(u32 slot) {}

void HostInterface::SetDefaultSettings(SettingsInterface& si)
{
  si.SetStringValue("Console", "Region", Settings::GetConsoleRegionName(ConsoleRegion::Auto));

  si.SetFloatValue("Main", "EmulationSpeed", 1.0f);
  si.SetBoolValue("Main", "SpeedLimiterEnabled", true);
  si.SetBoolValue("Main", "IncreaseTimerResolution", true);
  si.SetBoolValue("Main", "StartPaused", false);
  si.SetBoolValue("Main", "SaveStateOnExit", true);
  si.SetBoolValue("Main", "ConfirmPowerOff", true);

  si.SetStringValue("CPU", "ExecutionMode", Settings::GetCPUExecutionModeName(CPUExecutionMode::Interpreter));

  si.SetStringValue("GPU", "Renderer", Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER));
  si.SetIntValue("GPU", "ResolutionScale", 1);
  si.SetBoolValue("GPU", "UseDebugDevice", false);
  si.SetBoolValue("GPU", "TrueColor", false);
  si.SetBoolValue("GPU", "ScaledDithering", true);
  si.SetBoolValue("GPU", "TextureFiltering", false);
  si.SetBoolValue("GPU", "DisableInterlacing", true);
  si.SetBoolValue("GPU", "ForceNTSCTimings", false);

  si.SetStringValue("Display", "CropMode", "Overscan");
  si.SetStringValue("Display", "PixelAspectRatio", "4:3");
  si.SetBoolValue("Display", "LinearFiltering", true);
  si.SetBoolValue("Display", "IntegerScaling", false);
  si.SetBoolValue("Display", "ShowOSDMessages", true);
  si.SetBoolValue("Display", "ShowFPS", false);
  si.SetBoolValue("Display", "ShowVPS", false);
  si.SetBoolValue("Display", "ShowSpeed", false);
  si.SetBoolValue("Display", "Fullscreen", false);
  si.SetBoolValue("Display", "VSync", true);
  si.SetStringValue("Display", "SoftwareCursorPath", "");
  si.SetFloatValue("Display", "SoftwareCursorScale", 1.0f);

  si.SetBoolValue("CDROM", "ReadThread", true);
  si.SetBoolValue("CDROM", "RegionCheck", true);

  si.SetStringValue("Audio", "Backend", Settings::GetAudioBackendName(AudioBackend::Cubeb));
  si.SetIntValue("Audio", "OutputVolume", 100);
  si.SetIntValue("Audio", "BufferSize", DEFAULT_AUDIO_BUFFER_SIZE);
  si.SetIntValue("Audio", "OutputMuted", false);
  si.SetBoolValue("Audio", "Sync", true);
  si.SetBoolValue("Audio", "DumpOnBoot", false);

  si.SetStringValue("BIOS", "Path", "bios/scph1001.bin");
  si.SetBoolValue("BIOS", "PatchTTYEnable", false);
  si.SetBoolValue("BIOS", "PatchFastBoot", false);

  si.SetStringValue("Controller1", "Type", Settings::GetControllerTypeName(ControllerType::DigitalController));
  si.SetStringValue("Controller2", "Type", Settings::GetControllerTypeName(ControllerType::None));

  si.SetBoolValue("MemoryCards", "LoadFromSaveStates", false);
  si.SetStringValue("MemoryCards", "Card1Type", Settings::GetMemoryCardTypeName(MemoryCardType::PerGameTitle));
  si.SetStringValue("MemoryCards", "Card1Path", "memcards/shared_card_1.mcd");
  si.SetStringValue("MemoryCards", "Card2Type", "None");
  si.SetStringValue("MemoryCards", "Card2Path", "memcards/shared_card_2.mcd");

  si.SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(LOGLEVEL_INFO));
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
}

void HostInterface::LoadSettings(SettingsInterface& si)
{
  m_settings.Load(si);
}

void HostInterface::SaveSettings(SettingsInterface& si)
{
  m_settings.Save(si);
}

void HostInterface::CheckForSettingsChanges(const Settings& old_settings)
{
  if (m_system)
  {
    if (m_settings.gpu_renderer != old_settings.gpu_renderer ||
        m_settings.gpu_use_debug_device != old_settings.gpu_use_debug_device)
    {
      ReportFormattedMessage("Switching to %s%s GPU renderer.", Settings::GetRendererName(m_settings.gpu_renderer),
                             m_settings.gpu_use_debug_device ? " (debug)" : "");
      RecreateSystem();
    }

    if (m_settings.audio_backend != old_settings.audio_backend ||
        m_settings.audio_buffer_size != old_settings.audio_buffer_size)
    {
      if (m_settings.audio_backend != old_settings.audio_backend)
        ReportFormattedMessage("Switching to %s audio backend.",
                               Settings::GetAudioBackendName(m_settings.audio_backend));
      DebugAssert(m_audio_stream);
      m_audio_stream.reset();
      CreateAudioStream();
      m_audio_stream->PauseOutput(false);
    }

    if (m_settings.emulation_speed != old_settings.emulation_speed)
      m_system->UpdateThrottlePeriod();

    if (m_settings.cpu_execution_mode != old_settings.cpu_execution_mode)
    {
      ReportFormattedMessage("Switching to %s CPU execution mode.",
                             Settings::GetCPUExecutionModeName(m_settings.cpu_execution_mode));
      m_system->SetCPUExecutionMode(m_settings.cpu_execution_mode);
    }

    m_audio_stream->SetOutputVolume(m_settings.audio_output_muted ? 0 : m_settings.audio_output_volume);

    if (m_settings.gpu_resolution_scale != old_settings.gpu_resolution_scale ||
        m_settings.gpu_fifo_size != old_settings.gpu_fifo_size ||
        m_settings.gpu_max_run_ahead != old_settings.gpu_max_run_ahead ||
        m_settings.gpu_true_color != old_settings.gpu_true_color ||
        m_settings.gpu_scaled_dithering != old_settings.gpu_scaled_dithering ||
        m_settings.gpu_texture_filtering != old_settings.gpu_texture_filtering ||
        m_settings.gpu_disable_interlacing != old_settings.gpu_disable_interlacing ||
        m_settings.gpu_force_ntsc_timings != old_settings.gpu_force_ntsc_timings ||
        m_settings.display_crop_mode != old_settings.display_crop_mode ||
        m_settings.display_aspect_ratio != old_settings.display_aspect_ratio)
    {
      m_system->UpdateGPUSettings();
    }

    if (m_settings.cdrom_read_thread != old_settings.cdrom_read_thread)
      m_system->GetCDROM()->SetUseReadThread(m_settings.cdrom_read_thread);

    if (m_settings.memory_card_types != old_settings.memory_card_types ||
        m_settings.memory_card_paths != old_settings.memory_card_paths)
    {
      m_system->UpdateMemoryCards();
    }

    m_system->GetDMA()->SetMaxSliceTicks(m_settings.dma_max_slice_ticks);
    m_system->GetDMA()->SetHaltTicks(m_settings.dma_halt_ticks);
  }

  bool controllers_updated = false;
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (m_settings.controller_types[i] != old_settings.controller_types[i])
    {
      if (m_system && !controllers_updated)
      {
        m_system->UpdateControllers();
        controllers_updated = true;
      }

      OnControllerTypeChanged(i);
    }
  }

  if (m_display && m_settings.display_linear_filtering != old_settings.display_linear_filtering)
    m_display->SetDisplayLinearFiltering(m_settings.display_linear_filtering);

  if (m_display && m_settings.display_integer_scaling != old_settings.display_integer_scaling)
    m_display->SetDisplayIntegerScaling(m_settings.display_integer_scaling);

  if (m_software_cursor_use_count > 0 && m_display &&
      (m_settings.display_software_cursor_path != old_settings.display_software_cursor_path ||
       m_settings.display_software_cursor_scale != old_settings.display_software_cursor_scale))
  {
    if (m_settings.display_software_cursor_path.empty())
    {
      m_display->ClearSoftwareCursor();
    }
    else
    {
      m_display->SetSoftwareCursor(m_settings.display_software_cursor_path.c_str(),
                                   m_settings.display_software_cursor_scale);
    }
  }
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

void HostInterface::ToggleSoftwareRendering()
{
  if (!m_system || m_settings.gpu_renderer == GPURenderer::Software)
    return;

  const GPURenderer new_renderer =
    m_system->GetGPU()->IsHardwareRenderer() ? GPURenderer::Software : m_settings.gpu_renderer;

  AddFormattedOSDMessage(2.0f, "Switching to %s renderer...", Settings::GetRendererDisplayName(new_renderer));
  m_system->RecreateGPU(new_renderer);
}

void HostInterface::ModifyResolutionScale(s32 increment)
{
  const u32 new_resolution_scale = std::clamp<u32>(
    static_cast<u32>(static_cast<s32>(m_settings.gpu_resolution_scale) + increment), 1, GPU::MAX_RESOLUTION_SCALE);
  if (new_resolution_scale == m_settings.gpu_resolution_scale)
    return;

  m_settings.gpu_resolution_scale = new_resolution_scale;
  AddFormattedOSDMessage(2.0f, "Resolution scale set to %ux (%ux%u)", m_settings.gpu_resolution_scale,
                         GPU::VRAM_WIDTH * m_settings.gpu_resolution_scale,
                         GPU::VRAM_HEIGHT * m_settings.gpu_resolution_scale);

  if (m_system)
    m_system->GetGPU()->UpdateSettings();
}

void HostInterface::RecreateSystem()
{
  std::unique_ptr<ByteStream> stream = ByteStream_CreateGrowableMemoryStream(nullptr, 8 * 1024);
  if (!m_system->SaveState(stream.get()) || !stream->SeekAbsolute(0))
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

  m_system->ResetPerformanceCounters();
}

void HostInterface::DisplayLoadingScreen(const char* message, int progress_min /*= -1*/, int progress_max /*= -1*/,
                                         int progress_value /*= -1*/)
{
  Log_InfoPrintf("Loading: %s %d of %d-%d", message, progress_value, progress_min, progress_max);
}

void HostInterface::EnableSoftwareCursor()
{
  if (m_software_cursor_use_count++ > 0 || m_settings.display_software_cursor_path.empty())
    return;

  m_display->SetSoftwareCursor(m_settings.display_software_cursor_path.c_str(),
                               m_settings.display_software_cursor_scale);
}

void HostInterface::DisableSoftwareCursor()
{
  DebugAssert(m_software_cursor_use_count > 0);
  if (--m_software_cursor_use_count > 0)
    return;

  m_display->ClearSoftwareCursor();
}

void HostInterface::GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title) {}

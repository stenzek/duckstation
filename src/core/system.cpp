#include "system.h"
#include "bios.h"
#include "bus.h"
#include "cdrom.h"
#include "common/audio_stream.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "controller.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "dma.h"
#include "game_list.h"
#include "gpu.h"
#include "gte.h"
#include "host_display.h"
#include "host_interface.h"
#include "host_interface_progress_callback.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "memory_card.h"
#include "pad.h"
#include "psf_loader.h"
#include "save_state_version.h"
#include "sio.h"
#include "spu.h"
#include "timers.h"
#include <cstdio>
#include <imgui.h>
#include <limits>
Log_SetChannel(System);

#ifdef WIN32
#include "common/windows_headers.h"
#else
#include <time.h>
#endif

std::unique_ptr<System> g_system;

SystemBootParameters::SystemBootParameters() = default;

SystemBootParameters::SystemBootParameters(std::string filename_) : filename(filename_) {}

SystemBootParameters::SystemBootParameters(const SystemBootParameters& copy)
  : filename(copy.filename), override_fast_boot(copy.override_fast_boot), override_fullscreen(copy.override_fullscreen)
{
  // only exists for qt, we can't copy the state stream
  Assert(!copy.state_stream);
}

SystemBootParameters::~SystemBootParameters() = default;

System::System()
{
  m_region = g_settings.region;
  m_cpu_execution_mode = g_settings.cpu_execution_mode;
}

System::~System()
{
  // we have to explicitly destroy components because they can deregister events
  DestroyComponents();
}

ConsoleRegion System::GetConsoleRegionForDiscRegion(DiscRegion region)
{
  switch (region)
  {
    case DiscRegion::NTSC_J:
      return ConsoleRegion::NTSC_J;

    case DiscRegion::NTSC_U:
    case DiscRegion::Other:
    default:
      return ConsoleRegion::NTSC_U;

    case DiscRegion::PAL:
      return ConsoleRegion::PAL;
  }
}

std::unique_ptr<System> System::Create()
{
  return std::unique_ptr<System>(new System());
}

bool System::RecreateGPU(GPURenderer renderer)
{
  // save current state
  std::unique_ptr<ByteStream> state_stream = ByteStream_CreateGrowableMemoryStream();
  StateWrapper sw(state_stream.get(), StateWrapper::Mode::Write);
  const bool state_valid = g_gpu->DoState(sw) && DoEventsState(sw);
  if (!state_valid)
    Log_ErrorPrintf("Failed to save old GPU state when switching renderers");

  // create new renderer
  g_gpu.reset();
  if (!CreateGPU(renderer))
  {
    Panic("Failed to recreate GPU");
    return false;
  }

  if (state_valid)
  {
    state_stream->SeekAbsolute(0);
    sw.SetMode(StateWrapper::Mode::Read);
    g_gpu->DoState(sw);
    DoEventsState(sw);
  }

  return true;
}

void System::UpdateGPUSettings()
{
  g_gpu->UpdateSettings();
}

void System::SetCPUExecutionMode(CPUExecutionMode mode)
{
  m_cpu_execution_mode = mode;
  CPU::CodeCache::SetUseRecompiler(mode == CPUExecutionMode::Recompiler);
}

std::unique_ptr<CDImage> System::OpenCDImage(const char* path, bool force_preload)
{
  std::unique_ptr<CDImage> media = CDImage::Open(path);
  if (!media)
    return {};

  if (force_preload || g_settings.cdrom_load_image_to_ram)
  {
    HostInterfaceProgressCallback callback;
    std::unique_ptr<CDImage> memory_image = CDImage::CreateMemoryImage(media.get(), &callback);
    if (memory_image)
      media = std::move(memory_image);
    else
      Log_WarningPrintf("Failed to preload image '%s' to RAM", path);
  }

  return media;
}

bool System::Boot(const SystemBootParameters& params)
{
  if (params.state_stream)
    return DoLoadState(params.state_stream.get(), true, params.force_software_renderer);

  // Load CD image up and detect region.
  std::unique_ptr<CDImage> media;
  bool exe_boot = false;
  bool psf_boot = false;
  if (!params.filename.empty())
  {
    exe_boot = GameList::IsExeFileName(params.filename.c_str());
    psf_boot = (!exe_boot && GameList::IsPsfFileName(params.filename.c_str()));
    if (exe_boot || psf_boot)
    {
      // TODO: Pull region from PSF
      if (m_region == ConsoleRegion::Auto)
      {
        Log_InfoPrintf("Defaulting to NTSC-U region for executable.");
        m_region = ConsoleRegion::NTSC_U;
      }
    }
    else
    {
      u32 playlist_index;
      if (GameList::IsM3UFileName(params.filename.c_str()))
      {
        m_media_playlist = GameList::ParseM3UFile(params.filename.c_str());
        if (m_media_playlist.empty())
        {
          g_host_interface->ReportFormattedError("Failed to parse playlist '%s'", params.filename.c_str());
          return false;
        }

        if (params.media_playlist_index >= m_media_playlist.size())
        {
          Log_WarningPrintf("Media playlist index %u out of range, using first", params.media_playlist_index);
          playlist_index = 0;
        }
        else
        {
          playlist_index = params.media_playlist_index;
        }
      }
      else
      {
        AddMediaPathToPlaylist(params.filename);
        playlist_index = 0;
      }

      const std::string& media_path = m_media_playlist[playlist_index];
      Log_InfoPrintf("Loading CD image '%s' from playlist index %u...", media_path.c_str(), playlist_index);
      media = OpenCDImage(media_path.c_str(), params.load_image_to_ram);
      if (!media)
      {
        g_host_interface->ReportFormattedError("Failed to load CD image '%s'", params.filename.c_str());
        return false;
      }

      if (m_region == ConsoleRegion::Auto)
      {
        const DiscRegion disc_region = GameList::GetRegionForImage(media.get());
        if (disc_region != DiscRegion::Other)
        {
          m_region = GetConsoleRegionForDiscRegion(disc_region);
          Log_InfoPrintf("Auto-detected console %s region for '%s' (region %s)",
                         Settings::GetConsoleRegionName(m_region), params.filename.c_str(),
                         Settings::GetDiscRegionName(disc_region));
        }
        else
        {
          m_region = ConsoleRegion::NTSC_U;
          Log_WarningPrintf("Could not determine console region for disc region %s. Defaulting to %s.",
                            Settings::GetDiscRegionName(disc_region), Settings::GetConsoleRegionName(m_region));
        }
      }
    }
  }
  else
  {
    // Default to NTSC for BIOS boot.
    if (m_region == ConsoleRegion::Auto)
      m_region = ConsoleRegion::NTSC_U;
  }

  // Load BIOS image.
  std::optional<BIOS::Image> bios_image = g_host_interface->GetBIOSImage(m_region);
  if (!bios_image)
  {
    g_host_interface->ReportFormattedError("Failed to load %s BIOS", Settings::GetConsoleRegionName(m_region));
    return false;
  }

  // Component setup.
  if (!InitializeComponents(params.force_software_renderer))
    return false;

  // Notify change of disc.
  UpdateRunningGame(params.filename.c_str(), media.get());
  UpdateControllers();
  UpdateMemoryCards();
  Reset();

  // Enable tty by patching bios.
  const BIOS::Hash bios_hash = BIOS::GetHash(*bios_image);
  if (g_settings.bios_patch_tty_enable)
    BIOS::PatchBIOSEnableTTY(*bios_image, bios_hash);

  // Load EXE late after BIOS.
  if (exe_boot && !LoadEXE(params.filename.c_str(), *bios_image))
  {
    g_host_interface->ReportFormattedError("Failed to load EXE file '%s'", params.filename.c_str());
    return false;
  }
  else if (psf_boot && !LoadPSF(params.filename.c_str(), *bios_image))
  {
    g_host_interface->ReportFormattedError("Failed to load PSF file '%s'", params.filename.c_str());
    return false;
  }

  // Insert CD, and apply fastboot patch if enabled.
  if (media)
    g_cdrom.InsertMedia(std::move(media));
  if (g_cdrom.HasMedia() &&
      (params.override_fast_boot.has_value() ? params.override_fast_boot.value() : g_settings.bios_patch_fast_boot))
  {
    BIOS::PatchBIOSFastBoot(*bios_image, bios_hash);
  }

  // Load the patched BIOS up.
  Bus::SetBIOS(*bios_image);

  // Good to go.
  return true;
}

bool System::InitializeComponents(bool force_software_renderer)
{
  const Settings& settings = g_settings;
  if (!CreateGPU(force_software_renderer ? GPURenderer::Software : settings.gpu_renderer))
    return false;

  CPU::Initialize();
  CPU::CodeCache::Initialize(m_cpu_execution_mode == CPUExecutionMode::Recompiler);
  Bus::Initialize();

  g_dma.Initialize();

  g_interrupt_controller.Initialize();

  g_cdrom.Initialize();
  g_pad.Initialize();
  g_timers.Initialize();
  g_spu.Initialize();
  g_mdec.Initialize();
  g_sio.Initialize();

  // load settings
  GTE::SetWidescreenHack(settings.gpu_widescreen_hack);

  UpdateThrottlePeriod();
  return true;
}

void System::DestroyComponents()
{
  g_sio.Shutdown();
  g_mdec.Shutdown();
  g_spu.Shutdown();
  g_timers.Shutdown();
  g_pad.Shutdown();
  g_cdrom.Shutdown();
  g_gpu.reset();
  g_interrupt_controller.Shutdown();
  g_dma.Shutdown();
  CPU::CodeCache::Shutdown();
  Bus::Shutdown();
  CPU::Shutdown();
}

bool System::CreateGPU(GPURenderer renderer)
{
  switch (renderer)
  {
    case GPURenderer::HardwareOpenGL:
      g_gpu = GPU::CreateHardwareOpenGLRenderer();
      break;

    case GPURenderer::HardwareVulkan:
      g_gpu = GPU::CreateHardwareVulkanRenderer();
      break;

#ifdef WIN32
    case GPURenderer::HardwareD3D11:
      g_gpu = GPU::CreateHardwareD3D11Renderer();
      break;
#endif

    case GPURenderer::Software:
    default:
      g_gpu = GPU::CreateSoftwareRenderer();
      break;
  }

  if (!g_gpu || !g_gpu->Initialize(g_host_interface->GetDisplay()))
  {
    Log_ErrorPrintf("Failed to initialize GPU, falling back to software");
    g_gpu.reset();
    g_gpu = GPU::CreateSoftwareRenderer();
    if (!g_gpu->Initialize(g_host_interface->GetDisplay()))
      return false;
  }

  return true;
}

bool System::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("System"))
    return false;

  sw.Do(&m_region);
  sw.Do(&m_frame_number);
  sw.Do(&m_internal_frame_number);
  sw.Do(&m_global_tick_counter);

  if (!sw.DoMarker("CPU") || !CPU::DoState(sw))
    return false;

  if (sw.IsReading())
    CPU::CodeCache::Flush();

  if (!sw.DoMarker("Bus") || !Bus::DoState(sw))
    return false;

  if (!sw.DoMarker("DMA") || !g_dma.DoState(sw))
    return false;

  if (!sw.DoMarker("InterruptController") || !g_interrupt_controller.DoState(sw))
    return false;

  if (!sw.DoMarker("GPU") || !g_gpu->DoState(sw))
    return false;

  if (!sw.DoMarker("CDROM") || !g_cdrom.DoState(sw))
    return false;

  if (!sw.DoMarker("Pad") || !g_pad.DoState(sw))
    return false;

  if (!sw.DoMarker("Timers") || !g_timers.DoState(sw))
    return false;

  if (!sw.DoMarker("SPU") || !g_spu.DoState(sw))
    return false;

  if (!sw.DoMarker("MDEC") || !g_mdec.DoState(sw))
    return false;

  if (!sw.DoMarker("SIO") || !g_sio.DoState(sw))
    return false;

  if (!sw.DoMarker("Events") || !DoEventsState(sw))
    return false;

  return !sw.HasError();
}

void System::Reset()
{
  CPU::Reset();
  CPU::CodeCache::Flush();
  Bus::Reset();
  g_dma.Reset();
  g_interrupt_controller.Reset();
  g_gpu->Reset();
  g_cdrom.Reset();
  g_pad.Reset();
  g_timers.Reset();
  g_spu.Reset();
  g_mdec.Reset();
  g_sio.Reset();
  m_frame_number = 1;
  m_internal_frame_number = 0;
  m_global_tick_counter = 0;
  m_last_event_run_time = 0;
  ResetPerformanceCounters();
}

bool System::LoadState(ByteStream* state)
{
  return DoLoadState(state, false, false);
}

bool System::DoLoadState(ByteStream* state, bool init_components, bool force_software_renderer)
{
  SAVE_STATE_HEADER header;
  if (!state->Read2(&header, sizeof(header)))
    return false;

  if (header.magic != SAVE_STATE_MAGIC)
    return false;

  if (header.version != SAVE_STATE_VERSION)
  {
    g_host_interface->ReportFormattedError("Save state is incompatible: expecting version %u but state is version %u.",
                                           SAVE_STATE_VERSION, header.version);
    return false;
  }

  std::string media_filename;
  std::unique_ptr<CDImage> media;
  if (header.media_filename_length > 0)
  {
    media_filename.resize(header.media_filename_length);
    if (!state->SeekAbsolute(header.offset_to_media_filename) ||
        !state->Read2(media_filename.data(), header.media_filename_length))
    {
      return false;
    }

    media = g_cdrom.RemoveMedia();
    if (!media || media->GetFileName() != media_filename)
    {
      media = OpenCDImage(media_filename.c_str(), false);
      if (!media)
      {
        g_host_interface->ReportFormattedError("Failed to open CD image from save state: '%s'.",
                                               media_filename.c_str());
        return false;
      }
    }
  }

  UpdateRunningGame(media_filename.c_str(), media.get());

  if (init_components)
  {
    if (!InitializeComponents(force_software_renderer))
      return false;

    UpdateControllers();
    UpdateMemoryCards();

    if (media)
      g_cdrom.InsertMedia(std::move(media));
  }
  else
  {
    g_cdrom.Reset();
    if (media)
      g_cdrom.InsertMedia(std::move(media));
    else
      g_cdrom.RemoveMedia();

    // ensure the correct card is loaded
    if (g_settings.HasAnyPerGameMemoryCards())
      UpdateMemoryCards();
  }

  if (header.data_compression_type != 0)
  {
    g_host_interface->ReportFormattedError("Unknown save state compression type %u", header.data_compression_type);
    return false;
  }

  if (!state->SeekAbsolute(header.offset_to_data))
    return false;

  StateWrapper sw(state, StateWrapper::Mode::Read);
  return DoState(sw);
}

bool System::SaveState(ByteStream* state, u32 screenshot_size /* = 128 */)
{
  SAVE_STATE_HEADER header = {};

  const u64 header_position = state->GetPosition();
  if (!state->Write2(&header, sizeof(header)))
    return false;

  // fill in header
  header.magic = SAVE_STATE_MAGIC;
  header.version = SAVE_STATE_VERSION;
  StringUtil::Strlcpy(header.title, m_running_game_title.c_str(), sizeof(header.title));
  StringUtil::Strlcpy(header.game_code, m_running_game_code.c_str(), sizeof(header.game_code));

  if (g_cdrom.HasMedia())
  {
    const std::string& media_filename = g_cdrom.GetMediaFileName();
    header.offset_to_media_filename = static_cast<u32>(state->GetPosition());
    header.media_filename_length = static_cast<u32>(media_filename.length());
    if (!media_filename.empty() && !state->Write2(media_filename.data(), header.media_filename_length))
      return false;
  }

  // save screenshot
  if (screenshot_size > 0)
  {
    std::vector<u32> screenshot_buffer;
    g_gpu->ResetGraphicsAPIState();
    const bool screenshot_saved =
      g_host_interface->GetDisplay()->WriteDisplayTextureToBuffer(&screenshot_buffer, screenshot_size, screenshot_size);
    g_gpu->RestoreGraphicsAPIState();
    if (screenshot_saved && !screenshot_buffer.empty())
    {
      header.offset_to_screenshot = static_cast<u32>(state->GetPosition());
      header.screenshot_width = screenshot_size;
      header.screenshot_height = screenshot_size;
      header.screenshot_size = static_cast<u32>(screenshot_buffer.size() * sizeof(u32));
      if (!state->Write2(screenshot_buffer.data(), header.screenshot_size))
        return false;
    }
  }

  // write data
  {
    header.offset_to_data = static_cast<u32>(state->GetPosition());

    StateWrapper sw(state, StateWrapper::Mode::Write);
    if (!DoState(sw))
      return false;

    header.data_compression_type = 0;
    header.data_uncompressed_size = static_cast<u32>(state->GetPosition() - header.offset_to_data);
  }

  // re-write header
  const u64 end_position = state->GetPosition();
  if (!state->SeekAbsolute(header_position) || !state->Write2(&header, sizeof(header)) ||
      !state->SeekAbsolute(end_position))
  {
    return false;
  }

  return true;
}

void System::RunFrame()
{
  m_frame_timer.Reset();
  m_frame_done = false;

  // Duplicated to avoid branch in the while loop, as the downcount can be quite low at times.
  if (m_cpu_execution_mode == CPUExecutionMode::Interpreter)
  {
    do
    {
      UpdateCPUDowncount();
      CPU::Execute();
      RunEvents();
    } while (!m_frame_done);
  }
  else
  {
    do
    {
      UpdateCPUDowncount();
      CPU::CodeCache::Execute();
      RunEvents();
    } while (!m_frame_done);
  }

  // Generate any pending samples from the SPU before sleeping, this way we reduce the chances of underruns.
  g_spu.GeneratePendingSamples();
}

void System::SetThrottleFrequency(float frequency)
{
  m_throttle_frequency = frequency;
  UpdateThrottlePeriod();
}

void System::UpdateThrottlePeriod()
{
  m_throttle_period = static_cast<s32>(1000000000.0 / static_cast<double>(m_throttle_frequency) /
                                       static_cast<double>(g_settings.emulation_speed));
}

void System::Throttle()
{
  // Allow variance of up to 40ms either way.
  constexpr s64 MAX_VARIANCE_TIME = INT64_C(40000000);

  // Don't sleep for <1ms or >=period.
  constexpr s64 MINIMUM_SLEEP_TIME = INT64_C(1000000);

  // Use unsigned for defined overflow/wrap-around.
  const u64 time = static_cast<u64>(m_throttle_timer.GetTimeNanoseconds());
  const s64 sleep_time = static_cast<s64>(m_last_throttle_time - time);
  if (sleep_time < -MAX_VARIANCE_TIME)
  {
#ifndef _DEBUG
    // Don't display the slow messages in debug, it'll always be slow...
    // Limit how often the messages are displayed.
    if (m_speed_lost_time_timestamp.GetTimeSeconds() >= 1.0f)
    {
      Log_WarningPrintf("System too slow, lost %.2f ms",
                        static_cast<double>(-sleep_time - MAX_VARIANCE_TIME) / 1000000.0);
      m_speed_lost_time_timestamp.Reset();
    }
#endif
    m_last_throttle_time = 0;
    m_throttle_timer.Reset();
  }
  else if (sleep_time >= MINIMUM_SLEEP_TIME && sleep_time <= m_throttle_period)
  {
#ifdef WIN32
    Sleep(static_cast<u32>(sleep_time / 1000000));
#else
    const struct timespec ts = {0, static_cast<long>(sleep_time)};
    nanosleep(&ts, nullptr);
#endif
  }

  m_last_throttle_time += m_throttle_period;
}

void System::UpdatePerformanceCounters()
{
  const float frame_time = static_cast<float>(m_frame_timer.GetTimeMilliseconds());
  m_average_frame_time_accumulator += frame_time;
  m_worst_frame_time_accumulator = std::max(m_worst_frame_time_accumulator, frame_time);

  // update fps counter
  const float time = static_cast<float>(m_fps_timer.GetTimeSeconds());
  if (time < 1.0f)
    return;

  const float frames_presented = static_cast<float>(m_frame_number - m_last_frame_number);

  m_worst_frame_time = m_worst_frame_time_accumulator;
  m_worst_frame_time_accumulator = 0.0f;
  m_average_frame_time = m_average_frame_time_accumulator / frames_presented;
  m_average_frame_time_accumulator = 0.0f;
  m_vps = static_cast<float>(frames_presented / time);
  m_last_frame_number = m_frame_number;
  m_fps = static_cast<float>(m_internal_frame_number - m_last_internal_frame_number) / time;
  m_last_internal_frame_number = m_internal_frame_number;
  m_speed = static_cast<float>(static_cast<double>(m_global_tick_counter - m_last_global_tick_counter) /
                               (static_cast<double>(MASTER_CLOCK) * time)) *
            100.0f;
  m_last_global_tick_counter = m_global_tick_counter;
  m_fps_timer.Reset();

  g_host_interface->OnSystemPerformanceCountersUpdated();
}

void System::ResetPerformanceCounters()
{
  m_last_frame_number = m_frame_number;
  m_last_internal_frame_number = m_internal_frame_number;
  m_last_global_tick_counter = m_global_tick_counter;
  m_average_frame_time_accumulator = 0.0f;
  m_worst_frame_time_accumulator = 0.0f;
  m_fps_timer.Reset();
  m_throttle_timer.Reset();
  m_last_throttle_time = 0;
}

bool System::LoadEXE(const char* filename, std::vector<u8>& bios_image)
{
  std::FILE* fp = std::fopen(filename, "rb");
  if (!fp)
    return false;

  std::fseek(fp, 0, SEEK_END);
  const u32 file_size = static_cast<u32>(std::ftell(fp));
  std::fseek(fp, 0, SEEK_SET);

  BIOS::PSEXEHeader header;
  if (std::fread(&header, sizeof(header), 1, fp) != 1 || !BIOS::IsValidPSExeHeader(header, file_size))
  {
    std::fclose(fp);
    return false;
  }

  if (header.memfill_size > 0)
  {
    const u32 words_to_write = header.memfill_size / 4;
    u32 address = header.memfill_start & ~UINT32_C(3);
    for (u32 i = 0; i < words_to_write; i++)
    {
      CPU::SafeWriteMemoryWord(address, 0);
      address += sizeof(u32);
    }
  }

  if (header.file_size >= 4)
  {
    std::vector<u32> data_words((header.file_size + 3) / 4);
    if (std::fread(data_words.data(), header.file_size, 1, fp) != 1)
    {
      std::fclose(fp);
      return false;
    }

    const u32 num_words = header.file_size / 4;
    u32 address = header.load_address;
    for (u32 i = 0; i < num_words; i++)
    {
      CPU::SafeWriteMemoryWord(address, data_words[i]);
      address += sizeof(u32);
    }
  }

  std::fclose(fp);

  // patch the BIOS to jump to the executable directly
  const u32 r_pc = header.initial_pc;
  const u32 r_gp = header.initial_gp;
  const u32 r_sp = header.initial_sp_base + header.initial_sp_offset;
  const u32 r_fp = header.initial_sp_base + header.initial_sp_offset;
  return BIOS::PatchBIOSForEXE(bios_image, r_pc, r_gp, r_sp, r_fp);
}

bool System::LoadEXEFromBuffer(const void* buffer, u32 buffer_size, std::vector<u8>& bios_image)
{
  const u8* buffer_ptr = static_cast<const u8*>(buffer);
  const u8* buffer_end = static_cast<const u8*>(buffer) + buffer_size;

  BIOS::PSEXEHeader header;
  if (buffer_size < sizeof(header))
    return false;

  std::memcpy(&header, buffer_ptr, sizeof(header));
  buffer_ptr += sizeof(header);

  if (!BIOS::IsValidPSExeHeader(header, static_cast<u32>(buffer_end - buffer_ptr)))
    return false;

  if (header.memfill_size > 0)
  {
    const u32 words_to_write = header.memfill_size / 4;
    u32 address = header.memfill_start & ~UINT32_C(3);
    for (u32 i = 0; i < words_to_write; i++)
    {
      CPU::SafeWriteMemoryWord(address, 0);
      address += sizeof(u32);
    }
  }

  if (header.file_size >= 4)
  {
    std::vector<u32> data_words((header.file_size + 3) / 4);
    if ((buffer_end - buffer_ptr) < header.file_size)
      return false;

    std::memcpy(data_words.data(), buffer_ptr, header.file_size);

    const u32 num_words = header.file_size / 4;
    u32 address = header.load_address;
    for (u32 i = 0; i < num_words; i++)
    {
      CPU::SafeWriteMemoryWord(address, data_words[i]);
      address += sizeof(u32);
    }
  }

  // patch the BIOS to jump to the executable directly
  const u32 r_pc = header.initial_pc;
  const u32 r_gp = header.initial_gp;
  const u32 r_sp = header.initial_sp_base + header.initial_sp_offset;
  const u32 r_fp = header.initial_sp_base + header.initial_sp_offset;
  return BIOS::PatchBIOSForEXE(bios_image, r_pc, r_gp, r_sp, r_fp);
}

bool System::LoadPSF(const char* filename, std::vector<u8>& bios_image)
{
  Log_InfoPrintf("Loading PSF file from '%s'", filename);

  PSFLoader::File psf;
  if (!psf.Load(filename))
    return false;

  const std::vector<u8>& exe_data = psf.GetProgramData();
  return LoadEXEFromBuffer(exe_data.data(), static_cast<u32>(exe_data.size()), bios_image);
}

bool System::SetExpansionROM(const char* filename)
{
  std::FILE* fp = std::fopen(filename, "rb");
  if (!fp)
  {
    Log_ErrorPrintf("Failed to open '%s'", filename);
    return false;
  }

  std::fseek(fp, 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(fp));
  std::fseek(fp, 0, SEEK_SET);

  std::vector<u8> data(size);
  if (std::fread(data.data(), size, 1, fp) != 1)
  {
    Log_ErrorPrintf("Failed to read ROM data from '%s'", filename);
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);

  Log_InfoPrintf("Loaded expansion ROM from '%s': %u bytes", filename, size);
  Bus::SetExpansionROM(std::move(data));
  return true;
}

void System::StallCPU(TickCount ticks)
{
  CPU::AddPendingTicks(ticks);
#if 0
  if (CPU::GetPendingTicks() >= CPU::GetDowncount() && !m_running_events)
    RunEvents();
#endif
}

Controller* System::GetController(u32 slot) const
{
  return g_pad.GetController(slot);
}

void System::UpdateControllers()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    g_pad.SetController(i, nullptr);

    const ControllerType type = g_settings.controller_types[i];
    if (type != ControllerType::None)
    {
      std::unique_ptr<Controller> controller = Controller::Create(type, i);
      if (controller)
      {
        controller->LoadSettings(TinyString::FromFormat("Controller%u", i + 1u));
        g_pad.SetController(i, std::move(controller));
      }
    }
  }
}

void System::UpdateControllerSettings()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = g_pad.GetController(i);
    if (controller)
      controller->LoadSettings(TinyString::FromFormat("Controller%u", i + 1u));
  }
}

void System::ResetControllers()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = g_pad.GetController(i);
    if (controller)
      controller->Reset();
  }
}

void System::UpdateMemoryCards()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    g_pad.SetMemoryCard(i, nullptr);

    std::unique_ptr<MemoryCard> card;
    const MemoryCardType type = g_settings.memory_card_types[i];
    switch (type)
    {
      case MemoryCardType::None:
        continue;

      case MemoryCardType::PerGame:
      {
        if (m_running_game_code.empty())
        {
          g_host_interface->AddFormattedOSDMessage(5.0f,
                                                   "Per-game memory card cannot be used for slot %u as the running "
                                                   "game has no code. Using shared card instead.",
                                                   i + 1u);
          card = MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(i));
        }
        else
        {
          card = MemoryCard::Open(g_host_interface->GetGameMemoryCardPath(m_running_game_code.c_str(), i));
        }
      }
      break;

      case MemoryCardType::PerGameTitle:
      {
        if (m_running_game_title.empty())
        {
          g_host_interface->AddFormattedOSDMessage(5.0f,
                                                   "Per-game memory card cannot be used for slot %u as the running "
                                                   "game has no title. Using shared card instead.",
                                                   i + 1u);
          card = MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(i));
        }
        else
        {
          card = MemoryCard::Open(g_host_interface->GetGameMemoryCardPath(m_running_game_title.c_str(), i));
        }
      }
      break;

      case MemoryCardType::Shared:
      {
        if (g_settings.memory_card_paths[i].empty())
        {
          g_host_interface->AddFormattedOSDMessage(2.0f, "Memory card path for slot %u is missing, using default.",
                                                   i + 1u);
          card = MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(i));
        }
        else
        {
          card = MemoryCard::Open(g_settings.memory_card_paths[i]);
        }
      }
      break;
    }

    if (card)
      g_pad.SetMemoryCard(i, std::move(card));
  }
}

bool System::HasMedia() const
{
  return g_cdrom.HasMedia();
}

bool System::InsertMedia(const char* path)
{
  std::unique_ptr<CDImage> image = OpenCDImage(path, false);
  if (!image)
    return false;

  UpdateRunningGame(path, image.get());
  g_cdrom.InsertMedia(std::move(image));
  Log_InfoPrintf("Inserted media from %s (%s, %s)", m_running_game_path.c_str(), m_running_game_code.c_str(),
                 m_running_game_title.c_str());

  if (g_settings.HasAnyPerGameMemoryCards())
  {
    g_host_interface->AddOSDMessage("Game changed, reloading memory cards.", 2.0f);
    UpdateMemoryCards();
  }

  return true;
}

void System::RemoveMedia()
{
  g_cdrom.RemoveMedia();
}

std::unique_ptr<TimingEvent> System::CreateTimingEvent(std::string name, TickCount period, TickCount interval,
                                                       TimingEventCallback callback, bool activate)
{
  std::unique_ptr<TimingEvent> event =
    std::make_unique<TimingEvent>(std::move(name), period, interval, std::move(callback));
  if (activate)
    event->Activate();

  return event;
}

static bool CompareEvents(const TimingEvent* lhs, const TimingEvent* rhs)
{
  return lhs->GetDowncount() > rhs->GetDowncount();
}

void System::AddActiveEvent(TimingEvent* event)
{
  m_events.push_back(event);
  if (!m_running_events)
  {
    std::push_heap(m_events.begin(), m_events.end(), CompareEvents);
    if (!m_frame_done)
      UpdateCPUDowncount();
  }
  else
  {
    m_events_need_sorting = true;
  }
}

void System::RemoveActiveEvent(TimingEvent* event)
{
  auto iter = std::find_if(m_events.begin(), m_events.end(), [event](const auto& it) { return event == it; });
  if (iter == m_events.end())
  {
    Panic("Attempt to remove inactive event");
    return;
  }

  m_events.erase(iter);
  if (!m_running_events)
  {
    std::make_heap(m_events.begin(), m_events.end(), CompareEvents);
    if (!m_events.empty() && !m_frame_done)
      UpdateCPUDowncount();
  }
  else
  {
    m_events_need_sorting = true;
  }
}

void System::SortEvents()
{
  if (!m_running_events)
  {
    std::make_heap(m_events.begin(), m_events.end(), CompareEvents);
    if (!m_frame_done)
      UpdateCPUDowncount();
  }
  else
  {
    m_events_need_sorting = true;
  }
}

void System::RunEvents()
{
  DebugAssert(!m_running_events && !m_events.empty());

  m_running_events = true;

  TickCount pending_ticks = (m_global_tick_counter + CPU::GetPendingTicks()) - m_last_event_run_time;
  CPU::ResetPendingTicks();
  while (pending_ticks > 0)
  {
    const TickCount time = std::min(pending_ticks, m_events[0]->m_downcount);
    m_global_tick_counter += static_cast<u32>(time);
    pending_ticks -= time;

    // Apply downcount to all events.
    // This will result in a negative downcount for those events which are late.
    for (TimingEvent* evt : m_events)
    {
      evt->m_downcount -= time;
      evt->m_time_since_last_run += time;
    }

    // Now we can actually run the callbacks.
    while (m_events.front()->GetDowncount() <= 0)
    {
      TimingEvent* evt = m_events.front();
      const TickCount ticks_late = -evt->m_downcount;
      std::pop_heap(m_events.begin(), m_events.end(), CompareEvents);

      // Factor late time into the time for the next invocation.
      const TickCount ticks_to_execute = evt->m_time_since_last_run;
      evt->m_downcount += evt->m_interval;
      evt->m_time_since_last_run = 0;

      // The cycles_late is only an indicator, it doesn't modify the cycles to execute.
      evt->m_callback(ticks_to_execute, ticks_late);

      // Place it in the appropriate position in the queue.
      if (m_events_need_sorting)
      {
        // Another event may have been changed by this event, or the interval/downcount changed.
        std::make_heap(m_events.begin(), m_events.end(), CompareEvents);
        m_events_need_sorting = false;
      }
      else
      {
        // Keep the event list in a heap. The event we just serviced will be in the last place,
        // so we can use push_here instead of make_heap, which should be faster.
        std::push_heap(m_events.begin(), m_events.end(), CompareEvents);
      }
    }
  }

  m_last_event_run_time = m_global_tick_counter;
  m_running_events = false;
  CPU::SetDowncount(m_events.front()->GetDowncount());
}

void System::UpdateCPUDowncount()
{
  CPU::SetDowncount(m_events[0]->GetDowncount());
}

bool System::DoEventsState(StateWrapper& sw)
{
  if (sw.IsReading())
  {
    // Load timestamps for the clock events.
    // Any oneshot events should be recreated by the load state method, so we can fix up their times here.
    u32 event_count = 0;
    sw.Do(&event_count);

    for (u32 i = 0; i < event_count; i++)
    {
      std::string event_name;
      TickCount downcount, time_since_last_run, period, interval;
      sw.Do(&event_name);
      sw.Do(&downcount);
      sw.Do(&time_since_last_run);
      sw.Do(&period);
      sw.Do(&interval);
      if (sw.HasError())
        return false;

      TimingEvent* event = FindActiveEvent(event_name.c_str());
      if (!event)
      {
        Log_WarningPrintf("Save state has event '%s', but couldn't find this event when loading.", event_name.c_str());
        continue;
      }

      // Using reschedule is safe here since we call sort afterwards.
      event->m_downcount = downcount;
      event->m_time_since_last_run = time_since_last_run;
      event->m_period = period;
      event->m_interval = interval;
    }

    sw.Do(&m_last_event_run_time);

    Log_DevPrintf("Loaded %u events from save state.", event_count);
    SortEvents();
  }
  else
  {
    u32 event_count = static_cast<u32>(m_events.size());
    sw.Do(&event_count);

    for (TimingEvent* evt : m_events)
    {
      sw.Do(&evt->m_name);
      sw.Do(&evt->m_downcount);
      sw.Do(&evt->m_time_since_last_run);
      sw.Do(&evt->m_period);
      sw.Do(&evt->m_interval);
    }

    sw.Do(&m_last_event_run_time);

    Log_DevPrintf("Wrote %u events to save state.", event_count);
  }

  return !sw.HasError();
}

TimingEvent* System::FindActiveEvent(const char* name)
{
  auto iter =
    std::find_if(m_events.begin(), m_events.end(), [&name](auto& ev) { return ev->GetName().compare(name) == 0; });

  return (iter != m_events.end()) ? *iter : nullptr;
}

void System::UpdateRunningGame(const char* path, CDImage* image)
{
  m_running_game_path.clear();
  m_running_game_code.clear();
  m_running_game_title.clear();

  if (path && std::strlen(path) > 0)
  {
    m_running_game_path = path;
    g_host_interface->GetGameInfo(path, image, &m_running_game_code, &m_running_game_title);
  }

  g_host_interface->OnRunningGameChanged();
}

u32 System::GetMediaPlaylistIndex() const
{
  if (!g_cdrom.HasMedia())
    return std::numeric_limits<u32>::max();

  const std::string& media_path = g_cdrom.GetMediaFileName();
  for (u32 i = 0; i < static_cast<u32>(m_media_playlist.size()); i++)
  {
    if (m_media_playlist[i] == media_path)
      return i;
  }

  return std::numeric_limits<u32>::max();
}

bool System::AddMediaPathToPlaylist(const std::string_view& path)
{
  if (std::any_of(m_media_playlist.begin(), m_media_playlist.end(),
                  [&path](const std::string& p) { return (path == p); }))
  {
    return false;
  }

  m_media_playlist.emplace_back(path);
  return true;
}

bool System::RemoveMediaPathFromPlaylist(const std::string_view& path)
{
  for (u32 i = 0; i < static_cast<u32>(m_media_playlist.size()); i++)
  {
    if (path == m_media_playlist[i])
      return RemoveMediaPathFromPlaylist(i);
  }

  return false;
}

bool System::RemoveMediaPathFromPlaylist(u32 index)
{
  if (index >= static_cast<u32>(m_media_playlist.size()))
    return false;

  if (GetMediaPlaylistIndex() == index)
  {
    g_host_interface->ReportMessage("Removing current media from playlist, removing media from CD-ROM.");
    g_cdrom.RemoveMedia();
  }

  m_media_playlist.erase(m_media_playlist.begin() + index);
  return true;
}

bool System::ReplaceMediaPathFromPlaylist(u32 index, const std::string_view& path)
{
  if (index >= static_cast<u32>(m_media_playlist.size()))
    return false;

  if (GetMediaPlaylistIndex() == index)
  {
    g_host_interface->ReportMessage("Changing current media from playlist, replacing current media.");
    g_cdrom.RemoveMedia();

    m_media_playlist[index] = path;
    InsertMedia(m_media_playlist[index].c_str());
  }
  else
  {
    m_media_playlist[index] = path;
  }

  return true;
}

bool System::SwitchMediaFromPlaylist(u32 index)
{
  if (index >= m_media_playlist.size())
    return false;

  const std::string& path = m_media_playlist[index];
  if (g_cdrom.HasMedia() && g_cdrom.GetMediaFileName() == path)
    return true;

  return InsertMedia(path.c_str());
}

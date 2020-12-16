#include "system.h"
#include "bios.h"
#include "bus.h"
#include "cdrom.h"
#include "cheats.h"
#include "common/audio_stream.h"
#include "common/file_system.h"
#include "common/iso_reader.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "controller.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "dma.h"
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
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
Log_SetChannel(System);

#ifdef WIN32
#include "common/windows_headers.h"
#else
#include <time.h>
#endif

SystemBootParameters::SystemBootParameters() = default;

SystemBootParameters::SystemBootParameters(SystemBootParameters&& other) = default;

SystemBootParameters::SystemBootParameters(std::string filename_) : filename(std::move(filename_)) {}

SystemBootParameters::~SystemBootParameters() = default;

namespace System {

static bool LoadEXE(const char* filename);
static bool LoadEXEFromBuffer(const void* buffer, u32 buffer_size);
static bool LoadPSF(const char* filename);
static bool SetExpansionROM(const char* filename);

/// Opens CD image, preloading if needed.
static std::unique_ptr<CDImage> OpenCDImage(const char* path, bool force_preload);

static bool DoLoadState(ByteStream* stream, bool force_software_renderer, bool update_display);
static bool DoState(StateWrapper& sw, bool update_display);
static bool CreateGPU(GPURenderer renderer);

static bool Initialize(bool force_software_renderer);

static void UpdateRunningGame(const char* path, CDImage* image);

static State s_state = State::Shutdown;

static ConsoleRegion s_region = ConsoleRegion::NTSC_U;
TickCount g_ticks_per_second = MASTER_CLOCK;
static TickCount s_max_slice_ticks = MASTER_CLOCK / 10;
static u32 s_frame_number = 1;
static u32 s_internal_frame_number = 1;

static std::string s_running_game_path;
static std::string s_running_game_code;
static std::string s_running_game_title;

static float s_throttle_frequency = 60.0f;
static float s_target_speed = 1.0f;
static s32 s_throttle_period = 0;
static u64 s_last_throttle_time = 0;
static Common::Timer s_throttle_timer;
static Common::Timer s_speed_lost_time_timestamp;

static float s_average_frame_time_accumulator = 0.0f;
static float s_worst_frame_time_accumulator = 0.0f;

static float s_vps = 0.0f;
static float s_fps = 0.0f;
static float s_speed = 0.0f;
static float s_worst_frame_time = 0.0f;
static float s_average_frame_time = 0.0f;
static u32 s_last_frame_number = 0;
static u32 s_last_internal_frame_number = 0;
static u32 s_last_global_tick_counter = 0;
static Common::Timer s_fps_timer;
static Common::Timer s_frame_timer;

// Playlist of disc images.
static std::vector<std::string> s_media_playlist;
static std::string s_media_playlist_filename;

static std::unique_ptr<CheatList> s_cheat_list;

State GetState()
{
  return s_state;
}

void SetState(State new_state)
{
  if (s_state == new_state)
    return;

  Assert(s_state == State::Paused || s_state == State::Running);
  Assert(new_state == State::Paused || new_state == State::Running);
  s_state = new_state;

  if (new_state == State::Paused)
    CPU::ForceDispatcherExit();
}

bool IsRunning()
{
  return s_state == State::Running;
}

bool IsPaused()
{
  return s_state == State::Paused;
}

bool IsShutdown()
{
  return s_state == State::Shutdown;
}

bool IsValid()
{
  return s_state != State::Shutdown && s_state != State::Starting;
}

ConsoleRegion GetRegion()
{
  return s_region;
}

bool IsPALRegion()
{
  return s_region == ConsoleRegion::PAL;
}

TickCount GetMaxSliceTicks()
{
  return s_max_slice_ticks;
}

void UpdateOverclock()
{
  g_ticks_per_second = ScaleTicksToOverclock(MASTER_CLOCK);
  s_max_slice_ticks = ScaleTicksToOverclock(MASTER_CLOCK / 10);
  g_spu.CPUClockChanged();
  g_cdrom.CPUClockChanged();
  g_gpu->CPUClockChanged();
  g_timers.CPUClocksChanged();
  UpdateThrottlePeriod();
}

u32 GetFrameNumber()
{
  return s_frame_number;
}

u32 GetInternalFrameNumber()
{
  return s_internal_frame_number;
}

void FrameDone()
{
  s_frame_number++;
  CPU::g_state.frame_done = true;
  CPU::g_state.downcount = 0;
}

void IncrementInternalFrameNumber()
{
  s_internal_frame_number++;
}

const std::string& GetRunningPath()
{
  return s_running_game_path;
}
const std::string& GetRunningCode()
{
  return s_running_game_code;
}

const std::string& GetRunningTitle()
{
  return s_running_game_title;
}

float GetFPS()
{
  return s_fps;
}
float GetVPS()
{
  return s_vps;
}
float GetEmulationSpeed()
{
  return s_speed;
}
float GetAverageFrameTime()
{
  return s_average_frame_time;
}
float GetWorstFrameTime()
{
  return s_worst_frame_time;
}
float GetThrottleFrequency()
{
  return s_throttle_frequency;
}

bool IsExeFileName(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  return (extension &&
          (StringUtil::Strcasecmp(extension, ".exe") == 0 || StringUtil::Strcasecmp(extension, ".psexe") == 0));
}

bool IsPsfFileName(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  return (extension && StringUtil::Strcasecmp(extension, ".psf") == 0);
}

bool IsM3UFileName(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  return (extension && StringUtil::Strcasecmp(extension, ".m3u") == 0);
}

std::vector<std::string> ParseM3UFile(const char* path)
{
  std::ifstream ifs(path);
  if (!ifs.is_open())
  {
    Log_ErrorPrintf("Failed to open %s", path);
    return {};
  }

  std::vector<std::string> entries;
  std::string line;
  while (std::getline(ifs, line))
  {
    u32 start_offset = 0;
    while (start_offset < line.size() && std::isspace(line[start_offset]))
      start_offset++;

    // skip comments
    if (start_offset == line.size() || line[start_offset] == '#')
      continue;

    // strip ending whitespace
    u32 end_offset = static_cast<u32>(line.size()) - 1;
    while (std::isspace(line[end_offset]) && end_offset > start_offset)
      end_offset--;

    // anything?
    if (start_offset == end_offset)
      continue;

    std::string entry_path(line.begin() + start_offset, line.begin() + end_offset + 1);
    if (!FileSystem::IsAbsolutePath(entry_path))
    {
      SmallString absolute_path;
      FileSystem::BuildPathRelativeToFile(absolute_path, path, entry_path.c_str());
      entry_path = absolute_path;
    }

    Log_DevPrintf("Read path from m3u: '%s'", entry_path.c_str());
    entries.push_back(std::move(entry_path));
  }

  Log_InfoPrintf("Loaded %zu paths from m3u '%s'", entries.size(), path);
  return entries;
}

ConsoleRegion GetConsoleRegionForDiscRegion(DiscRegion region)
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

std::string_view GetTitleForPath(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  if (path == extension)
    return path;

  const char* path_end = path + std::strlen(path);
  const char* title_end = extension ? (extension - 1) : (path_end);
  const char* title_start = std::max(std::strrchr(path, '/'), std::strrchr(path, '\\'));
  if (!title_start || title_start == path)
    return std::string_view(path, title_end - title_start);
  else
    return std::string_view(title_start + 1, title_end - title_start);
}

std::string GetGameCodeForPath(const char* image_path)
{
  std::unique_ptr<CDImage> cdi = CDImage::Open(image_path);
  if (!cdi)
    return {};

  return GetGameCodeForImage(cdi.get());
}

std::string GetGameCodeForImage(CDImage* cdi)
{
  ISOReader iso;
  if (!iso.Open(cdi, 1))
    return {};

  // Read SYSTEM.CNF
  std::vector<u8> system_cnf_data;
  if (!iso.ReadFile("SYSTEM.CNF", &system_cnf_data))
    return {};

  // Parse lines
  std::vector<std::pair<std::string, std::string>> lines;
  std::pair<std::string, std::string> current_line;
  bool reading_value = false;
  for (size_t pos = 0; pos < system_cnf_data.size(); pos++)
  {
    const char ch = static_cast<char>(system_cnf_data[pos]);
    if (ch == '\r' || ch == '\n')
    {
      if (!current_line.first.empty())
      {
        lines.push_back(std::move(current_line));
        current_line = {};
        reading_value = false;
      }
    }
    else if (ch == ' ' || (ch >= 0x09 && ch <= 0x0D))
    {
      continue;
    }
    else if (ch == '=' && !reading_value)
    {
      reading_value = true;
    }
    else
    {
      if (reading_value)
        current_line.second.push_back(ch);
      else
        current_line.first.push_back(ch);
    }
  }

  if (!current_line.first.empty())
    lines.push_back(std::move(current_line));

  // Find the BOOT line
  auto iter = std::find_if(lines.begin(), lines.end(),
                           [](const auto& it) { return StringUtil::Strcasecmp(it.first.c_str(), "boot") == 0; });
  if (iter == lines.end())
    return {};

  // cdrom:\SCES_123.45;1
  std::string code = iter->second;
  std::string::size_type pos = code.rfind('\\');
  if (pos != std::string::npos)
  {
    code.erase(0, pos + 1);
  }
  else
  {
    // cdrom:SCES_123.45;1
    pos = code.rfind(':');
    if (pos != std::string::npos)
      code.erase(0, pos + 1);
  }

  pos = code.find(';');
  if (pos != std::string::npos)
    code.erase(pos);

  // SCES_123.45 -> SCES-12345
  for (pos = 0; pos < code.size();)
  {
    if (code[pos] == '.')
    {
      code.erase(pos, 1);
      continue;
    }

    if (code[pos] == '_')
      code[pos] = '-';
    else
      code[pos] = static_cast<char>(std::toupper(code[pos]));

    pos++;
  }

  return code;
}

DiscRegion GetRegionForCode(std::string_view code)
{
  std::string prefix;
  for (size_t pos = 0; pos < code.length(); pos++)
  {
    const int ch = std::tolower(code[pos]);
    if (ch < 'a' || ch > 'z')
      break;

    prefix.push_back(static_cast<char>(ch));
  }

  if (prefix == "sces" || prefix == "sced" || prefix == "sles" || prefix == "sled")
    return DiscRegion::PAL;
  else if (prefix == "scps" || prefix == "slps" || prefix == "slpm" || prefix == "sczs" || prefix == "papx")
    return DiscRegion::NTSC_J;
  else if (prefix == "scus" || prefix == "slus")
    return DiscRegion::NTSC_U;
  else
    return DiscRegion::Other;
}

DiscRegion GetRegionFromSystemArea(CDImage* cdi)
{
  // The license code is on sector 4 of the disc.
  u8 sector[CDImage::DATA_SECTOR_SIZE];
  if (!cdi->Seek(1, 4) || cdi->Read(CDImage::ReadMode::DataOnly, 1, sector) != 1)
    return DiscRegion::Other;

  static constexpr char ntsc_u_string[] = "          Licensed  by          Sony Computer Entertainment Amer  ica ";
  static constexpr char ntsc_j_string[] = "          Licensed  by          Sony Computer Entertainment Inc.";
  static constexpr char pal_string[] = "          Licensed  by          Sony Computer Entertainment Euro pe";

  // subtract one for the terminating null
  if (std::equal(ntsc_u_string, ntsc_u_string + countof(ntsc_u_string) - 1, sector))
    return DiscRegion::NTSC_U;
  else if (std::equal(ntsc_j_string, ntsc_j_string + countof(ntsc_j_string) - 1, sector))
    return DiscRegion::NTSC_J;
  else if (std::equal(pal_string, pal_string + countof(pal_string) - 1, sector))
    return DiscRegion::PAL;
  else
    return DiscRegion::Other;
}

DiscRegion GetRegionForImage(CDImage* cdi)
{
  DiscRegion system_area_region = GetRegionFromSystemArea(cdi);
  if (system_area_region != DiscRegion::Other)
    return system_area_region;

  std::string code = GetGameCodeForImage(cdi);
  if (code.empty())
    return DiscRegion::Other;

  return GetRegionForCode(code);
}

std::optional<DiscRegion> GetRegionForPath(const char* image_path)
{
  std::unique_ptr<CDImage> cdi = CDImage::Open(image_path);
  if (!cdi)
    return {};

  return GetRegionForImage(cdi.get());
}

bool RecreateGPU(GPURenderer renderer, bool update_display /* = true*/)
{
  g_gpu->RestoreGraphicsAPIState();

  // save current state
  std::unique_ptr<ByteStream> state_stream = ByteStream_CreateGrowableMemoryStream();
  StateWrapper sw(state_stream.get(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  const bool state_valid = g_gpu->DoState(sw, false) && TimingEvents::DoState(sw);
  if (!state_valid)
    Log_ErrorPrintf("Failed to save old GPU state when switching renderers");

  g_gpu->ResetGraphicsAPIState();

  // create new renderer
  g_gpu.reset();
  if (!CreateGPU(renderer))
  {
    Panic("Failed to recreate GPU");
    return false;
  }

  // reinitialize the code cache because the address space could change
  CPU::CodeCache::Reinitialize();

  if (state_valid)
  {
    state_stream->SeekAbsolute(0);
    sw.SetMode(StateWrapper::Mode::Read);
    g_gpu->RestoreGraphicsAPIState();
    g_gpu->DoState(sw, update_display);
    TimingEvents::DoState(sw);
    g_gpu->ResetGraphicsAPIState();
  }

  return true;
}

std::unique_ptr<CDImage> OpenCDImage(const char* path, bool force_preload)
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

bool Boot(const SystemBootParameters& params)
{
  Assert(s_state == State::Shutdown);
  Assert(s_media_playlist.empty());
  s_state = State::Starting;
  s_region = g_settings.region;

  if (params.state_stream)
  {
    if (!DoLoadState(params.state_stream.get(), params.force_software_renderer, true))
    {
      Shutdown();
      return false;
    }

    return true;
  }

  // Load CD image up and detect region.
  std::unique_ptr<CDImage> media;
  bool exe_boot = false;
  bool psf_boot = false;
  if (!params.filename.empty())
  {
    exe_boot = IsExeFileName(params.filename.c_str());
    psf_boot = (!exe_boot && IsPsfFileName(params.filename.c_str()));
    if (exe_boot || psf_boot)
    {
      // TODO: Pull region from PSF
      if (s_region == ConsoleRegion::Auto)
      {
        Log_InfoPrintf("Defaulting to NTSC-U/C region for executable.");
        s_region = ConsoleRegion::NTSC_U;
      }
    }
    else
    {
      u32 playlist_index;
      if (IsM3UFileName(params.filename.c_str()))
      {
        s_media_playlist = ParseM3UFile(params.filename.c_str());
        s_media_playlist_filename = params.filename;
        if (s_media_playlist.empty())
        {
          g_host_interface->ReportFormattedError("Failed to parse playlist '%s'", params.filename.c_str());
          Shutdown();
          return false;
        }

        if (params.media_playlist_index >= s_media_playlist.size())
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

      const std::string& media_path = s_media_playlist[playlist_index];
      Log_InfoPrintf("Loading CD image '%s' from playlist index %u...", media_path.c_str(), playlist_index);
      media = OpenCDImage(media_path.c_str(), params.load_image_to_ram);
      if (!media)
      {
        g_host_interface->ReportFormattedError("Failed to load CD image '%s'", params.filename.c_str());
        Shutdown();
        return false;
      }

      if (s_region == ConsoleRegion::Auto)
      {
        const DiscRegion disc_region = GetRegionForImage(media.get());
        if (disc_region != DiscRegion::Other)
        {
          s_region = GetConsoleRegionForDiscRegion(disc_region);
          Log_InfoPrintf("Auto-detected console %s region for '%s' (region %s)",
                         Settings::GetConsoleRegionName(s_region), params.filename.c_str(),
                         Settings::GetDiscRegionName(disc_region));
        }
        else
        {
          s_region = ConsoleRegion::NTSC_U;
          Log_WarningPrintf("Could not determine console region for disc region %s. Defaulting to %s.",
                            Settings::GetDiscRegionName(disc_region), Settings::GetConsoleRegionName(s_region));
        }
      }
    }
  }
  else
  {
    // Default to NTSC for BIOS boot.
    if (s_region == ConsoleRegion::Auto)
      s_region = ConsoleRegion::NTSC_U;
  }

  // Load BIOS image.
  std::optional<BIOS::Image> bios_image = g_host_interface->GetBIOSImage(s_region);
  if (!bios_image)
  {
    g_host_interface->ReportFormattedError("Failed to load %s BIOS", Settings::GetConsoleRegionName(s_region));
    Shutdown();
    return false;
  }

  // Notify change of disc.
  UpdateRunningGame(media ? media->GetFileName().c_str() : params.filename.c_str(), media.get());

  // Component setup.
  if (!Initialize(params.force_software_renderer))
  {
    Shutdown();
    return false;
  }

  Bus::SetBIOS(*bios_image);
  UpdateControllers();
  UpdateMemoryCards();
  Reset();

  // Enable tty by patching bios.
  const BIOS::Hash bios_hash = BIOS::GetHash(*bios_image);
  if (g_settings.bios_patch_tty_enable)
    BIOS::PatchBIOSEnableTTY(Bus::g_bios, Bus::BIOS_SIZE, bios_hash);

  // Load EXE late after BIOS.
  if (exe_boot && !LoadEXE(params.filename.c_str()))
  {
    g_host_interface->ReportFormattedError("Failed to load EXE file '%s'", params.filename.c_str());
    Shutdown();
    return false;
  }
  else if (psf_boot && !LoadPSF(params.filename.c_str()))
  {
    g_host_interface->ReportFormattedError("Failed to load PSF file '%s'", params.filename.c_str());
    Shutdown();
    return false;
  }

  // Insert CD, and apply fastboot patch if enabled.
  if (media)
    g_cdrom.InsertMedia(std::move(media));
  if (g_cdrom.HasMedia() &&
      (params.override_fast_boot.has_value() ? params.override_fast_boot.value() : g_settings.bios_patch_fast_boot))
  {
    BIOS::PatchBIOSFastBoot(Bus::g_bios, Bus::BIOS_SIZE, bios_hash);
  }

  // Good to go.
  s_state = State::Running;
  return true;
}

bool Initialize(bool force_software_renderer)
{
  g_ticks_per_second = ScaleTicksToOverclock(MASTER_CLOCK);
  s_max_slice_ticks = ScaleTicksToOverclock(MASTER_CLOCK / 10);
  s_frame_number = 1;
  s_internal_frame_number = 1;

  s_throttle_frequency = 60.0f;
  s_throttle_period = 0;
  s_last_throttle_time = 0;
  s_throttle_timer.Reset();
  s_speed_lost_time_timestamp.Reset();

  s_average_frame_time_accumulator = 0.0f;
  s_worst_frame_time_accumulator = 0.0f;

  s_vps = 0.0f;
  s_fps = 0.0f;
  s_speed = 0.0f;
  s_worst_frame_time = 0.0f;
  s_average_frame_time = 0.0f;
  s_last_frame_number = 0;
  s_last_internal_frame_number = 0;
  s_last_global_tick_counter = 0;
  s_fps_timer.Reset();
  s_frame_timer.Reset();

  TimingEvents::Initialize();

  CPU::Initialize();

  if (!Bus::Initialize())
    return false;

  if (!CreateGPU(force_software_renderer ? GPURenderer::Software : g_settings.gpu_renderer))
    return false;

  // CPU code cache must happen after GPU, because it might steal our address space.
  CPU::CodeCache::Initialize();

  g_dma.Initialize();
  g_interrupt_controller.Initialize();

  g_cdrom.Initialize();
  g_pad.Initialize();
  g_timers.Initialize();
  g_spu.Initialize();
  g_mdec.Initialize();
  g_sio.Initialize();

  if (g_settings.cpu_overclock_active)
  {
    g_host_interface->AddFormattedOSDMessage(
      10.0f,
      g_host_interface->TranslateString("OSDMessage",
                                        "CPU clock speed is set to %u%% (%u / %u). This may result in instability."),
      g_settings.GetCPUOverclockPercent(), g_settings.cpu_overclock_numerator, g_settings.cpu_overclock_denominator);
  }

  UpdateThrottlePeriod();
  return true;
}

void Shutdown()
{
  if (s_state == State::Shutdown)
    return;

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
  TimingEvents::Shutdown();
  s_running_game_code.clear();
  s_running_game_path.clear();
  s_running_game_title.clear();
  s_media_playlist.clear();
  s_media_playlist_filename.clear();
  s_cheat_list.reset();
  s_state = State::Shutdown;
}

bool CreateGPU(GPURenderer renderer)
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

  // we put this here rather than in Initialize() because of the virtual calls
  g_gpu->Reset();
  return true;
}

bool DoState(StateWrapper& sw, bool update_display)
{
  if (!sw.DoMarker("System"))
    return false;

  sw.Do(&s_region);
  sw.Do(&s_frame_number);
  sw.Do(&s_internal_frame_number);

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

  g_gpu->RestoreGraphicsAPIState();
  const bool gpu_result = sw.DoMarker("GPU") && g_gpu->DoState(sw, update_display);
  g_gpu->ResetGraphicsAPIState();
  if (!gpu_result)
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

  if (!sw.DoMarker("Events") || !TimingEvents::DoState(sw))
    return false;

  if (!sw.DoMarker("Overclock"))
    return false;

  bool cpu_overclock_active = g_settings.cpu_overclock_active;
  u32 cpu_overclock_numerator = g_settings.cpu_overclock_numerator;
  u32 cpu_overclock_denominator = g_settings.cpu_overclock_denominator;
  sw.Do(&cpu_overclock_active);
  sw.Do(&cpu_overclock_numerator);
  sw.Do(&cpu_overclock_denominator);

  if (sw.IsReading() && (cpu_overclock_active != g_settings.cpu_overclock_active ||
                         (cpu_overclock_active && (g_settings.cpu_overclock_numerator != cpu_overclock_numerator ||
                                                   g_settings.cpu_overclock_denominator != cpu_overclock_denominator))))
  {
    g_host_interface->AddFormattedOSDMessage(
      10.0f,
      g_host_interface->TranslateString("OSDMessage",
                                        "WARNING: CPU overclock (%u%%) was different in save state (%u%%)."),
      g_settings.cpu_overclock_enable ? g_settings.GetCPUOverclockPercent() : 100u,
      cpu_overclock_active ?
        Settings::CPUOverclockFractionToPercent(cpu_overclock_numerator, cpu_overclock_denominator) :
        100u);
    UpdateOverclock();
  }

  return !sw.HasError();
}

void Reset()
{
  if (IsShutdown())
    return;

  g_gpu->RestoreGraphicsAPIState();

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
  s_frame_number = 1;
  s_internal_frame_number = 0;
  TimingEvents::Reset();
  ResetPerformanceCounters();

  g_gpu->ResetGraphicsAPIState();
}

bool LoadState(ByteStream* state, bool update_display)
{
  if (IsShutdown())
    return false;

  return DoLoadState(state, false, update_display);
}

bool DoLoadState(ByteStream* state, bool force_software_renderer, bool update_display)
{
  SAVE_STATE_HEADER header;
  if (!state->Read2(&header, sizeof(header)))
    return false;

  if (header.magic != SAVE_STATE_MAGIC)
    return false;

  if (header.version < SAVE_STATE_MINIMUM_VERSION)
  {
    g_host_interface->ReportFormattedError(
      g_host_interface->TranslateString("System",
                                        "Save state is incompatible: minimum version is %u but state is version %u."),
      SAVE_STATE_MINIMUM_VERSION, header.version);
    return false;
  }

  if (header.version > SAVE_STATE_VERSION)
  {
    g_host_interface->ReportFormattedError(
      g_host_interface->TranslateString("System",
                                        "Save state is incompatible: maximum version is %u but state is version %u."),
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

    std::unique_ptr<CDImage> old_media = g_cdrom.RemoveMedia();
    if (old_media && old_media->GetFileName() == media_filename)
    {
      Log_InfoPrintf("Re-using same media '%s'", media_filename.c_str());
      media = std::move(old_media);
    }
    else
    {
      media = OpenCDImage(media_filename.c_str(), false);
      if (!media)
      {
        if (old_media)
        {
          g_host_interface->AddFormattedOSDMessage(
            30.0f,
            g_host_interface->TranslateString("OSDMessage", "Failed to open CD image from save state: '%s'. Using "
                                                            "existing image '%s', this may result in instability."),
            media_filename.c_str(), old_media->GetFileName().c_str());
          media = std::move(old_media);
        }
        else
        {
          g_host_interface->ReportFormattedError(
            g_host_interface->TranslateString("System", "Failed to open CD image from save state: '%s'."),
            media_filename.c_str());
          return false;
        }
      }
    }
  }

  std::string playlist_filename;
  std::vector<std::string> playlist_entries;
  if (header.playlist_filename_length > 0)
  {
    playlist_filename.resize(header.offset_to_playlist_filename);
    if (!state->SeekAbsolute(header.offset_to_playlist_filename) ||
        !state->Read2(playlist_filename.data(), header.playlist_filename_length))
    {
      return false;
    }

    playlist_entries = ParseM3UFile(playlist_filename.c_str());
    if (playlist_entries.empty())
    {
      g_host_interface->ReportFormattedError("Failed to load save state playlist entries from '%s'",
                                             playlist_filename.c_str());
      return false;
    }
  }

  UpdateRunningGame(media_filename.c_str(), media.get());

  if (s_state == State::Starting)
  {
    if (!Initialize(force_software_renderer))
      return false;

    if (media)
      g_cdrom.InsertMedia(std::move(media));

    s_media_playlist_filename = std::move(playlist_filename);
    s_media_playlist = std::move(playlist_entries);

    UpdateControllers();
    UpdateMemoryCards();
  }
  else
  {
    g_cdrom.Reset();
    if (media)
      g_cdrom.InsertMedia(std::move(media));
    else
      g_cdrom.RemoveMedia();

    s_media_playlist_filename = std::move(playlist_filename);
    s_media_playlist = std::move(playlist_entries);

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

  StateWrapper sw(state, StateWrapper::Mode::Read, header.version);
  if (!DoState(sw, update_display))
    return false;

  if (s_state == State::Starting)
    s_state = State::Running;

  return true;
}

bool SaveState(ByteStream* state, u32 screenshot_size /* = 128 */)
{
  if (IsShutdown())
    return false;

  SAVE_STATE_HEADER header = {};

  const u64 header_position = state->GetPosition();
  if (!state->Write2(&header, sizeof(header)))
    return false;

  // fill in header
  header.magic = SAVE_STATE_MAGIC;
  header.version = SAVE_STATE_VERSION;
  StringUtil::Strlcpy(header.title, s_running_game_title.c_str(), sizeof(header.title));
  StringUtil::Strlcpy(header.game_code, s_running_game_code.c_str(), sizeof(header.game_code));

  if (g_cdrom.HasMedia())
  {
    const std::string& media_filename = g_cdrom.GetMediaFileName();
    header.offset_to_media_filename = static_cast<u32>(state->GetPosition());
    header.media_filename_length = static_cast<u32>(media_filename.length());
    if (!media_filename.empty() && !state->Write2(media_filename.data(), header.media_filename_length))
      return false;
  }

  if (!s_media_playlist_filename.empty())
  {
    header.offset_to_playlist_filename = static_cast<u32>(state->GetPosition());
    header.playlist_filename_length = static_cast<u32>(s_media_playlist_filename.length());
    if (!state->Write2(s_media_playlist_filename.data(), header.playlist_filename_length))
      return false;
  }

  // save screenshot
  if (screenshot_size > 0)
  {
    std::vector<u32> screenshot_buffer;
    if (g_host_interface->GetDisplay()->WriteDisplayTextureToBuffer(&screenshot_buffer, screenshot_size,
                                                                    screenshot_size) &&
        !screenshot_buffer.empty())
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

    g_gpu->RestoreGraphicsAPIState();

    StateWrapper sw(state, StateWrapper::Mode::Write, SAVE_STATE_VERSION);
    const bool result = DoState(sw, false);

    g_gpu->ResetGraphicsAPIState();

    if (!result)
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

void SingleStepCPU()
{
  const u32 old_frame_number = s_frame_number;

  s_frame_timer.Reset();

  g_gpu->RestoreGraphicsAPIState();

  CPU::SingleStep();

  g_spu.GeneratePendingSamples();

  if (s_frame_number != old_frame_number && s_cheat_list)
    s_cheat_list->Apply();

  g_gpu->ResetGraphicsAPIState();
}

void RunFrame()
{
  s_frame_timer.Reset();

  g_gpu->RestoreGraphicsAPIState();

  if (CPU::g_state.use_debug_dispatcher)
  {
    CPU::ExecuteDebug();
  }
  else
  {
    switch (g_settings.cpu_execution_mode)
    {
      case CPUExecutionMode::Recompiler:
#ifdef WITH_RECOMPILER
        CPU::CodeCache::ExecuteRecompiler();
#else
        CPU::CodeCache::Execute();
#endif
        break;

      case CPUExecutionMode::CachedInterpreter:
        CPU::CodeCache::Execute();
        break;

      case CPUExecutionMode::Interpreter:
      default:
        CPU::Execute();
        break;
    }
  }

  // Generate any pending samples from the SPU before sleeping, this way we reduce the chances of underruns.
  g_spu.GeneratePendingSamples();

  if (s_cheat_list)
    s_cheat_list->Apply();

  g_gpu->ResetGraphicsAPIState();
}

void SetTargetSpeed(float speed)
{
  s_target_speed = speed;
  UpdateThrottlePeriod();
}

void SetThrottleFrequency(float frequency)
{
  s_throttle_frequency = frequency;
  UpdateThrottlePeriod();
}

void UpdateThrottlePeriod()
{
  s_throttle_period =
    static_cast<s32>(1000000000.0 / static_cast<double>(s_throttle_frequency) / static_cast<double>(s_target_speed));
  ResetThrottler();
}

void ResetThrottler()
{
  s_last_throttle_time = 0;
  s_throttle_timer.Reset();
}

void Throttle()
{
  // Reset the throttler on audio buffer overflow, so we don't end up out of phase.
  if (g_host_interface->GetAudioStream()->DidUnderflow())
  {
    Log_DevPrintf("Audio buffer underflowed, resetting throttler");
    ResetThrottler();
    return;
  }

  // Allow variance of up to 40ms either way.
  constexpr s64 MAX_VARIANCE_TIME = INT64_C(40000000);

  // Don't sleep for <1ms or >=period.
  constexpr s64 MINIMUM_SLEEP_TIME = INT64_C(1000000);

  // Use unsigned for defined overflow/wrap-around.
  const u64 time = static_cast<u64>(s_throttle_timer.GetTimeNanoseconds());
  const s64 sleep_time = static_cast<s64>(s_last_throttle_time - time);
  if (sleep_time < -MAX_VARIANCE_TIME)
  {
#ifndef _DEBUG
    // Don't display the slow messages in debug, it'll always be slow...
    // Limit how often the messages are displayed.
    if (s_speed_lost_time_timestamp.GetTimeSeconds() >= 1.0f)
    {
      Log_WarningPrintf("System too slow, lost %.2f ms",
                        static_cast<double>(-sleep_time - MAX_VARIANCE_TIME) / 1000000.0);
      s_speed_lost_time_timestamp.Reset();
    }
#endif
    ResetThrottler();
  }
  else if (sleep_time >= MINIMUM_SLEEP_TIME)
  {
#ifdef WIN32
    Sleep(static_cast<u32>(sleep_time / 1000000));
#else
    const struct timespec ts = {0, static_cast<long>(sleep_time)};
    nanosleep(&ts, nullptr);
#endif
  }

  s_last_throttle_time += s_throttle_period;
}

void UpdatePerformanceCounters()
{
  const float frame_time = static_cast<float>(s_frame_timer.GetTimeMilliseconds());
  s_average_frame_time_accumulator += frame_time;
  s_worst_frame_time_accumulator = std::max(s_worst_frame_time_accumulator, frame_time);

  // update fps counter
  const float time = static_cast<float>(s_fps_timer.GetTimeSeconds());
  if (time < 1.0f)
    return;

  const float frames_presented = static_cast<float>(s_frame_number - s_last_frame_number);
  const u32 global_tick_counter = TimingEvents::GetGlobalTickCounter();

  s_worst_frame_time = s_worst_frame_time_accumulator;
  s_worst_frame_time_accumulator = 0.0f;
  s_average_frame_time = s_average_frame_time_accumulator / frames_presented;
  s_average_frame_time_accumulator = 0.0f;
  s_vps = static_cast<float>(frames_presented / time);
  s_last_frame_number = s_frame_number;
  s_fps = static_cast<float>(s_internal_frame_number - s_last_internal_frame_number) / time;
  s_last_internal_frame_number = s_internal_frame_number;
  s_speed = static_cast<float>(static_cast<double>(global_tick_counter - s_last_global_tick_counter) /
                               (static_cast<double>(g_ticks_per_second) * time)) *
            100.0f;
  s_last_global_tick_counter = global_tick_counter;
  s_fps_timer.Reset();

  Log_VerbosePrintf("FPS: %.2f VPS: %.2f Average: %.2fms Worst: %.2fms", s_fps, s_vps, s_average_frame_time,
                    s_worst_frame_time);

  g_host_interface->OnSystemPerformanceCountersUpdated();
}

void ResetPerformanceCounters()
{
  s_last_frame_number = s_frame_number;
  s_last_internal_frame_number = s_internal_frame_number;
  s_last_global_tick_counter = TimingEvents::GetGlobalTickCounter();
  s_average_frame_time_accumulator = 0.0f;
  s_worst_frame_time_accumulator = 0.0f;
  s_fps_timer.Reset();
  ResetThrottler();
}

bool LoadEXE(const char* filename)
{
  std::FILE* fp = FileSystem::OpenCFile(filename, "rb");
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
  return BIOS::PatchBIOSForEXE(Bus::g_bios, Bus::BIOS_SIZE, r_pc, r_gp, r_sp, r_fp);
}

bool LoadEXEFromBuffer(const void* buffer, u32 buffer_size)
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
  return BIOS::PatchBIOSForEXE(Bus::g_bios, Bus::BIOS_SIZE, r_pc, r_gp, r_sp, r_fp);
}

bool LoadPSF(const char* filename)
{
  Log_InfoPrintf("Loading PSF file from '%s'", filename);

  PSFLoader::File psf;
  if (!psf.Load(filename))
    return false;

  const std::vector<u8>& exe_data = psf.GetProgramData();
  return LoadEXEFromBuffer(exe_data.data(), static_cast<u32>(exe_data.size()));
}

bool SetExpansionROM(const char* filename)
{
  std::FILE* fp = FileSystem::OpenCFile(filename, "rb");
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

void StallCPU(TickCount ticks)
{
  CPU::AddPendingTicks(ticks);
#if 0
  if (CPU::GetPendingTicks() >= CPU::GetDowncount() && !m_running_events)
    RunEvents();
#endif
}

Controller* GetController(u32 slot)
{
  return g_pad.GetController(slot);
}

void UpdateControllers()
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

void UpdateControllerSettings()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = g_pad.GetController(i);
    if (controller)
      controller->LoadSettings(TinyString::FromFormat("Controller%u", i + 1u));
  }
}

void ResetControllers()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = g_pad.GetController(i);
    if (controller)
      controller->Reset();
  }
}

void UpdateMemoryCards()
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
        if (s_running_game_code.empty())
        {
          g_host_interface->AddFormattedOSDMessage(
            5.0f,
            g_host_interface->TranslateString("System",
                                              "Per-game memory card cannot be used for slot %u as the running "
                                              "game has no code. Using shared card instead."),
            i + 1u);
          card = MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(i));
        }
        else
        {
          card = MemoryCard::Open(g_host_interface->GetGameMemoryCardPath(s_running_game_code.c_str(), i));
        }
      }
      break;

      case MemoryCardType::PerGameTitle:
      {
        if (!s_media_playlist_filename.empty() && g_settings.memory_card_use_playlist_title)
        {
          const std::string playlist_title(GetTitleForPath(s_media_playlist_filename.c_str()));
          card = MemoryCard::Open(g_host_interface->GetGameMemoryCardPath(playlist_title.c_str(), i));
        }
        else if (s_running_game_title.empty())
        {
          g_host_interface->AddFormattedOSDMessage(
            5.0f,
            g_host_interface->TranslateString("System",
                                              "Per-game memory card cannot be used for slot %u as the running "
                                              "game has no title. Using shared card instead."),
            i + 1u);
          card = MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(i));
        }
        else
        {
          card = MemoryCard::Open(g_host_interface->GetGameMemoryCardPath(s_running_game_title.c_str(), i));
        }
      }
      break;

      case MemoryCardType::Shared:
      {
        if (g_settings.memory_card_paths[i].empty())
        {
          g_host_interface->AddFormattedOSDMessage(
            10.0f,
            g_host_interface->TranslateString("System", "Memory card path for slot %u is missing, using default."),
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

bool DumpRAM(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "wb");
  if (!fp)
    return false;

  return std::fwrite(Bus::g_ram, Bus::RAM_SIZE, 1, fp.get()) == 1;
}

bool HasMedia()
{
  return g_cdrom.HasMedia();
}

bool InsertMedia(const char* path)
{
  std::unique_ptr<CDImage> image = OpenCDImage(path, false);
  if (!image)
    return false;

  UpdateRunningGame(path, image.get());
  g_cdrom.InsertMedia(std::move(image));
  Log_InfoPrintf("Inserted media from %s (%s, %s)", s_running_game_path.c_str(), s_running_game_code.c_str(),
                 s_running_game_title.c_str());

  if (g_settings.HasAnyPerGameMemoryCards())
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("System", "Game changed, reloading memory cards."), 10.0f);
    UpdateMemoryCards();
  }

  // reinitialize recompiler, because especially with preloading this might overlap the fastmem area
  if (g_settings.IsUsingCodeCache())
    CPU::CodeCache::Reinitialize();

  return true;
}

void RemoveMedia()
{
  g_cdrom.RemoveMedia();
}

void UpdateRunningGame(const char* path, CDImage* image)
{
  if (s_running_game_path == path)
    return;

  s_running_game_path.clear();
  s_running_game_code.clear();
  s_running_game_title.clear();

  if (path && std::strlen(path) > 0)
  {
    s_running_game_path = path;
    g_host_interface->GetGameInfo(path, image, &s_running_game_code, &s_running_game_title);
  }

  g_host_interface->OnRunningGameChanged();
}

bool HasMediaPlaylist()
{
  return !s_media_playlist_filename.empty();
}

u32 GetMediaPlaylistCount()
{
  return static_cast<u32>(s_media_playlist.size());
}

const std::string& GetMediaPlaylistPath(u32 index)
{
  return s_media_playlist[index];
}

u32 GetMediaPlaylistIndex()
{
  if (!g_cdrom.HasMedia())
    return std::numeric_limits<u32>::max();

  const std::string& media_path = g_cdrom.GetMediaFileName();
  for (u32 i = 0; i < static_cast<u32>(s_media_playlist.size()); i++)
  {
    if (s_media_playlist[i] == media_path)
      return i;
  }

  return std::numeric_limits<u32>::max();
}

bool AddMediaPathToPlaylist(const std::string_view& path)
{
  if (std::any_of(s_media_playlist.begin(), s_media_playlist.end(),
                  [&path](const std::string& p) { return (path == p); }))
  {
    return false;
  }

  s_media_playlist.emplace_back(path);
  return true;
}

bool RemoveMediaPathFromPlaylist(const std::string_view& path)
{
  for (u32 i = 0; i < static_cast<u32>(s_media_playlist.size()); i++)
  {
    if (path == s_media_playlist[i])
      return RemoveMediaPathFromPlaylist(i);
  }

  return false;
}

bool RemoveMediaPathFromPlaylist(u32 index)
{
  if (index >= static_cast<u32>(s_media_playlist.size()))
    return false;

  if (GetMediaPlaylistIndex() == index)
  {
    g_host_interface->AddFormattedOSDMessage(
      10.0f,
      g_host_interface->TranslateString("System", "Removing current media from playlist, removing media from CD-ROM."));
    g_cdrom.RemoveMedia();
  }

  s_media_playlist.erase(s_media_playlist.begin() + index);
  return true;
}

bool ReplaceMediaPathFromPlaylist(u32 index, const std::string_view& path)
{
  if (index >= static_cast<u32>(s_media_playlist.size()))
    return false;

  if (GetMediaPlaylistIndex() == index)
  {
    g_host_interface->AddFormattedOSDMessage(
      10.0f,
      g_host_interface->TranslateString("System", "Changing current media from playlist, replacing current media."));
    g_cdrom.RemoveMedia();

    s_media_playlist[index] = path;
    InsertMedia(s_media_playlist[index].c_str());
  }
  else
  {
    s_media_playlist[index] = path;
  }

  return true;
}

bool SwitchMediaFromPlaylist(u32 index)
{
  if (index >= s_media_playlist.size())
    return false;

  const std::string& path = s_media_playlist[index];
  if (g_cdrom.HasMedia() && g_cdrom.GetMediaFileName() == path)
    return true;

  return InsertMedia(path.c_str());
}

bool HasCheatList()
{
  return static_cast<bool>(s_cheat_list);
}

CheatList* GetCheatList()
{
  return s_cheat_list.get();
}

void ApplyCheatCode(const CheatCode& code)
{
  Assert(!IsShutdown());
  code.Apply();
}

void SetCheatList(std::unique_ptr<CheatList> cheats)
{
  Assert(!IsShutdown());
  s_cheat_list = std::move(cheats);
}

} // namespace System
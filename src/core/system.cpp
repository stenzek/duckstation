#include "system.h"
#include "bios.h"
#include "bus.h"
#include "cdrom.h"
#include "cheats.h"
#include "common/audio_stream.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/iso_reader.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "common/timestamp.h"
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
#include "libcrypt_game_codes.h"
#include "mdec.h"
#include "memory_card.h"
#include "multitap.h"
#include "pad.h"
#include "pgxp.h"
#include "psf_loader.h"
#include "save_state_version.h"
#include "sio.h"
#include "spu.h"
#include "texture_replacements.h"
#include "timers.h"
#include "xxhash.h"
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <thread>
Log_SetChannel(System);

SystemBootParameters::SystemBootParameters() = default;

SystemBootParameters::SystemBootParameters(SystemBootParameters&& other) = default;

SystemBootParameters::SystemBootParameters(std::string filename_) : filename(std::move(filename_)) {}

SystemBootParameters::~SystemBootParameters() = default;

namespace System {

struct MemorySaveState
{
  std::unique_ptr<HostDisplayTexture> vram_texture;
  std::unique_ptr<GrowableMemoryByteStream> state_stream;
};

static bool SaveMemoryState(MemorySaveState* mss);
static bool LoadMemoryState(const MemorySaveState& mss);

static bool LoadEXE(const char* filename);
static bool SetExpansionROM(const char* filename);

/// Opens CD image, preloading if needed.
static std::unique_ptr<CDImage> OpenCDImage(const char* path, Common::Error* error, bool force_preload,
                                            bool check_for_patches);
static bool ShouldCheckForImagePatches();

static bool DoLoadState(ByteStream* stream, bool force_software_renderer, bool update_display);
static bool DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display);
static void DoRunFrame();
static bool CreateGPU(GPURenderer renderer);

static bool SaveRewindState();
static void DoRewind();

static void SaveRunaheadState();
static void DoRunahead();

static void DoMemorySaveStates();

static bool Initialize(bool force_software_renderer);

static void UpdateRunningGame(const char* path, CDImage* image);
static bool CheckForSBIFile(CDImage* image);

static State s_state = State::Shutdown;
static std::atomic_bool s_startup_cancelled{false};

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
static Common::Timer::Value s_frame_period = 0;
static Common::Timer::Value s_next_frame_time = 0;

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

static std::unique_ptr<CheatList> s_cheat_list;

static bool s_memory_saves_enabled = false;

static std::deque<MemorySaveState> s_rewind_states;
static s32 s_rewind_load_frequency = -1;
static s32 s_rewind_load_counter = -1;
static s32 s_rewind_save_frequency = -1;
static s32 s_rewind_save_counter = -1;
static bool s_rewinding_first_save = false;

static std::deque<MemorySaveState> s_runahead_states;
static std::unique_ptr<AudioStream> s_runahead_audio_stream;
static bool s_runahead_replay_pending = false;
static u32 s_runahead_frames = 0;

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

bool IsStartupCancelled()
{
  return s_startup_cancelled.load();
}

void CancelPendingStartup()
{
  if (s_state == State::Starting)
    s_startup_cancelled.store(true);
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
  return (extension &&
          (StringUtil::Strcasecmp(extension, ".psf") == 0 || StringUtil::Strcasecmp(extension, ".minipsf") == 0));
}

bool IsLoadableFilename(const char* path)
{
  static constexpr auto extensions = make_array(".bin", ".cue", ".img", ".iso", ".chd", ".ecm", ".mds", // discs
                                                ".exe", ".psexe",                                       // exes
                                                ".psf", ".minipsf",                                     // psf
                                                ".m3u",                                                 // playlists
                                                ".pbp");
  const char* extension = std::strrchr(path, '.');
  if (!extension)
    return false;

  for (const char* test_extension : extensions)
  {
    if (StringUtil::Strcasecmp(extension, test_extension) == 0)
      return true;
  }

  return false;
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

std::string GetGameCodeForPath(const char* image_path, bool fallback_to_hash)
{
  std::unique_ptr<CDImage> cdi = CDImage::Open(image_path, nullptr);
  if (!cdi)
    return {};

  return GetGameCodeForImage(cdi.get(), fallback_to_hash);
}

std::string GetGameCodeForImage(CDImage* cdi, bool fallback_to_hash)
{
  std::string code(GetExecutableNameForImage(cdi));
  if (!code.empty())
  {
    // SCES_123.45 -> SCES-12345
    for (std::string::size_type pos = 0; pos < code.size();)
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

  if (!fallback_to_hash)
    return {};

  return GetGameHashCodeForImage(cdi);
}

std::string GetGameHashCodeForImage(CDImage* cdi)
{
  std::string exe_name;
  std::vector<u8> exe_buffer;
  if (!ReadExecutableFromImage(cdi, &exe_name, &exe_buffer))
    return {};

  XXH64_state_t* state = XXH64_createState();
  XXH64_reset(state, 0x4242D00C);
  XXH64_update(state, exe_name.c_str(), exe_name.size());
  XXH64_update(state, exe_buffer.data(), exe_buffer.size());
  const u64 hash = XXH64_digest(state);
  XXH64_freeState(state);

  Log_InfoPrintf("Hash for '%s' - %" PRIX64, exe_name.c_str(), hash);
  return StringUtil::StdStringFromFormat("HASH-%" PRIX64, hash);
}

static std::string GetExecutableNameForImage(CDImage* cdi, ISOReader& iso, bool strip_subdirectories)
{
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

  std::string code = iter->second;
  std::string::size_type pos;
  if (strip_subdirectories)
  {
    // cdrom:\SCES_123.45;1
    pos = code.rfind('\\');
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
  }
  else
  {
    if (code.compare(0, 6, "cdrom:") == 0)
      code.erase(0, 6);
    else
      Log_WarningPrintf("Unknown prefix in executable path: '%s'", code.c_str());

    // remove leading slashes
    while (code[0] == '/' || code[0] == '\\')
      code.erase(0, 1);
  }

  // strip off ; or version number
  pos = code.rfind(';');
  if (pos != std::string::npos)
    code.erase(pos);

  return code;
}

std::string GetExecutableNameForImage(CDImage* cdi)
{
  ISOReader iso;
  if (!iso.Open(cdi, 1))
    return {};

  return GetExecutableNameForImage(cdi, iso, true);
}

bool ReadExecutableFromImage(CDImage* cdi, std::string* out_executable_name, std::vector<u8>* out_executable_data)
{
  ISOReader iso;
  if (!iso.Open(cdi, 1))
    return false;

  bool result = false;

  std::string executable_path(GetExecutableNameForImage(cdi, iso, false));
  Log_DevPrintf("Executable path: '%s'", executable_path.c_str());
  if (!executable_path.empty())
  {
    result = iso.ReadFile(executable_path.c_str(), out_executable_data);
    if (!result)
      Log_ErrorPrintf("Failed to read executable '%s' from disc", executable_path.c_str());
  }

  if (!result)
  {
    // fallback to PSX.EXE
    executable_path = "PSX.EXE";
    result = iso.ReadFile(executable_path.c_str(), out_executable_data);
    if (!result)
      Log_ErrorPrint("Failed to read fallback PSX.EXE from disc");
  }

  if (!result)
    return false;

  if (out_executable_name)
    *out_executable_name = std::move(executable_path);

  return true;
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

  std::string code = GetGameCodeForImage(cdi, false);
  if (code.empty())
    return DiscRegion::Other;

  return GetRegionForCode(code);
}

DiscRegion GetRegionForExe(const char* path)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb");
  if (!fp)
    return DiscRegion::Other;

  BIOS::PSEXEHeader header;
  if (std::fread(&header, sizeof(header), 1, fp.get()) != 1)
    return DiscRegion::Other;

  return BIOS::GetPSExeDiscRegion(header);
}

DiscRegion GetRegionForPsf(const char* path)
{
  PSFLoader::File psf;
  if (!psf.Load(path))
    return DiscRegion::Other;

  return psf.GetRegion();
}

std::optional<DiscRegion> GetRegionForPath(const char* image_path)
{
  if (IsExeFileName(image_path))
    return GetRegionForExe(image_path);
  else if (IsPsfFileName(image_path))
    return GetRegionForPsf(image_path);

  std::unique_ptr<CDImage> cdi = CDImage::Open(image_path, nullptr);
  if (!cdi)
    return {};

  return GetRegionForImage(cdi.get());
}

bool RecreateGPU(GPURenderer renderer, bool update_display /* = true*/)
{
  ClearMemorySaveStates();
  g_gpu->RestoreGraphicsAPIState();

  // save current state
  std::unique_ptr<ByteStream> state_stream = ByteStream_CreateGrowableMemoryStream();
  StateWrapper sw(state_stream.get(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  const bool state_valid = g_gpu->DoState(sw, nullptr, false) && TimingEvents::DoState(sw);
  if (!state_valid)
    Log_ErrorPrintf("Failed to save old GPU state when switching renderers");

  g_gpu->ResetGraphicsAPIState();

  // create new renderer
  g_gpu.reset();
  if (!CreateGPU(renderer))
  {
    if (!IsStartupCancelled())
      g_host_interface->ReportError("Failed to recreate GPU.");

    System::Shutdown();
    return false;
  }

  if (state_valid)
  {
    state_stream->SeekAbsolute(0);
    sw.SetMode(StateWrapper::Mode::Read);
    g_gpu->RestoreGraphicsAPIState();
    g_gpu->DoState(sw, nullptr, update_display);
    TimingEvents::DoState(sw);
    g_gpu->ResetGraphicsAPIState();
  }

  return true;
}

std::unique_ptr<CDImage> OpenCDImage(const char* path, Common::Error* error, bool force_preload, bool check_for_patches)
{
  std::unique_ptr<CDImage> media = CDImage::Open(path, error);
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

  if (check_for_patches)
  {
    const std::string ppf_filename(FileSystem::BuildRelativePath(
      path, FileSystem::ReplaceExtension(FileSystem::GetDisplayNameFromPath(path), "ppf")));
    if (FileSystem::FileExists(ppf_filename.c_str()))
    {
      media = CDImage::OverlayPPFPatch(ppf_filename.c_str(), std::move(media));
      if (!media)
      {
        g_host_interface->AddFormattedOSDMessage(
          30.0f,
          g_host_interface->TranslateString("OSDMessage",
                                            "Failed to apply ppf patch from '%s', using unpatched image."),
          ppf_filename.c_str());
        return OpenCDImage(path, error, force_preload, false);
      }
    }
  }

  return media;
}

bool ShouldCheckForImagePatches()
{
  return g_host_interface->GetBoolSettingValue("CDROM", "LoadImagePatches", false);
}

bool Boot(const SystemBootParameters& params)
{
  Assert(s_state == State::Shutdown);
  s_state = State::Starting;
  s_startup_cancelled.store(false);
  s_region = g_settings.region;

  if (params.state_stream)
  {
    if (!DoLoadState(params.state_stream.get(), params.force_software_renderer, true))
    {
      Shutdown();
      return false;
    }

    if (g_settings.start_paused || params.override_start_paused.value_or(false))
    {
      DebugAssert(s_state == State::Running);
      s_state = State::Paused;
    }

    return true;
  }

  // Load CD image up and detect region.
  Common::Error error;
  std::unique_ptr<CDImage> media;
  bool exe_boot = false;
  bool psf_boot = false;
  if (!params.filename.empty())
  {
    exe_boot = IsExeFileName(params.filename.c_str());
    psf_boot = (!exe_boot && IsPsfFileName(params.filename.c_str()));
    if (exe_boot || psf_boot)
    {
      if (s_region == ConsoleRegion::Auto)
      {
        const DiscRegion file_region =
          (exe_boot ? GetRegionForExe(params.filename.c_str()) : GetRegionForPsf(params.filename.c_str()));
        Log_InfoPrintf("EXE/PSF Region: %s", Settings::GetDiscRegionDisplayName(file_region));
        s_region = GetConsoleRegionForDiscRegion(file_region);
      }
    }
    else
    {
      Log_InfoPrintf("Loading CD image '%s'...", params.filename.c_str());
      media = OpenCDImage(params.filename.c_str(), &error, params.load_image_to_ram, ShouldCheckForImagePatches());
      if (!media)
      {
        g_host_interface->ReportFormattedError("Failed to load CD image '%s': %s", params.filename.c_str(),
                                               error.GetCodeAndMessage().GetCharArray());
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

  Log_InfoPrintf("Console Region: %s", Settings::GetConsoleRegionDisplayName(s_region));

  // Load BIOS image.
  std::optional<BIOS::Image> bios_image = g_host_interface->GetBIOSImage(s_region);
  if (!bios_image)
  {
    g_host_interface->ReportFormattedError(g_host_interface->TranslateString("System", "Failed to load %s BIOS."),
                                           Settings::GetConsoleRegionName(s_region));
    Shutdown();
    return false;
  }

  // Notify change of disc.
  UpdateRunningGame(media ? media->GetFileName().c_str() : params.filename.c_str(), media.get());

  // Check for SBI.
  if (!CheckForSBIFile(media.get()))
  {
    Shutdown();
    return false;
  }

  // Switch subimage.
  if (media && params.media_playlist_index != 0 && !media->SwitchSubImage(params.media_playlist_index, &error))
  {
    g_host_interface->ReportFormattedError("Failed to switch to subimage %u in '%s': %s", params.media_playlist_index,
                                           params.filename.c_str(), error.GetCodeAndMessage().GetCharArray());
    Shutdown();
    return false;
  }

  // Component setup.
  if (!Initialize(params.force_software_renderer))
  {
    Shutdown();
    return false;
  }

  Bus::SetBIOS(*bios_image);
  UpdateControllers();
  UpdateMemoryCardTypes();
  UpdateMultitaps();
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
  else if (psf_boot && !PSFLoader::Load(params.filename.c_str()))
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
  s_state = (g_settings.start_paused || params.override_start_paused.value_or(false)) ? State::Paused : State::Running;
  return true;
}

bool Initialize(bool force_software_renderer)
{
  g_ticks_per_second = ScaleTicksToOverclock(MASTER_CLOCK);
  s_max_slice_ticks = ScaleTicksToOverclock(MASTER_CLOCK / 10);
  s_frame_number = 1;
  s_internal_frame_number = 1;

  s_throttle_frequency = 60.0f;
  s_frame_period = 0;
  s_next_frame_time = 0;

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
  {
    CPU::Shutdown();
    return false;
  }

  if (!CreateGPU(force_software_renderer ? GPURenderer::Software : g_settings.gpu_renderer))
  {
    Bus::Shutdown();
    CPU::Shutdown();
    return false;
  }

  if (g_settings.gpu_pgxp_enable)
    PGXP::Initialize();

  // Was startup cancelled? (e.g. shading compilers took too long and the user closed the application)
  if (IsStartupCancelled())
  {
    Shutdown();
    return false;
  }

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

  static constexpr float WARNING_DURATION = 15.0f;

  if (g_settings.cpu_overclock_active)
  {
    g_host_interface->AddFormattedOSDMessage(
      WARNING_DURATION,
      g_host_interface->TranslateString("OSDMessage",
                                        "CPU clock speed is set to %u%% (%u / %u). This may result in instability."),
      g_settings.GetCPUOverclockPercent(), g_settings.cpu_overclock_numerator, g_settings.cpu_overclock_denominator);
  }
  if (g_settings.cdrom_read_speedup > 1)
  {
    g_host_interface->AddFormattedOSDMessage(
      WARNING_DURATION,
      g_host_interface->TranslateString(
        "OSDMessage", "CD-ROM read speedup set to %ux (effective speed %ux). This may result in instability."),
      g_settings.cdrom_read_speedup, g_settings.cdrom_read_speedup * 2);
  }
  if (g_settings.cdrom_seek_speedup != 1)
  {
    if (g_settings.cdrom_seek_speedup == 0)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage",
                                             "CD-ROM seek speedup set to instant. This may result in instability."),
        WARNING_DURATION);
    }
    else
    {
      g_host_interface->AddFormattedOSDMessage(
        WARNING_DURATION,
        g_host_interface->TranslateString("OSDMessage",
                                          "CD-ROM seek speedup set to %ux. This may result in instability."),
        g_settings.cdrom_seek_speedup);
    }
  }

  UpdateThrottlePeriod();
  UpdateMemorySaveStateSettings();
  return true;
}

void Shutdown()
{
  if (s_state == State::Shutdown)
    return;

  ClearMemorySaveStates();
  s_runahead_audio_stream.reset();

  g_texture_replacements.Shutdown();

  g_sio.Shutdown();
  g_mdec.Shutdown();
  g_spu.Shutdown();
  g_timers.Shutdown();
  g_pad.Shutdown();
  g_cdrom.Shutdown();
  g_gpu.reset();
  g_interrupt_controller.Shutdown();
  g_dma.Shutdown();
  PGXP::Shutdown();
  CPU::CodeCache::Shutdown();
  Bus::Shutdown();
  CPU::Shutdown();
  TimingEvents::Shutdown();
  s_running_game_code.clear();
  s_running_game_path.clear();
  s_running_game_title.clear();
  s_cheat_list.reset();
  s_state = State::Shutdown;

  g_host_interface->OnRunningGameChanged(s_running_game_path, nullptr, s_running_game_code, s_running_game_title);
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
    Log_ErrorPrintf("Failed to initialize %s renderer, falling back to software renderer",
                    Settings::GetRendererName(renderer));
    g_host_interface->AddFormattedOSDMessage(
      30.0f,
      g_host_interface->TranslateString("OSDMessage",
                                        "Failed to initialize %s renderer, falling back to software renderer."),
      Settings::GetRendererName(renderer));
    g_gpu.reset();
    g_gpu = GPU::CreateSoftwareRenderer();
    if (!g_gpu->Initialize(g_host_interface->GetDisplay()))
      return false;
  }

  // we put this here rather than in Initialize() because of the virtual calls
  g_gpu->Reset(true);
  return true;
}

bool DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display)
{
  const bool is_memory_state = (host_texture != nullptr);

  if (!sw.DoMarker("System"))
    return false;

  sw.Do(&s_region);
  sw.Do(&s_frame_number);
  sw.Do(&s_internal_frame_number);

  if (!sw.DoMarker("CPU") || !CPU::DoState(sw))
    return false;

  if (sw.IsReading())
    CPU::CodeCache::Flush();

  // only reset pgxp if we're not runahead-rollbacking. the value checks will save us from broken rendering, and it
  // saves using imprecise values for a frame in 30fps games.
  if (sw.IsReading() && g_settings.gpu_pgxp_enable && !is_memory_state)
    PGXP::Initialize();

  if (!sw.DoMarker("Bus") || !Bus::DoState(sw))
    return false;

  if (!sw.DoMarker("DMA") || !g_dma.DoState(sw))
    return false;

  if (!sw.DoMarker("InterruptController") || !g_interrupt_controller.DoState(sw))
    return false;

  g_gpu->RestoreGraphicsAPIState();
  const bool gpu_result = sw.DoMarker("GPU") && g_gpu->DoState(sw, host_texture, update_display);
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
  if (g_settings.gpu_pgxp_enable)
    PGXP::Initialize();

  Bus::Reset();
  g_dma.Reset();
  g_interrupt_controller.Reset();
  g_gpu->Reset(true);
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

  Common::Error error;
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
      media = OpenCDImage(media_filename.c_str(), &error, false, ShouldCheckForImagePatches());
      if (!media)
      {
        if (old_media)
        {
          g_host_interface->AddFormattedOSDMessage(
            30.0f,
            g_host_interface->TranslateString("OSDMessage", "Failed to open CD image from save state '%s': %s. Using "
                                                            "existing image '%s', this may result in instability."),
            media_filename.c_str(), error.GetCodeAndMessage().GetCharArray(), old_media->GetFileName().c_str());
          media = std::move(old_media);
        }
        else
        {
          g_host_interface->ReportFormattedError(
            g_host_interface->TranslateString("System", "Failed to open CD image '%s' used by save state: %s."),
            media_filename.c_str(), error.GetCodeAndMessage().GetCharArray());
          return false;
        }
      }
    }
  }

  UpdateRunningGame(media_filename.c_str(), media.get());

  if (media && header.version >= 51)
  {
    const u32 num_subimages = media->HasSubImages() ? media->GetSubImageCount() : 1;
    if (header.media_subimage_index >= num_subimages ||
        (media->HasSubImages() && media->GetCurrentSubImage() != header.media_subimage_index &&
         !media->SwitchSubImage(header.media_subimage_index, &error)))
    {
      g_host_interface->ReportFormattedError(
        g_host_interface->TranslateString("System",
                                          "Failed to switch to subimage %u in CD image '%s' used by save state: %s."),
        header.media_subimage_index + 1u, media_filename.c_str(), error.GetCodeAndMessage().GetCharArray());
      return false;
    }
    else
    {
      Log_InfoPrintf("Switched to subimage %u in '%s'", header.media_subimage_index, media_filename.c_str());
    }
  }

  ClearMemorySaveStates();

  if (s_state == State::Starting)
  {
    if (!Initialize(force_software_renderer))
      return false;

    if (media)
      g_cdrom.InsertMedia(std::move(media));

    UpdateControllers();
    UpdateMemoryCardTypes();
    UpdateMultitaps();
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
      UpdatePerGameMemoryCards();
  }

  if (header.data_compression_type != 0)
  {
    g_host_interface->ReportFormattedError("Unknown save state compression type %u", header.data_compression_type);
    return false;
  }

  if (!state->SeekAbsolute(header.offset_to_data))
    return false;

  StateWrapper sw(state, StateWrapper::Mode::Read, header.version);
  if (!DoState(sw, nullptr, update_display))
    return false;

  if (s_state == State::Starting)
    s_state = State::Running;

  g_host_interface->GetAudioStream()->EmptyBuffers();
  return true;
}

bool SaveState(ByteStream* state, u32 screenshot_size /* = 256 */)
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
    header.media_subimage_index = g_cdrom.GetMedia()->HasSubImages() ? g_cdrom.GetMedia()->GetCurrentSubImage() : 0;
    if (!media_filename.empty() && !state->Write2(media_filename.data(), header.media_filename_length))
      return false;
  }

  // save screenshot
  if (screenshot_size > 0)
  {
    // assume this size is the width
    HostDisplay* display = g_host_interface->GetDisplay();
    const float display_aspect_ratio = display->GetDisplayAspectRatio();
    const u32 screenshot_width = screenshot_size;
    const u32 screenshot_height =
      std::max(1u, static_cast<u32>(static_cast<float>(screenshot_width) /
                                    ((display_aspect_ratio > 0.0f) ? display_aspect_ratio : 1.0f)));
    Log_VerbosePrintf("Saving %ux%u screenshot for state", screenshot_width, screenshot_height);

    std::vector<u32> screenshot_buffer;
    u32 screenshot_stride;
    HostDisplayPixelFormat screenshot_format;
    if (display->RenderScreenshot(screenshot_width, screenshot_height, &screenshot_buffer, &screenshot_stride,
                                  &screenshot_format) ||
        !display->ConvertTextureDataToRGBA8(screenshot_width, screenshot_height, screenshot_buffer, screenshot_stride,
                                            HostDisplayPixelFormat::RGBA8))
    {
      if (screenshot_stride != (screenshot_width * sizeof(u32)))
      {
        Log_WarningPrintf("Failed to save %ux%u screenshot for save state due to incorrect stride(%u)",
                          screenshot_width, screenshot_height, screenshot_stride);
      }
      else
      {
        if (display->UsesLowerLeftOrigin())
          display->FlipTextureDataRGBA8(screenshot_width, screenshot_height, screenshot_buffer, screenshot_stride);

        header.offset_to_screenshot = static_cast<u32>(state->GetPosition());
        header.screenshot_width = screenshot_width;
        header.screenshot_height = screenshot_height;
        header.screenshot_size = static_cast<u32>(screenshot_buffer.size() * sizeof(u32));
        if (!state->Write2(screenshot_buffer.data(), header.screenshot_size))
          return false;
      }
    }
    else
    {
      Log_WarningPrintf("Failed to save %ux%u screenshot for save state due to render/conversion failure",
                        screenshot_width, screenshot_height);
    }
  }

  // write data
  {
    header.offset_to_data = static_cast<u32>(state->GetPosition());

    g_gpu->RestoreGraphicsAPIState();

    StateWrapper sw(state, StateWrapper::Mode::Write, SAVE_STATE_VERSION);
    const bool result = DoState(sw, nullptr, false);

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

void DoRunFrame()
{
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

void RunFrame()
{
  s_frame_timer.Reset();

  if (s_rewind_load_counter >= 0)
  {
    DoRewind();
    return;
  }

  if (s_runahead_frames > 0)
    DoRunahead();

  DoRunFrame();

  s_next_frame_time += s_frame_period;

  if (s_memory_saves_enabled)
    DoMemorySaveStates();
}

float GetTargetSpeed()
{
  return s_target_speed;
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
  if (s_target_speed > std::numeric_limits<double>::epsilon())
  {
    const double target_speed = std::max(static_cast<double>(s_target_speed), std::numeric_limits<double>::epsilon());
    s_frame_period =
      Common::Timer::ConvertSecondsToValue(1.0 / (static_cast<double>(s_throttle_frequency) * target_speed));
  }
  else
  {
    s_frame_period = 1;
  }

  ResetThrottler();
}

void ResetThrottler()
{
  s_next_frame_time = Common::Timer::GetValue();
}

void Throttle()
{
  // Reset the throttler on audio buffer overflow, so we don't end up out of phase.
  if (g_host_interface->GetAudioStream()->DidUnderflow() && s_target_speed >= 1.0f)
  {
    Log_VerbosePrintf("Audio buffer underflowed, resetting throttler");
    ResetThrottler();
    return;
  }

  // Allow variance of up to 40ms either way.
#ifndef __ANDROID__
  static constexpr double MAX_VARIANCE_TIME_NS = 40 * 1000000;
#else
  static constexpr double MAX_VARIANCE_TIME_NS = 50 * 1000000;
#endif

  // Don't sleep for <1ms or >=period.
  static constexpr double MINIMUM_SLEEP_TIME_NS = 1 * 1000000;

  // Use unsigned for defined overflow/wrap-around.
  const Common::Timer::Value time = Common::Timer::GetValue();
  const double sleep_time = (s_next_frame_time >= time) ?
                              Common::Timer::ConvertValueToNanoseconds(s_next_frame_time - time) :
                              -Common::Timer::ConvertValueToNanoseconds(time - s_next_frame_time);
  if (sleep_time < -MAX_VARIANCE_TIME_NS)
  {
    // Don't display the slow messages in debug, it'll always be slow...
#ifndef _DEBUG
    Log_VerbosePrintf("System too slow, lost %.2f ms", (-sleep_time - MAX_VARIANCE_TIME_NS) / 1000000.0);
#endif
    ResetThrottler();
  }
  else
  {
    Common::Timer::SleepUntil(s_next_frame_time, true);
  }
}

void RunFrames()
{
  // If we're running more than this in a single loop... we're in for a bad time.
  const u32 max_frames_to_run = 2;
  u32 frames_run = 0;

  Common::Timer::Value value = Common::Timer::GetValue();
  while (frames_run < max_frames_to_run)
  {
    if (value < s_next_frame_time)
      break;

    RunFrame();
    frames_run++;

    value = Common::Timer::GetValue();
  }

  if (frames_run != 1)
    Log_VerbosePrintf("Ran %u frames in a single host frame", frames_run);
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

bool InjectEXEFromBuffer(const void* buffer, u32 buffer_size, bool patch_bios)
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
  if (patch_bios)
  {
    const u32 r_pc = header.initial_pc;
    const u32 r_gp = header.initial_gp;
    const u32 r_sp = header.initial_sp_base + header.initial_sp_offset;
    const u32 r_fp = header.initial_sp_base + header.initial_sp_offset;
    if (!BIOS::PatchBIOSForEXE(Bus::g_bios, Bus::BIOS_SIZE, r_pc, r_gp, r_sp, r_fp))
      return false;
  }

  return true;
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

static std::unique_ptr<MemoryCard> GetMemoryCardForSlot(u32 slot, MemoryCardType type)
{
  // Disable memory cards when running PSFs.
  const bool is_running_psf = !s_running_game_path.empty() && IsPsfFileName(s_running_game_path.c_str());
  if (is_running_psf)
    return nullptr;

  switch (type)
  {
    case MemoryCardType::PerGame:
    {
      if (s_running_game_code.empty())
      {
        g_host_interface->AddFormattedOSDMessage(
          5.0f,
          g_host_interface->TranslateString("System", "Per-game memory card cannot be used for slot %u as the running "
                                                      "game has no code. Using shared card instead."),
          slot + 1u);
        return MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(slot));
      }
      else
      {
        return MemoryCard::Open(g_host_interface->GetGameMemoryCardPath(s_running_game_code.c_str(), slot));
      }
    }

    case MemoryCardType::PerGameTitle:
    {
      if (s_running_game_title.empty())
      {
        g_host_interface->AddFormattedOSDMessage(
          5.0f,
          g_host_interface->TranslateString("System", "Per-game memory card cannot be used for slot %u as the running "
                                                      "game has no title. Using shared card instead."),
          slot + 1u);
        return MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(slot));
      }
      else
      {
        return MemoryCard::Open(g_host_interface->GetGameMemoryCardPath(
          MemoryCard::SanitizeGameTitleForFileName(s_running_game_title).c_str(), slot));
      }
    }

    case MemoryCardType::PerGameFileTitle:
    {
      const std::string display_name(FileSystem::GetDisplayNameFromPath(s_running_game_path));
      const std::string_view file_title(FileSystem::GetFileTitleFromPath(display_name));
      if (file_title.empty())
      {
        g_host_interface->AddFormattedOSDMessage(
          5.0f,
          g_host_interface->TranslateString("System", "Per-game memory card cannot be used for slot %u as the running "
                                                      "game has no path. Using shared card instead."),
          slot + 1u);
        return MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(slot));
      }
      else
      {
        return MemoryCard::Open(
          g_host_interface->GetGameMemoryCardPath(MemoryCard::SanitizeGameTitleForFileName(file_title).c_str(), slot));
      }
    }

    case MemoryCardType::Shared:
    {
      if (g_settings.memory_card_paths[slot].empty())
        return MemoryCard::Open(g_host_interface->GetSharedMemoryCardPath(slot));
      else
        return MemoryCard::Open(g_settings.memory_card_paths[slot]);
    }

    case MemoryCardType::NonPersistent:
      return MemoryCard::Create();

    case MemoryCardType::None:
    default:
      return nullptr;
  }
}

void UpdateMemoryCardTypes()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    g_pad.SetMemoryCard(i, nullptr);

    const MemoryCardType type = g_settings.memory_card_types[i];
    std::unique_ptr<MemoryCard> card = GetMemoryCardForSlot(i, type);
    if (card)
      g_pad.SetMemoryCard(i, std::move(card));
  }
}

void UpdatePerGameMemoryCards()
{
  // Disable memory cards when running PSFs.
  const bool is_running_psf = !s_running_game_path.empty() && IsPsfFileName(s_running_game_path.c_str());

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    const MemoryCardType type = g_settings.memory_card_types[i];
    if (!Settings::IsPerGameMemoryCardType(type))
      continue;

    g_pad.SetMemoryCard(i, nullptr);

    std::unique_ptr<MemoryCard> card = GetMemoryCardForSlot(i, type);
    if (card)
      g_pad.SetMemoryCard(i, std::move(card));
  }
}

bool HasMemoryCard(u32 slot)
{
  return (g_pad.GetMemoryCard(slot) != nullptr);
}

void SwapMemoryCards()
{
  std::unique_ptr<MemoryCard> first = g_pad.RemoveMemoryCard(0);
  std::unique_ptr<MemoryCard> second = g_pad.RemoveMemoryCard(1);
  g_pad.SetMemoryCard(0, std::move(second));
  g_pad.SetMemoryCard(1, std::move(first));
}

void UpdateMultitaps()
{
  switch (g_settings.multitap_mode)
  {
    case MultitapMode::Disabled:
    {
      g_pad.GetMultitap(0)->SetEnable(false, 0);
      g_pad.GetMultitap(1)->SetEnable(false, 0);
    }
    break;

    case MultitapMode::Port1Only:
    {
      g_pad.GetMultitap(0)->SetEnable(true, 0);
      g_pad.GetMultitap(1)->SetEnable(false, 0);
    }
    break;

    case MultitapMode::Port2Only:
    {
      g_pad.GetMultitap(0)->SetEnable(false, 0);
      g_pad.GetMultitap(1)->SetEnable(true, 1);
    }
    break;

    case MultitapMode::BothPorts:
    {
      g_pad.GetMultitap(0)->SetEnable(true, 0);
      g_pad.GetMultitap(1)->SetEnable(true, 4);
    }
    break;
  }
}

bool DumpRAM(const char* filename)
{
  if (!IsValid())
    return false;

  return FileSystem::WriteBinaryFile(filename, Bus::g_ram, Bus::g_ram_size);
}

bool DumpVRAM(const char* filename)
{
  if (!IsValid())
    return false;

  g_gpu->RestoreGraphicsAPIState();
  const bool result = g_gpu->DumpVRAMToFile(filename);
  g_gpu->ResetGraphicsAPIState();

  return result;
}

bool DumpSPURAM(const char* filename)
{
  if (!IsValid())
    return false;

  return FileSystem::WriteBinaryFile(filename, g_spu.GetRAM().data(), SPU::RAM_SIZE);
}

bool HasMedia()
{
  return g_cdrom.HasMedia();
}

std::string GetMediaFileName()
{
  if (!g_cdrom.HasMedia())
    return {};

  return g_cdrom.GetMediaFileName();
}

bool InsertMedia(const char* path)
{
  Common::Error error;
  std::unique_ptr<CDImage> image = OpenCDImage(path, &error, false, ShouldCheckForImagePatches());
  if (!image)
  {
    g_host_interface->AddFormattedOSDMessage(
      10.0f, g_host_interface->TranslateString("OSDMessage", "Failed to open disc image '%s': %s."), path,
      error.GetCodeAndMessage().GetCharArray());
    return false;
  }

  UpdateRunningGame(path, image.get());
  g_cdrom.InsertMedia(std::move(image));
  Log_InfoPrintf("Inserted media from %s (%s, %s)", s_running_game_path.c_str(), s_running_game_code.c_str(),
                 s_running_game_title.c_str());
  g_host_interface->AddFormattedOSDMessage(10.0f,
                                           g_host_interface->TranslateString("OSDMessage", "Inserted disc '%s' (%s)."),
                                           s_running_game_title.c_str(), s_running_game_code.c_str());

  if (g_settings.HasAnyPerGameMemoryCards())
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("System", "Game changed, reloading memory cards."), 10.0f);
    UpdatePerGameMemoryCards();
  }

  ClearMemorySaveStates();
  return true;
}

void RemoveMedia()
{
  g_cdrom.RemoveMedia();
  ClearMemorySaveStates();
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

    if (image && image->HasSubImages() && g_settings.memory_card_use_playlist_title)
    {
      std::string image_title(image->GetMetadata("title"));
      if (!image_title.empty())
        s_running_game_title = std::move(image_title);
    }
  }

  g_texture_replacements.SetGameID(s_running_game_code);

  g_host_interface->OnRunningGameChanged(s_running_game_path, image, s_running_game_code, s_running_game_title);
}

bool CheckForSBIFile(CDImage* image)
{
  if (s_running_game_code.empty() || !LibcryptGameList::IsLibcryptGameCode(s_running_game_code) || !image ||
      image->HasNonStandardSubchannel())
  {
    return true;
  }

  Log_WarningPrintf("SBI file missing but required for %s (%s)", s_running_game_code.c_str(),
                    s_running_game_title.c_str());

  if (g_host_interface->GetBoolSettingValue("CDROM", "AllowBootingWithoutSBIFile", false))
  {
    return g_host_interface->ConfirmMessage(
      StringUtil::StdStringFromFormat(
        g_host_interface->TranslateString(
          "System",
          "You are attempting to run a libcrypt protected game without an SBI file:\n\n%s: %s\n\nThe game will "
          "likely not run properly.\n\nPlease check the README for instructions on how to add an SBI file.\n\nDo "
          "you wish to continue?"),
        s_running_game_code.c_str(), s_running_game_title.c_str())
        .c_str());
  }
  else
  {
    g_host_interface->ReportError(SmallString::FromFormat(
      g_host_interface->TranslateString(
        "System", "You are attempting to run a libcrypt protected game without an SBI file:\n\n%s: %s\n\nYour dump is "
                  "incomplete, you must add the SBI file to run this game."),
      s_running_game_code.c_str(), s_running_game_title.c_str()));
    return false;
  }
}

bool HasMediaSubImages()
{
  const CDImage* cdi = g_cdrom.GetMedia();
  return cdi ? cdi->HasSubImages() : false;
}

u32 GetMediaSubImageCount()
{
  const CDImage* cdi = g_cdrom.GetMedia();
  return cdi ? cdi->GetSubImageCount() : 0;
}

u32 GetMediaSubImageIndex()
{
  const CDImage* cdi = g_cdrom.GetMedia();
  return cdi ? cdi->GetCurrentSubImage() : 0;
}

u32 GetMediaSubImageIndexForTitle(const std::string_view& title)
{
  const CDImage* cdi = g_cdrom.GetMedia();
  if (!cdi)
    return 0;

  const u32 count = cdi->GetSubImageCount();
  for (u32 i = 0; i < count; i++)
  {
    if (title == cdi->GetSubImageMetadata(i, "title"))
      return i;
  }

  return std::numeric_limits<u32>::max();
}

std::string GetMediaSubImageTitle(u32 index)
{
  const CDImage* cdi = g_cdrom.GetMedia();
  if (!cdi)
    return {};

  return cdi->GetSubImageMetadata(index, "title");
}

bool SwitchMediaSubImage(u32 index)
{
  if (!g_cdrom.HasMedia())
    return false;

  std::unique_ptr<CDImage> image = g_cdrom.RemoveMedia();
  Assert(image);

  Common::Error error;
  if (!image->SwitchSubImage(index, &error))
  {
    g_host_interface->AddFormattedOSDMessage(
      10.0f, g_host_interface->TranslateString("OSDMessage", "Failed to switch to subimage %u in '%s': %s."),
      index + 1u, image->GetFileName().c_str(), error.GetCodeAndMessage().GetCharArray());
    g_cdrom.InsertMedia(std::move(image));
    return false;
  }

  g_host_interface->AddFormattedOSDMessage(
    20.0f, g_host_interface->TranslateString("OSDMessage", "Switched to sub-image %s (%u) in '%s'."),
    image->GetSubImageMetadata(index, "title").c_str(), index + 1u, image->GetMetadata("title").c_str());
  g_cdrom.InsertMedia(std::move(image));

  ClearMemorySaveStates();
  return true;
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

void CalculateRewindMemoryUsage(u32 num_saves, u64* ram_usage, u64* vram_usage)
{
  *ram_usage = MAX_SAVE_STATE_SIZE * static_cast<u64>(num_saves);
  *vram_usage = (VRAM_WIDTH * VRAM_HEIGHT * 4) * static_cast<u64>(std::max(g_settings.gpu_resolution_scale, 1u)) *
                static_cast<u64>(g_settings.gpu_multisamples) * static_cast<u64>(num_saves);
}

void ClearMemorySaveStates()
{
  s_rewind_states.clear();
  s_runahead_states.clear();
}

void UpdateMemorySaveStateSettings()
{
  ClearMemorySaveStates();

  s_memory_saves_enabled = g_settings.rewind_enable;

  if (g_settings.rewind_enable)
  {
    s_rewind_save_frequency = static_cast<s32>(std::ceil(g_settings.rewind_save_frequency * s_throttle_frequency));
    s_rewind_save_counter = 0;

    u64 ram_usage, vram_usage;
    CalculateRewindMemoryUsage(g_settings.rewind_save_slots, &ram_usage, &vram_usage);
    Log_InfoPrintf(
      "Rewind is enabled, saving every %d frames, with %u slots and %" PRIu64 "MB RAM and %" PRIu64 "MB VRAM usage",
      std::max(s_rewind_save_frequency, 1), g_settings.rewind_save_slots, ram_usage / 1048576, vram_usage / 1048576);
  }
  else
  {
    s_rewind_save_frequency = -1;
    s_rewind_save_counter = -1;
  }

  s_rewind_load_frequency = -1;
  s_rewind_load_counter = -1;

  s_runahead_frames = g_settings.runahead_frames;
  s_runahead_replay_pending = false;
  if (s_runahead_frames > 0)
  {
    Log_InfoPrintf("Runahead is active with %u frames", s_runahead_frames);

    if (!s_runahead_audio_stream)
    {
      // doesn't matter if it's not resampled here since it eats everything anyway, nom nom nom.
      s_runahead_audio_stream = AudioStream::CreateNullAudioStream();
      s_runahead_audio_stream->Reconfigure(HostInterface::AUDIO_SAMPLE_RATE, HostInterface::AUDIO_SAMPLE_RATE,
                                           HostInterface::AUDIO_CHANNELS);
    }
  }
  else
  {
    s_runahead_audio_stream.reset();
  }
}

bool LoadMemoryState(const MemorySaveState& mss)
{
  mss.state_stream->SeekAbsolute(0);

  StateWrapper sw(mss.state_stream.get(), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  HostDisplayTexture* host_texture = mss.vram_texture.get();
  if (!DoState(sw, &host_texture, true))
  {
    g_host_interface->ReportError("Failed to load memory save state, resetting.");
    Reset();
    return false;
  }

  return true;
}

bool SaveMemoryState(MemorySaveState* mss)
{
  if (!mss->state_stream)
    mss->state_stream = std::make_unique<GrowableMemoryByteStream>(nullptr, MAX_SAVE_STATE_SIZE);
  else
    mss->state_stream->SeekAbsolute(0);

  HostDisplayTexture* host_texture = mss->vram_texture.release();
  StateWrapper sw(mss->state_stream.get(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  if (!DoState(sw, &host_texture, false))
  {
    Log_ErrorPrint("Failed to create rewind state.");
    delete host_texture;
    return false;
  }

  mss->vram_texture.reset(host_texture);
  return true;
}

bool SaveRewindState()
{
  Common::Timer save_timer;

  // try to reuse the frontmost slot
  const u32 save_slots = g_settings.rewind_save_slots;
  MemorySaveState mss;
  while (s_rewind_states.size() >= save_slots)
  {
    mss = std::move(s_rewind_states.front());
    s_rewind_states.pop_front();
  }

  if (!SaveMemoryState(&mss))
    return false;

  s_rewind_states.push_back(std::move(mss));

  Log_DevPrintf("Saved rewind state (%" PRIu64 " bytes, took %.4f ms)", s_rewind_states.back().state_stream->GetSize(),
                save_timer.GetTimeMilliseconds());

  return true;
}

bool LoadRewindState(u32 skip_saves /*= 0*/, bool consume_state /*=true */)
{
  while (skip_saves > 0 && !s_rewind_states.empty())
  {
    s_rewind_states.pop_back();
    skip_saves--;
  }

  if (s_rewind_states.empty())
    return false;

  Common::Timer load_timer;

  if (!LoadMemoryState(s_rewind_states.back()))
    return false;

  if (consume_state)
    s_rewind_states.pop_back();

  Log_DevPrintf("Rewind load took %.4f ms", load_timer.GetTimeMilliseconds());
  return true;
}

void SetRewinding(bool enabled)
{
  if (enabled)
  {
    // Try to rewind at the replay speed, or one per second maximum.
    const float load_frequency = std::min(g_settings.rewind_save_frequency, 1.0f);
    s_rewind_load_frequency = static_cast<s32>(std::ceil(load_frequency * s_throttle_frequency));
    s_rewind_load_counter = 0;
  }
  else
  {
    s_rewind_load_frequency = -1;
    s_rewind_load_counter = -1;
  }

  s_rewinding_first_save = true;
}

void DoRewind()
{
  s_frame_timer.Reset();

  if (s_rewind_load_counter == 0)
  {
    const u32 skip_saves = BoolToUInt32(!s_rewinding_first_save);
    s_rewinding_first_save = false;
    LoadRewindState(skip_saves, false);
    ResetPerformanceCounters();
    s_rewind_load_counter = s_rewind_load_frequency;
  }
  else
  {
    s_rewind_load_counter--;
  }
}

void SaveRunaheadState()
{
  // try to reuse the frontmost slot
  MemorySaveState mss;
  while (s_runahead_states.size() >= s_runahead_frames)
  {
    mss = std::move(s_runahead_states.front());
    s_runahead_states.pop_front();
  }

  if (!SaveMemoryState(&mss))
  {
    Log_ErrorPrint("Failed to save runahead state.");
    return;
  }

  s_runahead_states.push_back(std::move(mss));
}

void DoRunahead()
{
  Common::Timer timer;
  Log_DevPrintf("runahead starting at frame %u", s_frame_number);

  if (s_runahead_replay_pending)
  {
    // we need to replay and catch up - load the state,
    s_runahead_replay_pending = false;
    if (s_runahead_states.empty() || !LoadMemoryState(s_runahead_states.front()))
    {
      s_runahead_states.clear();
      return;
    }

    // and throw away all the states, forcing us to catch up below
    // TODO: can we leave one frame here and run, avoiding the extra save?
    s_runahead_states.clear();
    Log_VerbosePrintf("Rewound to frame %u, took %.2f ms", s_frame_number, timer.GetTimeMilliseconds());
  }

  // run the frames with no audio
  s32 frames_to_run = static_cast<s32>(s_runahead_frames) - static_cast<s32>(s_runahead_states.size());
  if (frames_to_run > 0)
  {
    Common::Timer timer2;
    const s32 temp = frames_to_run;

    g_spu.SetAudioStream(s_runahead_audio_stream.get());

    while (frames_to_run > 0)
    {
      DoRunFrame();
      SaveRunaheadState();
      frames_to_run--;
    }

    g_spu.SetAudioStream(g_host_interface->GetAudioStream());

    Log_VerbosePrintf("Running %d frames to catch up took %.2f ms", temp, timer2.GetTimeMilliseconds());
  }
  else
  {
    // save this frame
    SaveRunaheadState();
  }

  Log_DevPrintf("runahead ending at frame %u, took %.2f ms", s_frame_number, timer.GetTimeMilliseconds());
}

void DoMemorySaveStates()
{
  if (s_rewind_save_counter >= 0)
  {
    if (s_rewind_save_counter == 0)
    {
      SaveRewindState();
      s_rewind_save_counter = s_rewind_save_frequency;
    }
    else
    {
      s_rewind_save_counter--;
    }
  }

  if (s_runahead_frames > 0)
    SaveRunaheadState();
}

void SetRunaheadReplayFlag()
{
  if (s_runahead_frames == 0 || s_runahead_states.empty())
    return;

  Log_DevPrintf("Runahead rewind pending...");
  s_runahead_replay_pending = true;
}

} // namespace System

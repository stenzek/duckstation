#include "libretro_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/analog_controller.h"
#include "core/bus.h"
#include "core/cheats.h"
#include "core/digital_controller.h"
#include "core/gpu.h"
#include "core/system.h"
#include "libretro_audio_stream.h"
#include "libretro_host_display.h"
#include "libretro_opengl_host_display.h"
#include "libretro_settings_interface.h"
#include "libretro_vulkan_host_display.h"
#include <array>
#include <cstring>
#include <tuple>
#include <utility>
#include <vector>
Log_SetChannel(LibretroHostInterface);

#ifdef WIN32
#include "libretro_d3d11_host_display.h"
#endif

LibretroHostInterface g_libretro_host_interface;
#define P_THIS (&g_libretro_host_interface)

retro_environment_t g_retro_environment_callback;
retro_video_refresh_t g_retro_video_refresh_callback;
retro_audio_sample_t g_retro_audio_sample_callback;
retro_audio_sample_batch_t g_retro_audio_sample_batch_callback;
retro_input_poll_t g_retro_input_poll_callback;
retro_input_state_t g_retro_input_state_callback;

static retro_log_callback s_libretro_log_callback = {};
static bool s_libretro_log_callback_valid = false;
static bool s_libretro_log_callback_registered = false;

static void LibretroLogCallback(void* pUserParam, const char* channelName, const char* functionName, LOGLEVEL level,
                                const char* message)
{
  static constexpr std::array<retro_log_level, LOGLEVEL_COUNT> levels = {
    {RETRO_LOG_ERROR, RETRO_LOG_ERROR, RETRO_LOG_WARN, RETRO_LOG_INFO, RETRO_LOG_INFO, RETRO_LOG_INFO, RETRO_LOG_DEBUG,
     RETRO_LOG_DEBUG, RETRO_LOG_DEBUG, RETRO_LOG_DEBUG}};

  s_libretro_log_callback.log(levels[level], "[%s] %s\n", (level <= LOGLEVEL_PERF) ? functionName : channelName,
                              message);
}

LibretroHostInterface::LibretroHostInterface() = default;

LibretroHostInterface::~LibretroHostInterface()
{
  // should be cleaned up by the context destroy, but just in case
  if (m_hw_render_display)
  {
    m_hw_render_display->DestroyRenderDevice();
    m_hw_render_display.reset();
  }
}

void LibretroHostInterface::InitInterfaces()
{
  SetCoreOptions();
  InitDiskControlInterface();

  if (!m_interfaces_initialized)
  {
    InitLogging();
    InitRumbleInterface();

    unsigned dummy = 0;
    m_supports_input_bitmasks = g_retro_environment_callback(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, &dummy);

    m_interfaces_initialized = true;
  }
}

void LibretroHostInterface::InitLogging()
{
  if (s_libretro_log_callback_registered)
    return;

  s_libretro_log_callback_valid =
    g_retro_environment_callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &s_libretro_log_callback);

  if (s_libretro_log_callback_valid)
  {
    Log::RegisterCallback(LibretroLogCallback, nullptr);
    s_libretro_log_callback_registered = true;
  }
}

bool LibretroHostInterface::Initialize()
{
  if (!HostInterface::Initialize())
    return false;

  LoadSettings();
  FixIncompatibleSettings(true);
  UpdateLogging();

  m_last_aspect_ratio = Settings::GetDisplayAspectRatioValue(g_settings.display_aspect_ratio);
  return true;
}

void LibretroHostInterface::Shutdown()
{
  HostInterface::Shutdown();
}

void LibretroHostInterface::ReportError(const char* message)
{
  AddFormattedOSDMessage(10.0f, "ERROR: %s", message);
  Log_ErrorPrint(message);
}

void LibretroHostInterface::ReportMessage(const char* message)
{
  AddOSDMessage(message, 5.0f);
  Log_InfoPrint(message);
}

bool LibretroHostInterface::ConfirmMessage(const char* message)
{
  Log_InfoPrintf("Confirm: %s", message);
  return false;
}

void LibretroHostInterface::GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title)
{
  // Just use the filename for now... we don't have the game list. Unless we can pull this from the frontend somehow?
  *title = System::GetTitleForPath(path);
  code->clear();
}

static const char* GetSaveDirectory()
{
  const char* save_directory = nullptr;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_directory) || !save_directory)
    save_directory = "saves";

  return save_directory;
}

std::string LibretroHostInterface::GetSharedMemoryCardPath(u32 slot) const
{
  return StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "duckstation_shared_card_%d.mcd",
                                         GetSaveDirectory(), slot + 1);
}

std::string LibretroHostInterface::GetGameMemoryCardPath(const char* game_code, u32 slot) const
{
  return StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s_%d.mcd", GetSaveDirectory(), game_code,
                                         slot + 1);
}

std::string LibretroHostInterface::GetShaderCacheBasePath() const
{
  // Use the save directory, and failing that, the system directory.
  const char* save_directory_ptr = nullptr;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_directory_ptr) || !save_directory_ptr)
  {
    save_directory_ptr = nullptr;
    if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &save_directory_ptr) ||
        !save_directory_ptr)
    {
      Log_WarningPrint("No shader cache directory available, startup will be slower.");
      return std::string();
    }
  }

  // Use a directory named "duckstation_cache" in the save/system directory.
  std::string shader_cache_path = StringUtil::StdStringFromFormat(
    "%s" FS_OSPATH_SEPARATOR_STR "duckstation_cache" FS_OSPATH_SEPARATOR_STR, save_directory_ptr);
  if (!FileSystem::DirectoryExists(shader_cache_path.c_str()) &&
      !FileSystem::CreateDirectory(shader_cache_path.c_str(), false))
  {
    Log_ErrorPrintf("Failed to create shader cache directory: '%s'", shader_cache_path.c_str());
    return std::string();
  }

  Log_InfoPrintf("Shader cache directory: '%s'", shader_cache_path.c_str());
  return shader_cache_path;
}

std::string LibretroHostInterface::GetStringSettingValue(const char* section, const char* key,
                                                         const char* default_value /*= ""*/)
{
  TinyString name;
  name.Format("duckstation_%s.%s", section, key);
  retro_variable var{name, default_value};
  if (g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    return var.value;
  else
    return default_value;
}

void LibretroHostInterface::AddOSDMessage(std::string message, float duration /*= 2.0f*/)
{
  if (!g_settings.display_show_osd_messages)
    return;

  retro_message msg = {};
  msg.msg = message.c_str();
  msg.frames = static_cast<u32>(duration * (System::IsShutdown() ? 60.0f : System::GetThrottleFrequency()));
  g_retro_environment_callback(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

void LibretroHostInterface::retro_get_system_av_info(struct retro_system_av_info* info)
{
  const bool use_resolution_scale = (g_settings.gpu_renderer != GPURenderer::Software);
  GetSystemAVInfo(info, use_resolution_scale);

  Log_InfoPrintf("base = %ux%u, max = %ux%u, aspect ratio = %.2f, fps = %.2f", info->geometry.base_width,
                 info->geometry.base_height, info->geometry.max_width, info->geometry.max_height,
                 info->geometry.aspect_ratio, info->timing.fps);
}

void LibretroHostInterface::GetSystemAVInfo(struct retro_system_av_info* info, bool use_resolution_scale)
{
  const u32 resolution_scale = use_resolution_scale ? g_settings.gpu_resolution_scale : 1u;
  Assert(System::IsValid());

  std::memset(info, 0, sizeof(*info));

  info->geometry.aspect_ratio = m_last_aspect_ratio;
  info->geometry.base_width = 320;
  info->geometry.base_height = 240;
  info->geometry.max_width = GPU::VRAM_WIDTH * resolution_scale;
  info->geometry.max_height = GPU::VRAM_HEIGHT * resolution_scale;

  info->timing.fps = System::GetThrottleFrequency();
  info->timing.sample_rate = static_cast<double>(AUDIO_SAMPLE_RATE);
}

void LibretroHostInterface::UpdateSystemAVInfo(bool use_resolution_scale)
{
  struct retro_system_av_info avi;
  GetSystemAVInfo(&avi, use_resolution_scale);

  Log_InfoPrintf("base = %ux%u, max = %ux%u, aspect ratio = %.2f, fps = %.2f", avi.geometry.base_width,
                 avi.geometry.base_height, avi.geometry.max_width, avi.geometry.max_height, avi.geometry.aspect_ratio,
                 avi.timing.fps);

  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &avi))
    Log_ErrorPrintf("Failed to update system AV info on resolution change");
}

void LibretroHostInterface::UpdateGeometry()
{
  struct retro_system_av_info avi;
  const bool use_resolution_scale = (g_settings.gpu_renderer != GPURenderer::Software);
  GetSystemAVInfo(&avi, use_resolution_scale);

  Log_InfoPrintf("base = %ux%u, max = %ux%u, aspect ratio = %.2f", avi.geometry.base_width, avi.geometry.base_height,
                 avi.geometry.max_width, avi.geometry.max_height, avi.geometry.aspect_ratio);

  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_SET_GEOMETRY, &avi.geometry))
    Log_WarningPrint("RETRO_ENVIRONMENT_SET_GEOMETRY failed");
}

void LibretroHostInterface::UpdateLogging()
{
  Log::SetFilterLevel(g_settings.log_level);

  if (s_libretro_log_callback_valid)
    Log::SetConsoleOutputParams(false);
  else
    Log::SetConsoleOutputParams(true, nullptr, g_settings.log_level);
}

bool LibretroHostInterface::retro_load_game(const struct retro_game_info* game)
{
  SystemBootParameters bp;
  bp.filename = game->path;
  bp.media_playlist_index = m_next_disc_index.value_or(0);
  bp.force_software_renderer = !m_hw_render_callback_valid;

  struct retro_input_descriptor desc[] = {
#define JOYP(port)                                                                                                     \
  {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},                                           \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},                                             \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},                                         \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},                                       \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross"},                                                 \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle"},                                                \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle"},                                              \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square"},                                                \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L1"},                                                    \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"},                                                   \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"},                                                   \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R1"},                                                    \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"},                                                   \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3"},                                                   \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},                                           \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},                                             \
    {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X"},            \
    {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y"},            \
    {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X"},          \
    {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y"},

    JOYP(0) JOYP(1) JOYP(2) JOYP(3) JOYP(4) JOYP(5) JOYP(6) JOYP(7)

      {0},
  };

  g_retro_environment_callback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

  if (!BootSystem(bp))
    return false;

  if (g_settings.gpu_renderer != GPURenderer::Software)
  {
    if (!m_hw_render_callback_valid)
      RequestHardwareRendererContext();
    else
      SwitchToHardwareRenderer();
  }

  return true;
}

void LibretroHostInterface::retro_run_frame()
{
  Assert(!System::IsShutdown());

  if (HasCoreVariablesChanged())
    UpdateSettings();

  UpdateControllers();

  System::RunFrame();

  const float aspect_ratio = m_display->GetDisplayAspectRatio();
  if (aspect_ratio != m_last_aspect_ratio)
  {
    m_last_aspect_ratio = aspect_ratio;
    UpdateGeometry();
  }

  m_display->Render();
}

unsigned LibretroHostInterface::retro_get_region()
{
  return System::IsPALRegion() ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

size_t LibretroHostInterface::retro_serialize_size()
{
  return System::MAX_SAVE_STATE_SIZE;
}

bool LibretroHostInterface::retro_serialize(void* data, size_t size)
{
  std::unique_ptr<ByteStream> stream = ByteStream_CreateMemoryStream(data, static_cast<u32>(size));
  if (!System::SaveState(stream.get(), 0))
  {
    Log_ErrorPrintf("Failed to save state to memory stream");
    return false;
  }

  return true;
}

bool LibretroHostInterface::retro_unserialize(const void* data, size_t size)
{
  std::unique_ptr<ByteStream> stream = ByteStream_CreateReadOnlyMemoryStream(data, static_cast<u32>(size));
  if (!System::LoadState(stream.get()))
  {
    Log_ErrorPrintf("Failed to load save state from memory stream");
    return false;
  }

  return true;
}

void* LibretroHostInterface::retro_get_memory_data(unsigned id)
{
  switch (id)
  {
    case RETRO_MEMORY_SYSTEM_RAM:
      return System::IsShutdown() ? nullptr : Bus::g_ram;

    default:
      return nullptr;
  }
}

size_t LibretroHostInterface::retro_get_memory_size(unsigned id)
{
  switch (id)
  {
    case RETRO_MEMORY_SYSTEM_RAM:
      return Bus::RAM_SIZE;

    default:
      return 0;
  }
}

void LibretroHostInterface::retro_cheat_reset()
{
  System::SetCheatList(nullptr);
}

void LibretroHostInterface::retro_cheat_set(unsigned index, bool enabled, const char* code)
{
  CheatList* cl = System::GetCheatList();
  if (!cl)
  {
    System::SetCheatList(std::make_unique<CheatList>());
    cl = System::GetCheatList();
  }

  CheatCode cc;
  cc.description = StringUtil::StdStringFromFormat("Cheat%u", index);
  cc.enabled = true;
  if (!CheatList::ParseLibretroCheat(&cc, code))
    Log_ErrorPrintf("Failed to parse cheat %u '%s'", index, code);

  cl->SetCode(index, std::move(cc));
}

bool LibretroHostInterface::AcquireHostDisplay()
{
  // start in software mode, switch to hardware later
  m_display = std::make_unique<LibretroHostDisplay>();
  return true;
}

void LibretroHostInterface::ReleaseHostDisplay()
{
  if (m_hw_render_display)
  {
    m_hw_render_display->DestroyRenderDevice();
    m_hw_render_display.reset();
  }

  m_display->DestroyRenderDevice();
  m_display.reset();
}

std::unique_ptr<AudioStream> LibretroHostInterface::CreateAudioStream(AudioBackend backend)
{
  return std::make_unique<LibretroAudioStream>();
}

void LibretroHostInterface::OnSystemDestroyed()
{
  HostInterface::OnSystemDestroyed();
  m_using_hardware_renderer = false;
}

static std::array<retro_core_option_definition, 42> s_option_definitions = {{
  {"duckstation_Console.Region",
   "Console Region",
   "Determines which region/hardware to emulate. Auto-Detect will use the region of the disc inserted.",
   {{"Auto", "Auto-Detect"},
    {"NTSC-J", "NTSC-J (Japan)"},
    {"NTSC-U", "NTSC-U/C (US, Canada)"},
    {"PAL", "PAL (Europe, Australia)"}},
   "Auto"},
  {"duckstation_BIOS.PatchFastBoot",
   "Fast Boot",
   "Skips the BIOS shell/intro, booting directly into the game. Usually safe to enable, but some games break.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_CDROM.RegionCheck",
   "CD-ROM Region Check",
   "Prevents discs from incorrect regions being read by the emulator. Usually safe to disable.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_CDROM.ReadThread",
   "CD-ROM Read Thread",
   "Reads CD-ROM sectors ahead asynchronously, reducing the risk of frame time spikes.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {"duckstation_CDROM.LoadImageToRAM",
   "Preload CD-ROM Image To RAM",
   "Loads the disc image to RAM before starting emulation. May reduce hitching if you are running off a network share, "
   "at a cost of a greater startup time. As libretro provides no way to draw overlays, the emulator will appear to "
   "lock up while the image is preloaded.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_CDROM.MuteCDAudio",
   "Mute CD Audio",
   "Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to disable background music in some games.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_CDROM.ReadSpeedup",
   "CD-ROM Read Speedup",
   "Speeds up CD-ROM reads by the specified factor. Only applies to double-speed reads, and is ignored when audio "
   "is playing. May improve loading speeds in some games, at the cost of breaking others.",
   {{"1", "None (Double Speed)"},
    {"2", "2x (Quad Speed)"},
    {"3", "3x (6x Speed)"},
    {"4", "4x (8x Speed)"},
    {"5", "5x (10x Speed)"},
    {"6", "6x (12x Speed)"},
    {"7", "7x (14x Speed)"},
    {"8", "8x (16x Speed)"},
    {"9", "9x (18x Speed)"},
    {"10", "10x (20x Speed)"}},
   "1"},
  {"duckstation_CPU.ExecutionMode",
   "CPU Execution Mode",
   "Which mode to use for CPU emulation. Recompiler provides the best performance.",
   {{"Interpreter", "Interpreter"}, {"CachedIntepreter", "Cached Interpreter"}, {"Recompiler", "Recompiler"}},
   "Recompiler"},
  {"duckstation_CPU.Overclock",
   "CPU Overclocking",
   "Runs the emulated CPU faster or slower than native speed, which can improve framerates in some games. Will break "
   "other games and increase system requirements, use with caution.",
   {{"25", "25%"},   {"50", "50%"},   {"100", "100% (Default)"}, {"125", "125%"}, {"150", "150%"},
    {"175", "175%"}, {"200", "200%"}, {"225", "225%"},           {"250", "250%"}, {"275", "275%"},
    {"300", "300%"}, {"350", "350%"}, {"400", "400%"},           {"450", "450%"}, {"500", "500%"},
    {"600", "600%"}, {"700", "700%"}, {"800", "800%"},           {"900", "900%"}, {"1000", "1000%"}},
   "100"},
  {"duckstation_GPU.Renderer",
   "GPU Renderer",
   "Which renderer to use to emulate the GPU",
   {{"Auto", "Hardware (Auto)"},
#ifdef WIN32
    {"D3D11", "Hardware (D3D11)"},
#endif
    {"OpenGL", "Hardware (OpenGL)"},
    {"Vulkan", "Hardware (Vulkan)"},
    {"Software", "Software"}},
#ifdef WIN32
   "D3D11"
#else
   "OpenGL"
#endif
  },
  {"duckstation_GPU.ResolutionScale",
   "Internal Resolution Scale",
   "Scales internal VRAM resolution by the specified multiplier. Larger values are slower. Some games require "
   "1x VRAM resolution or they will have rendering issues.",
   {{"1", "1x"},
    {"2", "2x"},
    {"3", "3x (for 720p)"},
    {"4", "4x"},
    {"5", "5x (for 1080p)"},
    {"6", "6x (for 1440p)"},
    {"7", "7x"},
    {"8", "8x"},
    {"9", "9x (for 4K)"},
    {"10", "10x"},
    {"11", "11x"},
    {"12", "12x"},
    {"13", "13x"},
    {"14", "14x"},
    {"15", "15x"},
    {"16", "16x"}},
   "1"},
  {"duckstation_GPU.MSAA",
   "Multisample Antialiasing",
   "Uses multisample antialiasing for rendering 3D objects. Can smooth out jagged edges on polygons at a lower "
   "cost to performance compared to increasing the resolution scale, but may be more likely to cause rendering "
   "errors in some games.",
   {{"1", "Disabled"},
    {"2", "2x MSAA"},
    {"4", "4x MSAA"},
    {"8", "8x MSAA"},
    {"16", "16x MSAA"},
    {"32", "32x MSAA"},
    {"2-ssaa", "2x SSAA"},
    {"4-ssaa", "4x SSAA"},
    {"8-ssaa", "8x SSAA"},
    {"16-ssaa", "16x SSAA"},
    {"32-ssaa", "32x SSAA"}},
   "1"},
  {"duckstation_GPU.TrueColor",
   "True Color Rendering",
   "Disables dithering and uses the full 8 bits per channel of color information. May break rendering in some games.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_GPU.ScaledDithering",
   "Scaled Dithering",
   "Scales the dithering pattern with the internal rendering resolution, making it less noticeable. Usually safe to "
   "enable.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {"duckstation_GPU.DisableInterlacing",
   "Disable Interlacing",
   "Disables interlaced rendering and display in the GPU. Some games can render in 480p this way, but others will "
   "break.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_GPU.ForceNTSCTimings",
   "Force NTSC Timings",
   "Forces PAL games to run at NTSC timings, i.e. 60hz. Some PAL games will run at their \"normal\" speeds, while "
   "others will break.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_Display.Force4_3For24Bit",
   "Force 4:3 For 24-Bit Display",
   "Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_GPU.ChromaSmoothing24Bit",
   "Chroma Smoothing For 24-Bit Display",
   "Smooths out blockyness between colour transitions in 24-bit content, usually FMVs. Only applies to the hardware "
   "renderers.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_GPU.TextureFilter",
   "Texture Filtering",
   "Smooths out the blockyness of magnified textures on 3D object by using bilinear filtering. Will have a "
   "greater effect on higher resolution scales. Only applies to the hardware renderers.",
   {{"Nearest", "Nearest-Neighbor"},
    {"Bilinear", "Bilinear"},
    {"BilinearBinAlpha", "Bilinear (No Edge Blending)"},
    {"JINC2", "JINC2"},
    {"JINC2BinAlpha", "JINC2 (No Edge Blending)"},
    {"xBR", "xBR"},
    {"xBRBinAlpha", "xBR (No Edge Blending)"}},
   "Nearest"},
  {"duckstation_GPU.WidescreenHack",
   "Widescreen Hack",
   "Increases the field of view from 4:3 to 16:9 in 3D games. For 2D games, or games which use pre-rendered "
   "backgrounds, this enhancement will not work as expected.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_GPU.PGXPEnable",
   "PGXP Geometry Correction",
   "Reduces \"wobbly\" polygons by attempting to preserve the fractional component through memory transfers. Only "
   "works with the hardware renderers, and may not be compatible with all games.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_GPU.PGXPCulling",
   "PGXP Culling Correction",
   "Increases the precision of polygon culling, reducing the number of holes in geometry. Requires geometry correction "
   "enabled.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {"duckstation_GPU.PGXPTextureCorrection",
   "PGXP Texture Correction",
   "Uses perspective-correct interpolation for texture coordinates and colors, straightening out warped textures. "
   "Requires geometry correction enabled.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {"duckstation_GPU.PGXPVertexCache",
   "PGXP Vertex Cache",
   "Uses screen coordinates as a fallback when tracking vertices through memory fails. May improve PGXP compatibility.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_GPU.PGXPCPU",
   "PGXP CPU Mode",
   "Tries to track vertex manipulation through the CPU. Some games require this option for PGXP to be effective. "
   "Very slow, and incompatible with the recompiler.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_Display.CropMode",
   "Crop Mode",
   "Changes how much of the image is cropped. Some games display garbage in the overscan area which is typically "
   "hidden.",
   {{"None", "None"}, {"Overscan", "Only Overscan Area"}, {"Borders", "All Borders"}},
   "Overscan"},
  {"duckstation_Display.AspectRatio",
   "Aspect Ratio",
   "Sets the core-provided aspect ratio.",
   {{"4:3", "4:3"}, {"16:9", "16:9"}, {"2:1", "2:1 (VRAM 1:1)"}, {"1:1", "1:1"}},
   "4:3"},
  {"duckstation_Main.LoadDevicesFromSaveStates",
   "Load Devices From Save States",
   "Sets whether the contents of devices and memory cards will be loaded when a save state is loaded.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_MemoryCards.Card1Type",
   "Memory Card 1 Type",
   "Sets the type of memory card for Slot 1.",
   {{"None", "No Memory Card"},
    {"Shared", "Shared Between All Games"},
    {"PerGame", "Separate Card Per Game (Game Code)"},
    {"PerGameTitle", "Separate Card Per Game (Game Title)"}},
   "PerGameTitle"},
  {"duckstation_MemoryCards.Card2Type",
   "Memory Card 2 Type",
   "Sets the type of memory card for Slot 2.",
   {{"None", "No Memory Card"},
    {"Shared", "Shared Between All Games"},
    {"PerGame", "Separate Card Per Game (Game Code)"},
    {"PerGameTitle", "Separate Card Per Game (Game Title)"}},
   "None"},
  {"duckstation_MemoryCards.UsePlaylistTitle",
   "Use Single Card For Playlist",
   "When using a playlist (m3u) and per-game (title) memory cards, a single memory card "
   "will be used for all discs. If unchecked, a separate card will be used for each disc.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {"duckstation_Controller1.Type",
   "Controller 1 Type",
   "Sets the type of controller for Slot 1.",
   {{"None", "None"},
    {"DigitalController", "Digital Controller"},
    {"AnalogController", "Analog Controller (DualShock)"},
    {"NamcoGunCon", "Namco GunCon"},
    {"PlayStationMouse", "PlayStation Mouse"},
    {"NeGcon", "NeGcon"}},
   "DigitalController"},
  {"duckstation_Controller1.AutoEnableAnalog",
   "Controller 1 Auto Analog Mode",
   "Automatically enables analog mode in supported controllers at start/reset.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_Controller1.AnalogDPadInDigitalMode",
   "Controller 1 Use Analog Sticks for D-Pad in Digital Mode",
   "Allows you to use the analog sticks to control the d-pad in digital mode, as well as the buttons.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_Controller2.Type",
   "Controller 2 Type",
   "Sets the type of controller for Slot 2.",
   {{"None", "None"},
    {"DigitalController", "Digital Controller"},
    {"AnalogController", "Analog Controller (DualShock)"},
    {"NamcoGunCon", "Namco GunCon"},
    {"PlayStationMouse", "PlayStation Mouse"},
    {"NeGcon", "NeGcon"}},
   "None"},
  {"duckstation_Controller2.AutoEnableAnalog",
   "Controller 2 Auto Analog Mode",
   "Automatically enables analog mode in supported controllers at start/reset.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_Controller2.AnalogDPadInDigitalMode",
   "Controller 2 Use Analog Sticks for D-Pad in Digital Mode",
   "Allows you to use the analog sticks to control the d-pad in digital mode, as well as the buttons.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_Display.ShowOSDMessages",
   "Display OSD Messages",
   "Shows on-screen messages generated by the core.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {"duckstation_Logging.LogLevel",
   "Log Level",
   "Sets the level of information logged by the core.",
   {{"None", "None"},
    {"Error", "Error"},
    {"Warning", "Warning"},
    {"Perf", "Performance"},
    {"Success", "Success"},
    {"Info", "Information"},
    {"Dev", "Developer"},
    {"Profile", "Profile"},
    {"Debug", "Debug"},
    {"Trace", "Trace"}},
   "Info"},
  {"duckstation_CPU.RecompilerICache",
   "CPU Recompiler ICache",
   "Determines whether the CPU's instruction cache is simulated in the recompiler. Improves accuracy at a small cost "
   "to performance. If games are running too fast, try enabling this option.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"duckstation_CPU.Fastmem",
   "CPU Recompiler Fast Memory Access",
   "Uses page faults to determine hardware memory accesses at runtime. Can provide a significant performance "
   "improvement in some games, but make the core more difficult to debug.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {},
}};

bool LibretroHostInterface::SetCoreOptions()
{
  unsigned options_version = 0;
  if (g_retro_environment_callback(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &options_version) &&
      options_version >= 1)
  {
    return g_retro_environment_callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, &s_option_definitions);
  }

  // use legacy options struct, which sucks. do we need to?
  return false;
}

bool LibretroHostInterface::HasCoreVariablesChanged()
{
  bool changed = false;
  return (g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) && changed);
}

std::string LibretroHostInterface::GetBIOSDirectory()
{
  // Assume BIOS files are located in system directory.
  const char* system_directory = nullptr;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory) || !system_directory)
    return GetProgramDirectoryRelativePath("system");
  else
    return system_directory;
}

void LibretroHostInterface::LoadSettings()
{
  LibretroSettingsInterface si;
  HostInterface::LoadSettings(si);

  // turn percentage into fraction for overclock
  const u32 overclock_percent = static_cast<u32>(std::max(si.GetIntValue("CPU", "Overclock", 100), 1));
  Settings::CPUOverclockPercentToFraction(overclock_percent, &g_settings.cpu_overclock_numerator,
                                          &g_settings.cpu_overclock_denominator);
  g_settings.cpu_overclock_enable = (overclock_percent != 100);
  g_settings.UpdateOverclockActive();

  // convert msaa settings
  const std::string msaa = si.GetStringValue("GPU", "MSAA", "1");
  g_settings.gpu_multisamples = StringUtil::FromChars<u32>(msaa).value_or(1);
  g_settings.gpu_per_sample_shading = StringUtil::EndsWith(msaa, "-ssaa");

  // Ensure we don't use the standalone memcard directory in shared mode.
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    g_settings.memory_card_paths[i] = GetSharedMemoryCardPath(i);
}

void LibretroHostInterface::UpdateSettings()
{
  Settings old_settings(std::move(g_settings));
  LoadSettings();
  FixIncompatibleSettings(false);

  if (g_settings.gpu_resolution_scale != old_settings.gpu_resolution_scale &&
      g_settings.gpu_renderer != GPURenderer::Software)
  {
    ReportMessage("Resolution changed, updating system AV info...");

    // this will probably recreate the device... so save the state first by switching to software
    if (m_using_hardware_renderer)
      SwitchToSoftwareRenderer();

    UpdateSystemAVInfo(true);

    if (!m_hw_render_callback_valid)
      RequestHardwareRendererContext();
    else if (!m_using_hardware_renderer)
      SwitchToHardwareRenderer();

    // Don't let the base class mess with the GPU.
    old_settings.gpu_resolution_scale = g_settings.gpu_resolution_scale;
  }

  if (g_settings.gpu_renderer != old_settings.gpu_renderer)
  {
    ReportFormattedMessage("Switch to %s renderer pending, please restart the core to apply.",
                           Settings::GetRendererDisplayName(g_settings.gpu_renderer));
    g_settings.gpu_renderer = old_settings.gpu_renderer;
  }

  CheckForSettingsChanges(old_settings);
}

void LibretroHostInterface::CheckForSettingsChanges(const Settings& old_settings)
{
  HostInterface::CheckForSettingsChanges(old_settings);

  if (g_settings.display_aspect_ratio != old_settings.display_aspect_ratio)
    UpdateGeometry();

  if (g_settings.log_level != old_settings.log_level)
    UpdateLogging();
}

void LibretroHostInterface::InitRumbleInterface()
{
  m_rumble_interface_valid = g_retro_environment_callback(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &m_rumble_interface);
}

void LibretroHostInterface::UpdateControllers()
{
  g_retro_input_poll_callback();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    switch (g_settings.controller_types[i])
    {
      case ControllerType::None:
        break;

      case ControllerType::DigitalController:
        UpdateControllersDigitalController(i);
        break;

      case ControllerType::AnalogController:
        UpdateControllersAnalogController(i);
        break;

      default:
        ReportFormattedError("Unhandled controller type '%s'.",
                             Settings::GetControllerTypeDisplayName(g_settings.controller_types[i]));
        break;
    }
  }
}

void LibretroHostInterface::UpdateControllersDigitalController(u32 index)
{
  DigitalController* controller = static_cast<DigitalController*>(System::GetController(index));
  DebugAssert(controller);

  static constexpr std::array<std::pair<DigitalController::Button, u32>, 14> mapping = {
    {{DigitalController::Button::Left, RETRO_DEVICE_ID_JOYPAD_LEFT},
     {DigitalController::Button::Right, RETRO_DEVICE_ID_JOYPAD_RIGHT},
     {DigitalController::Button::Up, RETRO_DEVICE_ID_JOYPAD_UP},
     {DigitalController::Button::Down, RETRO_DEVICE_ID_JOYPAD_DOWN},
     {DigitalController::Button::Circle, RETRO_DEVICE_ID_JOYPAD_A},
     {DigitalController::Button::Cross, RETRO_DEVICE_ID_JOYPAD_B},
     {DigitalController::Button::Triangle, RETRO_DEVICE_ID_JOYPAD_X},
     {DigitalController::Button::Square, RETRO_DEVICE_ID_JOYPAD_Y},
     {DigitalController::Button::Start, RETRO_DEVICE_ID_JOYPAD_START},
     {DigitalController::Button::Select, RETRO_DEVICE_ID_JOYPAD_SELECT},
     {DigitalController::Button::L1, RETRO_DEVICE_ID_JOYPAD_L},
     {DigitalController::Button::L2, RETRO_DEVICE_ID_JOYPAD_L2},
     {DigitalController::Button::R1, RETRO_DEVICE_ID_JOYPAD_R},
     {DigitalController::Button::R2, RETRO_DEVICE_ID_JOYPAD_R2}}};

  if (m_supports_input_bitmasks)
  {
    const u16 active = g_retro_input_state_callback(index, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
    for (const auto& it : mapping)
      controller->SetButtonState(it.first, (active & (static_cast<u16>(1u) << it.second)) != 0u);
  }
  else
  {
    for (const auto& it : mapping)
    {
      const int16_t state = g_retro_input_state_callback(index, RETRO_DEVICE_JOYPAD, 0, it.second);
      controller->SetButtonState(it.first, state != 0);
    }
  }
}

void LibretroHostInterface::UpdateControllersAnalogController(u32 index)
{
  AnalogController* controller = static_cast<AnalogController*>(System::GetController(index));
  DebugAssert(controller);

  static constexpr std::array<std::pair<AnalogController::Button, u32>, 16> button_mapping = {
    {{AnalogController::Button::Left, RETRO_DEVICE_ID_JOYPAD_LEFT},
     {AnalogController::Button::Right, RETRO_DEVICE_ID_JOYPAD_RIGHT},
     {AnalogController::Button::Up, RETRO_DEVICE_ID_JOYPAD_UP},
     {AnalogController::Button::Down, RETRO_DEVICE_ID_JOYPAD_DOWN},
     {AnalogController::Button::Circle, RETRO_DEVICE_ID_JOYPAD_A},
     {AnalogController::Button::Cross, RETRO_DEVICE_ID_JOYPAD_B},
     {AnalogController::Button::Triangle, RETRO_DEVICE_ID_JOYPAD_X},
     {AnalogController::Button::Square, RETRO_DEVICE_ID_JOYPAD_Y},
     {AnalogController::Button::Start, RETRO_DEVICE_ID_JOYPAD_START},
     {AnalogController::Button::Select, RETRO_DEVICE_ID_JOYPAD_SELECT},
     {AnalogController::Button::L1, RETRO_DEVICE_ID_JOYPAD_L},
     {AnalogController::Button::L2, RETRO_DEVICE_ID_JOYPAD_L2},
     {AnalogController::Button::L3, RETRO_DEVICE_ID_JOYPAD_L3},
     {AnalogController::Button::R1, RETRO_DEVICE_ID_JOYPAD_R},
     {AnalogController::Button::R2, RETRO_DEVICE_ID_JOYPAD_R2},
     {AnalogController::Button::R3, RETRO_DEVICE_ID_JOYPAD_R3}}};

  static constexpr std::array<std::pair<AnalogController::Axis, std::pair<u32, u32>>, 4> axis_mapping = {
    {{AnalogController::Axis::LeftX, {RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X}},
     {AnalogController::Axis::LeftY, {RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y}},
     {AnalogController::Axis::RightX, {RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X}},
     {AnalogController::Axis::RightY, {RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y}}}};

  if (m_supports_input_bitmasks)
  {
    const u16 active = g_retro_input_state_callback(index, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
    for (const auto& it : button_mapping)
      controller->SetButtonState(it.first, (active & (static_cast<u16>(1u) << it.second)) != 0u);
  }
  else
  {
    for (const auto& it : button_mapping)
    {
      const int16_t state = g_retro_input_state_callback(index, RETRO_DEVICE_JOYPAD, 0, it.second);
      controller->SetButtonState(it.first, state != 0);
    }
  }

  for (const auto& it : axis_mapping)
  {
    const int16_t state = g_retro_input_state_callback(index, RETRO_DEVICE_ANALOG, it.second.first, it.second.second);
    controller->SetAxisState(static_cast<s32>(it.first), std::clamp(static_cast<float>(state) / 32767.0f, -1.0f, 1.0f));
  }

  if (m_rumble_interface_valid)
  {
    const u16 strong = static_cast<u16>(static_cast<u32>(controller->GetVibrationMotorStrength(0) * 65535.0f));
    const u16 weak = static_cast<u16>(static_cast<u32>(controller->GetVibrationMotorStrength(1) * 65535.0f));
    m_rumble_interface.set_rumble_state(index, RETRO_RUMBLE_STRONG, strong);
    m_rumble_interface.set_rumble_state(index, RETRO_RUMBLE_WEAK, weak);
  }
}

static std::optional<GPURenderer> RetroHwContextToRenderer(retro_hw_context_type type)
{
  switch (type)
  {
    case RETRO_HW_CONTEXT_OPENGL:
    case RETRO_HW_CONTEXT_OPENGL_CORE:
    case RETRO_HW_CONTEXT_OPENGLES3:
    case RETRO_HW_CONTEXT_OPENGLES_VERSION:
      return GPURenderer::HardwareOpenGL;

    case RETRO_HW_CONTEXT_VULKAN:
      return GPURenderer::HardwareVulkan;

#ifdef WIN32
    case RETRO_HW_CONTEXT_DIRECT3D:
      return GPURenderer::HardwareD3D11;
#endif

    default:
      return std::nullopt;
  }
}

static std::optional<GPURenderer> RenderAPIToRenderer(HostDisplay::RenderAPI api)
{
  switch (api)
  {
    case HostDisplay::RenderAPI::OpenGL:
    case HostDisplay::RenderAPI::OpenGLES:
      return GPURenderer::HardwareOpenGL;

    case HostDisplay::RenderAPI::Vulkan:
      return GPURenderer::HardwareVulkan;

#ifdef WIN32
    case HostDisplay::RenderAPI::D3D11:
      return GPURenderer::HardwareD3D11;
#endif

    default:
      return std::nullopt;
  }
}

bool LibretroHostInterface::RequestHardwareRendererContext()
{
  retro_variable renderer_variable{"duckstation_GPU.Renderer",
                                   Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER)};
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &renderer_variable) || !renderer_variable.value)
    renderer_variable.value = Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER);

  GPURenderer renderer = Settings::ParseRendererName(renderer_variable.value).value_or(Settings::DEFAULT_GPU_RENDERER);
  unsigned preferred_renderer = 0;
  g_retro_environment_callback(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred_renderer);
  if (std::strcmp(renderer_variable.value, "Auto") == 0)
  {
    std::optional<GPURenderer> preferred_gpu_renderer =
      RetroHwContextToRenderer(static_cast<retro_hw_context_type>(preferred_renderer));
    if (preferred_gpu_renderer.has_value())
      renderer = preferred_gpu_renderer.value();
  }

  Log_InfoPrintf("Renderer = %s", Settings::GetRendererName(renderer));
  if (renderer == GPURenderer::Software)
  {
    m_hw_render_callback_valid = false;
    return false;
  }

  Log_InfoPrintf("Requesting hardware renderer context for %s", Settings::GetRendererName(renderer));

  m_hw_render_callback = {};
  m_hw_render_callback.context_reset = HardwareRendererContextReset;
  m_hw_render_callback.context_destroy = HardwareRendererContextDestroy;

  switch (renderer)
  {
#ifdef WIN32
    case GPURenderer::HardwareD3D11:
      m_hw_render_callback_valid = LibretroD3D11HostDisplay::RequestHardwareRendererContext(&m_hw_render_callback);
      break;
#endif

    case GPURenderer::HardwareVulkan:
      m_hw_render_callback_valid = LibretroVulkanHostDisplay::RequestHardwareRendererContext(&m_hw_render_callback);
      break;

    case GPURenderer::HardwareOpenGL:
    {
      const bool prefer_gles =
        (preferred_renderer == RETRO_HW_CONTEXT_OPENGLES2 || preferred_renderer == RETRO_HW_CONTEXT_OPENGLES_VERSION);
      m_hw_render_callback_valid =
        LibretroOpenGLHostDisplay::RequestHardwareRendererContext(&m_hw_render_callback, prefer_gles);
    }
    break;

    default:
      Log_ErrorPrintf("Unhandled renderer %s", Settings::GetRendererName(renderer));
      m_hw_render_callback_valid = false;
      break;
  }

  return m_hw_render_callback_valid;
}

void LibretroHostInterface::HardwareRendererContextReset()
{
  Log_InfoPrintf("Hardware context reset, type = %u",
                 static_cast<unsigned>(g_libretro_host_interface.m_hw_render_callback.context_type));

  g_libretro_host_interface.m_hw_render_callback_valid = true;
  g_libretro_host_interface.SwitchToHardwareRenderer();
}

void LibretroHostInterface::SwitchToHardwareRenderer()
{
  // use the existing device if we just resized the window
  std::optional<GPURenderer> renderer;
  std::unique_ptr<HostDisplay> display = std::move(m_hw_render_display);
  if (display)
  {
    Log_InfoPrintf("Using existing hardware display");
    renderer = RenderAPIToRenderer(display->GetRenderAPI());
    if (!display->CreateResources())
      Panic("Failed to recreate resources after reinit");
  }
  else
  {
    renderer = RetroHwContextToRenderer(m_hw_render_callback.context_type);
    if (!renderer.has_value())
    {
      Log_ErrorPrintf("Unknown context type %u", static_cast<unsigned>(m_hw_render_callback.context_type));
      return;
    }

    switch (renderer.value())
    {
      case GPURenderer::HardwareOpenGL:
        display = std::make_unique<LibretroOpenGLHostDisplay>();
        break;

      case GPURenderer::HardwareVulkan:
        display = std::make_unique<LibretroVulkanHostDisplay>();
        break;

#ifdef WIN32
      case GPURenderer::HardwareD3D11:
        display = std::make_unique<LibretroD3D11HostDisplay>();
        break;
#endif

      default:
        Log_ErrorPrintf("Unhandled renderer '%s'", Settings::GetRendererName(renderer.value()));
        return;
    }

    struct retro_system_av_info avi;
    g_libretro_host_interface.GetSystemAVInfo(&avi, true);

    WindowInfo wi;
    wi.type = WindowInfo::Type::Libretro;
    wi.display_connection = &g_libretro_host_interface.m_hw_render_callback;
    wi.surface_width = avi.geometry.base_width;
    wi.surface_height = avi.geometry.base_height;
    wi.surface_scale = 1.0f;
    if (!display || !display->CreateRenderDevice(wi, {}, g_settings.gpu_use_debug_device) ||
        !display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device))
    {
      Log_ErrorPrintf("Failed to create hardware host display");
      return;
    }
  }

  std::swap(display, g_libretro_host_interface.m_display);
  System::RecreateGPU(renderer.value());
  display->DestroyRenderDevice();
  m_using_hardware_renderer = true;
}

void LibretroHostInterface::HardwareRendererContextDestroy()
{
  // switch back to software
  if (g_libretro_host_interface.m_using_hardware_renderer)
  {
    Log_InfoPrintf("Lost hardware renderer context, switching to software renderer");
    g_libretro_host_interface.SwitchToSoftwareRenderer();
  }

  if (g_libretro_host_interface.m_hw_render_display)
  {
    g_libretro_host_interface.m_hw_render_display->DestroyRenderDevice();
    g_libretro_host_interface.m_hw_render_display.reset();
  }

  g_libretro_host_interface.m_hw_render_callback_valid = false;
}

void LibretroHostInterface::SwitchToSoftwareRenderer()
{
  // keep the hw renderer around in case we need it later
  if (m_using_hardware_renderer)
  {
    m_hw_render_display = std::move(m_display);
    m_hw_render_display->DestroyResources();
    m_using_hardware_renderer = false;
  }

  m_display = std::make_unique<LibretroHostDisplay>();
  System::RecreateGPU(GPURenderer::Software);
}

bool LibretroHostInterface::DiskControlSetEjectState(bool ejected)
{
  if (System::IsShutdown())
  {
    Log_ErrorPrintf("DiskControlSetEjectState() - no system");
    return false;
  }

  Log_DevPrintf("DiskControlSetEjectState(%u)", static_cast<unsigned>(ejected));

  if (ejected)
  {
    if (!System::HasMedia())
      return false;

    System::RemoveMedia();
    return true;
  }
  else
  {
    const u32 image_to_insert = P_THIS->m_next_disc_index.value_or(0);
    Log_DevPrintf("Inserting image %u", image_to_insert);
    return System::SwitchMediaFromPlaylist(image_to_insert);
  }
}

bool LibretroHostInterface::DiskControlGetEjectState()
{
  if (System::IsShutdown())
  {
    Log_ErrorPrintf("DiskControlGetEjectState() - no system");
    return false;
  }

  Log_DevPrintf("DiskControlGetEjectState() -> %u", static_cast<unsigned>(System::HasMedia()));
  return System::HasMedia();
}

unsigned LibretroHostInterface::DiskControlGetImageIndex()
{
  if (System::IsShutdown())
  {
    Log_ErrorPrintf("DiskControlGetImageIndex() - no system");
    return false;
  }

  const u32 index = P_THIS->m_next_disc_index.value_or(System::GetMediaPlaylistIndex());
  Log_DevPrintf("DiskControlGetImageIndex() -> %u", index);
  return index;
}

bool LibretroHostInterface::DiskControlSetImageIndex(unsigned index)
{
  if (System::IsShutdown())
  {
    Log_ErrorPrintf("DiskControlSetImageIndex() - no system");
    return false;
  }

  Log_DevPrintf("DiskControlSetImageIndex(%u)", index);

  if (index >= System::GetMediaPlaylistCount())
    return false;

  P_THIS->m_next_disc_index = index;
  return true;
}

unsigned LibretroHostInterface::DiskControlGetNumImages()
{
  if (System::IsShutdown())
  {
    Log_ErrorPrintf("DiskControlGetNumImages() - no system");
    return false;
  }

  Log_DevPrintf("DiskControlGetNumImages() -> %u", System::GetMediaPlaylistCount());
  return static_cast<unsigned>(System::GetMediaPlaylistCount());
}

bool LibretroHostInterface::DiskControlReplaceImageIndex(unsigned index, const retro_game_info* info)
{
  if (System::IsShutdown())
  {
    Log_ErrorPrintf("DiskControlReplaceImageIndex() - no system");
    return false;
  }

  Log_DevPrintf("DiskControlReplaceImageIndex(%u, %s)", index, info ? info->path : "null");
  if (info && info->path)
    return System::ReplaceMediaPathFromPlaylist(index, info->path);
  else
    return System::RemoveMediaPathFromPlaylist(index);
}

bool LibretroHostInterface::DiskControlAddImageIndex()
{
  if (System::IsShutdown())
  {
    Log_ErrorPrintf("DiskControlAddImageIndex() - no system");
    return false;
  }

  Log_DevPrintf("DiskControlAddImageIndex() -> %zu", System::GetMediaPlaylistCount());
  System::AddMediaPathToPlaylist({});
  return true;
}

bool LibretroHostInterface::DiskControlSetInitialImage(unsigned index, const char* path)
{
  Log_DevPrintf("DiskControlSetInitialImage(%u, %s)", index, path);
  P_THIS->m_next_disc_index = index;
  return true;
}

bool LibretroHostInterface::DiskControlGetImagePath(unsigned index, char* path, size_t len)
{
  if (System::IsShutdown() || index >= System::GetMediaPlaylistCount())
    return false;

  const std::string& image_path = System::GetMediaPlaylistPath(index);
  Log_DevPrintf("DiskControlGetImagePath(%u) -> %s", index, image_path.c_str());
  if (image_path.empty())
    return false;

  StringUtil::Strlcpy(path, image_path.c_str(), len);
  return true;
}

bool LibretroHostInterface::DiskControlGetImageLabel(unsigned index, char* label, size_t len)
{
  if (System::IsShutdown() || index >= System::GetMediaPlaylistCount())
    return false;

  const std::string& image_path = System::GetMediaPlaylistPath(index);
  if (image_path.empty())
    return false;

  const std::string_view title = System::GetTitleForPath(label);
  StringUtil::Strlcpy(label, title, len);
  Log_DevPrintf("DiskControlGetImagePath(%u) -> %s", index, label);
  return true;
}

void LibretroHostInterface::InitDiskControlInterface()
{
  unsigned version = 0;
  if (g_retro_environment_callback(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &version) && version >= 1)
  {
    retro_disk_control_ext_callback ext_cb = {
      &LibretroHostInterface::DiskControlSetEjectState, &LibretroHostInterface::DiskControlGetEjectState,
      &LibretroHostInterface::DiskControlGetImageIndex, &LibretroHostInterface::DiskControlSetImageIndex,
      &LibretroHostInterface::DiskControlGetNumImages,  &LibretroHostInterface::DiskControlReplaceImageIndex,
      &LibretroHostInterface::DiskControlAddImageIndex, &LibretroHostInterface::DiskControlSetInitialImage,
      &LibretroHostInterface::DiskControlGetImagePath,  &LibretroHostInterface::DiskControlGetImageLabel};
    if (g_retro_environment_callback(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &ext_cb))
      return;
  }

  retro_disk_control_callback cb = {
    &LibretroHostInterface::DiskControlSetEjectState, &LibretroHostInterface::DiskControlGetEjectState,
    &LibretroHostInterface::DiskControlGetImageIndex, &LibretroHostInterface::DiskControlSetImageIndex,
    &LibretroHostInterface::DiskControlGetNumImages,  &LibretroHostInterface::DiskControlReplaceImageIndex,
    &LibretroHostInterface::DiskControlAddImageIndex};
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &cb))
    Log_WarningPrint("Failed to set disk control interface");
}

#include "libretro_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/analog_controller.h"
#include "core/bus.h"
#include "core/digital_controller.h"
#include "core/game_list.h"
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
  UpdateLogging();
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
  *title = GameList::GetTitleForPath(path);
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
  return StringUtil::StdStringFromFormat("%s%cshared_card_%d.mcd", GetSaveDirectory(), FS_OSPATH_SEPERATOR_CHARACTER,
                                         slot + 1);
}

std::string LibretroHostInterface::GetGameMemoryCardPath(const char* game_code, u32 slot) const
{
  return StringUtil::StdStringFromFormat("%s%c%s_%d.mcd", GetSaveDirectory(), FS_OSPATH_SEPERATOR_CHARACTER, game_code,
                                         slot + 1);
}

std::string LibretroHostInterface::GetShaderCacheBasePath() const
{
  // TODO: Is there somewhere we can save our shaders?
  Log_WarningPrint("No shader cache directory available, startup will be slower.");
  return std::string();
}

std::string LibretroHostInterface::GetSettingValue(const char* section, const char* key,
                                                   const char* default_value /*= ""*/)
{
  TinyString name;
  name.Format("%s.%s", section, key);
  retro_variable var{name, default_value};
  if (g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    return var.value;
  else
    return default_value;
}

void LibretroHostInterface::AddOSDMessage(std::string message, float duration /*= 2.0f*/)
{
  retro_message msg = {};
  msg.msg = message.c_str();
  msg.frames = static_cast<u32>(duration * (m_system ? m_system->GetThrottleFrequency() : 60.0f));
  g_retro_environment_callback(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

void LibretroHostInterface::retro_get_system_av_info(struct retro_system_av_info* info)
{
  const bool use_resolution_scale = (m_settings.gpu_renderer != GPURenderer::Software);
  GetSystemAVInfo(info, use_resolution_scale);

  Log_InfoPrintf("base = %ux%u, max = %ux%u, aspect ratio = %.2f, fps = %.2f", info->geometry.base_width,
                 info->geometry.base_height, info->geometry.max_width, info->geometry.max_height,
                 info->geometry.aspect_ratio, info->timing.fps);
}

void LibretroHostInterface::GetSystemAVInfo(struct retro_system_av_info* info, bool use_resolution_scale)
{
  const u32 resolution_scale = use_resolution_scale ? m_settings.gpu_resolution_scale : 1u;
  Assert(m_system);

  std::memset(info, 0, sizeof(*info));

  info->geometry.aspect_ratio = Settings::GetDisplayAspectRatioValue(m_settings.display_aspect_ratio);
  info->geometry.base_width = 320;
  info->geometry.base_height = 240;
  info->geometry.max_width = GPU::VRAM_WIDTH * resolution_scale;
  info->geometry.max_height = GPU::VRAM_HEIGHT * resolution_scale;

  info->timing.fps = m_system->GetThrottleFrequency();
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
  const bool use_resolution_scale = (m_settings.gpu_renderer != GPURenderer::Software);
  GetSystemAVInfo(&avi, use_resolution_scale);

  Log_InfoPrintf("base = %ux%u, max = %ux%u, aspect ratio = %.2f", avi.geometry.base_width, avi.geometry.base_height,
                 avi.geometry.max_width, avi.geometry.max_height, avi.geometry.aspect_ratio);

  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_SET_GEOMETRY, &avi.geometry))
    Log_WarningPrint("RETRO_ENVIRONMENT_SET_GEOMETRY failed");
}

void LibretroHostInterface::UpdateLogging()
{
  Log::SetFilterLevel(m_settings.log_level);

  if (s_libretro_log_callback_valid)
    Log::SetConsoleOutputParams(false);
  else
    Log::SetConsoleOutputParams(true, nullptr, m_settings.log_level);
}

bool LibretroHostInterface::retro_load_game(const struct retro_game_info* game)
{
  SystemBootParameters bp;
  bp.filename = game->path;
  bp.force_software_renderer = !m_hw_render_callback_valid;

  if (!BootSystem(bp))
    return false;

  if (m_settings.gpu_renderer != GPURenderer::Software)
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
  Assert(m_system);

  if (HasCoreVariablesChanged())
    UpdateSettings();

  UpdateControllers();

  m_system->GetGPU()->RestoreGraphicsAPIState();

  m_system->RunFrame();

  m_system->GetGPU()->ResetGraphicsAPIState();

  m_display->Render();
}

unsigned LibretroHostInterface::retro_get_region()
{
  return m_system->IsPALRegion() ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

size_t LibretroHostInterface::retro_serialize_size()
{
  return System::MAX_SAVE_STATE_SIZE;
}

bool LibretroHostInterface::retro_serialize(void* data, size_t size)
{
  std::unique_ptr<ByteStream> stream = ByteStream_CreateMemoryStream(data, static_cast<u32>(size));
  if (!m_system->SaveState(stream.get(), 0))
  {
    Log_ErrorPrintf("Failed to save state to memory stream");
    return false;
  }

  return true;
}

bool LibretroHostInterface::retro_unserialize(const void* data, size_t size)
{
  std::unique_ptr<ByteStream> stream = ByteStream_CreateReadOnlyMemoryStream(data, static_cast<u32>(size));
  if (!m_system->LoadState(stream.get()))
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
      return m_system ? m_system->GetBus()->GetRAM() : nullptr;

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

bool LibretroHostInterface::AcquireHostDisplay()
{
  // start in software mode, switch to hardware later
  m_display = std::make_unique<LibretroHostDisplay>();
  return true;
}

void LibretroHostInterface::ReleaseHostDisplay()
{
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

static std::array<retro_core_option_definition, 22> s_option_definitions = {{
  {"Console.Region",
   "Console Region",
   "Determines which region/hardware to emulate. Auto-Detect will use the region of the disc inserted.",
   {{"Auto", "Auto-Detect"},
    {"NTSC-J", "NTSC-J (Japan)"},
    {"NTSC-U", "NTSC-U (US)"},
    {"PAL", "PAL (Europe, Australia)"}},
   "Auto"},
  {"BIOS.FastBoot",
   "Fast Boot",
   "Skips the BIOS shell/intro, booting directly into the game. Usually safe to enable, but some games break.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"CDROM.RegionCheck",
   "CD-ROM Region Check",
   "Prevents discs from incorrect regions being read by the emulator. Usually safe to disable.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"CDROM.ReadThread",
   "CD-ROM Read Thread",
   "Reads CD-ROM sectors ahead asynchronously, reducing the risk of frame time spikes.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {"CPU.ExecutionMode",
   "CPU Execution Mode",
   "Which mode to use for CPU emulation. Recompiler provides the best performance.",
   {{"Interpreter", "Interpreter"}, {"CachedIntepreter", "Cached Interpreter"}, {"Recompiler", "Recompiler"}},
   "Recompiler"},
  {"GPU.Renderer",
   "GPU Renderer",
   "Which renderer to use to emulate the GPU",
   {
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
  {"GPU.ResolutionScale",
   "Internal Resolution Scale",
   "Scales internal VRAM resolution by the specified multiplier. Larger values are slower. Some games require "
   "1x VRAM resolution or they will have rendering issues.",
   {{"1", "1x (1024x512 VRAM)"},
    {"2", "2x (2048x1024 VRAM)"},
    {"3", "3x (3072x1536 VRAM)"},
    {"4", "4x (4096x2048 VRAM)"},
    {"5", "5x (5120x2160 VRAM)"},
    {"6", "6x (6144x3072 VRAM)"},
    {"7", "7x (7168x3584 VRAM)"},
    {"8", "8x (8192x4096 VRAM)"},
    {"9", "9x (9216x4608 VRAM)"},
    {"10", "10x (10240x5120 VRAM)"},
    {"11", "11x (11264x5632 VRAM)"},
    {"12", "12x (12288x6144 VRAM)"},
    {"13", "13x (13312x6656 VRAM)"},
    {"14", "14x (14336x7168 VRAM)"},
    {"15", "15x (15360x7680 VRAM)"},
    {"16", "16x (16384x8192 VRAM)"}},
   "1"},
  {"GPU.TrueColor",
   "True Color Rendering",
   "Disables dithering and uses the full 8 bits per channel of color information. May break rendering in some games.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"GPU.ScaledDithering",
   "Scaled Dithering",
   "Scales the dithering pattern with the internal rendering resolution, making it less noticeable. Usually safe to "
   "enable.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "true"},
  {"GPU.DisableInterlacing",
   "Disable Interlacing",
   "Disables interlaced rendering and display in the GPU. Some games can render in 480p this way, but others will "
   "break.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"GPU.ForceNTSCTimings",
   "Force NTSC Timings",
   "Forces PAL games to run at NTSC timings, i.e. 60hz. Some PAL games will run at their \"normal\" speeds, while "
   "others will break.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"Display.CropMode",
   "Crop Mode",
   "Changes how much of the image is cropped. Some games display garbage in the overscan area which is typically "
   "hidden.",
   {{"None", "None"}, {"Overscan", "Only Overscan Area"}, {"Borders", "All Borders"}},
   "Overscan"},
  {"Display.AspectRatio",
   "Aspect Ratio",
   "Sets the core-provided aspect ratio.",
   {{"4:3", "4:3"}, {"16:9", "16:9"}, {"2:1", "2:1 (VRAM 1:1)"}, {"1:1", "1:1"}},
   "4:3"},
  {"MemoryCards.LoadFromSaveStates",
   "Load Memory Cards From Save States",
   "Sets whether the contents of memory cards will be loaded when a save state is loaded.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"MemoryCards.Card1Type",
   "Memory Card 1 Type",
   "Sets the type of memory card for Slot 1.",
   {{"None", "No Memory Card"},
    {"Shared", "Shared Between All Games"},
    {"PerGame", "Separate Card Per Game (Game Code)"},
    {"PerGameTitle", "Separate Card Per Game (Game Title)"}},
   "PerGameTitle"},
  {"MemoryCards.Card2Type",
   "Memory Card 2 Type",
   "Sets the type of memory card for Slot 2.",
   {{"None", "No Memory Card"},
    {"Shared", "Shared Between All Games"},
    {"PerGame", "Separate Card Per Game (Game Code)"},
    {"PerGameTitle", "Separate Card Per Game (Game Title)"}},
   "None"},
  {"Controller1.Type",
   "Controller 1 Type",
   "Sets the type of controller for Slot 1.",
   {{"None", "None"},
    {"DigitalController", "Digital Controller"},
    {"AnalogController", "Analog Controller (DualShock)"},
    {"NamcoGunCon", "Namco GunCon"},
    {"PlayStationMouse", "PlayStation Mouse"},
    {"NeGcon", "NeGcon"}},
   "DigitalController"},
  {"Controller1.AutoEnableAnalog",
   "Controller 1 Auto Analog Mode",
   "Automatically enables analog mode in supported controllers at start/reset.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"Controller2.Type",
   "Controller 2 Type",
   "Sets the type of controller for Slot 2.",
   {{"None", "None"},
    {"DigitalController", "Digital Controller"},
    {"AnalogController", "Analog Controller (DualShock)"},
    {"NamcoGunCon", "Namco GunCon"},
    {"PlayStationMouse", "PlayStation Mouse"},
    {"NeGcon", "NeGcon"}},
   "None"},
  {"Controller2.AutoEnableAnalog",
   "Controller 2 Auto Analog Mode",
   "Automatically enables analog mode in supported controllers at start/reset.",
   {{"true", "Enabled"}, {"false", "Disabled"}},
   "false"},
  {"Logging.LogLevel",
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

void LibretroHostInterface::LoadSettings()
{
  LibretroSettingsInterface si;
  m_settings.Load(si);

  // Assume BIOS files are located in system directory.
  const char* system_directory = nullptr;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory) || !system_directory)
    system_directory = "bios";
  m_settings.bios_path =
    StringUtil::StdStringFromFormat("%s%cscph1001.bin", system_directory, FS_OSPATH_SEPERATOR_CHARACTER);
}

void LibretroHostInterface::UpdateSettings()
{
  Settings old_settings(std::move(m_settings));
  LoadSettings();

  if (m_settings.gpu_resolution_scale != old_settings.gpu_resolution_scale &&
      m_settings.gpu_renderer != GPURenderer::Software)
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
    old_settings.gpu_resolution_scale = m_settings.gpu_resolution_scale;
  }

  if (m_settings.gpu_renderer != old_settings.gpu_renderer)
  {
    ReportFormattedMessage("Switch to %s renderer pending, please restart the core to apply.",
                           Settings::GetRendererDisplayName(m_settings.gpu_renderer));
    m_settings.gpu_renderer = old_settings.gpu_renderer;
  }

  CheckForSettingsChanges(old_settings);
}

void LibretroHostInterface::CheckForSettingsChanges(const Settings& old_settings)
{
  HostInterface::CheckForSettingsChanges(old_settings);

  if (m_settings.display_aspect_ratio != old_settings.display_aspect_ratio)
    UpdateGeometry();

  if (m_settings.log_level != old_settings.log_level)
    UpdateLogging();
}

void LibretroHostInterface::UpdateControllers()
{
  g_retro_input_poll_callback();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    switch (m_settings.controller_types[i])
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
                             Settings::GetControllerTypeDisplayName(m_settings.controller_types[i]));
        break;
    }
  }
}

void LibretroHostInterface::UpdateControllersDigitalController(u32 index)
{
  DigitalController* controller = static_cast<DigitalController*>(m_system->GetController(index));
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

  for (const auto& it : mapping)
  {
    const int16_t state = g_retro_input_state_callback(index, RETRO_DEVICE_JOYPAD, 0, it.second);
    controller->SetButtonState(it.first, state != 0);
  }
}

void LibretroHostInterface::UpdateControllersAnalogController(u32 index)
{
  AnalogController* controller = static_cast<AnalogController*>(m_system->GetController(index));
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

  for (const auto& it : button_mapping)
  {
    const int16_t state = g_retro_input_state_callback(index, RETRO_DEVICE_JOYPAD, 0, it.second);
    controller->SetButtonState(it.first, state != 0);
  }

  for (const auto& it : axis_mapping)
  {
    const int16_t state = g_retro_input_state_callback(index, RETRO_DEVICE_ANALOG, it.second.first, it.second.second);
    controller->SetAxisState(static_cast<s32>(it.first), std::clamp(static_cast<float>(state) / 32767.0f, -1.0f, 1.0f));
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
  GPURenderer renderer = Settings::DEFAULT_GPU_RENDERER;
  retro_variable renderer_variable{"GPU.Renderer", Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER)};
  if (g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &renderer_variable) && renderer_variable.value)
    renderer = Settings::ParseRendererName(renderer_variable.value).value_or(Settings::DEFAULT_GPU_RENDERER);

  Log_InfoPrintf("Renderer = %s", Settings::GetRendererName(renderer));

  unsigned preferred_renderer = 0;
  if (g_retro_environment_callback(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred_renderer))
  {
    std::optional<GPURenderer> preferred_gpu_renderer =
      RetroHwContextToRenderer(static_cast<retro_hw_context_type>(preferred_renderer));
    if (!preferred_gpu_renderer.has_value() || preferred_gpu_renderer.value() != renderer)
    {
      const char* preferred_name =
        preferred_gpu_renderer.has_value() ? Settings::GetRendererName(preferred_gpu_renderer.value()) : "Unknown";
      const char* config_name = Settings::GetRendererName(renderer);
      renderer = preferred_gpu_renderer.value_or(GPURenderer::Software);
      ReportFormattedError(
        "Mismatch between frontend's preferred GPU renderer '%s' and configured renderer '%s'. Please "
        "change your video driver to '%s'. Using '%s' renderer for now.",
        preferred_name, config_name, config_name, Settings::GetRendererName(renderer));
    }
  }

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
      m_hw_render_callback_valid = LibretroOpenGLHostDisplay::RequestHardwareRendererContext(&m_hw_render_callback);
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
    if (!display || !display->CreateRenderDevice(wi, {}, g_libretro_host_interface.m_settings.gpu_use_debug_device) ||
        !display->InitializeRenderDevice({}, m_settings.gpu_use_debug_device))
    {
      Log_ErrorPrintf("Failed to create hardware host display");
      return;
    }
  }

  std::swap(display, g_libretro_host_interface.m_display);
  g_libretro_host_interface.m_system->RecreateGPU(renderer.value());
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
    m_using_hardware_renderer = false;
  }

  m_display = std::make_unique<LibretroHostDisplay>();
  m_system->RecreateGPU(GPURenderer::Software);
}

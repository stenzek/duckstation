#include "libretro_host_interface.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/analog_controller.h"
#include "core/digital_controller.h"
#include "core/gpu.h"
#include "core/system.h"
#include "libretro_audio_stream.h"
#include "libretro_host_display.h"
#include "libretro_settings_interface.h"
#include "opengl_host_display.h"
#include <array>
#include <cstring>
#include <tuple>
#include <utility>
#include <vector>
Log_SetChannel(LibretroHostInterface);

#ifdef WIN32
#include "d3d11_host_display.h"
#endif

//////////////////////////////////////////////////////////////////////////
// TODO:
//  - Fix up D3D11
//  - Save states
//  - Expose the rest of the options
//  - Memory card and controller settings
//  - Better paths for memory cards/BIOS
//////////////////////////////////////////////////////////////////////////

LibretroHostInterface g_libretro_host_interface;

retro_environment_t g_retro_environment_callback;
retro_video_refresh_t g_retro_video_refresh_callback;
retro_audio_sample_t g_retro_audio_sample_callback;
retro_audio_sample_batch_t g_retro_audio_sample_batch_callback;
retro_input_poll_t g_retro_input_poll_callback;
retro_input_state_t g_retro_input_state_callback;

LibretroHostInterface::LibretroHostInterface() = default;

LibretroHostInterface::~LibretroHostInterface() = default;

bool LibretroHostInterface::Initialize()
{
  if (!HostInterface::Initialize())
    return false;

  LoadSettings();
  return true;
}

void LibretroHostInterface::Shutdown()
{
  HostInterface::Shutdown();
}

void LibretroHostInterface::ReportError(const char* message)
{
  Log_ErrorPrint(message);
}

void LibretroHostInterface::ReportMessage(const char* message)
{
  Log_InfoPrint(message);
}

bool LibretroHostInterface::ConfirmMessage(const char* message)
{
  Log_InfoPrintf("Confirm: %s", message);
  return false;
}

void LibretroHostInterface::retro_get_system_av_info(struct retro_system_av_info* info)
{
  Assert(m_system);

  std::memset(info, 0, sizeof(*info));

  info->geometry.aspect_ratio = Settings::GetDisplayAspectRatioValue(m_settings.display_aspect_ratio);

  if (!m_system->IsPALRegion())
  {
    info->geometry.base_width = 640;
    info->geometry.base_height = 480;
  }
  else
  {
    info->geometry.base_width = 720;
    info->geometry.base_height = 576;
  }

  info->geometry.max_width = 1024;
  info->geometry.max_height = 512;

  info->timing.fps = m_system->GetThrottleFrequency();
  info->timing.sample_rate = static_cast<double>(AUDIO_SAMPLE_RATE);
}

bool LibretroHostInterface::retro_load_game(const struct retro_game_info* game)
{
  SystemBootParameters bp;
  bp.filename = game->path;

  if (!BootSystem(bp))
    return false;

  RequestHardwareRendererContext();
  return true;
}

void LibretroHostInterface::retro_run_frame()
{
  Assert(m_system);

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

bool LibretroHostInterface::AcquireHostDisplay()
{
  // start in software mode, switch to hardware later
  m_display = new LibretroHostDisplay();
  return true;
}

void LibretroHostInterface::ReleaseHostDisplay()
{
  delete m_display;
  m_display = nullptr;
}

std::unique_ptr<AudioStream> LibretroHostInterface::CreateAudioStream(AudioBackend backend)
{
  return std::make_unique<LibretroAudioStream>();
}

static std::array<retro_core_option_definition, 14> s_option_definitions = {{
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
     {"Software", "Software"}},
   "OpenGL"},
  {"GPU.ResolutionScale",
   "Rendering Resolution Scale",
   "Scales internal rendering resolution by the specified multiplier. Larger values are slower. Some games require "
   "1x rendering resolution or they will have rendering issues.",
   {{"1", "1x (1024x512)"},
    {"2", "2x (2048x1024)"},
    {"3", "3x (3072x1536)"},
    {"4", "4x (4096x2048)"},
    {"5", "5x (5120x2160)"},
    {"6", "6x (6144x3072)"},
    {"7", "7x (7168x3584)"},
    {"8", "8x (8192x4096)"}},
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
   {{"4:3", "4:3"}, {"16:9", "16:9"}, {"1:1", "1:1"}},
   "4:3"},
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

void LibretroHostInterface::LoadSettings()
{
  LibretroSettingsInterface si;
  m_settings.Load(si);

  // Overrides
  m_settings.log_level = LOGLEVEL_DEV;
  m_settings.log_to_console = true;

  // start in software, switch later
  m_settings.gpu_renderer = GPURenderer::Software;

  // Assume BIOS files are located in system directory.
  const char* system_directory = nullptr;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory) || !system_directory)
    system_directory = "bios";
  m_settings.bios_path =
    StringUtil::StdStringFromFormat("%s%cscph1001.bin", system_directory, FS_OSPATH_SEPERATOR_CHARACTER);

  // TODOs - expose via config
  m_settings.controller_types[0] = ControllerType::DigitalController;
  m_settings.controller_types[1] = ControllerType::None;
  m_settings.memory_card_types[0] = MemoryCardType::None;
  m_settings.memory_card_types[1] = MemoryCardType::None;
}

void LibretroHostInterface::UpdateSettings()
{
  Settings old_settings(std::move(m_settings));
  LoadSettings();
  CheckForSettingsChanges(old_settings);
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

      default:
        Log_ErrorPrintf("Unhandled controller type '%s'",
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

bool LibretroHostInterface::RequestHardwareRendererContext()
{
  GPURenderer renderer = Settings::DEFAULT_GPU_RENDERER;
  retro_variable renderer_variable{"GPU.Renderer", "OpenGL"};
  if (g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &renderer_variable) && renderer_variable.value)
    renderer = Settings::ParseRendererName(renderer_variable.value).value_or(Settings::DEFAULT_GPU_RENDERER);

  if (renderer == GPURenderer::Software)
    return true;

  m_hw_render_callback = {};
  m_hw_render_callback.context_reset = HardwareRendererContextReset;
  m_hw_render_callback.context_destroy = HardwareRendererContextDestroy;

#ifdef WIN32
  if (renderer == GPURenderer::HardwareD3D11 && false)
    return D3D11HostDisplay::RequestHardwareRendererContext(&m_hw_render_callback);
#endif

  if (renderer == GPURenderer::HardwareOpenGL)
    return OpenGLHostDisplay::RequestHardwareRendererContext(&m_hw_render_callback);

  return false;
}

void LibretroHostInterface::HardwareRendererContextReset()
{
  Log_InfoPrintf("Hardware context reset, type = %u",
                 static_cast<unsigned>(g_libretro_host_interface.m_hw_render_callback.context_type));

  std::unique_ptr<HostDisplay> new_display = nullptr;
  GPURenderer new_renderer = GPURenderer::Software;

  switch (g_libretro_host_interface.m_hw_render_callback.context_type)
  {
    case RETRO_HW_CONTEXT_OPENGL:
    case RETRO_HW_CONTEXT_OPENGL_CORE:
    case RETRO_HW_CONTEXT_OPENGLES3:
    case RETRO_HW_CONTEXT_OPENGLES_VERSION:
      new_display = OpenGLHostDisplay::Create(g_libretro_host_interface.m_settings.gpu_use_debug_device);
      new_renderer = GPURenderer::HardwareOpenGL;
      break;

#ifdef WIN32
    case RETRO_HW_CONTEXT_DIRECT3D:
      new_display = D3D11HostDisplay::Create(g_libretro_host_interface.m_settings.gpu_use_debug_device);
      new_renderer = GPURenderer::HardwareD3D11;
      break;
#endif

    default:
      break;
  }

  if (!new_display)
  {
    Log_ErrorPrintf("Failed to create hardware host display");
    return;
  }

  HostDisplay* old_display = g_libretro_host_interface.m_display;
  g_libretro_host_interface.m_display = new_display.release();
  g_libretro_host_interface.m_settings.gpu_renderer = new_renderer;
  g_libretro_host_interface.m_system->RecreateGPU(new_renderer);
  delete old_display;
}

void LibretroHostInterface::HardwareRendererContextDestroy()
{
  // switch back to software
  HostDisplay* old_display = g_libretro_host_interface.m_display;
  g_libretro_host_interface.m_display = new LibretroHostDisplay();
  g_libretro_host_interface.m_settings.gpu_renderer = GPURenderer::Software;
  g_libretro_host_interface.m_system->RecreateGPU(GPURenderer::Software);
  delete old_display;
}

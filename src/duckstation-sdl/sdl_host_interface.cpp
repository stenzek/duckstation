#include "sdl_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "icon.h"
#include "imgui_impl_sdl.h"
#include "imgui_styles.h"
#include "opengl_host_display.h"
#include "sdl_audio_stream.h"
#include "sdl_settings_interface.h"
#include <cinttypes>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <nfd.h>
Log_SetChannel(SDLHostInterface);

#ifdef WIN32
#include "common/windows_headers.h"
#include "d3d11_host_display.h"
#include <mmsystem.h>
#endif

SDLHostInterface::SDLHostInterface()
{
  // Increase timer/sleep resolution since we use it for throttling.
#ifdef WIN32
  timeBeginPeriod(1);
#endif

  m_switch_gpu_renderer_event_id = SDL_RegisterEvents(1);
}

SDLHostInterface::~SDLHostInterface()
{
  CloseGameControllers();
  m_display.reset();
  ImGui::DestroyContext();

  if (m_window)
    SDL_DestroyWindow(m_window);

#ifdef WIN32
  timeEndPeriod(1);
#endif
}

bool SDLHostInterface::CreateSDLWindow()
{
  constexpr u32 DEFAULT_WINDOW_WIDTH = 900;
  constexpr u32 DEFAULT_WINDOW_HEIGHT = 700;

  // Create window.
  const u32 window_flags =
    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (UseOpenGLRenderer() ? SDL_WINDOW_OPENGL : 0);

  m_window = SDL_CreateWindow("DuckStation", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, DEFAULT_WINDOW_WIDTH,
                              DEFAULT_WINDOW_HEIGHT, window_flags);
  if (!m_window)
    return false;

  // Set window icon.
  SDL_Surface* icon_surface =
    SDL_CreateRGBSurfaceFrom(const_cast<unsigned int*>(WINDOW_ICON_DATA), WINDOW_ICON_WIDTH, WINDOW_ICON_HEIGHT, 32,
                             WINDOW_ICON_WIDTH * sizeof(u32), UINT32_C(0x000000FF), UINT32_C(0x0000FF00),
                             UINT32_C(0x00FF0000), UINT32_C(0xFF000000));
  if (icon_surface)
  {
    SDL_SetWindowIcon(m_window, icon_surface);
    SDL_FreeSurface(icon_surface);
  }

  return true;
}

void SDLHostInterface::DestroySDLWindow()
{
  SDL_DestroyWindow(m_window);
  m_window = nullptr;
}

bool SDLHostInterface::CreateDisplay()
{
  const bool debug_device = m_settings.gpu_use_debug_device;
#ifdef WIN32
  m_display = UseOpenGLRenderer() ? OpenGLHostDisplay::Create(m_window, debug_device) :
                                    D3D11HostDisplay::Create(m_window, debug_device);
#else
  m_display = OpenGLHostDisplay::Create(m_window, debug_device);
#endif

  if (!m_display)
    return false;

  m_display->SetDisplayLinearFiltering(m_settings.display_linear_filtering);

  m_app_icon_texture =
    m_display->CreateTexture(APP_ICON_WIDTH, APP_ICON_HEIGHT, APP_ICON_DATA, APP_ICON_WIDTH * sizeof(u32));
  if (!m_app_icon_texture)
    return false;

  return true;
}

void SDLHostInterface::DestroyDisplay()
{
  m_app_icon_texture.reset();
  m_display.reset();
}

void SDLHostInterface::CreateImGuiContext()
{
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont();
}

void SDLHostInterface::CreateAudioStream()
{
  switch (m_settings.audio_backend)
  {
    case AudioBackend::Null:
      m_audio_stream = AudioStream::CreateNullAudioStream();
      break;

    case AudioBackend::Cubeb:
      m_audio_stream = AudioStream::CreateCubebAudioStream();
      break;

    case AudioBackend::Default:
    default:
      m_audio_stream = std::make_unique<SDLAudioStream>();
      break;
  }

  if (!m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS))
  {
    ReportError("Failed to recreate audio stream, falling back to null");
    m_audio_stream.reset();
    m_audio_stream = AudioStream::CreateNullAudioStream();
    if (!m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS))
      Panic("Failed to reconfigure null audio stream");
  }
}

void SDLHostInterface::SaveSettings()
{
  SDLSettingsInterface si(GetSettingsFileName().c_str());
  m_settings.Save(si);
}

void SDLHostInterface::QueueSwitchGPURenderer()
{
  SDL_Event ev = {};
  ev.type = SDL_USEREVENT;
  ev.user.code = m_switch_gpu_renderer_event_id;
  SDL_PushEvent(&ev);
}

void SDLHostInterface::SwitchGPURenderer()
{
  // Due to the GPU class owning textures, we have to shut the system down.
  std::unique_ptr<ByteStream> stream;
  if (m_system)
  {
    stream = ByteStream_CreateGrowableMemoryStream(nullptr, 8 * 1024);
    if (!m_system->SaveState(stream.get()) || !stream->SeekAbsolute(0))
      ReportError("Failed to save state before GPU renderer switch");

    DestroySystem();
  }

  ImGui::EndFrame();
  DestroyDisplay();
  DestroySDLWindow();

  if (!CreateSDLWindow())
    Panic("Failed to recreate SDL window on GPU renderer switch");

  if (!CreateDisplay())
    Panic("Failed to recreate display on GPU renderer switch");

  ImGui::NewFrame();

  if (stream)
  {
    CreateSystem();
    if (!BootSystem(nullptr, nullptr) || !m_system->LoadState(stream.get()))
    {
      ReportError("Failed to load state after GPU renderer switch, resetting");
      m_system->Reset();
    }
  }

  UpdateFullscreen();
  if (m_system)
    m_system->ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::SwitchAudioBackend()
{
  m_audio_stream.reset();
  CreateAudioStream();

  if (m_system)
  {
    m_audio_stream->PauseOutput(false);
    UpdateSpeedLimiterState();
  }
}

void SDLHostInterface::UpdateFullscreen()
{
  SDL_SetWindowFullscreen(m_window, m_settings.display_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

  // We set the margin only in windowed mode, the menu bar is drawn on top in fullscreen.
  m_display->SetDisplayTopMargin(
    m_settings.display_fullscreen ? 0 : static_cast<int>(20.0f * ImGui::GetIO().DisplayFramebufferScale.x));
}

void SDLHostInterface::UpdateControllerMapping()
{
  UpdateKeyboardControllerMapping();
  UpdateControllerControllerMapping();
}

std::unique_ptr<SDLHostInterface> SDLHostInterface::Create(const char* filename /* = nullptr */,
                                                           const char* exp1_filename /* = nullptr */,
                                                           const char* save_state_filename /* = nullptr */)
{
  std::unique_ptr<SDLHostInterface> intf = std::make_unique<SDLHostInterface>();

  // Settings need to be loaded prior to creating the window for OpenGL bits.
  SDLSettingsInterface si(intf->GetSettingsFileName().c_str());
  intf->m_settings.Load(si);

  if (!intf->CreateSDLWindow())
  {
    Log_ErrorPrintf("Failed to create SDL window");
    return nullptr;
  }

  intf->CreateImGuiContext();
  if (!intf->CreateDisplay())
  {
    Log_ErrorPrintf("Failed to create host display");
    return nullptr;
  }

  intf->CreateAudioStream();

  ImGui::NewFrame();

  intf->UpdateSpeedLimiterState();

  const bool boot = (filename != nullptr || exp1_filename != nullptr || save_state_filename != nullptr);
  if (boot)
  {
    if (!intf->CreateSystem() || !intf->BootSystem(filename, exp1_filename))
      return nullptr;

    if (save_state_filename)
      intf->LoadState(save_state_filename);

    intf->UpdateControllerMapping();
  }

  intf->UpdateFullscreen();

  return intf;
}

std::string SDLHostInterface::GetSaveStateFilename(u32 index)
{
  return StringUtil::StdStringFromFormat("savestate_%u.bin", index);
}

void SDLHostInterface::ReportError(const char* message)
{
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DuckStation Error", message, m_window);
}

void SDLHostInterface::ReportMessage(const char* message)
{
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "DuckStation Information", message, m_window);
}

void SDLHostInterface::HandleSDLEvent(const SDL_Event* event)
{
  ImGui_ImplSDL2_ProcessEvent(event);

  switch (event->type)
  {
    case SDL_WINDOWEVENT:
    {
      if (event->window.event == SDL_WINDOWEVENT_RESIZED)
        m_display->WindowResized();
    }
    break;

    case SDL_QUIT:
      m_quit_request = true;
      break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
      if (!ImGui::GetIO().WantCaptureKeyboard)
        HandleSDLKeyEvent(event);
    }
    break;

    case SDL_CONTROLLERDEVICEADDED:
    {
      Log_InfoPrintf("Controller %d inserted", event->cdevice.which);
      OpenGameController(event->cdevice.which);
    }
    break;

    case SDL_CONTROLLERDEVICEREMOVED:
    {
      Log_InfoPrintf("Controller %d removed", event->cdevice.which);
      CloseGameController(event->cdevice.which);
    }
    break;

    case SDL_CONTROLLERAXISMOTION:
      HandleSDLControllerAxisEventForController(event);
      break;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    {
      if (event->type == SDL_CONTROLLERBUTTONDOWN && event->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK)
      {
        // focus the menu bar
        m_focus_main_menu_bar = true;
      }

      HandleSDLControllerButtonEventForController(event);
    }
    break;

    case SDL_USEREVENT:
    {
      if (static_cast<u32>(event->user.code) == m_switch_gpu_renderer_event_id)
        SwitchGPURenderer();
    }
    break;
  }
}

void SDLHostInterface::HandleSDLKeyEvent(const SDL_Event* event)
{
  const bool repeat = event->key.repeat != 0;
  if (!repeat && HandleSDLKeyEventForController(event))
    return;

  const bool pressed = (event->type == SDL_KEYDOWN);
  switch (event->key.keysym.scancode)
  {
    case SDL_SCANCODE_F1:
    case SDL_SCANCODE_F2:
    case SDL_SCANCODE_F3:
    case SDL_SCANCODE_F4:
    case SDL_SCANCODE_F5:
    case SDL_SCANCODE_F6:
    case SDL_SCANCODE_F7:
    case SDL_SCANCODE_F8:
    {
      if (!pressed)
      {
        const u32 index = event->key.keysym.scancode - SDL_SCANCODE_F1 + 1;
        if (event->key.keysym.mod & (KMOD_LSHIFT | KMOD_RSHIFT))
          DoSaveState(index);
        else
          DoLoadState(index);
      }
    }
    break;

    case SDL_SCANCODE_F11:
    {
      if (!pressed)
        DoToggleFullscreen();
    }
    break;

    case SDL_SCANCODE_TAB:
    {
      if (!repeat)
      {
        m_speed_limiter_temp_disabled = pressed;
        UpdateSpeedLimiterState();
      }
    }
    break;

    case SDL_SCANCODE_PAUSE:
    {
      if (pressed)
        DoTogglePause();
    }
    break;

    case SDL_SCANCODE_SPACE:
    {
      if (pressed)
        DoFrameStep();
    }
    break;

    case SDL_SCANCODE_HOME:
    {
      if (pressed && !repeat && m_system)
      {
        m_settings.speed_limiter_enabled = !m_settings.speed_limiter_enabled;
        UpdateSpeedLimiterState();
        AddOSDMessage(m_system->GetSettings().speed_limiter_enabled ? "Speed limiter enabled." :
                                                                      "Speed limiter disabled.");
      }
    }
    break;

    case SDL_SCANCODE_END:
    {
      if (pressed)
        ToggleSoftwareRendering();
    }
    break;

    case SDL_SCANCODE_PAGEUP:
    case SDL_SCANCODE_PAGEDOWN:
    {
      if (pressed)
        ModifyResolutionScale(event->key.keysym.scancode == SDL_SCANCODE_PAGEUP ? 1 : -1);
    }
    break;
  }
}

void SDLHostInterface::UpdateKeyboardControllerMapping()
{
  m_keyboard_button_mapping.fill(-1);

  const Controller* controller = m_system ? m_system->GetController(0) : nullptr;
  if (controller)
  {
#define SET_BUTTON_MAP(action, name)                                                                                   \
  m_keyboard_button_mapping[static_cast<int>(action)] = controller->GetButtonCodeByName(name).value_or(-1)

    SET_BUTTON_MAP(KeyboardControllerAction::Up, "Up");
    SET_BUTTON_MAP(KeyboardControllerAction::Down, "Down");
    SET_BUTTON_MAP(KeyboardControllerAction::Left, "Left");
    SET_BUTTON_MAP(KeyboardControllerAction::Right, "Right");
    SET_BUTTON_MAP(KeyboardControllerAction::Triangle, "Triangle");
    SET_BUTTON_MAP(KeyboardControllerAction::Cross, "Cross");
    SET_BUTTON_MAP(KeyboardControllerAction::Square, "Square");
    SET_BUTTON_MAP(KeyboardControllerAction::Circle, "Circle");
    SET_BUTTON_MAP(KeyboardControllerAction::L1, "L1");
    SET_BUTTON_MAP(KeyboardControllerAction::R1, "R1");
    SET_BUTTON_MAP(KeyboardControllerAction::L2, "L2");
    SET_BUTTON_MAP(KeyboardControllerAction::R2, "R2");
    SET_BUTTON_MAP(KeyboardControllerAction::Start, "Start");
    SET_BUTTON_MAP(KeyboardControllerAction::Select, "Select");

#undef SET_BUTTON_MAP
  }
}

bool SDLHostInterface::HandleSDLKeyEventForController(const SDL_Event* event)
{
  const bool pressed = (event->type == SDL_KEYDOWN);
  Controller* controller;

#define DO_ACTION(action)                                                                                              \
  if ((controller = m_system ? m_system->GetController(0) : nullptr) != nullptr &&                                     \
      m_keyboard_button_mapping[static_cast<int>(action)])                                                             \
  {                                                                                                                    \
    controller->SetButtonState(m_keyboard_button_mapping[static_cast<int>(action)], pressed);                          \
  }

  switch (event->key.keysym.scancode)
  {
    case SDL_SCANCODE_KP_8:
    case SDL_SCANCODE_I:
      DO_ACTION(KeyboardControllerAction::Triangle);
      return true;
    case SDL_SCANCODE_KP_2:
    case SDL_SCANCODE_K:
      DO_ACTION(KeyboardControllerAction::Cross);
      return true;
    case SDL_SCANCODE_KP_4:
    case SDL_SCANCODE_J:
      DO_ACTION(KeyboardControllerAction::Square);
      return true;
    case SDL_SCANCODE_KP_6:
    case SDL_SCANCODE_L:
      DO_ACTION(KeyboardControllerAction::Circle);
      return true;

    case SDL_SCANCODE_W:
    case SDL_SCANCODE_UP:
      DO_ACTION(KeyboardControllerAction::Up);
      return true;
    case SDL_SCANCODE_S:
    case SDL_SCANCODE_DOWN:
      DO_ACTION(KeyboardControllerAction::Down);
      return true;
    case SDL_SCANCODE_A:
    case SDL_SCANCODE_LEFT:
      DO_ACTION(KeyboardControllerAction::Left);
      return true;
    case SDL_SCANCODE_D:
    case SDL_SCANCODE_RIGHT:
      DO_ACTION(KeyboardControllerAction::Right);
      return true;

    case SDL_SCANCODE_Q:
      DO_ACTION(KeyboardControllerAction::L1);
      return true;
    case SDL_SCANCODE_E:
      DO_ACTION(KeyboardControllerAction::R1);
      return true;

    case SDL_SCANCODE_1:
      DO_ACTION(KeyboardControllerAction::L2);
      return true;
    case SDL_SCANCODE_3:
      DO_ACTION(KeyboardControllerAction::R2);
      return true;

    case SDL_SCANCODE_RETURN:
      DO_ACTION(KeyboardControllerAction::Start);
      return true;
    case SDL_SCANCODE_BACKSPACE:
      DO_ACTION(KeyboardControllerAction::Select);
      return true;

    default:
      break;
  }

#undef DO_ACTION

  return false;
}

bool SDLHostInterface::OpenGameController(int index)
{
  if (m_sdl_controllers.find(index) != m_sdl_controllers.end())
    CloseGameController(index);

  SDL_GameController* gcontroller = SDL_GameControllerOpen(index);
  if (!gcontroller)
  {
    Log_WarningPrintf("Failed to open controller %d", index);
    return false;
  }

  Log_InfoPrintf("Opened controller %d: %s", index, SDL_GameControllerName(gcontroller));

  ControllerData cd = {};
  cd.controller = gcontroller;

  SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gcontroller);
  if (joystick)
  {
    SDL_Haptic* haptic = SDL_HapticOpenFromJoystick(joystick);
    if (SDL_HapticRumbleSupported(haptic) && SDL_HapticRumbleInit(haptic) == 0)
      cd.haptic = haptic;
    else
      SDL_HapticClose(haptic);
  }

  if (cd.haptic)
    Log_InfoPrintf("Rumble is supported on '%s'", SDL_GameControllerName(gcontroller));
  else
    Log_WarningPrintf("Rumble is not supported on '%s'", SDL_GameControllerName(gcontroller));

  m_sdl_controllers.emplace(index, cd);
  return true;
}

void SDLHostInterface::CloseGameControllers()
{
  while (!m_sdl_controllers.empty())
    CloseGameController(m_sdl_controllers.begin()->first);
}

bool SDLHostInterface::CloseGameController(int index)
{
  auto it = m_sdl_controllers.find(index);
  if (it == m_sdl_controllers.end())
    return false;

  if (it->second.haptic)
    SDL_HapticClose(it->second.haptic);

  SDL_GameControllerClose(it->second.controller);
  return true;
}

void SDLHostInterface::UpdateControllerControllerMapping()
{
  m_controller_axis_mapping.fill(-1);
  m_controller_button_mapping.fill(-1);

  Controller* controller = m_system ? m_system->GetController(0) : nullptr;
  if (controller)
  {
#define SET_AXIS_MAP(axis, name) m_controller_axis_mapping[axis] = controller->GetAxisCodeByName(name).value_or(-1)
#define SET_BUTTON_MAP(button, name)                                                                                   \
  m_controller_button_mapping[button] = controller->GetButtonCodeByName(name).value_or(-1)

    SET_AXIS_MAP(SDL_CONTROLLER_AXIS_LEFTX, "LeftX");
    SET_AXIS_MAP(SDL_CONTROLLER_AXIS_LEFTY, "LeftY");
    SET_AXIS_MAP(SDL_CONTROLLER_AXIS_RIGHTX, "RightX");
    SET_AXIS_MAP(SDL_CONTROLLER_AXIS_RIGHTY, "RightY");
    SET_AXIS_MAP(SDL_CONTROLLER_AXIS_TRIGGERLEFT, "LeftTrigger");
    SET_AXIS_MAP(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, "RightTrigger");

    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_UP, "Up");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_DOWN, "Down");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_LEFT, "Left");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "Right");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_Y, "Triangle");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_A, "Cross");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_X, "Square");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_B, "Circle");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L1");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R1");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_START, "Start");
    SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_BACK, "Select");

#undef SET_AXIS_MAP
#undef SET_BUTTON_MAP
  }
}

void SDLHostInterface::HandleSDLControllerAxisEventForController(const SDL_Event* ev)
{
  // Log_DevPrintf("axis %d %d", ev->caxis.axis, ev->caxis.value);
  Controller* controller = m_system ? m_system->GetController(0) : nullptr;
  if (!controller)
    return;

  // proper axis mapping
  if (m_controller_axis_mapping[ev->caxis.axis] >= 0)
  {
    const float value = static_cast<float>(ev->caxis.value) / (ev->caxis.value < 0 ? 32768.0f : 32767.0f);
    controller->SetAxisState(m_controller_axis_mapping[ev->caxis.axis], value);
    return;
  }

  // axis-as-button mapping
  static constexpr int deadzone = 8192;
  const bool negative = (ev->caxis.value < 0);
  const bool active = (std::abs(ev->caxis.value) >= deadzone);

  // FIXME
  if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
  {
    auto button = controller->GetButtonCodeByName("L2");
    if (button)
      controller->SetButtonState(button.value(), active);
  }
  else if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
  {
    auto button = controller->GetButtonCodeByName("R2");
    if (button)
      controller->SetButtonState(button.value(), active);
  }
  else
  {
    SDL_GameControllerButton negative_button, positive_button;
    if (ev->caxis.axis & 1)
    {
      negative_button = SDL_CONTROLLER_BUTTON_DPAD_UP;
      positive_button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    }
    else
    {
      negative_button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
      positive_button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    }

    if (m_controller_button_mapping[negative_button] >= 0)
      controller->SetButtonState(m_controller_button_mapping[negative_button], negative && active);
    if (m_controller_button_mapping[positive_button] >= 0)
      controller->SetButtonState(m_controller_button_mapping[positive_button], !negative && active);
  }
}

void SDLHostInterface::HandleSDLControllerButtonEventForController(const SDL_Event* ev)
{
  // Log_DevPrintf("button %d %s", ev->cbutton.button, ev->cbutton.state == SDL_PRESSED ? "pressed" : "released");

  Controller* controller = m_system ? m_system->GetController(0) : nullptr;
  if (!controller)
    return;

  if (m_controller_button_mapping[ev->cbutton.button] >= 0)
    controller->SetButtonState(m_controller_button_mapping[ev->cbutton.button], ev->cbutton.state == SDL_PRESSED);
}

void SDLHostInterface::UpdateControllerRumble()
{
  for (auto& it : m_sdl_controllers)
  {
    ControllerData& cd = it.second;
    if (!cd.haptic)
      continue;

    float new_strength = 0.0f;
    if (m_system)
    {
      Controller* controller = m_system->GetController(cd.controller_index);
      if (controller)
      {
        const u32 motor_count = controller->GetVibrationMotorCount();
        for (u32 i = 0; i < motor_count; i++)
          new_strength = std::max(new_strength, controller->GetVibrationMotorStrength(i));
      }
    }

    if (cd.last_rumble_strength == new_strength)
      continue;

    if (new_strength > 0.01f)
      SDL_HapticRumblePlay(cd.haptic, new_strength, 100000);
    else
      SDL_HapticRumbleStop(cd.haptic);

    cd.last_rumble_strength = new_strength;
  }
}

void SDLHostInterface::DrawImGui()
{
  DrawMainMenuBar();

  if (m_system)
    DrawDebugWindows();
  else
    DrawPoweredOffWindow();

  if (m_settings_window_open)
    DrawSettingsWindow();

  if (m_about_window_open)
    DrawAboutWindow();

  DrawOSDMessages();

  ImGui::Render();
}

void SDLHostInterface::DrawMainMenuBar()
{
  // We skip drawing the menu bar if we're in fullscreen and the mouse pointer isn't in range.
  const float SHOW_THRESHOLD = 20.0f;
  if (m_settings.display_fullscreen &&
      ImGui::GetIO().MousePos.y >= (SHOW_THRESHOLD * ImGui::GetIO().DisplayFramebufferScale.x) &&
      !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
  {
    return;
  }

  if (!ImGui::BeginMainMenuBar())
    return;

  const bool system_enabled = static_cast<bool>(m_system);

  if (m_focus_main_menu_bar)
  {
    ImGui::OpenPopup("System");
    m_focus_main_menu_bar = false;
  }

  if (ImGui::BeginMenu("System"))
  {
    if (ImGui::MenuItem("Start Disc", nullptr, false, !system_enabled))
      DoStartDisc();
    if (ImGui::MenuItem("Start BIOS", nullptr, false, !system_enabled))
      DoStartBIOS();

    ImGui::Separator();

    if (ImGui::MenuItem("Power Off", nullptr, false, system_enabled))
      DoPowerOff();

    if (ImGui::MenuItem("Reset", nullptr, false, system_enabled))
      ResetSystem();

    if (ImGui::MenuItem("Pause", nullptr, m_paused, system_enabled))
      DoTogglePause();

    ImGui::Separator();

    if (ImGui::MenuItem("Change Disc", nullptr, false, system_enabled))
      DoChangeDisc();

    if (ImGui::MenuItem("Frame Step", nullptr, false, system_enabled))
      DoFrameStep();

    ImGui::Separator();

    if (ImGui::BeginMenu("Load State"))
    {
      for (u32 i = 1; i <= NUM_QUICK_SAVE_STATES; i++)
      {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "State %u", i);
        if (ImGui::MenuItem(buf))
          DoLoadState(i);
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Save State", system_enabled))
    {
      for (u32 i = 1; i <= NUM_QUICK_SAVE_STATES; i++)
      {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "State %u", i);
        if (ImGui::MenuItem(buf))
          DoSaveState(i);
      }
      ImGui::EndMenu();
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Exit"))
      m_quit_request = true;

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Settings"))
  {
    if (ImGui::MenuItem("Change Settings..."))
      m_settings_window_open = true;

    ImGui::Separator();

    DrawQuickSettingsMenu();
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Debug", system_enabled))
  {
    DrawDebugMenu();
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help"))
  {
    if (ImGui::MenuItem("GitHub Repository"))
    {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Add URL Opener", "https://github.com/stenzek/duckstation",
                               m_window);
    }

    ImGui::Separator();

    if (ImGui::MenuItem("About"))
      m_about_window_open = true;

    ImGui::EndMenu();
  }

  if (m_system)
  {
    if (!m_paused)
    {
      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - 210.0f);

      const float speed = m_system->GetEmulationSpeed();
      const u32 rounded_speed = static_cast<u32>(std::round(speed));
      if (speed < 90.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%u%%", rounded_speed);
      else if (speed < 110.0f)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%u%%", rounded_speed);
      else
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u%%", rounded_speed);

      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - 165.0f);
      ImGui::Text("FPS: %.2f", m_system->GetFPS());

      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - 80.0f);
      ImGui::Text("VPS: %.2f", m_system->GetVPS());
    }
    else
    {
      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - 50.0f);
      ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused");
    }
  }

  ImGui::EndMainMenuBar();
}

void SDLHostInterface::DrawQuickSettingsMenu()
{
  bool settings_changed = false;
  bool gpu_settings_changed = false;
  if (ImGui::MenuItem("Enable Speed Limiter", nullptr, &m_settings.speed_limiter_enabled))
  {
    settings_changed = true;
    UpdateSpeedLimiterState();
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("CPU Execution Mode"))
  {
    const CPUExecutionMode current = m_settings.cpu_execution_mode;
    for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        m_settings.cpu_execution_mode = static_cast<CPUExecutionMode>(i);
        settings_changed = true;
        if (m_system)
          m_system->SetCPUExecutionMode(m_settings.cpu_execution_mode);
      }
    }

    ImGui::EndMenu();
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("Renderer"))
  {
    const GPURenderer current = m_settings.gpu_renderer;
    for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        m_settings.gpu_renderer = static_cast<GPURenderer>(i);
        settings_changed = true;
        QueueSwitchGPURenderer();
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::MenuItem("Fullscreen", nullptr, &m_settings.display_fullscreen))
  {
    settings_changed = true;
    UpdateFullscreen();
  }

  if (ImGui::MenuItem("VSync", nullptr, &m_settings.video_sync_enabled))
  {
    settings_changed = true;
    UpdateSpeedLimiterState();
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("Resolution Scale"))
  {
    const u32 current_internal_resolution = m_settings.gpu_resolution_scale;
    for (u32 scale = 1; scale <= m_settings.max_gpu_resolution_scale; scale++)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux (%ux%u)", scale, scale * GPU::VRAM_WIDTH, scale * GPU::VRAM_HEIGHT);

      if (ImGui::MenuItem(buf, nullptr, current_internal_resolution == scale))
      {
        m_settings.gpu_resolution_scale = scale;
        gpu_settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  gpu_settings_changed |= ImGui::MenuItem("True (24-Bit) Color", nullptr, &m_settings.gpu_true_color);
  gpu_settings_changed |= ImGui::MenuItem("Texture Filtering", nullptr, &m_settings.gpu_texture_filtering);
  if (ImGui::MenuItem("Display Linear Filtering", nullptr, &m_settings.display_linear_filtering))
  {
    m_display->SetDisplayLinearFiltering(m_settings.display_linear_filtering);
    settings_changed = true;
  }

  if (settings_changed || gpu_settings_changed)
    SaveSettings();

  if (gpu_settings_changed && m_system)
    m_system->GetGPU()->UpdateSettings();
}

void SDLHostInterface::DrawDebugMenu()
{
  Settings::DebugSettings& debug_settings = m_settings.debugging;

  ImGui::MenuItem("Show System State");
  ImGui::Separator();

  ImGui::MenuItem("Show GPU State", nullptr, &debug_settings.show_gpu_state);
  ImGui::MenuItem("Show VRAM", nullptr, &debug_settings.show_vram);
  ImGui::MenuItem("Dump CPU to VRAM Copies", nullptr, &debug_settings.dump_cpu_to_vram_copies);
  ImGui::MenuItem("Dump VRAM to CPU Copies", nullptr, &debug_settings.dump_vram_to_cpu_copies);
  ImGui::Separator();

  ImGui::MenuItem("Show CDROM State", nullptr, &debug_settings.show_cdrom_state);
  ImGui::Separator();

  ImGui::MenuItem("Show SPU State", nullptr, &debug_settings.show_spu_state);
  ImGui::Separator();

  ImGui::MenuItem("Show Timers State", nullptr, &debug_settings.show_timers_state);
  ImGui::Separator();

  ImGui::MenuItem("Show MDEC State", nullptr, &debug_settings.show_mdec_state);
  ImGui::Separator();
}

void SDLHostInterface::DrawPoweredOffWindow()
{
  constexpr int WINDOW_WIDTH = 400;
  constexpr int WINDOW_HEIGHT = 650;
  constexpr int BUTTON_WIDTH = 200;
  constexpr int BUTTON_HEIGHT = 40;

  ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  if (!ImGui::Begin("Powered Off", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoBringToFrontOnFocus))
  {
    ImGui::End();
  }

  ImGui::SetCursorPosX((WINDOW_WIDTH - APP_ICON_WIDTH) / 2);
  ImGui::Image(m_app_icon_texture->GetHandle(), ImVec2(APP_ICON_WIDTH, APP_ICON_HEIGHT));
  ImGui::SetCursorPosY(APP_ICON_HEIGHT + 32);

  static const ImVec2 button_size(static_cast<float>(BUTTON_WIDTH), static_cast<float>(BUTTON_HEIGHT));
  constexpr float button_left = static_cast<float>((WINDOW_WIDTH - BUTTON_WIDTH) / 2);

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, 0xFF202020);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, 0xFF808080);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, 0xFF575757);

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Resume", button_size))
    DoResume();
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Start Disc", button_size))
    DoStartDisc();
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Start BIOS", button_size))
    DoStartBIOS();
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Load State", button_size))
    ImGui::OpenPopup("PowerOffWindow_LoadStateMenu");
  if (ImGui::BeginPopup("PowerOffWindow_LoadStateMenu"))
  {
    for (u32 i = 1; i <= NUM_QUICK_SAVE_STATES; i++)
    {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "State %u", i);
      if (ImGui::MenuItem(buf))
        DoLoadState(i);
    }
    ImGui::EndPopup();
  }
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Settings", button_size))
    m_settings_window_open = true;
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Exit", button_size))
    m_quit_request = true;

  ImGui::NewLine();

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);

  ImGui::End();
}

static bool DrawSettingsSectionHeader(const char* title)
{
  return ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen /* | ImGuiTreeNodeFlags_Leaf*/);
}

void SDLHostInterface::DrawSettingsWindow()
{
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("Settings", &m_settings_window_open, ImGuiWindowFlags_NoResize))
  {
    ImGui::End();
    return;
  }

  bool settings_changed = false;
  bool gpu_settings_changed = false;

  if (ImGui::BeginTabBar("SettingsTabBar", 0))
  {
    const float indent = 150.0f;

    if (ImGui::BeginTabItem("General"))
    {
      if (DrawSettingsSectionHeader("Console"))
      {
        ImGui::Text("Region:");
        ImGui::SameLine(indent);

        int region = static_cast<int>(m_settings.region);
        if (ImGui::Combo(
              "##region", &region,
              [](void*, int index, const char** out_text) {
                *out_text = Settings::GetConsoleRegionDisplayName(static_cast<ConsoleRegion>(index));
                return true;
              },
              nullptr, static_cast<int>(ConsoleRegion::Count)))
        {
          m_settings.region = static_cast<ConsoleRegion>(region);
          settings_changed = true;
        }

        ImGui::Text("BIOS Path:");
        ImGui::SameLine(indent);
        settings_changed |= DrawFileChooser("##bios_path", &m_settings.bios_path);

        settings_changed |= ImGui::Checkbox("Enable TTY Output", &m_settings.bios_patch_tty_enable);
        settings_changed |= ImGui::Checkbox("Fast Boot", &m_settings.bios_patch_fast_boot);
      }

      ImGui::NewLine();
      if (DrawSettingsSectionHeader("Behavior"))
      {
        if (ImGui::Checkbox("Enable Speed Limiter", &m_settings.speed_limiter_enabled))
        {
          settings_changed = true;
          UpdateSpeedLimiterState();
        }

        settings_changed |= ImGui::Checkbox("Pause On Start", &m_settings.start_paused);
      }

      ImGui::NewLine();
      if (DrawSettingsSectionHeader("Audio"))
      {
        ImGui::Text("Backend:");
        ImGui::SameLine(indent);

        int backend = static_cast<int>(m_settings.audio_backend);
        if (ImGui::Combo(
              "##backend", &backend,
              [](void*, int index, const char** out_text) {
                *out_text = Settings::GetAudioBackendDisplayName(static_cast<AudioBackend>(index));
                return true;
              },
              nullptr, static_cast<int>(AudioBackend::Count)))
        {
          m_settings.audio_backend = static_cast<AudioBackend>(backend);
          settings_changed = true;
          SwitchAudioBackend();
        }

        if (ImGui::Checkbox("Output Sync", &m_settings.audio_sync_enabled))
        {
          settings_changed = true;
          UpdateSpeedLimiterState();
        }
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Ports"))
    {
      for (int i = 0; i < 2; i++)
      {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Front Port %d", 1 + i);

        if (DrawSettingsSectionHeader(buf))
        {
          ImGui::Text("Controller:");
          ImGui::SameLine(indent);

          int controller_type = static_cast<int>(m_settings.controller_types[i]);
          if (ImGui::Combo(
                "##controller_type", &controller_type,
                [](void*, int index, const char** out_text) {
                  *out_text = Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(index));
                  return true;
                },
                nullptr, static_cast<int>(ControllerType::Count)))
          {
            m_settings.controller_types[i] = static_cast<ControllerType>(controller_type);
            settings_changed = true;
            if (m_system)
            {
              m_system->UpdateControllers();
              UpdateControllerControllerMapping();
            }
          }
        }

        ImGui::Text("Memory Card Path:");
        ImGui::SameLine(indent);

        std::string* path_ptr = &m_settings.memory_card_paths[i];
        std::snprintf(buf, sizeof(buf), "##memcard_%c_path", 'a' + i);
        if (DrawFileChooser(buf, path_ptr))
        {
          settings_changed = true;
          if (m_system)
            m_system->UpdateMemoryCards();
        }

        if (ImGui::Button("Eject Memory Card"))
        {
          path_ptr->clear();
          settings_changed = true;
          if (m_system)
            m_system->UpdateMemoryCards();
        }

        ImGui::NewLine();
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("CPU"))
    {
      ImGui::Text("Execution Mode:");
      ImGui::SameLine(indent);

      int execution_mode = static_cast<int>(m_settings.cpu_execution_mode);
      if (ImGui::Combo(
            "##execution_mode", &execution_mode,
            [](void*, int index, const char** out_text) {
              *out_text = Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(index));
              return true;
            },
            nullptr, static_cast<int>(CPUExecutionMode::Count)))
      {
        m_settings.cpu_execution_mode = static_cast<CPUExecutionMode>(execution_mode);
        settings_changed = true;
        if (m_system)
          m_system->SetCPUExecutionMode(m_settings.cpu_execution_mode);
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("GPU"))
    {
      if (DrawSettingsSectionHeader("Basic"))
      {
        ImGui::Text("Renderer:");
        ImGui::SameLine(indent);

        int gpu_renderer = static_cast<int>(m_settings.gpu_renderer);
        if (ImGui::Combo(
              "##gpu_renderer", &gpu_renderer,
              [](void*, int index, const char** out_text) {
                *out_text = Settings::GetRendererDisplayName(static_cast<GPURenderer>(index));
                return true;
              },
              nullptr, static_cast<int>(GPURenderer::Count)))
        {
          m_settings.gpu_renderer = static_cast<GPURenderer>(gpu_renderer);
          settings_changed = true;
          QueueSwitchGPURenderer();
        }
      }

      ImGui::NewLine();

      if (DrawSettingsSectionHeader("Display Output"))
      {
        if (ImGui::Checkbox("Fullscreen", &m_settings.display_fullscreen))
        {
          UpdateFullscreen();
          settings_changed = true;
        }

        if (ImGui::Checkbox("Linear Filtering", &m_settings.display_linear_filtering))
        {
          m_display->SetDisplayLinearFiltering(m_settings.display_linear_filtering);
          settings_changed = true;
        }

        if (ImGui::Checkbox("VSync", &m_settings.video_sync_enabled))
        {
          settings_changed = true;
          UpdateSpeedLimiterState();
        }
      }

      ImGui::NewLine();

      if (DrawSettingsSectionHeader("Enhancements"))
      {
        ImGui::Text("Resolution Scale:");
        ImGui::SameLine(indent);

        static constexpr std::array<const char*, 16> resolutions = {{
          "1x (1024x512)",
          "2x (2048x1024)",
          "3x (3072x1536)",
          "4x (4096x2048)",
          "5x (5120x2560)",
          "6x (6144x3072)",
          "7x (7168x3584)",
          "8x (8192x4096)",
          "9x (9216x4608)",
          "10x (10240x5120)",
          "11x (11264x5632)",
          "12x (12288x6144)",
          "13x (13312x6656)",
          "14x (14336x7168)",
          "15x (15360x7680)",
          "16x (16384x8192)",
        }};

        int current_resolution_index = static_cast<int>(m_settings.gpu_resolution_scale) - 1;
        if (ImGui::Combo("##gpu_resolution_scale", &current_resolution_index, resolutions.data(),
                         static_cast<int>(resolutions.size())))
        {
          m_settings.gpu_resolution_scale = static_cast<u32>(current_resolution_index + 1);
          gpu_settings_changed = true;
        }

        gpu_settings_changed |= ImGui::Checkbox("True 24-bit Color (disables dithering)", &m_settings.gpu_true_color);
        gpu_settings_changed |= ImGui::Checkbox("Texture Filtering", &m_settings.gpu_texture_filtering);
        gpu_settings_changed |= ImGui::Checkbox("Force Progressive Scan", &m_settings.gpu_force_progressive_scan);
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  const auto window_size = ImGui::GetWindowSize();
  ImGui::SetCursorPosX(window_size.x - 50.0f);
  ImGui::SetCursorPosY(window_size.y - 30.0f);
  if (ImGui::Button("Close"))
    m_settings_window_open = false;

  ImGui::End();

  if (settings_changed || gpu_settings_changed)
    SaveSettings();

  if (gpu_settings_changed && m_system)
    m_system->GetGPU()->UpdateSettings();
}

void SDLHostInterface::DrawAboutWindow()
{
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

  ImGui::OpenPopup("About DuckStation");
  if (!ImGui::BeginPopupModal("About DuckStation", &m_about_window_open, ImGuiWindowFlags_NoResize))
    return;

  ImGui::Text("DuckStation");
  ImGui::NewLine();
  ImGui::Text("Authors:");
  ImGui::Text("  Connor McLaughlin <stenzek@gmail.com>");
  ImGui::NewLine();
  ImGui::Text("Uses Dear ImGui (https://github.com/ocornut/imgui)");
  ImGui::Text("Uses libcue (https://github.com/lipnitsk/libcue)");
  ImGui::Text("Uses stb_image_write (https://github.com/nothings/stb)");
  ImGui::Text("Uses simpleini (https://github.com/brofield/simpleini)");
  ImGui::NewLine();
  ImGui::Text("Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)");

  ImGui::NewLine();

  ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 60.0f) / 2.0f);
  if (ImGui::Button("Close", ImVec2(60.0f, 20.0f)))
    m_about_window_open = false;

  ImGui::EndPopup();
}

bool SDLHostInterface::DrawFileChooser(const char* label, std::string* path, const char* filter /* = nullptr */)
{
  ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - 50.0f);
  bool result = ImGui::InputText(label, path);
  ImGui::SameLine();

  ImGui::SetNextItemWidth(50.0f);
  if (ImGui::Button("..."))
  {
    nfdchar_t* out_path = nullptr;
    if (NFD_OpenDialog(filter, path->c_str(), &out_path) == NFD_OKAY)
    {
      path->assign(out_path);
      result = true;
    }
  }

  return result;
}

void SDLHostInterface::DoPowerOff()
{
  Assert(m_system);
  DestroySystem();
  AddOSDMessage("System powered off.");
}

void SDLHostInterface::DoResume()
{
  Assert(!m_system);
  if (!CreateSystem() || !BootSystem(nullptr, RESUME_SAVESTATE_FILENAME))
  {
    DestroySystem();
    return;
  }

  UpdateControllerMapping();
  if (m_system)
    m_system->ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoStartDisc()
{
  Assert(!m_system);

  nfdchar_t* path = nullptr;
  if (!NFD_OpenDialog("bin,img,cue,chd,exe,psexe", nullptr, &path) || !path || std::strlen(path) == 0)
    return;

  AddFormattedOSDMessage(2.0f, "Starting disc from '%s'...", path);
  if (!CreateSystem() || !BootSystem(path, nullptr))
  {
    DestroySystem();
    return;
  }

  UpdateControllerMapping();
  if (m_system)
    m_system->ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoStartBIOS()
{
  Assert(!m_system);

  AddOSDMessage("Starting BIOS...");
  if (!CreateSystem() || !BootSystem(nullptr, nullptr))
  {
    DestroySystem();
    return;
  }

  UpdateControllerMapping();
  if (m_system)
    m_system->ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoChangeDisc()
{
  Assert(m_system);

  nfdchar_t* path = nullptr;
  if (!NFD_OpenDialog("bin,img,cue,chd,exe,psexe", nullptr, &path) || !path || std::strlen(path) == 0)
    return;

  if (m_system->InsertMedia(path))
    AddFormattedOSDMessage(2.0f, "Switched CD to '%s'", path);
  else
    AddOSDMessage("Failed to switch CD. The log may contain further information.");

  if (m_system)
    m_system->ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoLoadState(u32 index)
{
  if (HasSystem())
  {
    LoadState(GetSaveStateFilename(index).c_str());
  }
  else
  {
    if (!CreateSystem() || !BootSystem(nullptr, GetSaveStateFilename(index).c_str()))
    {
      DestroySystem();
      return;
    }
  }

  UpdateControllerMapping();
  if (m_system)
    m_system->ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoSaveState(u32 index)
{
  Assert(m_system);
  SaveState(GetSaveStateFilename(index).c_str());
  ClearImGuiFocus();
}

void SDLHostInterface::DoTogglePause()
{
  if (!m_system)
    return;

  m_paused = !m_paused;
  if (!m_paused)
    m_system->ResetPerformanceCounters();
}

void SDLHostInterface::DoFrameStep()
{
  if (!m_system)
    return;

  m_frame_step_request = true;
  m_paused = false;
}

void SDLHostInterface::DoToggleFullscreen()
{
  m_settings.display_fullscreen = !m_settings.display_fullscreen;
  UpdateFullscreen();
}

void SDLHostInterface::Run()
{
  m_audio_stream->PauseOutput(false);

  while (!m_quit_request)
  {
    for (;;)
    {
      SDL_Event ev;
      if (SDL_PollEvent(&ev))
        HandleSDLEvent(&ev);
      else
        break;
    }

    if (m_system && !m_paused)
    {
      m_system->RunFrame();
      if (m_frame_step_request)
      {
        m_frame_step_request = false;
        m_paused = true;
      }
    }

    UpdateControllerRumble();

    // rendering
    {
      DrawImGui();

      if (m_system)
        m_system->GetGPU()->ResetGraphicsAPIState();

      ImGui::Render();
      m_display->Render();

      ImGui::NewFrame();

      if (m_system)
      {
        m_system->GetGPU()->RestoreGraphicsAPIState();

        if (m_speed_limiter_enabled)
          m_system->Throttle();
      }
    }
  }

  // Save state on exit so it can be resumed
  if (m_system)
  {
    if (!SaveState(RESUME_SAVESTATE_FILENAME))
      ReportError("Saving state failed, you will not be able to resume this session.");

    DestroySystem();
  }
}

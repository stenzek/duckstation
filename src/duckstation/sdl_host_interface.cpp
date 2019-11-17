#include "sdl_host_interface.h"
#include "YBaseLib/AutoReleasePtr.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Error.h"
#include "YBaseLib/Log.h"
#include "core/cdrom.h"
#include "core/digital_controller.h"
#include "core/dma.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/mdec.h"
#include "core/memory_card.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/timers.h"
#ifdef Y_PLATFORM_WINDOWS
#include "YBaseLib/Windows/WindowsHeaders.h"
#include "d3d11_host_display.h"
#include <mmsystem.h>
#endif
#include "icon.h"
#include "imgui_styles.h"
#include "opengl_host_display.h"
#include "sdl_audio_stream.h"
#include <cinttypes>
#include <imgui.h>
#include <imgui_impl_sdl.h>
#include <imgui_stdlib.h>
#include <nfd.h>
Log_SetChannel(SDLHostInterface);

SDLHostInterface::SDLHostInterface() : m_settings_filename("settings.ini")
{
  // Increase timer/sleep resolution since we use it for throttling.
#ifdef Y_PLATFORM_WINDOWS
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

#ifdef Y_PLATFORM_WINDOWS
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
#ifdef WIN32
  m_display = UseOpenGLRenderer() ? OpenGLHostDisplay::Create(m_window) : D3D11HostDisplay::Create(m_window);
#else
  m_display = OpenGLHostDisplay::Create(m_window);
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
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont();
}

bool SDLHostInterface::CreateAudioStream()
{
  m_audio_stream = std::make_unique<SDLAudioStream>();
  return m_audio_stream->Reconfigure(44100, 2);
}

void SDLHostInterface::OpenGameControllers()
{
  for (int i = 0; i < SDL_NumJoysticks(); i++)
  {
    SDL_GameController* gcontroller = SDL_GameControllerOpen(i);
    if (gcontroller)
    {
      Log_InfoPrintf("Opened controller %d: %s", i, SDL_GameControllerName(gcontroller));
      m_sdl_controllers.emplace(i, gcontroller);
    }
    else
    {
      Log_WarningPrintf("Failed to open controller %d", i);
    }
  }
}

void SDLHostInterface::CloseGameControllers()
{
  for (auto& it : m_sdl_controllers)
    SDL_GameControllerClose(it.second);
  m_sdl_controllers.clear();
}

void SDLHostInterface::SaveSettings()
{
  m_settings.Save(m_settings_filename.c_str());
}

void SDLHostInterface::ConnectControllers()
{
  m_controller = DigitalController::Create();
  m_system->SetController(0, m_controller);
}

void SDLHostInterface::ResetPerformanceCounters()
{
  if (m_system)
  {
    m_last_frame_number = m_system->GetFrameNumber();
    m_last_internal_frame_number = m_system->GetInternalFrameNumber();
    m_last_global_tick_counter = m_system->GetGlobalTickCounter();
  }
  else
  {
    m_last_frame_number = 0;
    m_last_internal_frame_number = 0;
    m_last_global_tick_counter = 0;
  }
  m_fps_timer.Reset();
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
  AutoReleasePtr<ByteStream> stream;
  if (m_system)
  {
    stream = ByteStream_CreateGrowableMemoryStream(nullptr, 8 * 1024);
    if (!m_system->SaveState(stream) || !stream->SeekAbsolute(0))
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
    if (!BootSystem(nullptr, nullptr) || !m_system->LoadState(stream))
    {
      ReportError("Failed to load state after GPU renderer switch, resetting");
      m_system->Reset();
    }
  }

  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::UpdateFullscreen()
{
  SDL_SetWindowFullscreen(m_window, m_settings.display_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

  // We set the margin only in windowed mode, the menu bar is drawn on top in fullscreen.
  m_display->SetDisplayTopMargin(
    m_settings.display_fullscreen ? 0 : static_cast<int>(20.0f * ImGui::GetIO().DisplayFramebufferScale.x));
}

std::unique_ptr<SDLHostInterface> SDLHostInterface::Create(const char* filename /* = nullptr */,
                                                           const char* exp1_filename /* = nullptr */,
                                                           const char* save_state_filename /* = nullptr */)
{
  std::unique_ptr<SDLHostInterface> intf = std::make_unique<SDLHostInterface>();

  // Settings need to be loaded prior to creating the window for OpenGL bits.
  intf->m_settings.Load(intf->m_settings_filename.c_str());

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

  if (!intf->CreateAudioStream())
  {
    Log_ErrorPrintf("Failed to create host audio stream");
    return nullptr;
  }

  ImGui::NewFrame();

  intf->UpdateSpeedLimiterState();
  intf->OpenGameControllers();

  const bool boot = (filename != nullptr || exp1_filename != nullptr || save_state_filename != nullptr);
  if (boot)
  {
    if (!intf->CreateSystem() || !intf->BootSystem(filename, exp1_filename))
      return nullptr;

    if (save_state_filename)
      intf->LoadState(save_state_filename);
  }

  intf->UpdateFullscreen();

  return intf;
}

TinyString SDLHostInterface::GetSaveStateFilename(u32 index)
{
  return TinyString::FromFormat("savestate_%u.bin", index);
}

void SDLHostInterface::ReportError(const char* message)
{
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DuckStation Error", message, m_window);
}

void SDLHostInterface::ReportMessage(const char* message)
{
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "DuckStation Information", message, m_window);
}

static inline u32 SDLButtonToHostButton(u32 button)
{
  // SDL left = 1, middle = 2, right = 3 :/
  switch (button)
  {
    case 1:
      return 0;
    case 2:
      return 2;
    case 3:
      return 1;
    default:
      return 0xFFFFFFFF;
  }
}

static bool HandleSDLKeyEventForController(const SDL_Event* event, DigitalController* controller)
{
  const bool pressed = (event->type == SDL_KEYDOWN);
  switch (event->key.keysym.scancode)
  {
    case SDL_SCANCODE_KP_8:
    case SDL_SCANCODE_I:
      controller->SetButtonState(DigitalController::Button::Triangle, pressed);
      return true;
    case SDL_SCANCODE_KP_2:
    case SDL_SCANCODE_K:
      controller->SetButtonState(DigitalController::Button::Cross, pressed);
      return true;
    case SDL_SCANCODE_KP_4:
    case SDL_SCANCODE_J:
      controller->SetButtonState(DigitalController::Button::Square, pressed);
      return true;
    case SDL_SCANCODE_KP_6:
    case SDL_SCANCODE_L:
      controller->SetButtonState(DigitalController::Button::Circle, pressed);
      return true;

    case SDL_SCANCODE_W:
    case SDL_SCANCODE_UP:
      controller->SetButtonState(DigitalController::Button::Up, pressed);
      return true;
    case SDL_SCANCODE_S:
    case SDL_SCANCODE_DOWN:
      controller->SetButtonState(DigitalController::Button::Down, pressed);
      return true;
    case SDL_SCANCODE_A:
    case SDL_SCANCODE_LEFT:
      controller->SetButtonState(DigitalController::Button::Left, pressed);
      return true;
    case SDL_SCANCODE_D:
    case SDL_SCANCODE_RIGHT:
      controller->SetButtonState(DigitalController::Button::Right, pressed);
      return true;

    case SDL_SCANCODE_1:
      controller->SetButtonState(DigitalController::Button::L1, pressed);
      return true;
    case SDL_SCANCODE_3:
      controller->SetButtonState(DigitalController::Button::R1, pressed);
      return true;

    case SDL_SCANCODE_Q:
      controller->SetButtonState(DigitalController::Button::L2, pressed);
      return true;
    case SDL_SCANCODE_E:
      controller->SetButtonState(DigitalController::Button::R2, pressed);
      return true;

    case SDL_SCANCODE_RETURN:
      controller->SetButtonState(DigitalController::Button::Start, pressed);
      return true;
    case SDL_SCANCODE_BACKSPACE:
      controller->SetButtonState(DigitalController::Button::Select, pressed);
      return true;

    default:
      break;
  }

  return false;
}

static void HandleSDLControllerAxisEventForController(const SDL_Event* ev, DigitalController* controller)
{
  // Log_DevPrintf("axis %d %d", ev->caxis.axis, ev->caxis.value);

  static constexpr int deadzone = 8192;
  const bool negative = (ev->caxis.value < 0);
  const bool active = (std::abs(ev->caxis.value) >= deadzone);

  if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
  {
    controller->SetButtonState(DigitalController::Button::L2, active);
  }
  else if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
  {
    controller->SetButtonState(DigitalController::Button::R2, active);
  }
  else
  {
    DigitalController::Button negative_button, positive_button;
    if (ev->caxis.axis & 1)
    {
      negative_button = DigitalController::Button::Up;
      positive_button = DigitalController::Button::Down;
    }
    else
    {
      negative_button = DigitalController::Button::Left;
      positive_button = DigitalController::Button::Right;
    }

    controller->SetButtonState(negative_button, negative && active);
    controller->SetButtonState(positive_button, !negative && active);
  }
}

static void HandleSDLControllerButtonEventForController(const SDL_Event* ev, DigitalController* controller)
{
  // Log_DevPrintf("button %d %s", ev->cbutton.button, ev->cbutton.state == SDL_PRESSED ? "pressed" : "released");

  // For xbox one controller..
  static constexpr std::pair<SDL_GameControllerButton, DigitalController::Button> button_mapping[] = {
    {SDL_CONTROLLER_BUTTON_A, DigitalController::Button::Cross},
    {SDL_CONTROLLER_BUTTON_B, DigitalController::Button::Circle},
    {SDL_CONTROLLER_BUTTON_X, DigitalController::Button::Square},
    {SDL_CONTROLLER_BUTTON_Y, DigitalController::Button::Triangle},
    {SDL_CONTROLLER_BUTTON_BACK, DigitalController::Button::Select},
    {SDL_CONTROLLER_BUTTON_START, DigitalController::Button::Start},
    {SDL_CONTROLLER_BUTTON_GUIDE, DigitalController::Button::Start},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, DigitalController::Button::L3},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, DigitalController::Button::R3},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, DigitalController::Button::L1},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, DigitalController::Button::R1},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, DigitalController::Button::Up},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, DigitalController::Button::Down},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, DigitalController::Button::Left},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, DigitalController::Button::Right}};

  for (const auto& bm : button_mapping)
  {
    if (bm.first == ev->cbutton.button)
    {
      controller->SetButtonState(bm.second, ev->cbutton.state == SDL_PRESSED);
      break;
    }
  }
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
      auto iter = m_sdl_controllers.find(event->cdevice.which);
      if (iter == m_sdl_controllers.end())
      {
        SDL_GameController* gcontroller = SDL_GameControllerOpen(event->cdevice.which);
        if (gcontroller)
        {
          Log_InfoPrintf("Controller %s inserted", SDL_GameControllerName(gcontroller));
          m_sdl_controllers.emplace(event->cdevice.which, gcontroller);
        }
      }
    }
    break;

    case SDL_CONTROLLERDEVICEREMOVED:
    {
      auto iter = m_sdl_controllers.find(event->cdevice.which);
      if (iter != m_sdl_controllers.end())
      {
        Log_InfoPrintf("Controller %s removed", SDL_GameControllerName(iter->second));
        SDL_GameControllerClose(iter->second);
        m_sdl_controllers.erase(iter);
      }
    }
    break;

    case SDL_CONTROLLERAXISMOTION:
    {
      if (m_controller)
        HandleSDLControllerAxisEventForController(event, m_controller.get());
    }
    break;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    {
      if (event->type == SDL_CONTROLLERBUTTONDOWN && event->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK)
      {
        // focus the menu bar
        m_focus_main_menu_bar = true;
      }

      if (m_controller)
        HandleSDLControllerButtonEventForController(event, m_controller.get());
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
  if (!repeat && m_controller && HandleSDLKeyEventForController(event, m_controller.get()))
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
        DoToggleSoftwareRendering();
    }
    break;

    case SDL_SCANCODE_PAGEUP:
    case SDL_SCANCODE_PAGEDOWN:
    {
      if (pressed)
      {
        DoModifyInternalResolution(event->key.keysym.scancode == SDL_SCANCODE_PAGEUP ? 1 : -1);
      }
    }
    break;
  }
}

void SDLHostInterface::ClearImGuiFocus()
{
  ImGui::SetWindowFocus(nullptr);
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
      DoReset();

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
        if (ImGui::MenuItem(TinyString::FromFormat("State %u", i).GetCharArray()))
          DoLoadState(i);
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Save State", system_enabled))
    {
      for (u32 i = 1; i <= NUM_QUICK_SAVE_STATES; i++)
      {
        if (ImGui::MenuItem(TinyString::FromFormat("State %u", i).GetCharArray()))
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

      const u32 rounded_speed = static_cast<u32>(std::round(m_speed));
      if (m_speed < 90.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%u%%", rounded_speed);
      else if (m_speed < 110.0f)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%u%%", rounded_speed);
      else
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u%%", rounded_speed);

      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - 165.0f);
      ImGui::Text("FPS: %.2f", m_fps);

      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - 80.0f);
      ImGui::Text("VPS: %.2f", m_vps);
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
      if (ImGui::MenuItem(
            TinyString::FromFormat("%ux (%ux%u)", scale, scale * GPU::VRAM_WIDTH, scale * GPU::VRAM_HEIGHT), nullptr,
            current_internal_resolution == scale))
      {
        m_settings.gpu_resolution_scale = scale;
        gpu_settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  gpu_settings_changed |= ImGui::MenuItem("True (24-Bit) Color", nullptr, &m_settings.gpu_true_color);
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
      if (ImGui::MenuItem(TinyString::FromFormat("State %u", i).GetCharArray()))
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
      if (DrawSettingsSectionHeader("Host Synchronization"))
      {
        if (ImGui::Checkbox("Sync To Audio", &m_settings.audio_sync_enabled))
        {
          settings_changed = true;
          UpdateSpeedLimiterState();
        }
        if (ImGui::Checkbox("Sync To Video", &m_settings.video_sync_enabled))
        {
          settings_changed = true;
          UpdateSpeedLimiterState();
        }
      }

      ImGui::NewLine();
      if (DrawSettingsSectionHeader("BIOS"))
      {
        ImGui::Text("ROM Path:");
        ImGui::SameLine(indent);
        settings_changed |= DrawFileChooser("##bios_path", &m_settings.bios_path);

        settings_changed |= ImGui::Checkbox("Enable TTY Output", &m_settings.bios_patch_tty_enable);
        settings_changed |= ImGui::Checkbox("Fast Boot", &m_settings.bios_patch_fast_boot);
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Memory Cards"))
    {
      for (int i = 0; i < 2; i++)
      {
        if (!DrawSettingsSectionHeader(TinyString::FromFormat("Card %c", 'A' + i)))
          continue;

        ImGui::Text("Card %c", 'A' + i);

        ImGui::Text("Path:");
        ImGui::SameLine(indent);

        std::string* path_ptr = (i == 0) ? &m_settings.memory_card_a_path : &m_settings.memory_card_b_path;
        if (DrawFileChooser(TinyString::FromFormat("##memcard_%c_path", 'a' + i), path_ptr))
        {
          settings_changed = true;
          if (m_system)
            m_system->UpdateMemoryCards();
        }

        if (ImGui::Button("Eject"))
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

        ImGui::Checkbox("True 24-bit Color (disables dithering)", &m_settings.gpu_true_color);
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

void SDLHostInterface::DrawDebugWindows()
{
  const Settings::DebugSettings& debug_settings = m_system->GetSettings().debugging;

  if (debug_settings.show_gpu_state)
    m_system->GetGPU()->DrawDebugStateWindow();
  if (debug_settings.show_cdrom_state)
    m_system->GetCDROM()->DrawDebugWindow();
  if (debug_settings.show_timers_state)
    m_system->GetTimers()->DrawDebugStateWindow();
  if (debug_settings.show_spu_state)
    m_system->GetSPU()->DrawDebugStateWindow();
  if (debug_settings.show_mdec_state)
    m_system->GetMDEC()->DrawDebugStateWindow();
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

void SDLHostInterface::AddOSDMessage(const char* message, float duration /*= 2.0f*/)
{
  OSDMessage msg;
  msg.text = message;
  msg.duration = duration;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  m_osd_messages.push_back(std::move(msg));
}

void SDLHostInterface::DrawOSDMessages()
{
  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  const float scale = ImGui::GetIO().DisplayFramebufferScale.x;

  auto iter = m_osd_messages.begin();
  float position_x = 10.0f * scale;
  float position_y = (10.0f + (m_settings.display_fullscreen ? 0.0f : 20.0f)) * scale;
  u32 index = 0;
  while (iter != m_osd_messages.end())
  {
    const OSDMessage& msg = *iter;
    const double time = msg.time.GetTimeSeconds();
    const float time_remaining = static_cast<float>(msg.duration - time);
    if (time_remaining <= 0.0f)
    {
      iter = m_osd_messages.erase(iter);
      continue;
    }

    const float opacity = std::min(time_remaining, 1.0f);
    ImGui::SetNextWindowPos(ImVec2(position_x, position_y));
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);

    if (ImGui::Begin(SmallString::FromFormat("osd_%u", index++), nullptr, window_flags))
    {
      ImGui::TextUnformatted(msg.text);
      position_y += ImGui::GetWindowSize().y + (4.0f * scale);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ++iter;
  }
}

void SDLHostInterface::DoReset()
{
  m_system->Reset();
  ResetPerformanceCounters();
  AddOSDMessage("System reset.");
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

  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoStartDisc()
{
  Assert(!m_system);

  nfdchar_t* path = nullptr;
  if (!NFD_OpenDialog("bin,img,cue,exe,psexe", nullptr, &path) || !path || std::strlen(path) == 0)
    return;

  AddOSDMessage(SmallString::FromFormat("Starting disc from '%s'...", path));
  if (!CreateSystem() || !BootSystem(path, nullptr))
  {
    DestroySystem();
    return;
  }

  ResetPerformanceCounters();
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

  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoChangeDisc()
{
  Assert(m_system);

  nfdchar_t* path = nullptr;
  if (!NFD_OpenDialog("bin,img,cue,exe,psexe", nullptr, &path) || !path || std::strlen(path) == 0)
    return;

  if (m_system->InsertMedia(path))
    AddOSDMessage(SmallString::FromFormat("Switched CD to '%s'", path));
  else
    AddOSDMessage("Failed to switch CD. The log may contain further information.");

  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoLoadState(u32 index)
{
  if (HasSystem())
  {
    LoadState(GetSaveStateFilename(index));
  }
  else
  {
    if (!CreateSystem() || !BootSystem(nullptr, GetSaveStateFilename(index)))
    {
      DestroySystem();
      return;
    }
  }

  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLHostInterface::DoSaveState(u32 index)
{
  Assert(m_system);
  SaveState(GetSaveStateFilename(index));
  ClearImGuiFocus();
}

void SDLHostInterface::DoTogglePause()
{
  if (!m_system)
    return;

  m_paused = !m_paused;
  if (!m_paused)
    m_fps_timer.Reset();
}

void SDLHostInterface::DoFrameStep()
{
  if (!m_system)
    return;

  m_frame_step_request = true;
  m_paused = false;
}

void SDLHostInterface::DoToggleSoftwareRendering()
{
  if (!m_system)
    return;

  if (m_settings.gpu_renderer != GPURenderer::Software)
  {
    m_settings.gpu_renderer = GPURenderer::Software;
    AddOSDMessage("Switched to software GPU renderer.");
  }
  else
  {
    m_settings.gpu_renderer = m_display->GetRenderAPI() == HostDisplay::RenderAPI::D3D11 ? GPURenderer::HardwareD3D11 :
                                                                                           GPURenderer::HardwareOpenGL;
    AddOSDMessage("Switched to hardware GPU renderer.");
  }

  m_system->RecreateGPU();
}

void SDLHostInterface::DoToggleFullscreen()
{
  m_settings.display_fullscreen = !m_settings.display_fullscreen;
  UpdateFullscreen();
}

void SDLHostInterface::DoModifyInternalResolution(s32 increment)
{
  const u32 new_resolution_scale =
    std::clamp<u32>(static_cast<u32>(static_cast<s32>(m_settings.gpu_resolution_scale) + increment), 1,
                    m_settings.max_gpu_resolution_scale);
  if (new_resolution_scale == m_settings.gpu_resolution_scale)
    return;

  m_settings.gpu_resolution_scale = new_resolution_scale;
  if (m_system)
    m_system->GetGPU()->UpdateSettings();

  AddOSDMessage(TinyString::FromFormat("Resolution scale set to %ux (%ux%u)", m_settings.gpu_resolution_scale,
                                       GPU::VRAM_WIDTH * m_settings.gpu_resolution_scale,
                                       GPU::VRAM_HEIGHT * m_settings.gpu_resolution_scale));
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
          Throttle();
      }
    }

    if (m_system)
    {
      // update fps counter
      const double time = m_fps_timer.GetTimeSeconds();
      if (time >= 0.25f)
      {
        m_vps = static_cast<float>(static_cast<double>(m_system->GetFrameNumber() - m_last_frame_number) / time);
        m_last_frame_number = m_system->GetFrameNumber();
        m_fps = static_cast<float>(
          static_cast<double>(m_system->GetInternalFrameNumber() - m_last_internal_frame_number) / time);
        m_last_internal_frame_number = m_system->GetInternalFrameNumber();
        m_speed =
          static_cast<float>(static_cast<double>(m_system->GetGlobalTickCounter() - m_last_global_tick_counter) /
                             (static_cast<double>(MASTER_CLOCK) * time)) *
          100.0f;
        m_last_global_tick_counter = m_system->GetGlobalTickCounter();
        m_fps_timer.Reset();
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

#include "sdl_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/sdl_audio_stream.h"
#include "frontend-common/sdl_controller_interface.h"
#include "imgui_impl_sdl.h"
#include "opengl_host_display.h"
#include <cinttypes>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <nfd.h>
Log_SetChannel(SDLHostInterface);

#ifdef WIN32
#include "d3d11_host_display.h"
#endif

SDLHostInterface::SDLHostInterface()
{
  m_run_later_event_id = SDL_RegisterEvents(1);
}

SDLHostInterface::~SDLHostInterface()
{
  g_sdl_controller_interface.Shutdown();
  if (m_display)
  {
    DestroyDisplay();
    ImGui::DestroyContext();
  }

  if (m_window)
    DestroySDLWindow();
}

float SDLHostInterface::GetDPIScaleFactor(SDL_Window* window)
{
#ifdef __APPLE__
  static constexpr float DEFAULT_DPI = 72.0f;
#else
  static constexpr float DEFAULT_DPI = 96.0f;
#endif

  if (!window)
  {
    SDL_Window* dummy_window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1, 1,
                                                SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (!dummy_window)
      return 1.0f;

    const float scale = GetDPIScaleFactor(dummy_window);

    SDL_DestroyWindow(dummy_window);

    return scale;
  }

  int display_index = SDL_GetWindowDisplayIndex(window);
  float display_dpi = DEFAULT_DPI;
  if (SDL_GetDisplayDPI(display_index, &display_dpi, nullptr, nullptr) != 0)
    return 1.0f;

  return display_dpi / DEFAULT_DPI;
}

bool SDLHostInterface::CreateSDLWindow()
{
  static constexpr u32 DEFAULT_WINDOW_WIDTH = 900;
  static constexpr u32 DEFAULT_WINDOW_HEIGHT = 700;

  // Create window.
  const u32 window_flags =
    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (UseOpenGLRenderer() ? SDL_WINDOW_OPENGL : 0);

  u32 window_width = DEFAULT_WINDOW_WIDTH;
  u32 window_height = DEFAULT_WINDOW_HEIGHT;

  // macOS does DPI scaling differently..
#ifndef __APPLE__
  {
    // scale by default monitor's DPI
    float scale = GetDPIScaleFactor(nullptr);
    window_width = static_cast<u32>(std::round(static_cast<float>(window_width) * scale));
    window_height = static_cast<u32>(std::round(static_cast<float>(window_height) * scale));
  }
#endif

  m_window = SDL_CreateWindow("DuckStation", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_width,
                              window_height, window_flags);
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

  if (m_fullscreen)
    SDL_SetWindowFullscreen(m_window, SDL_WINDOW_FULLSCREEN_DESKTOP);

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
  std::unique_ptr<HostDisplay> display;
#ifdef WIN32
  display = UseOpenGLRenderer() ? OpenGLHostDisplay::Create(m_window, debug_device) :
                                  D3D11HostDisplay::Create(m_window, debug_device);
#else
  display = OpenGLHostDisplay::Create(m_window, debug_device);
#endif

  if (!display)
    return false;

  m_app_icon_texture =
    display->CreateTexture(APP_ICON_WIDTH, APP_ICON_HEIGHT, APP_ICON_DATA, APP_ICON_WIDTH * sizeof(u32));
  if (!display)
    return false;

  display->SetDisplayTopMargin(m_fullscreen ? 0 : static_cast<int>(20.0f * ImGui::GetIO().DisplayFramebufferScale.x));
  m_display = display.release();
  return true;
}

void SDLHostInterface::DestroyDisplay()
{
  m_app_icon_texture.reset();
  delete m_display;
  m_display = nullptr;
}

void SDLHostInterface::CreateImGuiContext()
{
  const float framebuffer_scale = GetDPIScaleFactor(m_window);

  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::GetIO().DisplayFramebufferScale.x = framebuffer_scale;
  ImGui::GetIO().DisplayFramebufferScale.y = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont(15.0f * framebuffer_scale);
}

void SDLHostInterface::UpdateFramebufferScale()
{
  const float framebuffer_scale = GetDPIScaleFactor(m_window);
  ImGui::GetIO().DisplayFramebufferScale.x = framebuffer_scale;
  ImGui::GetIO().DisplayFramebufferScale.y = framebuffer_scale;
}

bool SDLHostInterface::AcquireHostDisplay()
{
  // Handle renderer switch if required on Windows.
#ifdef WIN32
  const HostDisplay::RenderAPI render_api = m_display->GetRenderAPI();
  const bool render_api_is_gl =
    render_api == HostDisplay::RenderAPI::OpenGL || render_api == HostDisplay::RenderAPI::OpenGLES;
  const bool render_api_wants_gl = UseOpenGLRenderer();
  if (render_api_is_gl != render_api_wants_gl)
  {
    ImGui::EndFrame();
    DestroyDisplay();
    DestroySDLWindow();

    if (!CreateSDLWindow())
      Panic("Failed to recreate SDL window on GPU renderer switch");

    if (!CreateDisplay())
      Panic("Failed to recreate display on GPU renderer switch");

    ImGui::NewFrame();
  }
#endif

  return true;
}

void SDLHostInterface::ReleaseHostDisplay()
{
  // restore vsync, since we don't want to burn cycles at the menu
  m_display->SetVSync(true);
}

std::unique_ptr<AudioStream> SDLHostInterface::CreateAudioStream(AudioBackend backend)
{
  switch (backend)
  {
    case AudioBackend::Null:
      return AudioStream::CreateNullAudioStream();

    case AudioBackend::Cubeb:
      return AudioStream::CreateCubebAudioStream();

    case AudioBackend::SDL:
      return SDLAudioStream::Create();

    default:
      return nullptr;
  }
}

void SDLHostInterface::OnSystemCreated()
{
  HostInterface::OnSystemCreated();

  UpdateKeyboardControllerMapping();
  g_sdl_controller_interface.SetDefaultBindings();
  ClearImGuiFocus();
}

void SDLHostInterface::OnSystemPaused(bool paused)
{
  HostInterface::OnSystemPaused(paused);

  if (!paused)
    ClearImGuiFocus();
}

void SDLHostInterface::OnSystemDestroyed()
{
  HostInterface::OnSystemDestroyed();
}

void SDLHostInterface::OnControllerTypeChanged(u32 slot)
{
  HostInterface::OnControllerTypeChanged(slot);

  UpdateKeyboardControllerMapping();
  g_sdl_controller_interface.SetDefaultBindings();
}

void SDLHostInterface::RunLater(std::function<void()> callback)
{
  SDL_Event ev = {};
  ev.type = SDL_USEREVENT;
  ev.user.code = m_run_later_event_id;
  ev.user.data1 = new std::function<void()>(std::move(callback));
  SDL_PushEvent(&ev);
}

void SDLHostInterface::SaveSettings()
{
  INISettingsInterface si(GetSettingsFileName().c_str());
  m_settings_copy.Save(si);
}

void SDLHostInterface::UpdateSettings()
{
  HostInterface::UpdateSettings([this]() { m_settings = m_settings_copy; });
}

void SDLHostInterface::SetFullscreen(bool enabled)
{
  if (m_fullscreen == enabled)
    return;

  SDL_SetWindowFullscreen(m_window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

  // We set the margin only in windowed mode, the menu bar is drawn on top in fullscreen.
  m_display->SetDisplayTopMargin(enabled ? 0 : static_cast<int>(20.0f * ImGui::GetIO().DisplayFramebufferScale.x));

  int window_width, window_height;
  SDL_GetWindowSize(m_window, &window_width, &window_height);
  m_display->WindowResized(window_width, window_height);
  m_fullscreen = enabled;
}

std::unique_ptr<SDLHostInterface> SDLHostInterface::Create()
{
  std::unique_ptr<SDLHostInterface> intf = std::make_unique<SDLHostInterface>();

  // Settings need to be loaded prior to creating the window for OpenGL bits.
  INISettingsInterface si(intf->GetSettingsFileName().c_str());
  intf->m_settings_copy.Load(si);
  intf->m_settings = intf->m_settings_copy;
  intf->m_fullscreen = intf->m_settings_copy.start_fullscreen;

  if (!intf->CreateSDLWindow())
  {
    Log_ErrorPrintf("Failed to create SDL window");
    return nullptr;
  }

  if (!g_sdl_controller_interface.Initialize(intf.get()))
  {
    Log_ErrorPrintf("Failed to initialize controller interface.");
    return nullptr;
  }

  intf->CreateImGuiContext();
  if (!intf->CreateDisplay())
  {
    Log_ErrorPrintf("Failed to create host display");
    return nullptr;
  }

  ImGui::NewFrame();

  return intf;
}

void SDLHostInterface::ReportError(const char* message)
{
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DuckStation", message, m_window);
}

void SDLHostInterface::ReportMessage(const char* message)
{
  AddOSDMessage(message, 2.0f);
}

bool SDLHostInterface::ConfirmMessage(const char* message)
{
  SDL_MessageBoxData mbd = {};
  mbd.flags = SDL_MESSAGEBOX_INFORMATION;
  mbd.window = m_window;
  mbd.title = "DuckStation";
  mbd.message = message;
  mbd.numbuttons = 2;

  // Why the heck these are reversed I have no idea...
  SDL_MessageBoxButtonData buttons[2] = {};
  buttons[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
  buttons[1].buttonid = 0;
  buttons[1].text = "Yes";
  buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
  buttons[0].buttonid = 1;
  buttons[0].text = "No";
  mbd.buttons = buttons;
  mbd.numbuttons = countof(buttons);

  int button_id = 0;
  SDL_ShowMessageBox(&mbd, &button_id);
  return (button_id == 0);
}

void SDLHostInterface::HandleSDLEvent(const SDL_Event* event)
{
  ImGui_ImplSDL2_ProcessEvent(event);
  g_sdl_controller_interface.ProcessSDLEvent(event);

  switch (event->type)
  {
    case SDL_WINDOWEVENT:
    {
      if (event->window.event == SDL_WINDOWEVENT_RESIZED)
      {
        m_display->WindowResized(event->window.data1, event->window.data2);
        UpdateFramebufferScale();
      }
      else if (event->window.event == SDL_WINDOWEVENT_MOVED)
      {
        UpdateFramebufferScale();
      }
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
    case SDL_CONTROLLERDEVICEREMOVED:
      g_sdl_controller_interface.SetDefaultBindings();
      break;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    {
      if (event->type == SDL_CONTROLLERBUTTONDOWN && event->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK)
      {
        // focus the menu bar
        m_focus_main_menu_bar = true;
      }
    }
    break;

    case SDL_USEREVENT:
    {
      if (static_cast<u32>(event->user.code) == m_run_later_event_id)
      {
        std::function<void()>* callback = static_cast<std::function<void()>*>(event->user.data1);
        Assert(callback);
        (*callback)();
        delete callback;
      }
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
          SaveState(true, index);
        else
          LoadState(true, index);
      }
    }
    break;

    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
    {
      if ((event->key.keysym.mod & (KMOD_LALT | KMOD_RALT)) && !pressed)
        SetFullscreen(!m_fullscreen);
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
        PauseSystem(!m_paused);
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
        m_settings_copy.speed_limiter_enabled = m_settings.speed_limiter_enabled;
        UpdateSpeedLimiterState();
        AddOSDMessage(m_settings.speed_limiter_enabled ? "Speed limiter enabled." : "Speed limiter disabled.");
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
  if (m_fullscreen && !m_system &&
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
    {
      RunLater([this]() { DoStartDisc(); });
      ClearImGuiFocus();
    }
    if (ImGui::MenuItem("Start BIOS", nullptr, false, !system_enabled))
    {
      RunLater([this]() {
        SystemBootParameters boot_params;
        BootSystem(boot_params);
      });
      ClearImGuiFocus();
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Power Off", nullptr, false, system_enabled))
    {
      RunLater([this]() { DestroySystem(); });
      ClearImGuiFocus();
    }

    if (ImGui::MenuItem("Reset", nullptr, false, system_enabled))
    {
      RunLater([this]() { ResetSystem(); });
      ClearImGuiFocus();
    }

    if (ImGui::MenuItem("Pause", nullptr, m_paused, system_enabled))
    {
      RunLater([this]() { PauseSystem(!m_paused); });
      ClearImGuiFocus();
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Change Disc", nullptr, false, system_enabled))
    {
      RunLater([this]() { DoChangeDisc(); });
      ClearImGuiFocus();
    }

    if (ImGui::MenuItem("Frame Step", nullptr, false, system_enabled))
    {
      RunLater([this]() { DoFrameStep(); });
      ClearImGuiFocus();
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("Load State"))
    {
      for (u32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
      {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "State %u", i);
        if (ImGui::MenuItem(buf))
        {
          RunLater([this, i]() { LoadState(true, i); });
          ClearImGuiFocus();
        }
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Save State", system_enabled))
    {
      for (u32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
      {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "State %u", i);
        if (ImGui::MenuItem(buf))
        {
          RunLater([this, i]() { SaveState(true, i); });
          ClearImGuiFocus();
        }
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
    const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;

    if (!m_paused)
    {
      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - (210.0f * framebuffer_scale));

      const float speed = m_system->GetEmulationSpeed();
      const u32 rounded_speed = static_cast<u32>(std::round(speed));
      if (speed < 90.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%u%%", rounded_speed);
      else if (speed < 110.0f)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%u%%", rounded_speed);
      else
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u%%", rounded_speed);

      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - (165.0f * framebuffer_scale));
      ImGui::Text("FPS: %.2f", m_system->GetFPS());

      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - (80.0f * framebuffer_scale));
      ImGui::Text("VPS: %.2f", m_system->GetVPS());
    }
    else
    {
      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - (50.0f * framebuffer_scale));
      ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused");
    }
  }

  ImGui::EndMainMenuBar();
}

void SDLHostInterface::DrawQuickSettingsMenu()
{
  bool settings_changed = false;
  settings_changed |= ImGui::MenuItem("Enable Speed Limiter", nullptr, &m_settings_copy.speed_limiter_enabled);

  ImGui::Separator();

  if (ImGui::BeginMenu("CPU Execution Mode"))
  {
    const CPUExecutionMode current = m_settings.cpu_execution_mode;
    for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        m_settings_copy.cpu_execution_mode = static_cast<CPUExecutionMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("Renderer"))
  {
    const GPURenderer current = m_settings_copy.gpu_renderer;
    for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        m_settings_copy.gpu_renderer = static_cast<GPURenderer>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  bool fullscreen = m_fullscreen;
  if (ImGui::MenuItem("Fullscreen", nullptr, &fullscreen))
    RunLater([this, fullscreen] { SetFullscreen(fullscreen); });

  settings_changed |= ImGui::MenuItem("VSync", nullptr, &m_settings_copy.video_sync_enabled);

  ImGui::Separator();

  if (ImGui::BeginMenu("Resolution Scale"))
  {
    const u32 current_internal_resolution = m_settings_copy.gpu_resolution_scale;
    for (u32 scale = 1; scale <= GPU::MAX_RESOLUTION_SCALE; scale++)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux (%ux%u)", scale, scale * GPU::VRAM_WIDTH, scale * GPU::VRAM_HEIGHT);

      if (ImGui::MenuItem(buf, nullptr, current_internal_resolution == scale))
      {
        m_settings_copy.gpu_resolution_scale = scale;
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("True (24-Bit) Color", nullptr, &m_settings_copy.gpu_true_color);
  settings_changed |= ImGui::MenuItem("Scaled Dithering", nullptr, &m_settings_copy.gpu_scaled_dithering);
  settings_changed |= ImGui::MenuItem("Texture Filtering", nullptr, &m_settings_copy.gpu_texture_filtering);
  settings_changed |= ImGui::MenuItem("Display Linear Filtering", nullptr, &m_settings_copy.display_linear_filtering);

  if (settings_changed)
  {
    RunLater([this]() {
      SaveSettings();
      UpdateSettings();
    });
  }
}

void SDLHostInterface::DrawDebugMenu()
{
  Settings::DebugSettings& debug_settings = m_settings.debugging;
  bool settings_changed = false;

  ImGui::MenuItem("Show System State");
  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Show GPU State", nullptr, &debug_settings.show_gpu_state);
  settings_changed |= ImGui::MenuItem("Show VRAM", nullptr, &debug_settings.show_vram);
  settings_changed |= ImGui::MenuItem("Dump CPU to VRAM Copies", nullptr, &debug_settings.dump_cpu_to_vram_copies);
  settings_changed |= ImGui::MenuItem("Dump VRAM to CPU Copies", nullptr, &debug_settings.dump_vram_to_cpu_copies);
  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Show CDROM State", nullptr, &debug_settings.show_cdrom_state);
  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Show SPU State", nullptr, &debug_settings.show_spu_state);
  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Show Timers State", nullptr, &debug_settings.show_timers_state);
  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Show MDEC State", nullptr, &debug_settings.show_mdec_state);
  ImGui::Separator();

  if (settings_changed)
  {
    // have to apply it to the copy too, otherwise it won't save
    Settings::DebugSettings& debug_settings_copy = m_settings_copy.debugging;
    debug_settings_copy.show_gpu_state = debug_settings.show_gpu_state;
    debug_settings_copy.show_vram = debug_settings.show_vram;
    debug_settings_copy.dump_cpu_to_vram_copies = debug_settings.dump_cpu_to_vram_copies;
    debug_settings_copy.dump_vram_to_cpu_copies = debug_settings.dump_vram_to_cpu_copies;
    debug_settings_copy.show_cdrom_state = debug_settings.show_cdrom_state;
    debug_settings_copy.show_spu_state = debug_settings.show_spu_state;
    debug_settings_copy.show_timers_state = debug_settings.show_timers_state;
    debug_settings_copy.show_mdec_state = debug_settings.show_mdec_state;
    SaveSettings();
  }
}

void SDLHostInterface::DrawPoweredOffWindow()
{
  static constexpr int WINDOW_WIDTH = 400;
  static constexpr int WINDOW_HEIGHT = 650;
  static constexpr int BUTTON_WIDTH = 200;
  static constexpr int BUTTON_HEIGHT = 40;
  const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;

  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(WINDOW_WIDTH) * framebuffer_scale,
                                  static_cast<float>(WINDOW_HEIGHT) * framebuffer_scale));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  if (!ImGui::Begin("Powered Off", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoBringToFrontOnFocus))
  {
    ImGui::End();
  }

  ImGui::SetCursorPosX(static_cast<float>((WINDOW_WIDTH - APP_ICON_WIDTH) / 2) * framebuffer_scale);
  ImGui::Image(m_app_icon_texture->GetHandle(), ImVec2(static_cast<float>(APP_ICON_WIDTH) * framebuffer_scale,
                                                       static_cast<float>(APP_ICON_HEIGHT) * framebuffer_scale));
  ImGui::SetCursorPosY(static_cast<float>(APP_ICON_HEIGHT + 32) * framebuffer_scale);

  const ImVec2 button_size(static_cast<float>(BUTTON_WIDTH) * framebuffer_scale,
                           static_cast<float>(BUTTON_HEIGHT) * framebuffer_scale);
  const float button_left = static_cast<float>((WINDOW_WIDTH - BUTTON_WIDTH) / 2) * framebuffer_scale;

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * framebuffer_scale);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * framebuffer_scale);
  ImGui::PushStyleColor(ImGuiCol_Button, 0xFF202020);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, 0xFF808080);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, 0xFF575757);

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Resume", button_size))
  {
    RunLater([this]() { ResumeSystemFromMostRecentState(); });
    ClearImGuiFocus();
  }
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Start Disc", button_size))
  {
    RunLater([this]() { DoStartDisc(); });
    ClearImGuiFocus();
  }
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Start BIOS", button_size))
  {
    RunLater([this]() {
      SystemBootParameters boot_params;
      BootSystem(boot_params);
    });
    ClearImGuiFocus();
  }
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Load State", button_size))
    ImGui::OpenPopup("PowerOffWindow_LoadStateMenu");
  if (ImGui::BeginPopup("PowerOffWindow_LoadStateMenu"))
  {
    for (u32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
    {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "State %u", i);
      if (ImGui::MenuItem(buf))
      {
        RunLater([this, i]() { LoadState(true, i); });
        ClearImGuiFocus();
      }
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
  const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;

  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(500 * framebuffer_scale, 400.0f * framebuffer_scale), ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("Settings", &m_settings_window_open, ImGuiWindowFlags_NoResize))
  {
    ImGui::End();
    return;
  }

  bool settings_changed = false;

  if (ImGui::BeginTabBar("SettingsTabBar", 0))
  {
    const float indent = 150.0f * framebuffer_scale;

    if (ImGui::BeginTabItem("General"))
    {
      if (DrawSettingsSectionHeader("Console"))
      {
        ImGui::Text("Region:");
        ImGui::SameLine(indent);

        int region = static_cast<int>(m_settings_copy.region);
        if (ImGui::Combo(
              "##region", &region,
              [](void*, int index, const char** out_text) {
                *out_text = Settings::GetConsoleRegionDisplayName(static_cast<ConsoleRegion>(index));
                return true;
              },
              nullptr, static_cast<int>(ConsoleRegion::Count)))
        {
          m_settings_copy.region = static_cast<ConsoleRegion>(region);
          settings_changed = true;
        }

        ImGui::Text("BIOS Path:");
        ImGui::SameLine(indent);
        settings_changed |= DrawFileChooser("##bios_path", &m_settings_copy.bios_path);

        settings_changed |= ImGui::Checkbox("Enable TTY Output", &m_settings_copy.bios_patch_tty_enable);
        settings_changed |= ImGui::Checkbox("Fast Boot", &m_settings_copy.bios_patch_fast_boot);
      }

      ImGui::NewLine();
      if (DrawSettingsSectionHeader("Behavior"))
      {
        ImGui::Text("Emulation Speed:");
        ImGui::SameLine(indent);

        settings_changed |= ImGui::SliderFloat("##speed", &m_settings_copy.emulation_speed, 0.25f, 5.0f);
        settings_changed |= ImGui::Checkbox("Enable Speed Limiter", &m_settings_copy.speed_limiter_enabled);
        settings_changed |= ImGui::Checkbox("Increase Timer Resolution", &m_settings_copy.increase_timer_resolution);
        settings_changed |= ImGui::Checkbox("Pause On Start", &m_settings_copy.start_paused);
        settings_changed |= ImGui::Checkbox("Start Fullscreen", &m_settings_copy.start_fullscreen);
        settings_changed |= ImGui::Checkbox("Save State On Exit", &m_settings_copy.save_state_on_exit);
      }

      ImGui::NewLine();
      if (DrawSettingsSectionHeader("CDROM Emulation"))
      {
        settings_changed |= ImGui::Checkbox("Use Read Thread (Asynchronous)", &m_settings.cdrom_read_thread);
      }

      ImGui::NewLine();
      if (DrawSettingsSectionHeader("Audio"))
      {
        ImGui::Text("Backend:");
        ImGui::SameLine(indent);

        int backend = static_cast<int>(m_settings_copy.audio_backend);
        if (ImGui::Combo(
              "##backend", &backend,
              [](void*, int index, const char** out_text) {
                *out_text = Settings::GetAudioBackendDisplayName(static_cast<AudioBackend>(index));
                return true;
              },
              nullptr, static_cast<int>(AudioBackend::Count)))
        {
          m_settings_copy.audio_backend = static_cast<AudioBackend>(backend);
          settings_changed = true;
        }

        settings_changed |= ImGui::Checkbox("Output Sync", &m_settings_copy.audio_sync_enabled);
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

          int controller_type = static_cast<int>(m_settings_copy.controller_types[i]);
          if (ImGui::Combo(
                "##controller_type", &controller_type,
                [](void*, int index, const char** out_text) {
                  *out_text = Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(index));
                  return true;
                },
                nullptr, static_cast<int>(ControllerType::Count)))
          {
            m_settings_copy.controller_types[i] = static_cast<ControllerType>(controller_type);
            settings_changed = true;
          }
        }

        ImGui::Text("Memory Card Path:");
        ImGui::SameLine(indent);

        std::string* path_ptr = &m_settings_copy.memory_card_paths[i];
        std::snprintf(buf, sizeof(buf), "##memcard_%c_path", 'a' + i);
        settings_changed |= DrawFileChooser(buf, path_ptr);

        if (ImGui::Button("Eject Memory Card"))
        {
          path_ptr->clear();
          settings_changed = true;
        }

        ImGui::NewLine();
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("CPU"))
    {
      ImGui::Text("Execution Mode:");
      ImGui::SameLine(indent);

      int execution_mode = static_cast<int>(m_settings_copy.cpu_execution_mode);
      if (ImGui::Combo(
            "##execution_mode", &execution_mode,
            [](void*, int index, const char** out_text) {
              *out_text = Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(index));
              return true;
            },
            nullptr, static_cast<int>(CPUExecutionMode::Count)))
      {
        m_settings_copy.cpu_execution_mode = static_cast<CPUExecutionMode>(execution_mode);
        settings_changed = true;
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("GPU"))
    {
      if (DrawSettingsSectionHeader("Basic"))
      {
        ImGui::Text("Renderer:");
        ImGui::SameLine(indent);

        int gpu_renderer = static_cast<int>(m_settings_copy.gpu_renderer);
        if (ImGui::Combo(
              "##gpu_renderer", &gpu_renderer,
              [](void*, int index, const char** out_text) {
                *out_text = Settings::GetRendererDisplayName(static_cast<GPURenderer>(index));
                return true;
              },
              nullptr, static_cast<int>(GPURenderer::Count)))
        {
          m_settings_copy.gpu_renderer = static_cast<GPURenderer>(gpu_renderer);
          settings_changed = true;
        }
      }

      ImGui::NewLine();

      if (DrawSettingsSectionHeader("Display Output"))
      {
        ImGui::Text("Crop:");
        ImGui::SameLine(indent);

        int display_crop_mode = static_cast<int>(m_settings_copy.display_crop_mode);
        if (ImGui::Combo(
              "##display_crop_mode", &display_crop_mode,
              [](void*, int index, const char** out_text) {
                *out_text = Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(index));
                return true;
              },
              nullptr, static_cast<int>(DisplayCropMode::Count)))
        {
          m_settings_copy.display_crop_mode = static_cast<DisplayCropMode>(display_crop_mode);
          settings_changed = true;
        }

        settings_changed |= ImGui::Checkbox("Use Debug Device", &m_settings_copy.gpu_use_debug_device);
        settings_changed |= ImGui::Checkbox("Linear Filtering", &m_settings_copy.display_linear_filtering);
        settings_changed |= ImGui::Checkbox("VSync", &m_settings_copy.video_sync_enabled);
      }

      ImGui::NewLine();

      if (DrawSettingsSectionHeader("Enhancements"))
      {
        ImGui::Text("Resolution Scale:");
        ImGui::SameLine(indent);

        static constexpr std::array<const char*, GPU::MAX_RESOLUTION_SCALE> resolutions = {{
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

        int current_resolution_index = static_cast<int>(m_settings_copy.gpu_resolution_scale) - 1;
        if (ImGui::Combo("##gpu_resolution_scale", &current_resolution_index, resolutions.data(),
                         static_cast<int>(resolutions.size())))
        {
          m_settings_copy.gpu_resolution_scale = static_cast<u32>(current_resolution_index + 1);
          settings_changed = true;
        }

        settings_changed |= ImGui::Checkbox("True 24-bit Color (disables dithering)", &m_settings_copy.gpu_true_color);
        settings_changed |= ImGui::Checkbox("Texture Filtering", &m_settings_copy.gpu_texture_filtering);
        settings_changed |= ImGui::Checkbox("Force Progressive Scan", &m_settings_copy.display_force_progressive_scan);
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();

  if (settings_changed)
  {
    RunLater([this]() {
      SaveSettings();
      UpdateSettings();
    });
  }
}

void SDLHostInterface::DrawAboutWindow()
{
  const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;

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

  ImGui::SetCursorPosX((ImGui::GetWindowSize().x - (60.0f * framebuffer_scale)) / 2.0f);
  if (ImGui::Button("Close", ImVec2(60.0f * framebuffer_scale, 20.0f * framebuffer_scale)))
    m_about_window_open = false;

  ImGui::EndPopup();
}

bool SDLHostInterface::DrawFileChooser(const char* label, std::string* path, const char* filter /* = nullptr */)
{
  const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;

  ImGui::SetNextItemWidth((ImGui::CalcItemWidth() - 50.0f) * framebuffer_scale);
  bool result = ImGui::InputText(label, path);
  ImGui::SameLine();

  ImGui::SetNextItemWidth(50.0f * framebuffer_scale);
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

void SDLHostInterface::ClearImGuiFocus()
{
  ImGui::SetWindowFocus(nullptr);
}

void SDLHostInterface::DoStartDisc()
{
  Assert(!m_system);

  nfdchar_t* path = nullptr;
  if (!NFD_OpenDialog("bin,img,cue,chd,exe,psexe", nullptr, &path) || !path || std::strlen(path) == 0)
    return;

  AddFormattedOSDMessage(2.0f, "Starting disc from '%s'...", path);

  SystemBootParameters boot_params;
  boot_params.filename = path;
  BootSystem(boot_params);
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

  m_system->ResetPerformanceCounters();
}

void SDLHostInterface::DoFrameStep()
{
  if (!m_system)
    return;

  m_frame_step_request = true;
  m_paused = false;
}

void SDLHostInterface::Run()
{
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

    g_sdl_controller_interface.UpdateControllerRumble();

    // rendering
    {
      DrawImGui();

      if (m_system)
        m_system->GetGPU()->ResetGraphicsAPIState();

      m_display->Render();

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
    if (m_settings.save_state_on_exit)
      SaveResumeSaveState();
    DestroySystem();
  }
}

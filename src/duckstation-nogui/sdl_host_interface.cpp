#include "sdl_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/image.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/fullscreen_ui.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_fullscreen.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/sdl_audio_stream.h"
#include "frontend-common/sdl_controller_interface.h"
#include "frontend-common/vulkan_host_display.h"
#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_stdlib.h"
#include "scmversion/scmversion.h"
#include "sdl_key_names.h"
#include "sdl_util.h"
#include <cinttypes>
#include <cmath>
Log_SetChannel(SDLHostInterface);

#ifdef WIN32
#include "frontend-common/d3d11_host_display.h"
#endif

SDLHostInterface::SDLHostInterface()
{
  m_run_later_event_id = SDL_RegisterEvents(1);
}

SDLHostInterface::~SDLHostInterface() = default;

const char* SDLHostInterface::GetFrontendName() const
{
  return "DuckStation NoGUI Frontend";
}

ALWAYS_INLINE static TinyString GetWindowTitle()
{
  return TinyString::FromFormat("DuckStation %s (%s)", g_scm_tag_str, g_scm_branch_str);
}

bool SDLHostInterface::CreateSDLWindow()
{
  static constexpr u32 DEFAULT_WINDOW_WIDTH = 1280;
  static constexpr u32 DEFAULT_WINDOW_HEIGHT = 720;

  // Create window.
  const u32 window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

  u32 window_width = DEFAULT_WINDOW_WIDTH;
  u32 window_height = DEFAULT_WINDOW_HEIGHT;

  // macOS does DPI scaling differently..
#ifndef __APPLE__
  {
    // scale by default monitor's DPI
    float scale = SDLUtil::GetDPIScaleFactor(nullptr);
    window_width = static_cast<u32>(std::round(static_cast<float>(window_width) * scale));
    window_height = static_cast<u32>(std::round(static_cast<float>(window_height) * scale));
  }
#endif

  m_window = SDL_CreateWindow(GetWindowTitle(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_width,
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

  // Process events so that we have everything sorted out before creating a child window for the GL context (X11).
  SDL_PumpEvents();
  return true;
}

void SDLHostInterface::DestroySDLWindow()
{
  SDL_DestroyWindow(m_window);
  m_window = nullptr;
}

bool SDLHostInterface::CreateDisplay()
{
  std::optional<WindowInfo> wi = SDLUtil::GetWindowInfoForSDLWindow(m_window);
  if (!wi.has_value())
  {
    ReportError("Failed to get window info from SDL window");
    return false;
  }

  switch (g_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      m_display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
#ifndef WIN32
    default:
#endif
      m_display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;

#ifdef WIN32
    case GPURenderer::HardwareD3D11:
    default:
      m_display = std::make_unique<FrontendCommon::D3D11HostDisplay>();
      break;
#endif
  }

  Assert(m_display);
  if (!m_display->CreateRenderDevice(wi.value(), g_settings.gpu_adapter, g_settings.gpu_use_debug_device,
                                     g_settings.gpu_threaded_presentation) ||
      !m_display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device,
                                         g_settings.gpu_threaded_presentation))
  {
    ReportError("Failed to create/initialize display render device");
    m_display.reset();
    return false;
  }

  if (!ImGui_ImplSDL2_Init(m_window) || !m_display->CreateImGuiContext())
  {
    ReportError("Failed to initialize ImGui SDL2 wrapper");
    ImGui_ImplSDL2_Shutdown();
    m_display->DestroyRenderDevice();
    m_display.reset();
    return false;
  }

  if (!FullscreenUI::Initialize(this, m_settings_interface.get()) || !m_display->UpdateImGuiFontTexture())
  {
    ReportError("Failed to initialize fonts/fullscreen UI");
    FullscreenUI::Shutdown();
    m_display->DestroyImGuiContext();
    ImGui_ImplSDL2_Shutdown();
    m_display->DestroyRenderDevice();
    m_display.reset();
    return false;
  }

  m_fullscreen_ui_enabled = true;
  return true;
}

void SDLHostInterface::DestroyDisplay()
{
  FullscreenUI::Shutdown();
  m_display->DestroyImGuiContext();
  m_display->DestroyRenderDevice();
  m_display.reset();
}

void SDLHostInterface::CreateImGuiContext()
{
  const float framebuffer_scale = SDLUtil::GetDPIScaleFactor(m_window);

  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  ImGui::GetIO().DisplayFramebufferScale.x = framebuffer_scale;
  ImGui::GetIO().DisplayFramebufferScale.y = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

  ImGui::StyleColorsDarker();
}

void SDLHostInterface::UpdateFramebufferScale()
{
  ImGuiIO& io = ImGui::GetIO();
  const float framebuffer_scale = SDLUtil::GetDPIScaleFactor(m_window);
  if (framebuffer_scale != io.DisplayFramebufferScale.x)
  {
    io.DisplayFramebufferScale = ImVec2(framebuffer_scale, framebuffer_scale);
    ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);
  }

  if (ImGuiFullscreen::UpdateLayoutScale())
  {
    if (ImGuiFullscreen::UpdateFonts())
    {
      if (!m_display->UpdateImGuiFontTexture())
        Panic("Failed to update font texture");
    }
  }
}

bool SDLHostInterface::AcquireHostDisplay()
{
  // Handle renderer switch if required.
  const HostDisplay::RenderAPI render_api = m_display->GetRenderAPI();
  bool needs_switch = false;
  switch (g_settings.gpu_renderer)
  {
#ifdef WIN32
    case GPURenderer::HardwareD3D11:
      needs_switch = (render_api != HostDisplay::RenderAPI::D3D11);
      break;
#endif

    case GPURenderer::HardwareVulkan:
      needs_switch = (render_api != HostDisplay::RenderAPI::Vulkan);
      break;

    case GPURenderer::HardwareOpenGL:
      needs_switch = (render_api != HostDisplay::RenderAPI::OpenGL && render_api != HostDisplay::RenderAPI::OpenGLES);
      break;

    case GPURenderer::Software:
    default:
      needs_switch = false;
      break;
  }

  if (needs_switch)
  {
    ImGui::EndFrame();
    DestroyDisplay();

    // We need to recreate the window, otherwise bad things happen...
    DestroySDLWindow();
    if (!CreateSDLWindow())
      Panic("Failed to recreate SDL window on GPU renderer switch");

    if (!CreateDisplay())
      Panic("Failed to recreate display on GPU renderer switch");

    ImGui::NewFrame();
  }

  if (!CreateHostDisplayResources())
    return false;

  return true;
}

void SDLHostInterface::ReleaseHostDisplay()
{
  ReleaseHostDisplayResources();

  // restore vsync, since we don't want to burn cycles at the menu
  m_display->SetVSync(true);
}

std::optional<CommonHostInterface::HostKeyCode> SDLHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  const std::optional<u32> code = SDLKeyNames::ParseKeyString(key_code);
  if (!code)
    return std::nullopt;

  return static_cast<HostKeyCode>(*code);
}

void SDLHostInterface::UpdateInputMap()
{
  CommonHostInterface::UpdateInputMap(*m_settings_interface.get());
}

void SDLHostInterface::OnSystemCreated()
{
  CommonHostInterface::OnSystemCreated();
  FullscreenUI::SystemCreated();
}

void SDLHostInterface::OnSystemPaused(bool paused)
{
  CommonHostInterface::OnSystemPaused(paused);
  FullscreenUI::SystemPaused(paused);
}

void SDLHostInterface::OnSystemDestroyed()
{
  CommonHostInterface::OnSystemDestroyed();
  ReportFormattedMessage("System shut down.");
  FullscreenUI::SystemDestroyed();
}

void SDLHostInterface::OnRunningGameChanged()
{
  CommonHostInterface::OnRunningGameChanged();

  Settings old_settings(std::move(g_settings));
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::ApplyGameSettings(true);
  CommonHostInterface::FixIncompatibleSettings(true);
  CheckForSettingsChanges(old_settings);

  if (!System::GetRunningTitle().empty())
    SDL_SetWindowTitle(m_window, System::GetRunningTitle().c_str());
  else
    SDL_SetWindowTitle(m_window, GetWindowTitle());
}

void SDLHostInterface::RequestExit()
{
  m_quit_request = true;
}

void SDLHostInterface::RunLater(std::function<void()> callback)
{
  SDL_Event ev = {};
  ev.type = SDL_USEREVENT;
  ev.user.code = m_run_later_event_id;
  ev.user.data1 = new std::function<void()>(std::move(callback));
  SDL_PushEvent(&ev);
}

void SDLHostInterface::ApplySettings(bool display_osd_messages)
{
  Settings old_settings(std::move(g_settings));
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::ApplyGameSettings(display_osd_messages);
  CommonHostInterface::FixIncompatibleSettings(display_osd_messages);
  CheckForSettingsChanges(old_settings);
}

bool SDLHostInterface::IsFullscreen() const
{
  return m_fullscreen;
}

bool SDLHostInterface::SetFullscreen(bool enabled)
{
  if (m_fullscreen == enabled)
    return true;

  SDL_SetWindowFullscreen(m_window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

  int window_width, window_height;
  SDL_GetWindowSize(m_window, &window_width, &window_height);
  m_display->ResizeRenderWindow(window_width, window_height);

  if (!System::IsShutdown())
    g_gpu->UpdateResolutionScale();

  m_fullscreen = enabled;
  return true;
}

std::unique_ptr<SDLHostInterface> SDLHostInterface::Create()
{
  return std::make_unique<SDLHostInterface>();
}

bool SDLHostInterface::Initialize()
{
  if (!CommonHostInterface::Initialize())
    return false;

  // Change to the user directory so that all default/relative paths in the config are after this.
  if (!FileSystem::SetWorkingDirectory(m_user_directory.c_str()))
    Log_ErrorPrintf("Failed to set working directory to '%s'", m_user_directory.c_str());

  if (!CreateSDLWindow())
  {
    Log_ErrorPrintf("Failed to create SDL window");
    return false;
  }

  CreateImGuiContext();
  if (!CreateDisplay())
  {
    Log_ErrorPrintf("Failed to create host display");
    return false;
  }

  // process events to pick up controllers before updating input map
  ProcessEvents();
  UpdateInputMap();
  return true;
}

void SDLHostInterface::Shutdown()
{
  DestroySystem();

  CommonHostInterface::Shutdown();

  if (m_display)
  {
    DestroyDisplay();
    ImGui::DestroyContext();
  }

  if (m_window)
    DestroySDLWindow();
}

std::string SDLHostInterface::GetStringSettingValue(const char* section, const char* key,
                                                    const char* default_value /*= ""*/)
{
  return m_settings_interface->GetStringValue(section, key, default_value);
}

bool SDLHostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
  return m_settings_interface->GetBoolValue(section, key, default_value);
}

int SDLHostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /* = 0 */)
{
  return m_settings_interface->GetIntValue(section, key, default_value);
}

float SDLHostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /* = 0.0f */)
{
  return m_settings_interface->GetFloatValue(section, key, default_value);
}

bool SDLHostInterface::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  if (new_window_width <= 0 || new_window_height <= 0 || m_fullscreen)
    return false;

  // use imgui scale as the dpr
  const float dpi_scale = ImGui::GetIO().DisplayFramebufferScale.x;
  const s32 scaled_width =
    std::max<s32>(static_cast<s32>(std::ceil(static_cast<float>(new_window_width) * dpi_scale)), 1);
  const s32 scaled_height = std::max<s32>(
    static_cast<s32>(std::ceil(static_cast<float>(new_window_height) * dpi_scale)) + m_display->GetDisplayTopMargin(),
    1);

  SDL_SetWindowSize(m_window, scaled_width, scaled_height);

  s32 window_width, window_height;
  SDL_GetWindowSize(m_window, &window_width, &window_height);
  m_display->ResizeRenderWindow(window_width, window_height);

  UpdateFramebufferScale();

  if (!System::IsShutdown())
    g_gpu->UpdateResolutionScale();

  return true;
}

void SDLHostInterface::LoadSettings()
{
  // Settings need to be loaded prior to creating the window for OpenGL bits.
  m_settings_interface = std::make_unique<INISettingsInterface>(GetSettingsFileName());
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::FixIncompatibleSettings(false);
}

void SDLHostInterface::ReportError(const char* message)
{
  const bool was_fullscreen = IsFullscreen();
  if (was_fullscreen)
    SetFullscreen(false);

  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DuckStation", message, m_window);

  if (was_fullscreen)
    SetFullscreen(true);
}

void SDLHostInterface::ReportMessage(const char* message)
{
  AddOSDMessage(message, 2.0f);
}

bool SDLHostInterface::ConfirmMessage(const char* message)
{
  const bool was_fullscreen = IsFullscreen();
  if (was_fullscreen)
    SetFullscreen(false);

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
  const bool result = (button_id == 0);

  if (was_fullscreen)
    SetFullscreen(true);

  return result;
}

void SDLHostInterface::HandleSDLEvent(const SDL_Event* event)
{
  ImGui_ImplSDL2_ProcessEvent(event);

  if (m_controller_interface &&
      static_cast<SDLControllerInterface*>(m_controller_interface.get())->ProcessSDLEvent(event))
  {
    return;
  }

  switch (event->type)
  {
    case SDL_WINDOWEVENT:
    {
      if (event->window.event == SDL_WINDOWEVENT_RESIZED)
      {
        m_display->ResizeRenderWindow(event->window.data1, event->window.data2);
        UpdateFramebufferScale();

        if (!System::IsShutdown())
          g_gpu->UpdateResolutionScale();
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
      if (!ImGui::GetIO().WantCaptureKeyboard && event->key.repeat == 0)
      {
        const HostKeyCode code = static_cast<HostKeyCode>(SDLKeyNames::KeyEventToInt(event));
        const bool pressed = (event->type == SDL_KEYDOWN);
        HandleHostKeyEvent(code, pressed);
      }
    }
    break;

    case SDL_MOUSEMOTION:
    {
      m_display->SetMousePosition(event->motion.x, event->motion.y);
    }
    break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
      if (!ImGui::GetIO().WantCaptureMouse)
      {
        const s32 button = static_cast<s32>(ZeroExtend32(event->button.button));
        const bool pressed = (event->type == SDL_MOUSEBUTTONDOWN);
        HandleHostMouseEvent(button, pressed);
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

void SDLHostInterface::PollAndUpdate()
{
  ProcessEvents();
  CommonHostInterface::PollAndUpdate();
}

void SDLHostInterface::ProcessEvents()
{
  for (;;)
  {
    SDL_Event ev;
    if (SDL_PollEvent(&ev))
      HandleSDLEvent(&ev);
    else
      break;
  }
}

void SDLHostInterface::Run()
{
  while (!m_quit_request)
  {
    PollAndUpdate();

    if (System::IsRunning())
    {
      if (m_display_all_frames)
        System::RunFrame();
      else
        System::RunFrames();

      UpdateControllerRumble();
      if (m_frame_step_request)
      {
        m_frame_step_request = false;
        PauseSystem(true);
      }
    }

    // rendering
    {
      ImGui_ImplSDL2_NewFrame();
      FullscreenUI::SetImGuiNavInputs();
      ImGui::NewFrame();
      DrawImGuiWindows();
      ImGui::Render();
      ImGui::EndFrame();

      m_display->Render();

      if (System::IsRunning())
      {
        System::UpdatePerformanceCounters();

        if (m_throttler_enabled)
          System::Throttle();
      }
    }
  }

  // Save state on exit so it can be resumed
  if (!System::IsShutdown())
  {
    if (g_settings.save_state_on_exit)
      SaveResumeSaveState();
    DestroySystem();
  }
}

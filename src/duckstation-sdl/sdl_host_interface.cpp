#include "sdl_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/sdl_audio_stream.h"
#include "frontend-common/sdl_controller_interface.h"
#include "frontend-common/vulkan_host_display.h"
#include "imgui_impl_sdl.h"
#include "scmversion/scmversion.h"
#include "sdl_key_names.h"
#include "sdl_util.h"
#include <cinttypes>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <nfd.h>
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
  return "DuckStation SDL/ImGui Frontend";
}

ALWAYS_INLINE static TinyString GetWindowTitle()
{
  return TinyString::FromFormat("DuckStation %s (%s)", g_scm_tag_str, g_scm_branch_str);
}

bool SDLHostInterface::CreateSDLWindow()
{
  static constexpr u32 DEFAULT_WINDOW_WIDTH = 900;
  static constexpr u32 DEFAULT_WINDOW_HEIGHT = 700;

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

  std::unique_ptr<HostDisplay> display;
  switch (m_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
#ifndef WIN32
    default:
#endif
      display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;

#ifdef WIN32
    case GPURenderer::HardwareD3D11:
    default:
      display = std::make_unique<FrontendCommon::D3D11HostDisplay>();
      break;
#endif
  }

  Assert(display);
  if (!display->CreateRenderDevice(wi.value(), m_settings.gpu_adapter, m_settings.gpu_use_debug_device) ||
      !display->InitializeRenderDevice(GetShaderCacheBasePath(), m_settings.gpu_use_debug_device))
  {
    ReportError("Failed to create/initialize display render device");
    return false;
  }

  bool imgui_result;
  switch (display->GetRenderAPI())
  {
#ifdef WIN32
    case HostDisplay::RenderAPI::D3D11:
      imgui_result = ImGui_ImplSDL2_InitForD3D(m_window);
      break;
#endif

    case HostDisplay::RenderAPI::Vulkan:
      imgui_result = ImGui_ImplSDL2_InitForVulkan(m_window);
      break;

    case HostDisplay::RenderAPI::OpenGL:
    case HostDisplay::RenderAPI::OpenGLES:
      imgui_result = ImGui_ImplSDL2_InitForOpenGL(m_window, nullptr);
      break;

    default:
      imgui_result = true;
      break;
  }
  if (!imgui_result)
  {
    ReportError("Failed to initialize ImGui SDL2 wrapper");
    return false;
  }

  m_app_icon_texture =
    display->CreateTexture(APP_ICON_WIDTH, APP_ICON_HEIGHT, APP_ICON_DATA, APP_ICON_WIDTH * sizeof(u32));
  if (!m_app_icon_texture)
    return false;

  display->SetDisplayTopMargin(m_fullscreen ? 0 : static_cast<int>(20.0f * ImGui::GetIO().DisplayFramebufferScale.x));
  m_display = std::move(display);
  return true;
}

void SDLHostInterface::DestroyDisplay()
{
  m_app_icon_texture.reset();
  m_display->DestroyRenderDevice();
  m_display.reset();
}

void SDLHostInterface::CreateImGuiContext()
{
  const float framebuffer_scale = SDLUtil::GetDPIScaleFactor(m_window);

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
  const float framebuffer_scale = SDLUtil::GetDPIScaleFactor(m_window);
  ImGui::GetIO().DisplayFramebufferScale.x = framebuffer_scale;
  ImGui::GetIO().DisplayFramebufferScale.y = framebuffer_scale;
}

bool SDLHostInterface::AcquireHostDisplay()
{
  // Handle renderer switch if required.
  const HostDisplay::RenderAPI render_api = m_display->GetRenderAPI();
  bool needs_switch = false;
  switch (m_settings.gpu_renderer)
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

  return true;
}

void SDLHostInterface::ReleaseHostDisplay()
{
  if (m_fullscreen)
    SetFullscreen(false);

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

std::unique_ptr<ControllerInterface> SDLHostInterface::CreateControllerInterface()
{
  return std::make_unique<SDLControllerInterface>();
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

  ClearImGuiFocus();
}

void SDLHostInterface::OnSystemPaused(bool paused)
{
  CommonHostInterface::OnSystemPaused(paused);

  if (!paused)
    ClearImGuiFocus();
}

void SDLHostInterface::OnSystemDestroyed()
{
  CommonHostInterface::OnSystemDestroyed();
}

void SDLHostInterface::OnRunningGameChanged()
{
  CommonHostInterface::OnRunningGameChanged();

  if (m_system && !m_system->GetRunningTitle().empty())
    SDL_SetWindowTitle(m_window, m_system->GetRunningTitle().c_str());
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

void SDLHostInterface::SaveAndUpdateSettings()
{
  m_settings_copy.Save(*m_settings_interface.get());

  Settings old_settings(std::move(m_settings));
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CheckForSettingsChanges(old_settings);

  m_settings_interface->Save();
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

  // We set the margin only in windowed mode, the menu bar is not drawn fullscreen.
  m_display->SetDisplayTopMargin(enabled ? 0 : static_cast<int>(20.0f * ImGui::GetIO().DisplayFramebufferScale.x));

  int window_width, window_height;
  SDL_GetWindowSize(m_window, &window_width, &window_height);
  m_display->ResizeRenderWindow(window_width, window_height);
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

  ImGui::NewFrame();

  // process events to pick up controllers before updating input map
  ProcessEvents();
  UpdateInputMap();
  return true;
}

void SDLHostInterface::Shutdown()
{
  DestroySystem();

  if (m_display)
  {
    DestroyDisplay();
    ImGui::DestroyContext();
  }

  if (m_window)
    DestroySDLWindow();

  CommonHostInterface::Shutdown();
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

void SDLHostInterface::LoadSettings()
{
  // Settings need to be loaded prior to creating the window for OpenGL bits.
  m_settings_interface = std::make_unique<INISettingsInterface>(GetSettingsFileName());
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  m_settings_copy = m_settings;
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
  CommonHostInterface::PollAndUpdate();
  ProcessEvents();
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

void SDLHostInterface::DrawImGuiWindows()
{
  if (!m_fullscreen)
    DrawMainMenuBar();

  CommonHostInterface::DrawImGuiWindows();

  if (!m_system)
    DrawPoweredOffWindow();

  if (m_settings_window_open)
    DrawSettingsWindow();

  if (m_about_window_open)
    DrawAboutWindow();

  ImGui::Render();
}

void SDLHostInterface::DrawMainMenuBar()
{
  if (!ImGui::BeginMainMenuBar())
    return;

  const bool system_enabled = static_cast<bool>(m_system);

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

    if (ImGui::MenuItem("Remove Disc", nullptr, false, system_enabled))
    {
      RunLater([this]() { m_system->RemoveMedia(); });
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

  if (ImGui::BeginMenu("Debug"))
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
      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - (420.0f * framebuffer_scale));
      ImGui::Text("Average: %.2fms", m_system->GetAverageFrameTime());

      ImGui::SetCursorPosX(ImGui::GetIO().DisplaySize.x - (310.0f * framebuffer_scale));
      ImGui::Text("Worst: %.2fms", m_system->GetWorstFrameTime());

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
  settings_changed |= ImGui::MenuItem("Disable Interlacing", nullptr, &m_settings_copy.gpu_disable_interlacing);
  settings_changed |= ImGui::MenuItem("Widescreen Hack", nullptr, &m_settings_copy.gpu_widescreen_hack);
  settings_changed |= ImGui::MenuItem("Display Linear Filtering", nullptr, &m_settings_copy.display_linear_filtering);
  settings_changed |= ImGui::MenuItem("Display Integer Scaling", nullptr, &m_settings_copy.display_integer_scaling);

  ImGui::Separator();

  if (ImGui::MenuItem("Dump Audio", nullptr, IsDumpingAudio(), HasSystem()))
  {
    if (!IsDumpingAudio())
      StartDumpingAudio();
    else
      StopDumpingAudio();
  }

  if (ImGui::MenuItem("Save Screenshot"))
    RunLater([this]() { SaveScreenshot(); });

  if (settings_changed)
    RunLater([this]() { SaveAndUpdateSettings(); });
}

void SDLHostInterface::DrawDebugMenu()
{
  Settings::DebugSettings& debug_settings = m_settings.debugging;
  bool settings_changed = false;

  if (ImGui::BeginMenu("Log Level"))
  {
    for (u32 i = LOGLEVEL_NONE; i < LOGLEVEL_COUNT; i++)
    {
      if (ImGui::MenuItem(Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i)), nullptr,
                          m_settings.log_level == static_cast<LOGLEVEL>(i)))
      {
        m_settings_copy.log_level = static_cast<LOGLEVEL>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("Log To Console", nullptr, &m_settings_copy.log_to_console);
  settings_changed |= ImGui::MenuItem("Log To Debug", nullptr, &m_settings_copy.log_to_debug);
  settings_changed |= ImGui::MenuItem("Log To File", nullptr, &m_settings_copy.log_to_file);

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Dump CPU to VRAM Copies", nullptr, &debug_settings.dump_cpu_to_vram_copies);
  settings_changed |= ImGui::MenuItem("Dump VRAM to CPU Copies", nullptr, &debug_settings.dump_vram_to_cpu_copies);

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("Show VRAM", nullptr, &debug_settings.show_vram);
  settings_changed |= ImGui::MenuItem("Show GPU State", nullptr, &debug_settings.show_gpu_state);
  settings_changed |= ImGui::MenuItem("Show CDROM State", nullptr, &debug_settings.show_cdrom_state);
  settings_changed |= ImGui::MenuItem("Show SPU State", nullptr, &debug_settings.show_spu_state);
  settings_changed |= ImGui::MenuItem("Show Timers State", nullptr, &debug_settings.show_timers_state);
  settings_changed |= ImGui::MenuItem("Show MDEC State", nullptr, &debug_settings.show_mdec_state);

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
    RunLater([this]() { SaveAndUpdateSettings(); });
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
        settings_changed |=
          ImGui::Checkbox("Load Devices From Save States", &m_settings_copy.load_devices_from_save_states);
      }

      ImGui::NewLine();
      if (DrawSettingsSectionHeader("CDROM Emulation"))
      {
        settings_changed |= ImGui::Checkbox("Use Read Thread (Asynchronous)", &m_settings_copy.cdrom_read_thread);
        settings_changed |= ImGui::Checkbox("Enable Region Check", &m_settings_copy.cdrom_region_check);
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
        settings_changed |= ImGui::Checkbox("Start Dumping On Boot", &m_settings_copy.audio_dump_on_boot);
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
                TinyString::FromFormat("##controller_type%d", i), &controller_type,
                [](void*, int index, const char** out_text) {
                  *out_text = Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(index));
                  return true;
                },
                nullptr, static_cast<int>(ControllerType::Count)))
          {
            m_settings_copy.controller_types[i] = static_cast<ControllerType>(controller_type);
            settings_changed = true;
          }

          ImGui::Text("Memory Card Type:");
          ImGui::SameLine(indent);

          int memory_card_type = static_cast<int>(m_settings_copy.memory_card_types[i]);
          if (ImGui::Combo(
                TinyString::FromFormat("##memory_card_type%d", i), &memory_card_type,
                [](void*, int index, const char** out_text) {
                  *out_text = Settings::GetMemoryCardTypeDisplayName(static_cast<MemoryCardType>(index));
                  return true;
                },
                nullptr, static_cast<int>(MemoryCardType::Count)))
          {
            m_settings_copy.memory_card_types[i] = static_cast<MemoryCardType>(memory_card_type);
            settings_changed = true;
          }

          ImGui::Text("Shared Card Path:");
          ImGui::SameLine(indent);

          std::string* path_ptr = &m_settings_copy.memory_card_paths[i];
          std::snprintf(buf, sizeof(buf), "##memcard_%c_path", 'a' + i);
          settings_changed |= DrawFileChooser(buf, path_ptr);
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
        ImGui::Text("Aspect Ratio:");
        ImGui::SameLine(indent);
        int display_aspect_ratio = static_cast<int>(m_settings_copy.display_aspect_ratio);
        if (ImGui::Combo(
              "##display_aspect_ratio", &display_aspect_ratio,
              [](void*, int index, const char** out_text) {
                *out_text = Settings::GetDisplayAspectRatioName(static_cast<DisplayAspectRatio>(index));
                return true;
              },
              nullptr, static_cast<int>(DisplayAspectRatio::Count)))
        {
          m_settings_copy.display_aspect_ratio = static_cast<DisplayAspectRatio>(display_aspect_ratio);
          settings_changed = true;
        }

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
        settings_changed |= ImGui::Checkbox("Integer Scaling", &m_settings_copy.display_integer_scaling);
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
        settings_changed |= ImGui::Checkbox("Disable Interlacing", &m_settings_copy.gpu_disable_interlacing);
        settings_changed |= ImGui::Checkbox("Force NTSC Timings", &m_settings_copy.gpu_force_ntsc_timings);
        settings_changed |= ImGui::Checkbox("Widescreen Hack", &m_settings_copy.gpu_widescreen_hack);
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Advanced"))
    {
      ImGui::Text("These options are tweakable to improve performance/game compatibility.");
      ImGui::Text("Use at your own risk, modified values will not be supported.");
      ImGui::NewLine();

      ImGui::Text("DMA Max Slice Ticks:");
      ImGui::SameLine(indent);

      int dma_max_slice_ticks = static_cast<int>(m_settings_copy.dma_max_slice_ticks);
      if (ImGui::SliderInt("##dma_max_slice_ticks", &dma_max_slice_ticks, 100, 10000))
      {
        m_settings_copy.dma_max_slice_ticks = dma_max_slice_ticks;
        settings_changed = true;
      }

      ImGui::Text("DMA Halt Ticks:");
      ImGui::SameLine(indent);

      int dma_halt_ticks = static_cast<int>(m_settings_copy.dma_halt_ticks);
      if (ImGui::SliderInt("##dma_halt_ticks", &dma_halt_ticks, 100, 10000))
      {
        m_settings_copy.dma_halt_ticks = dma_halt_ticks;
        settings_changed = true;
      }

      ImGui::Text("FIFO Size:");
      ImGui::SameLine(indent);

      int gpu_fifo_size = static_cast<int>(m_settings_copy.gpu_fifo_size);
      if (ImGui::SliderInt("##gpu_fifo_size", &gpu_fifo_size, 16, GPU::MAX_FIFO_SIZE))
      {
        m_settings_copy.gpu_fifo_size = gpu_fifo_size;
        settings_changed = true;
      }

      ImGui::Text("Max Run-Ahead:");
      ImGui::SameLine(indent);

      int gpu_max_run_ahead = static_cast<int>(m_settings_copy.gpu_max_run_ahead);
      if (ImGui::SliderInt("##gpu_max_run_ahead", &gpu_max_run_ahead, 0, 1000))
      {
        m_settings_copy.gpu_max_run_ahead = gpu_max_run_ahead;
        settings_changed = true;
      }

      if (ImGui::Button("Reset"))
      {
        m_settings_copy.dma_max_slice_ticks = static_cast<TickCount>(Settings::DEFAULT_DMA_MAX_SLICE_TICKS);
        m_settings_copy.dma_halt_ticks = static_cast<TickCount>(Settings::DEFAULT_DMA_HALT_TICKS);
        m_settings_copy.gpu_fifo_size = Settings::DEFAULT_GPU_FIFO_SIZE;
        m_settings_copy.gpu_max_run_ahead = static_cast<TickCount>(Settings::DEFAULT_GPU_MAX_RUN_AHEAD);
        settings_changed = true;
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();

  if (settings_changed)
    RunLater([this]() { SaveAndUpdateSettings(); });
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

  ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - (50.0f * framebuffer_scale));
  bool result = ImGui::InputText(label, path);
  ImGui::SameLine();

  ImGui::SetNextItemWidth(50.0f * framebuffer_scale);
  if (ImGui::Button("..."))
  {
    nfdchar_t* out_path = nullptr;
    nfdresult_t nfd_result = NFD_OpenDialog(filter, path->c_str(), &out_path);
    if (nfd_result == NFD_ERROR)
    {
      // try without the path - it might not be valid
      nfd_result = NFD_OpenDialog(filter, nullptr, &out_path);
    }
    if (nfd_result == NFD_OKAY)
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
  if (!NFD_OpenDialog("bin,img,cue,chd,exe,psexe,psf", nullptr, &path) || !path || std::strlen(path) == 0)
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
  if (!NFD_OpenDialog("bin,img,cue,chd", nullptr, &path) || !path || std::strlen(path) == 0)
    return;

  if (m_system->InsertMedia(path))
    AddFormattedOSDMessage(2.0f, "Switched CD to '%s'", path);
  else
    AddOSDMessage("Failed to switch CD. The log may contain further information.");

  m_system->ResetPerformanceCounters();
}

void SDLHostInterface::Run()
{
  while (!m_quit_request)
  {
    PollAndUpdate();

    if (m_system && !m_paused)
    {
      m_system->RunFrame();
      UpdateControllerRumble();
      if (m_frame_step_request)
      {
        m_frame_step_request = false;
        PauseSystem(true);
      }
    }

    // rendering
    {
      DrawImGuiWindows();

      if (m_system)
        m_system->GetGPU()->ResetGraphicsAPIState();

      m_display->Render();
      ImGui_ImplSDL2_NewFrame(m_window);
      ImGui::NewFrame();

      if (m_system)
      {
        m_system->GetGPU()->RestoreGraphicsAPIState();
        m_system->UpdatePerformanceCounters();

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

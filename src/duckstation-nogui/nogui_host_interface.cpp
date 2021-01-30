#include "nogui_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/controller_interface.h"
#include "frontend-common/fullscreen_ui.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_fullscreen.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/vulkan_host_display.h"
#include <cinttypes>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
Log_SetChannel(NoGUIHostInterface);

#ifdef WIN32
#include "frontend-common/d3d11_host_display.h"
#endif

NoGUIHostInterface::NoGUIHostInterface() = default;

NoGUIHostInterface::~NoGUIHostInterface() = default;

const char* NoGUIHostInterface::GetFrontendName() const
{
  return "DuckStation NoGUI Frontend";
}

bool NoGUIHostInterface::Initialize()
{
  // TODO: Make command line.
  m_fullscreen_ui_enabled = true;

  // we're always in batch mode for now
  m_command_line_flags.batch_mode = !m_fullscreen_ui_enabled;

  if (!CommonHostInterface::Initialize())
    return false;

  CreateImGuiContext();

  if (!CreatePlatformWindow())
  {
    Log_ErrorPrintf("Failed to create platform window");
    ImGui::DestroyContext();
    return false;
  }

  if (!CreateDisplay())
  {
    Log_ErrorPrintf("Failed to create host display");
    DestroyPlatformWindow();
    ImGui::DestroyContext();
    return false;
  }

  // process events to pick up controllers before updating input map
  PollAndUpdate();
  UpdateInputMap();
  return true;
}

void NoGUIHostInterface::Shutdown()
{
  DestroySystem();

  CommonHostInterface::Shutdown();

  if (m_display)
  {
    DestroyDisplay();
    ImGui::DestroyContext();
  }

  DestroyPlatformWindow();
}

std::string NoGUIHostInterface::GetStringSettingValue(const char* section, const char* key,
                                                      const char* default_value /*= ""*/)
{
  return m_settings_interface->GetStringValue(section, key, default_value);
}

bool NoGUIHostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
  return m_settings_interface->GetBoolValue(section, key, default_value);
}

int NoGUIHostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /* = 0 */)
{
  return m_settings_interface->GetIntValue(section, key, default_value);
}

float NoGUIHostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /* = 0.0f */)
{
  return m_settings_interface->GetFloatValue(section, key, default_value);
}

void NoGUIHostInterface::LoadSettings()
{
  m_settings_interface = std::make_unique<INISettingsInterface>(GetSettingsFileName());
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::FixIncompatibleSettings(false);
}

void NoGUIHostInterface::UpdateInputMap()
{
  CommonHostInterface::UpdateInputMap(*m_settings_interface.get());
}

void NoGUIHostInterface::ApplySettings(bool display_osd_messages)
{
  Settings old_settings(std::move(g_settings));
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::ApplyGameSettings(display_osd_messages);
  CommonHostInterface::FixIncompatibleSettings(display_osd_messages);
  CheckForSettingsChanges(old_settings);
}

void NoGUIHostInterface::CreateImGuiContext()
{
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
}

void NoGUIHostInterface::OnPlatformWindowResized(u32 new_width, u32 new_height, float new_scale)
{
  if (new_scale != ImGui::GetIO().DisplayFramebufferScale.x)
  {
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(new_scale, new_scale);
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDarker();
    ImGui::GetStyle().ScaleAllSizes(new_scale);
  }

  if (ImGuiFullscreen::UpdateLayoutScale())
  {
    if (ImGuiFullscreen::UpdateFonts())
    {
      if (!m_display->UpdateImGuiFontTexture())
        Panic("Failed to update font texture");
    }
  }

  if (!System::IsShutdown())
    g_gpu->UpdateResolutionScale();
}

bool NoGUIHostInterface::CreateDisplay()
{
  std::optional<WindowInfo> wi = GetPlatformWindowInfo();
  if (!wi)
  {
    ReportError("Failed to get platform window info");
    return false;
  }

  // imgui init from window
  ImGui::GetIO().DisplayFramebufferScale.x = wi->surface_scale;
  ImGui::GetIO().DisplayFramebufferScale.y = wi->surface_scale;
  ImGui::GetStyle() = ImGuiStyle();
  ImGui::GetStyle().ScaleAllSizes(wi->surface_scale);
  ImGui::StyleColorsDarker();

  Assert(!m_display);
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

  if (!m_display->CreateRenderDevice(wi.value(), g_settings.gpu_adapter, g_settings.gpu_use_debug_device,
                                     g_settings.gpu_threaded_presentation) ||
      !m_display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device,
                                         g_settings.gpu_threaded_presentation))
  {
    ReportError("Failed to create/initialize display render device");
    m_display->DestroyRenderDevice();
    m_display.reset();
    return false;
  }

  if (!m_display->CreateImGuiContext() ||
      (m_fullscreen_ui_enabled && !FullscreenUI::Initialize(this, m_settings_interface.get())) ||
      !m_display->UpdateImGuiFontTexture())
  {
    ReportError("Failed to initialize imgui/fonts/fullscreen UI");
    if (m_fullscreen_ui_enabled)
      FullscreenUI::Shutdown();

    m_display->DestroyImGuiContext();
    m_display->DestroyRenderDevice();
    m_display.reset();
    return false;
  }

  return true;
}

void NoGUIHostInterface::DestroyDisplay()
{
  if (m_fullscreen_ui_enabled)
    FullscreenUI::Shutdown();

  if (m_display)
  {
    m_display->DestroyImGuiContext();
    m_display->DestroyRenderDevice();
  }
  m_display.reset();
}

bool NoGUIHostInterface::AcquireHostDisplay()
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
    DestroyPlatformWindow();
    if (!CreatePlatformWindow())
      Panic("Failed to recreate platform window on GPU renderer switch");

    if (!CreateDisplay())
      Panic("Failed to recreate display on GPU renderer switch");

    ImGui::NewFrame();
  }

  if (!CreateHostDisplayResources())
    return false;

  return true;
}

void NoGUIHostInterface::ReleaseHostDisplay()
{
  ReleaseHostDisplayResources();

  // restore vsync, since we don't want to burn cycles at the menu
  m_display->SetVSync(true);
}

void NoGUIHostInterface::OnSystemCreated()
{
  CommonHostInterface::OnSystemCreated();
  if (m_fullscreen_ui_enabled)
    FullscreenUI::SystemCreated();
}

void NoGUIHostInterface::OnSystemPaused(bool paused)
{
  CommonHostInterface::OnSystemPaused(paused);
  if (m_fullscreen_ui_enabled)
    FullscreenUI::SystemPaused(paused);
}

void NoGUIHostInterface::OnSystemDestroyed()
{
  CommonHostInterface::OnSystemDestroyed();
  ReportFormattedMessage("System shut down.");
  if (m_fullscreen_ui_enabled)
    FullscreenUI::SystemDestroyed();
}

void NoGUIHostInterface::OnRunningGameChanged()
{
  CommonHostInterface::OnRunningGameChanged();

  // TODO: Move to common
  if (g_settings.apply_game_settings)
    ApplySettings(true);
}

void NoGUIHostInterface::RequestExit()
{
  m_quit_request = true;
}

void NoGUIHostInterface::PollAndUpdate()
{
  CommonHostInterface::PollAndUpdate();

  if (m_controller_interface)
    m_controller_interface->PollEvents();
}

void NoGUIHostInterface::Run()
{
  while (!m_quit_request)
  {
    RunCallbacks();
    PollAndUpdate();
    if (m_fullscreen_ui_enabled)
      FullscreenUI::SetImGuiNavInputs();

    ImGui::NewFrame();

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

void NoGUIHostInterface::RunLater(std::function<void()> callback)
{
  std::unique_lock<std::mutex> lock(m_queued_callbacks_lock);
  m_queued_callbacks.push_back(std::move(callback));
}

void NoGUIHostInterface::RunCallbacks()
{
  std::unique_lock<std::mutex> lock(m_queued_callbacks_lock);

  while (!m_queued_callbacks.empty())
  {
    auto callback = std::move(m_queued_callbacks.front());
    m_queued_callbacks.pop_front();
    lock.unlock();
    callback();
    lock.lock();
  }
}
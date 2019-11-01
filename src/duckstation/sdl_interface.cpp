#include "sdl_interface.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Error.h"
#include "YBaseLib/Log.h"
#include "core/cdrom.h"
#include "core/digital_controller.h"
#include "core/dma.h"
#include "core/gpu.h"
#include "core/mdec.h"
#include "core/memory_card.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/timers.h"
#include "icon.h"
#include "sdl_audio_stream.h"
#include <cinttypes>
#include <glad.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl.h>
#include <nfd.h>
Log_SetChannel(SDLInterface);

static constexpr std::array<std::pair<Settings::GPURenderer, const char*>, 2> s_gpu_renderer_names = {
  {{Settings::GPURenderer::HardwareOpenGL, "Hardware (OpenGL)"}, {Settings::GPURenderer::Software, "Software"}}};

SDLInterface::SDLInterface() = default;

SDLInterface::~SDLInterface()
{
  CloseGameControllers();

  if (m_gl_context)
  {
    if (m_display_vao != 0)
      glDeleteVertexArrays(1, &m_display_vao);

    m_display_program.Destroy();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_MakeCurrent(nullptr, nullptr);
    SDL_GL_DeleteContext(m_gl_context);
  }

  if (m_window)
    SDL_DestroyWindow(m_window);
}

bool SDLInterface::CreateSDLWindow()
{
  constexpr u32 DEFAULT_WINDOW_WIDTH = 900;
  constexpr u32 DEFAULT_WINDOW_HEIGHT = 700;

  // Create window.
  constexpr u32 window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL;

  m_window = SDL_CreateWindow("DuckStation", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, DEFAULT_WINDOW_WIDTH,
                              DEFAULT_WINDOW_HEIGHT, window_flags);
  if (!m_window)
  {
    Panic("Failed to create window");
    return false;
  }

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

  SDL_GetWindowSize(m_window, &m_window_width, &m_window_height);
  return true;
}

static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                     const GLchar* message, const void* userParam)
{
  switch (severity)
  {
    case GL_DEBUG_SEVERITY_HIGH_KHR:
      Log_InfoPrint(message);
      break;
    case GL_DEBUG_SEVERITY_MEDIUM_KHR:
      Log_WarningPrint(message);
      break;
    case GL_DEBUG_SEVERITY_LOW_KHR:
      Log_InfoPrintf(message);
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      // Log_DebugPrint(message);
      break;
  }
}

bool SDLInterface::CreateGLContext()
{
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  m_gl_context = SDL_GL_CreateContext(m_window);
  if (!m_gl_context || SDL_GL_MakeCurrent(m_window, m_gl_context) != 0 || !gladLoadGL())
  {
    Panic("Failed to create GL context");
    return false;
  }

#if 0
  if (GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }
#endif

  SDL_GL_SetSwapInterval(0);
  return true;
}

bool SDLInterface::CreateImGuiContext()
{
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;

  if (!ImGui_ImplSDL2_InitForOpenGL(m_window, m_gl_context) || !ImGui_ImplOpenGL3_Init())
    return false;

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame(m_window);
  ImGui::NewFrame();
  return true;
}

bool SDLInterface::CreateGLResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
#version 330 core

out vec2 v_tex0;

void main()
{
  v_tex0 = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(v_tex0 * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
  gl_Position.y = -gl_Position.y;
}
)";

  static constexpr char display_fragment_shader[] = R"(
#version 330 core

uniform sampler2D samp0;
uniform vec4 u_src_rect;

in vec2 v_tex0;
out vec4 o_col0;

void main()
{
  vec2 coords = u_src_rect.xy + v_tex0 * u_src_rect.zw;
  o_col0 = texture(samp0, coords);
}
)";

  if (!m_display_program.Compile(fullscreen_quad_vertex_shader, display_fragment_shader))
    return false;

  m_display_program.BindFragData(0, "o_col0");
  if (!m_display_program.Link())
    return false;

  m_display_program.Bind();
  m_display_program.RegisterUniform("u_src_rect");
  m_display_program.RegisterUniform("samp0");
  m_display_program.Uniform1i(1, 0);

  glGenVertexArrays(1, &m_display_vao);

  m_app_icon_texture =
    std::make_unique<GL::Texture>(APP_ICON_WIDTH, APP_ICON_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, APP_ICON_DATA, true);

  // samplers
  glGenSamplers(1, &m_display_nearest_sampler);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenSamplers(1, &m_display_linear_sampler);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  return true;
}

bool SDLInterface::CreateAudioStream()
{
  m_audio_stream = std::make_unique<SDLAudioStream>();
  if (!m_audio_stream->Reconfigure(44100, 2))
  {
    Panic("Failed to open audio stream");
    return false;
  }

  return true;
}

void SDLInterface::UpdateAudioVisualSync()
{
  const bool speed_limiter_enabled = m_speed_limiter_enabled && !m_speed_limiter_temp_disabled;
  const bool audio_sync_enabled = speed_limiter_enabled;
  const bool vsync_enabled = !m_system || (speed_limiter_enabled && m_system->GetSettings().gpu_vsync);
  Log_InfoPrintf("Syncing to %s%s", audio_sync_enabled ? "audio" : "",
                 (speed_limiter_enabled && vsync_enabled) ? " and video" : "");

  m_audio_stream->SetSync(false);

  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  SDL_GL_SetSwapInterval(vsync_enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

void SDLInterface::OpenGameControllers()
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

void SDLInterface::CloseGameControllers()
{
  for (auto& it : m_sdl_controllers)
    SDL_GameControllerClose(it.second);
  m_sdl_controllers.clear();
}

bool SDLInterface::InitializeSystem(const char* filename, const char* exp1_filename)
{
  if (!HostInterface::InitializeSystem(filename, exp1_filename))
  {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "System initialization failed.", m_window);
    return false;
  }

  ConnectDevices();
  UpdateAudioVisualSync();
  m_paused = m_system->GetSettings().start_paused;
  return true;
}

void SDLInterface::ConnectDevices()
{
  m_controller = DigitalController::Create();
  m_system->SetController(0, m_controller);
}

void SDLInterface::ResetPerformanceCounters()
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

void SDLInterface::ShutdownSystem()
{
  m_system.reset();
  m_paused = false;
  m_display_texture = nullptr;
  UpdateAudioVisualSync();
}

std::unique_ptr<SDLInterface> SDLInterface::Create(const char* filename /* = nullptr */,
                                                   const char* exp1_filename /* = nullptr */,
                                                   const char* save_state_filename /* = nullptr */)
{
  std::unique_ptr<SDLInterface> intf = std::make_unique<SDLInterface>();
  if (!intf->CreateSDLWindow() || !intf->CreateGLContext() || !intf->CreateImGuiContext() ||
      !intf->CreateGLResources() || !intf->CreateAudioStream())
  {
    return nullptr;
  }

  intf->OpenGameControllers();

  const bool boot = (filename != nullptr || exp1_filename != nullptr || save_state_filename != nullptr);
  if (boot)
  {
    if (!intf->InitializeSystem(filename, exp1_filename))
      return nullptr;

    if (save_state_filename)
      intf->LoadState(save_state_filename);
  }

  intf->UpdateAudioVisualSync();
  return intf;
}

TinyString SDLInterface::GetSaveStateFilename(u32 index)
{
  return TinyString::FromFormat("savestate_%u.bin", index);
}

void SDLInterface::ReportMessage(const char* message)
{
  AddOSDMessage(message, 3.0f);
}

bool SDLInterface::IsWindowFullscreen() const
{
  return ((SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0);
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

    case SDL_SCANCODE_Q:
      controller->SetButtonState(DigitalController::Button::L1, pressed);
      return true;
    case SDL_SCANCODE_E:
      controller->SetButtonState(DigitalController::Button::R1, pressed);
      return true;

    case SDL_SCANCODE_1:
      controller->SetButtonState(DigitalController::Button::L2, pressed);
      return true;
    case SDL_SCANCODE_3:
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

void SDLInterface::HandleSDLEvent(const SDL_Event* event)
{
  ImGui_ImplSDL2_ProcessEvent(event);

  switch (event->type)
  {
    case SDL_WINDOWEVENT:
    {
      if (event->window.event == SDL_WINDOWEVENT_RESIZED)
      {
        m_window_width = event->window.data1;
        m_window_height = event->window.data2;
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
  }
}

void SDLInterface::HandleSDLKeyEvent(const SDL_Event* event)
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

    case SDL_SCANCODE_TAB:
    {
      if (!repeat)
      {
        m_speed_limiter_temp_disabled = pressed;
        UpdateAudioVisualSync();
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
        m_speed_limiter_enabled = !m_speed_limiter_enabled;
        UpdateAudioVisualSync();
        AddOSDMessage(m_speed_limiter_enabled ? "Speed limiter enabled." : "Speed limiter disabled.");
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

void SDLInterface::ClearImGuiFocus()
{
  ImGui::SetWindowFocus(nullptr);
}

void SDLInterface::Render()
{
  DrawImGui();

  if (m_system)
    m_system->GetGPU()->ResetGraphicsAPIState();

  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  SDL_GL_SwapWindow(m_window);

  ImGui_ImplSDL2_NewFrame(m_window);
  ImGui_ImplOpenGL3_NewFrame();

  ImGui::NewFrame();

  GL::Program::ResetLastProgram();

  if (m_system)
    m_system->GetGPU()->RestoreGraphicsAPIState();
}

static std::tuple<int, int, int, int> CalculateDrawRect(int window_width, int window_height, float display_ratio)
{
  const float window_ratio = float(window_width) / float(window_height);
  int left, top, width, height;
  if (window_ratio >= display_ratio)
  {
    width = static_cast<int>(float(window_height) * display_ratio);
    height = static_cast<int>(window_height);
    left = (window_width - width) / 2;
    top = 0;
  }
  else
  {
    width = static_cast<int>(window_width);
    height = static_cast<int>(float(window_width) / display_ratio);
    left = 0;
    top = (window_height - height) / 2;
  }

  return std::tie(left, top, width, height);
}

void SDLInterface::RenderDisplay()
{
  if (!m_display_texture)
    return;

  // - 20 for main menu padding
  const auto [vp_left, vp_top, vp_width, vp_height] =
    CalculateDrawRect(m_window_width, std::max(m_window_height - 20, 1), m_display_aspect_ratio);
  const bool linear_filter = m_system ? m_system->GetSettings().display_linear_filtering : false;

  glViewport(vp_left, m_window_height - (20 + vp_top) - vp_height, vp_width, vp_height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  m_display_program.Uniform4f(
    0, static_cast<float>(m_display_texture_offset_x) / static_cast<float>(m_display_texture->GetWidth()),
    static_cast<float>(m_display_texture_offset_y) / static_cast<float>(m_display_texture->GetHeight()),
    static_cast<float>(m_display_texture_width) / static_cast<float>(m_display_texture->GetWidth()),
    static_cast<float>(m_display_texture_height) / static_cast<float>(m_display_texture->GetHeight()));
  m_display_texture->Bind();
  glBindSampler(0, linear_filter ? m_display_linear_sampler : m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}

void SDLInterface::DrawImGui()
{
  DrawMainMenuBar();

  if (m_system)
    DrawDebugWindows();
  else
    DrawPoweredOffWindow();

  if (m_about_window_open)
    DrawAboutWindow();

  DrawOSDMessages();

  ImGui::Render();
}

void SDLInterface::DrawMainMenuBar()
{
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
    if (ImGui::MenuItem("Enable Speed Limiter", nullptr, &m_speed_limiter_enabled, system_enabled))
      UpdateAudioVisualSync();

    ImGui::Separator();

    if (ImGui::MenuItem("Fullscreen", nullptr, IsWindowFullscreen()))
      SDL_SetWindowFullscreen(m_window, IsWindowFullscreen() ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);

    ImGui::Separator();

    if (ImGui::BeginMenu("GPU", system_enabled))
    {
      if (ImGui::BeginMenu("Renderer"))
      {
        const Settings::GPURenderer current = m_system->GetSettings().gpu_renderer;
        for (const auto& it : s_gpu_renderer_names)
        {
          if (ImGui::MenuItem(it.second, nullptr, current == it.first))
          {
            m_system->GetSettings().gpu_renderer = it.first;
            m_system->RecreateGPU();
          }
        }

        ImGui::EndMenu();
      }

      if (ImGui::BeginMenu("Internal Resolution"))
      {
        const u32 current_internal_resolution = m_system->GetSettings().gpu_resolution_scale;
        for (u32 scale = 1; scale <= m_system->GetSettings().max_gpu_resolution_scale; scale++)
        {
          if (ImGui::MenuItem(
                TinyString::FromFormat("%ux (%ux%u)", scale, scale * GPU::VRAM_WIDTH, scale * GPU::VRAM_HEIGHT),
                nullptr, current_internal_resolution == scale))
          {
            m_system->GetSettings().gpu_resolution_scale = scale;
            m_system->GetGPU()->UpdateSettings();
          }
        }

        ImGui::EndMenu();
      }

      if (ImGui::MenuItem("VSync", nullptr, &m_system->GetSettings().gpu_vsync))
        UpdateAudioVisualSync();

      if (ImGui::MenuItem("True (24-Bit) Color", nullptr, &m_system->GetSettings().gpu_true_color))
        m_system->GetGPU()->UpdateSettings();

      if (ImGui::MenuItem("Display Linear Filtering", nullptr, &m_system->GetSettings().display_linear_filtering))
      {
        // this has to update the display texture for now..
        m_system->GetGPU()->UpdateSettings();
      }

      ImGui::EndMenu();
    }

    ImGui::EndMenu();
  }

  if (m_system)
    DrawDebugMenu();

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

void SDLInterface::DrawPoweredOffWindow()
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
  ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(m_app_icon_texture->GetGLId())),
               ImVec2(APP_ICON_WIDTH, APP_ICON_HEIGHT));
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
  ImGui::Button("Settings", button_size);
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Exit", button_size))
    m_quit_request = true;

  ImGui::NewLine();

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);

  ImGui::End();
}

void SDLInterface::DrawAboutWindow()
{
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("About DuckStation", &m_about_window_open))
  {
    ImGui::End();
    return;
  }

  ImGui::Text("DuckStation");
  ImGui::NewLine();
  ImGui::Text("Authors:");
  ImGui::Text("  Connor McLaughlin <stenzek@gmail.com>");
  ImGui::NewLine();
  ImGui::Text("Uses Dear ImGui (https://github.com/ocornut/imgui)");
  ImGui::Text("Uses libcue (https://github.com/lipnitsk/libcue)");
  ImGui::Text("Uses stb_image_write (https://github.com/nothings/stb)");
  ImGui::NewLine();
  ImGui::Text("Duck icon by icons8 (https://icons8.com/icon/74847/platforms.undefined.short-title)");

  ImGui::NewLine();

  ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 60.0f) / 2.0f);
  if (ImGui::Button("Close", ImVec2(60.0f, 20.0f)))
    m_about_window_open = false;

  ImGui::End();
}

void SDLInterface::DrawDebugMenu()
{
  if (!ImGui::BeginMenu("Debug", m_system != nullptr))
    return;

  Settings::DebugSettings& debug_settings = m_system->GetSettings().debugging;

  ImGui::MenuItem("Show System State");
  ImGui::Separator();

  ImGui::MenuItem("Show GPU State", nullptr, &debug_settings.show_gpu_state);
  ImGui::MenuItem("Show GPU Renderer Stats", nullptr, &debug_settings.show_gpu_renderer_stats);
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

  ImGui::EndMenu();
}

void SDLInterface::DrawDebugWindows()
{
  const Settings::DebugSettings& debug_settings = m_system->GetSettings().debugging;

  if (debug_settings.show_gpu_state)
    m_system->GetGPU()->DrawDebugStateWindow();
  if (debug_settings.show_gpu_renderer_stats)
    m_system->GetGPU()->DrawRendererStatsWindow();
  if (debug_settings.show_cdrom_state)
    m_system->GetCDROM()->DrawDebugWindow();
  if (debug_settings.show_timers_state)
    m_system->GetTimers()->DrawDebugStateWindow();
  if (debug_settings.show_spu_state)
    m_system->GetSPU()->DrawDebugStateWindow();
  if (debug_settings.show_mdec_state)
    m_system->GetMDEC()->DrawDebugStateWindow();
}

void SDLInterface::AddOSDMessage(const char* message, float duration /*= 2.0f*/)
{
  OSDMessage msg;
  msg.text = message;
  msg.duration = duration;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  m_osd_messages.push_back(std::move(msg));
}

void SDLInterface::SetDisplayTexture(GL::Texture* texture, u32 offset_x, u32 offset_y, u32 width, u32 height,
                                     float aspect_ratio)
{
  m_display_texture = texture;
  m_display_texture_offset_x = offset_x;
  m_display_texture_offset_y = offset_y;
  m_display_texture_width = width;
  m_display_texture_height = height;
  m_display_aspect_ratio = aspect_ratio;
  m_display_texture_changed = true;
}

void SDLInterface::DrawOSDMessages()
{
  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);

  auto iter = m_osd_messages.begin();
  float position_x = 10.0f;
  float position_y = 10.0f + 20.0f;
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
      position_y += ImGui::GetWindowSize().y + (4.0f * ImGui::GetIO().DisplayFramebufferScale.x);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ++iter;
  }
}

void SDLInterface::DoReset()
{
  m_system->Reset();
  ResetPerformanceCounters();
  AddOSDMessage("System reset.");
}

void SDLInterface::DoPowerOff()
{
  Assert(m_system);
  ShutdownSystem();
  AddOSDMessage("System powered off.");
}

void SDLInterface::DoResume()
{
  Assert(!m_system);
  if (!InitializeSystem())
    return;

  if (!LoadState(RESUME_SAVESTATE_FILENAME))
  {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Load state failed",
                             "Failed to load the resume save state. Stopping emulation.", m_window);
    ShutdownSystem();
    return;
  }

  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLInterface::DoStartDisc()
{
  Assert(!m_system);

  nfdchar_t* path = nullptr;
  if (!NFD_OpenDialog("bin,img,cue,exe,psexe", nullptr, &path) || !path || std::strlen(path) == 0)
    return;

  AddOSDMessage(SmallString::FromFormat("Starting disc from '%s'...", path));
  if (!InitializeSystem(path, nullptr))
    return;

  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLInterface::DoStartBIOS()
{
  Assert(!m_system);

  AddOSDMessage("Starting BIOS...");
  if (!InitializeSystem(nullptr, nullptr))
    return;

  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLInterface::DoChangeDisc()
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

void SDLInterface::DoLoadState(u32 index)
{
  if (!HasSystem() && !InitializeSystem(nullptr, nullptr))
    return;

  LoadState(GetSaveStateFilename(index));
  ResetPerformanceCounters();
  ClearImGuiFocus();
}

void SDLInterface::DoSaveState(u32 index)
{
  Assert(m_system);
  SaveState(GetSaveStateFilename(index));
  ClearImGuiFocus();
}

void SDLInterface::DoTogglePause()
{
  if (!m_system)
    return;

  m_paused = !m_paused;
  if (!m_paused)
    m_fps_timer.Reset();
}

void SDLInterface::DoFrameStep()
{
  if (!m_system)
    return;

  m_frame_step_request = true;
  m_paused = false;
}

void SDLInterface::DoToggleSoftwareRendering()
{
  if (!m_system)
    return;

  auto& settings = m_system->GetSettings();
  if (settings.gpu_renderer != Settings::GPURenderer::Software)
  {
    settings.gpu_renderer = Settings::GPURenderer::Software;
    AddOSDMessage("Switched to software GPU renderer.");
  }
  else
  {
    settings.gpu_renderer = Settings::GPURenderer::HardwareOpenGL;
    AddOSDMessage("Switched to hardware GPU renderer.");
  }

  m_system->RecreateGPU();
}

void SDLInterface::DoModifyInternalResolution(s32 increment)
{
  if (!m_system)
    return;

  auto& settings = m_system->GetSettings();
  const u32 new_resolution_scale =
    std::clamp<u32>(static_cast<u32>(static_cast<s32>(settings.gpu_resolution_scale) + increment), 1,
                    settings.max_gpu_resolution_scale);
  if (new_resolution_scale == settings.gpu_resolution_scale)
    return;

  settings.gpu_resolution_scale = new_resolution_scale;
  m_system->GetGPU()->UpdateSettings();

  AddOSDMessage(TinyString::FromFormat("Resolution scale set to %ux (%ux%u)", settings.gpu_resolution_scale,
                                       GPU::VRAM_WIDTH * settings.gpu_resolution_scale,
                                       GPU::VRAM_HEIGHT * settings.gpu_resolution_scale));
}

void SDLInterface::Run()
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

    Render();

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
    {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Save state failed",
                               "Saving state failed, you will not be able to resume this session.", m_window);
    }

    ShutdownSystem();
  }
}
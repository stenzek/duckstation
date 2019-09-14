#include "sdl_interface.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Error.h"
#include "YBaseLib/Log.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl.h"
#include "pse/system.h"
#include <cinttypes>
#include <glad.h>
Log_SetChannel(SDLInterface);

SDLInterface::SDLInterface() = default;

SDLInterface::~SDLInterface()
{
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

  m_window = SDL_CreateWindow("Some idea to emulate a system starting with a P", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT, window_flags);
  if (!m_window)
  {
    Panic("Failed to create window");
    return false;
  }

  SDL_GetWindowSize(m_window, &m_window_width, &m_window_height);
  return true;
}

static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                     const GLchar* message, const void* userParam)
{
  Log_InfoPrintf("%s", message);
}

bool SDLInterface::CreateGLContext()
{
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
  m_gl_context = SDL_GL_CreateContext(m_window);
  if (!m_gl_context || SDL_GL_MakeCurrent(m_window, m_gl_context) != 0 || !gladLoadGL())
  {
    Panic("Failed to create GL context");
    return false;
  }

  if (GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }

  SDL_GL_SetSwapInterval(0);
  return true;
}

bool SDLInterface::CreateImGuiContext()
{
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;

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

in vec2 v_tex0;
out vec4 o_col0;

void main()
{
  o_col0 = texture(samp0, v_tex0);
}
)";

  if (!m_display_program.Compile(fullscreen_quad_vertex_shader, display_fragment_shader))
    return false;

  m_display_program.BindFragData(0, "o_col0");
  if (!m_display_program.Link())
    return false;

  m_display_program.RegisterUniform("samp0");
  m_display_program.Bind();
  m_display_program.Uniform1i(0, 0);

  glGenVertexArrays(1, &m_display_vao);
  return true;
}

std::unique_ptr<SDLInterface> SDLInterface::Create()
{
  std::unique_ptr<SDLInterface> intf = std::make_unique<SDLInterface>();
  if (!intf->CreateSDLWindow() || !intf->CreateGLContext() || !intf->CreateImGuiContext() || !intf->CreateGLResources())
    return nullptr;

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

bool SDLInterface::HandleSDLEvent(const SDL_Event* event)
{
  if (PassEventToImGui(event))
    return true;

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
      m_running = false;
      break;
  }

  return false;
}

bool SDLInterface::PassEventToImGui(const SDL_Event* event)
{
  ImGuiIO& io = ImGui::GetIO();
  switch (event->type)
  {
    case SDL_MOUSEWHEEL:
    {
      if (event->wheel.x > 0)
        io.MouseWheelH += 1;
      if (event->wheel.x < 0)
        io.MouseWheelH -= 1;
      if (event->wheel.y > 0)
        io.MouseWheel += 1;
      if (event->wheel.y < 0)
        io.MouseWheel -= 1;
      return io.WantCaptureMouse;
    }

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
      bool down = event->type == SDL_MOUSEBUTTONDOWN;
      if (event->button.button == SDL_BUTTON_LEFT)
        io.MouseDown[0] = down;
      if (event->button.button == SDL_BUTTON_RIGHT)
        io.MouseDown[1] = down;
      if (event->button.button == SDL_BUTTON_MIDDLE)
        io.MouseDown[2] = down;
      return io.WantCaptureMouse;
    }

    case SDL_MOUSEMOTION:
    {
      io.MousePos.x = float(event->motion.x);
      io.MousePos.y = float(event->motion.y);
      return io.WantCaptureMouse;
    }

    case SDL_TEXTINPUT:
    {
      io.AddInputCharactersUTF8(event->text.text);
      return io.WantCaptureKeyboard;
    }

    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
      int key = event->key.keysym.scancode;
      IM_ASSERT(key >= 0 && key < IM_ARRAYSIZE(io.KeysDown));
      io.KeysDown[key] = (event->type == SDL_KEYDOWN);
      io.KeyShift = ((SDL_GetModState() & KMOD_SHIFT) != 0);
      io.KeyCtrl = ((SDL_GetModState() & KMOD_CTRL) != 0);
      io.KeyAlt = ((SDL_GetModState() & KMOD_ALT) != 0);
      io.KeySuper = ((SDL_GetModState() & KMOD_GUI) != 0);
      return io.WantCaptureKeyboard;
    }
  }
  return false;
}

void SDLInterface::Render()
{
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

  RenderImGui();

  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  SDL_GL_SwapWindow(m_window);

  ImGui_ImplSDL2_NewFrame(m_window);
  ImGui_ImplOpenGL3_NewFrame();

  ImGui::NewFrame();
}

void SDLInterface::RenderDisplay()
{
  if (!m_display_texture)
    return;

  glViewport(0, 0, m_window_width, m_window_height - 20);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  m_display_texture->Bind();
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SDLInterface::RenderImGui()
{
  RenderMainMenuBar();
  RenderOSDMessages();

  ImGui::Render();
}

void SDLInterface::RenderMainMenuBar()
{
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("System"))
  {
    if (ImGui::MenuItem("Reset"))
      m_system->Reset();

    ImGui::Separator();

#if 0
    if (ImGui::MenuItem("Enable Speed Limiter", nullptr, IsSpeedLimiterEnabled()))
      SetSpeedLimiterEnabled(!IsSpeedLimiterEnabled());
#endif

    ImGui::Separator();

    if (ImGui::BeginMenu("Load State"))
    {
      for (u32 i = 1; i <= 8; i++)
      {
        if (ImGui::MenuItem(TinyString::FromFormat("State %u", i).GetCharArray()))
          DoLoadState(i);
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Save State"))
    {
      for (u32 i = 1; i <= 8; i++)
      {
        if (ImGui::MenuItem(TinyString::FromFormat("State %u", i).GetCharArray()))
          DoSaveState(i);
      }
      ImGui::EndMenu();
    }

    if (ImGui::MenuItem("Exit"))
      m_running = false;

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View"))
  {
    if (ImGui::MenuItem("Fullscreen", nullptr, IsWindowFullscreen()))
      SDL_SetWindowFullscreen(m_window, IsWindowFullscreen() ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);

    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}

void SDLInterface::AddOSDMessage(const char* message, float duration /*= 2.0f*/)
{
  OSDMessage msg;
  msg.text = message;
  msg.duration = duration;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  m_osd_messages.push_back(std::move(msg));
}

void SDLInterface::SetDisplayTexture(GL::Texture* texture, u32 offset_x, u32 offset_y, u32 width, u32 height)
{
  m_display_texture = texture;
  m_display_texture_offset_x = 0;
  m_display_texture_offset_y = 0;
  m_display_texture_width = width;
  m_display_texture_height = height;
  m_display_texture_changed = true;
}

void SDLInterface::RenderOSDMessages()
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

void SDLInterface::DoLoadState(u32 index)
{
  LoadState(GetSaveStateFilename(index));
}

void SDLInterface::DoSaveState(u32 index)
{
  SaveState(GetSaveStateFilename(index));
}

void SDLInterface::Run()
{
  while (m_running)
  {
    for (;;)
    {
      SDL_Event ev;
      if (SDL_PollEvent(&ev))
        HandleSDLEvent(&ev);
      else
        break;
    }

    m_system->RunFrame();
    Render();
  }
}

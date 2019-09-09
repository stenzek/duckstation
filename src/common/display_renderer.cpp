#include "display_renderer.h"
#include "display_renderer_d3d.h"
#include "display_renderer_gl.h"

DisplayRenderer::DisplayRenderer(WindowHandleType window_handle, u32 window_width, u32 window_height)
  : m_window_handle(window_handle), m_window_width(window_width), m_window_height(window_height)
{
}

DisplayRenderer::~DisplayRenderer()
{
  Assert(m_primary_displays.empty());
  Assert(m_secondary_displays.empty());
}

float DisplayRenderer::GetPrimaryDisplayFramesPerSecond()
{
  std::lock_guard<std::mutex> guard(m_display_lock);
  if (m_active_displays.empty())
    return 0.0f;

  return m_active_displays.front()->GetFramesPerSecond();
}

bool DisplayRenderer::Initialize()
{
  return true;
}

void DisplayRenderer::AddDisplay(Display* display)
{
  std::lock_guard<std::mutex> guard(m_display_lock);

  if (display->GetType() == Display::Type::Primary)
    m_primary_displays.push_back(display);
  else
    m_secondary_displays.push_back(display);

  UpdateActiveDisplays();
}

void DisplayRenderer::RemoveDisplay(Display* display)
{
  std::lock_guard<std::mutex> guard(m_display_lock);

  auto& container = (display->GetType() == Display::Type::Primary) ? m_primary_displays : m_secondary_displays;
  auto iter = std::find(container.begin(), container.end(), display);
  if (iter != container.end())
    container.erase(iter);

  UpdateActiveDisplays();
}

void DisplayRenderer::DisplayEnabled(Display* display)
{
  std::lock_guard<std::mutex> guard(m_display_lock);
  UpdateActiveDisplays();
}

void DisplayRenderer::DisplayDisabled(Display* display)
{
  std::lock_guard<std::mutex> guard(m_display_lock);
  UpdateActiveDisplays();
}

void DisplayRenderer::DisplayResized(Display* display) {}

void DisplayRenderer::DisplayFramebufferSwapped(Display* display) {}

void DisplayRenderer::WindowResized(u32 window_width, u32 window_height)
{
  m_window_width = window_width;
  m_window_height = window_height;
}

void DisplayRenderer::UpdateActiveDisplays()
{
  m_active_displays.clear();

  // Find the primary display with the highest priority, and enabled.
  Display* primary_display = nullptr;
  for (Display* dpy : m_primary_displays)
  {
    dpy->SetActive(false);
    if (dpy->IsEnabled() && (!primary_display || dpy->GetPriority() > primary_display->GetPriority()))
      primary_display = dpy;
  }
  if (primary_display)
  {
    primary_display->SetActive(true);
    m_active_displays.push_back(primary_display);
  }

  // Add all enabled secondary displays.
  for (Display* dpy : m_secondary_displays)
  {
    if (dpy->IsEnabled())
    {
      dpy->SetActive(true);
      m_active_displays.push_back(dpy);
    }
    else
    {
      dpy->SetActive(false);
    }
  }
}

std::pair<u32, u32> DisplayRenderer::GetDisplayRenderSize(const Display* display)
{
  Assert(!m_active_displays.empty());
  const u32 window_width = m_window_width / u32(m_active_displays.size());
  const u32 window_height = u32(std::max(1, int(m_window_height) - int(m_top_padding)));
  const float display_ratio = float(display->GetDisplayWidth()) / float(display->GetDisplayHeight());
  const float window_ratio = float(window_width) / float(window_height);
  u32 viewport_width;
  u32 viewport_height;
  if (window_ratio >= display_ratio)
  {
    viewport_width = u32(float(window_height) * display_ratio);
    viewport_height = u32(window_height);
  }
  else
  {
    viewport_width = u32(window_width);
    viewport_height = u32(float(window_width) / display_ratio);
  }

  return std::make_pair(viewport_width, viewport_height);
}

namespace {
class DisplayRendererNull final : public DisplayRenderer
{
public:
  DisplayRendererNull(WindowHandleType window_handle, u32 window_width, u32 window_height)
    : DisplayRenderer(window_handle, window_width, window_height)
  {
  }

  BackendType GetBackendType() override { return DisplayRenderer::BackendType::Null; }

  virtual std::unique_ptr<Display> CreateDisplay(const char* name, Display::Type type,
                                                 u8 priority = Display::DEFAULT_PRIORITY) override
  {
    auto display = std::make_unique<Display>(this, name, type, priority);
    AddDisplay(display.get());
    return display;
  }

  virtual bool BeginFrame() override { return true; }

  virtual void RenderDisplays() override {}

  virtual void EndFrame() override {}
};
} // namespace

DisplayRenderer::BackendType DisplayRenderer::GetDefaultBackendType()
{
#ifdef Y_COMPILER_MSVC
  return BackendType::Direct3D;
#else
  return BackendType::OpenGL;
#endif
}

std::unique_ptr<DisplayRenderer> DisplayRenderer::Create(BackendType backend, WindowHandleType window_handle,
                                                         u32 window_width, u32 window_height)
{
  std::unique_ptr<DisplayRenderer> renderer;
  switch (backend)
  {
    case BackendType::Null:
      renderer = std::make_unique<DisplayRendererNull>(window_handle, window_width, window_height);
      break;

#ifdef Y_COMPILER_MSVC
    case BackendType::Direct3D:
      renderer = std::make_unique<DisplayRendererD3D>(window_handle, window_width, window_height);
      break;
#endif

    case BackendType::OpenGL:
      renderer = std::make_unique<DisplayRendererGL>(window_handle, window_width, window_height);
      break;

    default:
      return nullptr;
  }

  if (!renderer->Initialize())
    return nullptr;

  return renderer;
}

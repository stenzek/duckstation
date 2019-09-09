#pragma once
#include "display.h"
#include <memory>
#include <mutex>
#include <vector>

class DisplayRenderer;

class DisplayRenderer
{
public:
  using WindowHandleType = void*;
  enum class BackendType
  {
    Null,
    Direct3D,
    OpenGL
  };

  DisplayRenderer(WindowHandleType window_handle, u32 window_width, u32 window_height);
  virtual ~DisplayRenderer();

  u32 GetWindowWidth() const { return m_window_width; }
  u32 GetWindowHeight() const { return m_window_height; }

  u32 GetTopPadding() const { return m_top_padding; }
  void SetTopPadding(u32 padding) { m_top_padding = padding; }

  float GetPrimaryDisplayFramesPerSecond();

  virtual BackendType GetBackendType() = 0;

  virtual std::unique_ptr<Display> CreateDisplay(const char* name, Display::Type type,
                                                 u8 priority = Display::DEFAULT_PRIORITY) = 0;

  virtual void RemoveDisplay(Display* display);

  virtual void DisplayEnabled(Display* display);
  virtual void DisplayDisabled(Display* display);
  virtual void DisplayResized(Display* display);
  virtual void DisplayFramebufferSwapped(Display* display);

  virtual void WindowResized(u32 window_width, u32 window_height);

  virtual bool BeginFrame() = 0;
  virtual void RenderDisplays() = 0;
  virtual void EndFrame() = 0;

  /// Returns the default backend type for the system.
  static BackendType GetDefaultBackendType();

  static std::unique_ptr<DisplayRenderer> Create(BackendType backend, WindowHandleType window_handle, u32 window_width,
                                                 u32 window_height);

protected:
  virtual bool Initialize();

  void AddDisplay(Display* display);
  void UpdateActiveDisplays();

  std::pair<u32, u32> GetDisplayRenderSize(const Display* display);

  WindowHandleType m_window_handle;
  u32 m_window_width;
  u32 m_window_height;
  u32 m_top_padding = 0;

  std::vector<Display*> m_primary_displays;
  std::vector<Display*> m_secondary_displays;
  std::vector<Display*> m_active_displays;
  std::mutex m_display_lock;
};

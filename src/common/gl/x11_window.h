#pragma once
#include "../types.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace GL {
class X11Window
{
public:
  X11Window();
  ~X11Window();

  ALWAYS_INLINE Window GetWindow() const { return m_window; }
  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }

  bool Create(Display* display, Window parent_window, const XVisualInfo* vi);
  void Destroy();

  // Setting a width/height of 0 will use parent dimensions.
  void Resize(u32 width = 0, u32 height = 0);

private:
  Display* m_display = nullptr;
  Window m_parent_window = {};
  Window m_window = {};
  Colormap m_colormap = {};
  u32 m_width = 0;
  u32 m_height = 0;
};

// Helper class for managing X errors
class X11InhibitErrors
{
public:
  X11InhibitErrors();
  ~X11InhibitErrors();

  ALWAYS_INLINE bool HadError() const { return m_had_error; }

private:
  static int ErrorHandler(Display* display, XErrorEvent* ee);

  XErrorHandler m_old_handler = {};
  bool m_had_error = false;
};

} // namespace GL

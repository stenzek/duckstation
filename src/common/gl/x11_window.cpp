#include "x11_window.h"
#include "../assert.h"
#include "../log.h"
#include <cstdio>
Log_SetChannel(X11Window);

namespace GL {
X11Window::X11Window() = default;

X11Window::~X11Window()
{
  Destroy();
}

bool X11Window::Create(Display* display, Window parent_window, const XVisualInfo* vi)
{
  m_display = display;
  m_parent_window = parent_window;
  XSync(m_display, True);

  XWindowAttributes parent_wa = {};
  XGetWindowAttributes(m_display, m_parent_window, &parent_wa);
  m_width = static_cast<u32>(parent_wa.width);
  m_height = static_cast<u32>(parent_wa.height);

  // Failed X calls terminate the process so no need to check for errors.
  // We could swap the error handler out here as well.
  m_colormap = XCreateColormap(m_display, m_parent_window, vi->visual, AllocNone);

  XSetWindowAttributes wa = {};
  wa.colormap = m_colormap;

  m_window = XCreateWindow(m_display, m_parent_window, 0, 0, m_width, m_height, 0, vi->depth, InputOutput, vi->visual,
                           CWColormap, &wa);
  XMapWindow(m_display, m_window);
  XSync(m_display, True);

  return true;
}

void X11Window::Destroy()
{
  if (m_window)
  {
    XUnmapWindow(m_display, m_window);
    XDestroyWindow(m_display, m_window);
    m_window = {};
  }

  if (m_colormap)
  {
    XFreeColormap(m_display, m_colormap);
    m_colormap = {};
  }
}

void X11Window::Resize(u32 width, u32 height)
{
  if (width != 0 && height != 0)
  {
    m_width = width;
    m_height = height;
  }
  else
  {
    XWindowAttributes parent_wa = {};
    XGetWindowAttributes(m_display, m_parent_window, &parent_wa);
    m_width = static_cast<u32>(parent_wa.width);
    m_height = static_cast<u32>(parent_wa.height);
  }

  XResizeWindow(m_display, m_window, m_width, m_height);
}

static X11InhibitErrors* s_current_error_inhibiter;

X11InhibitErrors::X11InhibitErrors()
{
  Assert(!s_current_error_inhibiter);
  m_old_handler = XSetErrorHandler(ErrorHandler);
  s_current_error_inhibiter = this;
}

X11InhibitErrors::~X11InhibitErrors()
{
  Assert(s_current_error_inhibiter == this);
  s_current_error_inhibiter = nullptr;
  XSetErrorHandler(m_old_handler);
}

int X11InhibitErrors::ErrorHandler(Display* display, XErrorEvent* ee)
{
  char error_string[256] = {};
  XGetErrorText(display, ee->error_code, error_string, sizeof(error_string));
  Log_WarningPrintf("X11 Error: %s (Error %u Minor %u Request %u)", error_string, ee->error_code, ee->minor_code,
                    ee->request_code);

  s_current_error_inhibiter->m_had_error = true;
  return 0;
}
} // namespace GL

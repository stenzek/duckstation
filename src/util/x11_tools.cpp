// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "x11_tools.h"
#include "window_info.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"
#include "common/scoped_guard.h"

#include <X11/Xlib-xcb.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

#include <memory>

LOG_CHANNEL(WindowInfo);

namespace {

namespace dyn_libs {

#define XCB_FUNCTIONS(X)                                                                                               \
  X(xcb_get_geometry)                                                                                                  \
  X(xcb_get_geometry_reply)                                                                                            \
  X(xcb_get_setup)                                                                                                     \
  X(xcb_setup_roots_iterator)                                                                                          \
  X(xcb_screen_next)                                                                                                   \
  X(xcb_screen_allowed_depths_iterator)                                                                                \
  X(xcb_depth_next)                                                                                                    \
  X(xcb_depth_visuals_length)                                                                                          \
  X(xcb_depth_visuals)                                                                                                 \
  X(xcb_generate_id)                                                                                                   \
  X(xcb_request_check)                                                                                                 \
  X(xcb_create_colormap_checked)                                                                                       \
  X(xcb_create_window_checked)                                                                                         \
  X(xcb_map_window_checked)                                                                                            \
  X(xcb_unmap_window_checked)                                                                                          \
  X(xcb_destroy_window_checked)                                                                                        \
  X(xcb_free_colormap_checked)                                                                                         \
  X(xcb_configure_window_checked)

#define XCB_RANDR_FUNCTIONS(X)                                                                                         \
  X(xcb_randr_get_screen_resources_reply)                                                                              \
  X(xcb_randr_get_screen_resources)                                                                                    \
  X(xcb_randr_get_monitors_reply)                                                                                      \
  X(xcb_randr_get_monitors)                                                                                            \
  X(xcb_randr_get_monitors_monitors_iterator)                                                                          \
  X(xcb_randr_monitor_info_outputs)                                                                                    \
  X(xcb_randr_get_output_info_reply)                                                                                   \
  X(xcb_randr_get_output_info)                                                                                         \
  X(xcb_randr_get_crtc_info_reply)                                                                                     \
  X(xcb_randr_get_crtc_info)                                                                                           \
  X(xcb_randr_get_screen_resources_modes_iterator)                                                                     \
  X(xcb_randr_mode_info_next)

#define X11XCB_FUNCTIONS(X) X(XGetXCBConnection)

static bool OpenXcb(Error* error);
static void CloseXcb();
static bool OpenXcbRandR(Error* error);
static void CloseXcbRandR();
static bool OpenX11Xcb(Error* error);
static void CloseX11Xcb();
static void CloseAll();

static DynamicLibrary s_xcb_library;
static DynamicLibrary s_xcb_randr_library;
static DynamicLibrary s_x11xcb_library;
static bool s_close_registered = false;

#define ADD_FUNC(F) static decltype(&::F) F;
XCB_FUNCTIONS(ADD_FUNC);
XCB_RANDR_FUNCTIONS(ADD_FUNC);
X11XCB_FUNCTIONS(ADD_FUNC);
#undef ADD_FUNC

} // namespace dyn_libs

template<typename T>
struct XCBPointerDeleter
{
  void operator()(T* ptr) { free(ptr); }
};

template<typename T>
using XCBPointer = std::unique_ptr<T, XCBPointerDeleter<T>>;

} // namespace

bool dyn_libs::OpenXcb(Error* error)
{
  if (s_xcb_library.IsOpen())
    return true;

  const std::string libname = DynamicLibrary::GetVersionedFilename("xcb", 1);
  if (!s_xcb_library.Open(libname.c_str(), error))
  {
    Error::AddPrefix(error, "Failed to load xcb: ");
    return false;
  }

#define LOAD_FUNC(F)                                                                                                   \
  if (!s_xcb_library.GetSymbol(#F, &F))                                                                                \
  {                                                                                                                    \
    Error::SetStringFmt(error, "Failed to find function {}", #F);                                                      \
    CloseXcb();                                                                                                        \
    return false;                                                                                                      \
  }

  XCB_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

  if (!s_close_registered)
  {
    s_close_registered = true;
    std::atexit(&dyn_libs::CloseAll);
  }

  return true;
}

void dyn_libs::CloseXcb()
{
#define UNLOAD_FUNC(F) F = nullptr;
  XCB_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

  s_xcb_library.Close();
}

bool dyn_libs::OpenXcbRandR(Error* error)
{
  if (s_xcb_randr_library.IsOpen())
    return true;

  const std::string libname = DynamicLibrary::GetVersionedFilename("xcb-randr", 0);
  if (!s_xcb_randr_library.Open(libname.c_str(), error))
  {
    Error::AddPrefix(error, "Failed to load xcb-randr: ");
    return false;
  }

#define LOAD_FUNC(F)                                                                                                   \
  if (!s_xcb_randr_library.GetSymbol(#F, &F))                                                                          \
  {                                                                                                                    \
    Error::SetStringFmt(error, "Failed to find function {}", #F);                                                      \
    CloseXcb();                                                                                                        \
    return false;                                                                                                      \
  }

  XCB_RANDR_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

  if (!s_close_registered)
  {
    s_close_registered = true;
    std::atexit(&dyn_libs::CloseAll);
  }

  return true;
}

void dyn_libs::CloseXcbRandR()
{
#define UNLOAD_FUNC(F) F = nullptr;
  XCB_RANDR_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

  s_xcb_randr_library.Close();
}

bool dyn_libs::OpenX11Xcb(Error* error)
{
  if (s_x11xcb_library.IsOpen())
    return true;

  const std::string libname = DynamicLibrary::GetVersionedFilename("X11-xcb", 1);
  if (!s_x11xcb_library.Open(libname.c_str(), error))
  {
    Error::AddPrefix(error, "Failed to load X11-xcb: ");
    return false;
  }

#define LOAD_FUNC(F)                                                                                                   \
  if (!s_x11xcb_library.GetSymbol(#F, &F))                                                                             \
  {                                                                                                                    \
    Error::SetStringFmt(error, "Failed to find function {}", #F);                                                      \
    CloseXcb();                                                                                                        \
    return false;                                                                                                      \
  }

  X11XCB_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

  if (!s_close_registered)
  {
    s_close_registered = true;
    std::atexit(&dyn_libs::CloseAll);
  }

  return true;
}

void dyn_libs::CloseX11Xcb()
{
#define UNLOAD_FUNC(F) F = nullptr;
  X11XCB_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

  s_x11xcb_library.Close();
}

void dyn_libs::CloseAll()
{
  CloseX11Xcb();
  CloseXcbRandR();
  CloseXcb();
}

X11Window::X11Window() = default;

X11Window::X11Window(X11Window&& move)
{
  m_connection = move.m_connection;
  m_parent_window = move.m_parent_window;
  m_window = move.m_window;
  m_colormap = move.m_colormap;
  m_width = move.m_width;
  m_height = move.m_height;

  move.m_connection = nullptr;
  move.m_parent_window = {};
  move.m_window = {};
  move.m_colormap = {};
  move.m_width = 0;
  move.m_height = 0;
}

X11Window::~X11Window()
{
  Destroy();
}

X11Window& X11Window::operator=(X11Window&& move)
{
  m_connection = move.m_connection;
  m_parent_window = move.m_parent_window;
  m_window = move.m_window;
  m_colormap = move.m_colormap;
  m_width = move.m_width;
  m_height = move.m_height;

  move.m_connection = nullptr;
  move.m_parent_window = {};
  move.m_window = {};
  move.m_colormap = {};
  move.m_width = 0;
  move.m_height = 0;

  return *this;
}

static void SetErrorObject(Error* error, const char* prefix, const xcb_generic_error_t* xerror)
{
  Error::SetStringFmt(error, "{} failed: EC={} Major={} Minor={} Resource={:X}", xerror->error_code,
                      xerror->response_type, xerror->major_code, xerror->minor_code, xerror->resource_id);
}

bool X11Window::Create(xcb_connection_t* connection, xcb_window_t parent_window, xcb_visualid_t vi, Error* error)
{
  xcb_generic_error_t* xerror;

  if (!dyn_libs::OpenXcb(error))
    return false;

  m_connection = connection;
  m_parent_window = parent_window;

  XCBPointer<xcb_get_geometry_reply_t> gwa(
    dyn_libs::xcb_get_geometry_reply(connection, dyn_libs::xcb_get_geometry(connection, parent_window), &xerror));
  if (!gwa)
  {
    SetErrorObject(error, "xcb_get_geometry_reply() failed: ", xerror);
    return false;
  }

  m_width = gwa->width;
  m_height = gwa->height;

  // Need to find the root window to get an appropriate depth. Needed for NVIDIA+XWayland.
  int visual_depth = XCB_COPY_FROM_PARENT;
  for (xcb_screen_iterator_t it = dyn_libs::xcb_setup_roots_iterator(dyn_libs::xcb_get_setup(connection)); it.rem != 0;
       dyn_libs::xcb_screen_next(&it))
  {
    if (it.data->root == gwa->root)
    {
      for (xcb_depth_iterator_t dit = dyn_libs::xcb_screen_allowed_depths_iterator(it.data); dit.rem != 0;
           dyn_libs::xcb_depth_next(&dit))
      {
        const int len = dyn_libs::xcb_depth_visuals_length(dit.data);
        const xcb_visualtype_t* visuals = dyn_libs::xcb_depth_visuals(dit.data);
        int idx = 0;
        for (; idx < len; idx++)
        {
          if (vi == visuals[idx].visual_id)
          {
            visual_depth = dit.data->depth;
            break;
          }
        }
      }

      break;
    }
  }
  if (visual_depth == XCB_COPY_FROM_PARENT)
    WARNING_LOG("Could not find visual's depth.");

  // ID isn't "used" until the call succeeds.
  m_colormap = dyn_libs::xcb_generate_id(connection);
  if ((xerror = dyn_libs::xcb_request_check(
         connection,
         dyn_libs::xcb_create_colormap_checked(connection, XCB_COLORMAP_ALLOC_NONE, m_colormap, parent_window, vi))))
  {
    SetErrorObject(error, "xcb_create_colormap_checked() failed: ", xerror);
    m_colormap = {};
    return false;
  }

  m_window = dyn_libs::xcb_generate_id(connection);

  const u32 window_values[] = {XCB_PIXMAP_NONE, 0u, m_colormap};
  xerror = dyn_libs::xcb_request_check(
    connection,
    dyn_libs::xcb_create_window_checked(connection, visual_depth, m_window, parent_window, 0, 0, m_width, m_height, 0,
                                        XCB_WINDOW_CLASS_INPUT_OUTPUT, vi,
                                        XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP, window_values));
  if (xerror)
  {
    SetErrorObject(error, "xcb_create_window_checked() failed: ", xerror);
    m_window = {};
    return false;
  }

  xerror = dyn_libs::xcb_request_check(connection, dyn_libs::xcb_map_window_checked(connection, m_window));
  if (xerror)
  {
    SetErrorObject(error, "xcb_map_window_checked() failed: ", xerror);
    return false;
  }

  return true;
}

void X11Window::Destroy()
{
  xcb_generic_error_t* xerror;
  Error error;

  if (m_window)
  {
    if ((xerror =
           dyn_libs::xcb_request_check(m_connection, dyn_libs::xcb_unmap_window_checked(m_connection, m_window))))
    {
      SetErrorObject(&error, "xcb_unmap_window_checked() failed: ", xerror);
      ERROR_LOG(error.GetDescription());
    }

    if ((xerror =
           dyn_libs::xcb_request_check(m_connection, dyn_libs::xcb_destroy_window_checked(m_connection, m_window))))
    {
      SetErrorObject(&error, "xcb_destroy_window_checked() failed: ", xerror);
      ERROR_LOG(error.GetDescription());
    }

    m_window = {};
    m_parent_window = {};
  }

  if (m_colormap)
  {
    if ((xerror =
           dyn_libs::xcb_request_check(m_connection, dyn_libs::xcb_free_colormap_checked(m_connection, m_colormap))))
    {
      SetErrorObject(&error, "xcb_free_colormap_checked() failed: ", xerror);
      ERROR_LOG(error.GetDescription());
    }

    m_colormap = {};
  }
}

void X11Window::Resize(u16 width, u16 height)
{
  xcb_generic_error_t* xerror;
  Error error;

  if (width != 0 && height != 0)
  {
    m_width = width;
    m_height = height;
  }
  else
  {
    XCBPointer<xcb_get_geometry_reply_t> gwa(dyn_libs::xcb_get_geometry_reply(
      m_connection, dyn_libs::xcb_get_geometry(m_connection, m_parent_window), &xerror));
    if (!gwa)
    {
      SetErrorObject(&error, "xcb_get_geometry() failed: ", xerror);
      ERROR_LOG(error.GetDescription());
      return;
    }

    m_width = gwa->width;
    m_height = gwa->height;
  }

  u32 values[] = {width, height};
  if ((xerror = dyn_libs::xcb_request_check(
         m_connection, dyn_libs::xcb_configure_window_checked(
                         m_connection, m_window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values))))
  {
    SetErrorObject(&error, "xcb_configure_window_checked() failed: ", xerror);
    ERROR_LOG(error.GetDescription());
  }
}

std::optional<float> GetRefreshRateFromXRandR(const WindowInfo& wi, Error* error)
{
  xcb_connection_t* connection = nullptr;
  if (wi.type == WindowInfo::Type::Xlib)
  {
    if (!dyn_libs::OpenX11Xcb(error))
      return std::nullopt;

    connection = dyn_libs::XGetXCBConnection(static_cast<Display*>(wi.display_connection));
  }
  else if (wi.type == WindowInfo::Type::XCB)
  {
    connection = static_cast<xcb_connection_t*>(wi.display_connection);
  }

  xcb_window_t window = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(wi.window_handle));
  if (!connection || window == XCB_NONE)
  {
    Error::SetStringView(error, "Invalid window handle.");
    return std::nullopt;
  }

  if (!dyn_libs::OpenXcb(error) || !dyn_libs::OpenXcbRandR(error))
    return std::nullopt;

  xcb_generic_error_t* xerror;
  XCBPointer<xcb_randr_get_screen_resources_reply_t> gsr(dyn_libs::xcb_randr_get_screen_resources_reply(
    connection, dyn_libs::xcb_randr_get_screen_resources(connection, window), &xerror));
  if (xerror)
  {
    SetErrorObject(error, "xcb_randr_get_screen_resources() failed: ", xerror);
    return std::nullopt;
  }

  XCBPointer<xcb_randr_get_monitors_reply_t> gm(dyn_libs::xcb_randr_get_monitors_reply(
    connection, dyn_libs::xcb_randr_get_monitors(connection, window, true), &xerror));
  if (xerror || gm->nMonitors < 0)
  {
    SetErrorObject(error, "xcb_randr_get_screen_resources() failed: ", xerror);
    return std::nullopt;
  }

  if (gm->nMonitors > 1)
    WARNING_LOG("xcb_randr_get_monitors() returned {} monitors, using first", gm->nMonitors);

  if (gm->nOutputs <= 0)
  {
    Error::SetStringView(error, "Monitor has no outputs");
    return std::nullopt;
  }
  else if (gm->nOutputs > 1)
  {
    WARNING_LOG("Monitor has {} outputs, using first", gm->nOutputs);
  }

  xcb_randr_monitor_info_t* monitor_info = dyn_libs::xcb_randr_get_monitors_monitors_iterator(gm.get()).data;
  DebugAssert(monitor_info);

  xcb_randr_output_t* monitor_outputs = dyn_libs::xcb_randr_monitor_info_outputs(monitor_info);
  DebugAssert(monitor_outputs);

  XCBPointer<xcb_randr_get_output_info_reply_t> goi(dyn_libs::xcb_randr_get_output_info_reply(
    connection, dyn_libs::xcb_randr_get_output_info(connection, monitor_outputs[0], 0), &xerror));
  if (xerror)
  {
    SetErrorObject(error, "xcb_randr_get_output_info() failed: ", xerror);
    return std::nullopt;
  }

  XCBPointer<xcb_randr_get_crtc_info_reply_t> gci(dyn_libs::xcb_randr_get_crtc_info_reply(
    connection, dyn_libs::xcb_randr_get_crtc_info(connection, goi->crtc, 0), &xerror));
  if (xerror)
  {
    SetErrorObject(error, "xcb_randr_get_crtc_info_reply() failed: ", xerror);
    return std::nullopt;
  }

  xcb_randr_mode_info_t* mode = nullptr;
  for (xcb_randr_mode_info_iterator_t it = dyn_libs::xcb_randr_get_screen_resources_modes_iterator(gsr.get());
       it.rem != 0; dyn_libs::xcb_randr_mode_info_next(&it))
  {
    if (it.data->id == gci->mode)
    {
      mode = it.data;
      break;
    }
  }
  if (!mode)
  {
    Error::SetStringFmt(error, "Failed to look up mode ID {}", static_cast<int>(gci->mode));
    return std::nullopt;
  }

  if (mode->dot_clock == 0 || mode->htotal == 0 || mode->vtotal == 0)
  {
    ERROR_LOG("Modeline is invalid: {}/{}/{}", mode->dot_clock, mode->htotal, mode->vtotal);
    return std::nullopt;
  }

  return static_cast<float>(static_cast<double>(mode->dot_clock) /
                            (static_cast<double>(mode->htotal) * static_cast<double>(mode->vtotal)));
}

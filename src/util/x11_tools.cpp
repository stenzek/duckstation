// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "x11_tools.h"
#include "window_info.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"

#include <xcb/xcb.h>

#include <memory>
#include <mutex>

LOG_CHANNEL(WindowInfo);

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

namespace {

template<typename T>
struct XCBPointerDeleter
{
  static void operator()(T* ptr) { free(ptr); }
};

template<typename T>
using XCBPointer = std::unique_ptr<T, XCBPointerDeleter<T>>;

struct Locals
{
  DynamicLibrary xcb_library;
  std::once_flag xcb_once_flag;

#define ADD_FUNC(F) decltype(&::F) F;
  XCB_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC
};

} // namespace

static bool OpenXcb(Error* error);

static Locals s_locals;

bool OpenXcb(Error* error)
{
  if (s_locals.xcb_library.IsOpen())
    return true;

  std::call_once(s_locals.xcb_once_flag, [error]() {
    if (!s_locals.xcb_library.Open(DynamicLibrary::GetVersionedFilename("xcb", 1).c_str(), error))
    {
      Error::AddPrefix(error, "Failed to load xcb: ");
      return;
    }

    static const DynamicLibrary::SymbolTable symbols[] = {
#define SYMBOL_ENTRY(F) {#F, reinterpret_cast<void**>(&s_locals.F)},
      XCB_FUNCTIONS(SYMBOL_ENTRY)
#undef SYMBOL_ENTRY
    };

    if (!s_locals.xcb_library.ResolveSymbols(symbols, error))
      DynamicLibrary::ClearSymbols(symbols);
  });

  return s_locals.xcb_library.IsOpen();
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

  if (!OpenXcb(error))
    return false;

  m_connection = connection;
  m_parent_window = parent_window;

  XCBPointer<xcb_get_geometry_reply_t> gwa(
    s_locals.xcb_get_geometry_reply(connection, s_locals.xcb_get_geometry(connection, parent_window), &xerror));
  if (!gwa)
  {
    SetErrorObject(error, "xcb_get_geometry_reply() failed: ", xerror);
    return false;
  }

  m_width = gwa->width;
  m_height = gwa->height;

  // Need to find the root window to get an appropriate depth. Needed for NVIDIA+XWayland.
  int visual_depth = XCB_COPY_FROM_PARENT;
  for (xcb_screen_iterator_t it = s_locals.xcb_setup_roots_iterator(s_locals.xcb_get_setup(connection)); it.rem != 0;
       s_locals.xcb_screen_next(&it))
  {
    if (it.data->root == gwa->root)
    {
      for (xcb_depth_iterator_t dit = s_locals.xcb_screen_allowed_depths_iterator(it.data); dit.rem != 0;
           s_locals.xcb_depth_next(&dit))
      {
        const int len = s_locals.xcb_depth_visuals_length(dit.data);
        const xcb_visualtype_t* visuals = s_locals.xcb_depth_visuals(dit.data);
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
  m_colormap = s_locals.xcb_generate_id(connection);
  if ((xerror = s_locals.xcb_request_check(
         connection,
         s_locals.xcb_create_colormap_checked(connection, XCB_COLORMAP_ALLOC_NONE, m_colormap, parent_window, vi))))
  {
    SetErrorObject(error, "xcb_create_colormap_checked() failed: ", xerror);
    m_colormap = {};
    return false;
  }

  m_window = s_locals.xcb_generate_id(connection);

  const u32 window_values[] = {XCB_PIXMAP_NONE, 0u, m_colormap};
  xerror = s_locals.xcb_request_check(
    connection,
    s_locals.xcb_create_window_checked(connection, visual_depth, m_window, parent_window, 0, 0, m_width, m_height, 0,
                                       XCB_WINDOW_CLASS_INPUT_OUTPUT, vi,
                                       XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP, window_values));
  if (xerror)
  {
    SetErrorObject(error, "xcb_create_window_checked() failed: ", xerror);
    m_window = {};
    return false;
  }

  xerror = s_locals.xcb_request_check(connection, s_locals.xcb_map_window_checked(connection, m_window));
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
    if ((xerror = s_locals.xcb_request_check(m_connection, s_locals.xcb_unmap_window_checked(m_connection, m_window))))
    {
      SetErrorObject(&error, "xcb_unmap_window_checked() failed: ", xerror);
      ERROR_LOG(error.GetDescription());
    }

    if ((xerror =
           s_locals.xcb_request_check(m_connection, s_locals.xcb_destroy_window_checked(m_connection, m_window))))
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
           s_locals.xcb_request_check(m_connection, s_locals.xcb_free_colormap_checked(m_connection, m_colormap))))
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
    XCBPointer<xcb_get_geometry_reply_t> gwa(
      s_locals.xcb_get_geometry_reply(m_connection, s_locals.xcb_get_geometry(m_connection, m_parent_window), &xerror));
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
  if ((xerror = s_locals.xcb_request_check(
         m_connection, s_locals.xcb_configure_window_checked(
                         m_connection, m_window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values))))
  {
    SetErrorObject(&error, "xcb_configure_window_checked() failed: ", xerror);
    ERROR_LOG(error.GetDescription());
  }
}

std::optional<std::pair<int, int>> X11Tools::GetWindowSize(xcb_connection_t* connection, xcb_window_t window)
{
  if (!OpenXcb(nullptr))
    return std::nullopt;

  xcb_generic_error_t* xerror = nullptr;
  XCBPointer<xcb_get_geometry_reply_t> gwa(
    s_locals.xcb_get_geometry_reply(connection, s_locals.xcb_get_geometry(connection, window), &xerror));
  if (!gwa)
  {
    free(xerror);
    return std::nullopt;
  }

  return std::make_pair(static_cast<int>(gwa->width), static_cast<int>(gwa->height));
}

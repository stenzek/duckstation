// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "x11_tools.h"
#include "window_info.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/scoped_guard.h"

#include <X11/Xlib-xcb.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

#include <memory>

LOG_CHANNEL(WindowInfo);

namespace {
template<typename T>
struct XCBPointerDeleter
{
  void operator()(T* ptr) { free(ptr); }
};

template<typename T>
using XCBPointer = std::unique_ptr<T, XCBPointerDeleter<T>>;
} // namespace

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

  m_connection = connection;
  m_parent_window = parent_window;

  XCBPointer<xcb_get_geometry_reply_t> gwa(
    xcb_get_geometry_reply(connection, xcb_get_geometry(connection, parent_window), &xerror));
  if (!gwa)
  {
    SetErrorObject(error, "xcb_get_geometry_reply() failed: ", xerror);
    return false;
  }

  m_width = gwa->width;
  m_height = gwa->height;

  // Need to find the root window to get an appropriate depth. Needed for NVIDIA+XWayland.
  int visual_depth = XCB_COPY_FROM_PARENT;
  for (xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(connection)); it.rem != 0;
       xcb_screen_next(&it))
  {
    if (it.data->root == gwa->root)
    {
      for (xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(it.data); dit.rem != 0; xcb_depth_next(&dit))
      {
        const int len = xcb_depth_visuals_length(dit.data);
        const xcb_visualtype_t* visuals = xcb_depth_visuals(dit.data);
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
  m_colormap = xcb_generate_id(connection);
  if ((xerror = xcb_request_check(
         connection, xcb_create_colormap_checked(connection, XCB_COLORMAP_ALLOC_NONE, m_colormap, parent_window, vi))))
  {
    SetErrorObject(error, "xcb_create_colormap_checked() failed: ", xerror);
    m_colormap = {};
    return false;
  }

  m_window = xcb_generate_id(connection);

  const u32 window_values[] = {XCB_PIXMAP_NONE, 0u, m_colormap};
  xerror = xcb_request_check(
    connection, xcb_create_window_checked(connection, visual_depth, m_window, parent_window, 0, 0, m_width, m_height, 0,
                                          XCB_WINDOW_CLASS_INPUT_OUTPUT, vi,
                                          XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP, window_values));
  if (xerror)
  {
    SetErrorObject(error, "xcb_create_window_checked() failed: ", xerror);
    m_window = {};
    return false;
  }

  xerror = xcb_request_check(connection, xcb_map_window_checked(connection, m_window));
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
    if ((xerror = xcb_request_check(m_connection, xcb_unmap_window_checked(m_connection, m_window))))
    {
      SetErrorObject(&error, "xcb_unmap_window_checked() failed: ", xerror);
      ERROR_LOG(error.GetDescription());
    }

    if ((xerror = xcb_request_check(m_connection, xcb_destroy_window_checked(m_connection, m_window))))
    {
      SetErrorObject(&error, "xcb_destroy_window_checked() failed: ", xerror);
      ERROR_LOG(error.GetDescription());
    }

    m_window = {};
    m_parent_window = {};
  }

  if (m_colormap)
  {
    if ((xerror = xcb_request_check(m_connection, xcb_free_colormap_checked(m_connection, m_colormap))))
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
      xcb_get_geometry_reply(m_connection, xcb_get_geometry(m_connection, m_parent_window), &xerror));
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
  if ((xerror = xcb_request_check(
         m_connection, xcb_configure_window_checked(m_connection, m_window,
                                                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values))))
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
    connection = XGetXCBConnection(static_cast<Display*>(wi.display_connection));
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

  xcb_generic_error_t* xerror;
  XCBPointer<xcb_randr_get_screen_resources_reply_t> gsr(
    xcb_randr_get_screen_resources_reply(connection, xcb_randr_get_screen_resources(connection, window), &xerror));
  if (xerror)
  {
    SetErrorObject(error, "xcb_randr_get_screen_resources() failed: ", xerror);
    return std::nullopt;
  }

  XCBPointer<xcb_randr_get_monitors_reply_t> gm(
    xcb_randr_get_monitors_reply(connection, xcb_randr_get_monitors(connection, window, true), &xerror));
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

  xcb_randr_monitor_info_t* monitor_info = xcb_randr_get_monitors_monitors_iterator(gm.get()).data;
  DebugAssert(monitor_info);

  xcb_randr_output_t* monitor_outputs = xcb_randr_monitor_info_outputs(monitor_info);
  DebugAssert(monitor_outputs);

  XCBPointer<xcb_randr_get_output_info_reply_t> goi(
    xcb_randr_get_output_info_reply(connection, xcb_randr_get_output_info(connection, monitor_outputs[0], 0), &xerror));
  if (xerror)
  {
    SetErrorObject(error, "xcb_randr_get_output_info() failed: ", xerror);
    return std::nullopt;
  }

  XCBPointer<xcb_randr_get_crtc_info_reply_t> gci(
    xcb_randr_get_crtc_info_reply(connection, xcb_randr_get_crtc_info(connection, goi->crtc, 0), &xerror));
  if (xerror)
  {
    SetErrorObject(error, "xcb_randr_get_crtc_info_reply() failed: ", xerror);
    return std::nullopt;
  }

  xcb_randr_mode_info_t* mode = nullptr;
  for (xcb_randr_mode_info_iterator_t it = xcb_randr_get_screen_resources_modes_iterator(gsr.get()); it.rem != 0;
       xcb_randr_mode_info_next(&it))
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

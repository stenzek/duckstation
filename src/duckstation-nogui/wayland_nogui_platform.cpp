#include "wayland_nogui_platform.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "nogui_host.h"
#include "nogui_platform.h"

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

Log_SetChannel(WaylandNoGUIPlatform);

WaylandNoGUIPlatform::WaylandNoGUIPlatform()
{
  m_message_loop_running.store(true, std::memory_order_release);
}

WaylandNoGUIPlatform::~WaylandNoGUIPlatform()
{
  if (m_xkb_state)
    xkb_state_unref(m_xkb_state);
  if (m_xkb_keymap)
    xkb_keymap_unref(m_xkb_keymap);
  if (m_wl_keyboard)
    wl_keyboard_destroy(m_wl_keyboard);
  if (m_wl_pointer)
    wl_pointer_destroy(m_wl_pointer);
  if (m_wl_seat)
    wl_seat_destroy(m_wl_seat);
  if (m_xkb_context)
    xkb_context_unref(m_xkb_context);
  if (m_registry)
    wl_registry_destroy(m_registry);
}

bool WaylandNoGUIPlatform::Initialize()
{
  m_xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!m_xkb_context)
  {
    Panic("Failed to create XKB context");
    return false;
  }

  m_display = wl_display_connect(nullptr);
  if (!m_display)
  {
    Panic("Failed to connect to Wayland display.");
    return false;
  }

  static const wl_registry_listener registry_listener = {GlobalRegistryHandler, GlobalRegistryRemover};
  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &registry_listener, this);

  // Call back to registry listener to get compositor/shell.
  wl_display_dispatch_pending(m_display);
  wl_display_roundtrip(m_display);

  // We need a shell/compositor, or at least one we understand.
  if (!m_compositor || !m_xdg_wm_base)
  {
    Panic("Missing Wayland shell/compositor\n");
    return false;
  }

  static const xdg_wm_base_listener xdg_wm_base_listener = {XDGWMBasePing};
  xdg_wm_base_add_listener(m_xdg_wm_base, &xdg_wm_base_listener, this);
  wl_display_dispatch_pending(m_display);
  wl_display_roundtrip(m_display);
  return true;
}

void WaylandNoGUIPlatform::ReportError(const std::string_view& title, const std::string_view& message)
{
  // not implemented
}

bool WaylandNoGUIPlatform::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
  // not implemented
  return true;
}

void WaylandNoGUIPlatform::SetDefaultConfig(SettingsInterface& si) {}

bool WaylandNoGUIPlatform::CreatePlatformWindow(std::string title)
{
  s32 window_x, window_y, window_width, window_height;
  bool has_window_pos = NoGUIHost::GetSavedPlatformWindowGeometry(&window_x, &window_y, &window_width, &window_height);
  if (!has_window_pos)
  {
    window_x = 0;
    window_y = 0;
    window_width = DEFAULT_WINDOW_WIDTH;
    window_height = DEFAULT_WINDOW_HEIGHT;
  }

  // Create the compositor and shell surface.
  if (!(m_surface = wl_compositor_create_surface(m_compositor)) ||
      !(m_xdg_surface = xdg_wm_base_get_xdg_surface(m_xdg_wm_base, m_surface)) ||
      !(m_xdg_toplevel = xdg_surface_get_toplevel(m_xdg_surface)))
  {
    Log_ErrorPrintf("Failed to create compositor/shell surfaces");
    return false;
  }

  static const xdg_surface_listener shell_surface_listener = {XDGSurfaceConfigure};
  xdg_surface_add_listener(m_xdg_surface, &shell_surface_listener, this);

  static const xdg_toplevel_listener toplevel_listener = {TopLevelConfigure, TopLevelClose};
  xdg_toplevel_add_listener(m_xdg_toplevel, &toplevel_listener, this);

  // Create region in the surface to draw into.
  m_region = wl_compositor_create_region(m_compositor);
  wl_region_add(m_region, 0, 0, window_width, window_height);
  wl_surface_set_opaque_region(m_surface, m_region);
  wl_surface_commit(m_surface);

  // This doesn't seem to have any effect on kwin...
  if (has_window_pos)
  {
    xdg_surface_set_window_geometry(m_xdg_surface, window_x, window_y, window_width, window_height);
  }

  if (m_decoration_manager)
  {
    m_toplevel_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(m_decoration_manager, m_xdg_toplevel);
    if (m_toplevel_decoration)
      zxdg_toplevel_decoration_v1_set_mode(m_toplevel_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }

  m_window_info.surface_width = static_cast<u32>(window_width);
  m_window_info.surface_height = static_cast<u32>(window_height);
  m_window_info.surface_scale = 1.0f;
  m_window_info.type = WindowInfo::Type::Wayland;
  m_window_info.window_handle = m_surface;
  m_window_info.display_connection = m_display;

  wl_display_dispatch_pending(m_display);
  wl_display_roundtrip(m_display);
  return true;
}

void WaylandNoGUIPlatform::DestroyPlatformWindow()
{
  m_window_info = {};

  if (m_toplevel_decoration)
  {
    zxdg_toplevel_decoration_v1_destroy(m_toplevel_decoration);
    m_toplevel_decoration = {};
  }

  if (m_xdg_toplevel)
  {
    xdg_toplevel_destroy(m_xdg_toplevel);
    m_xdg_toplevel = {};
  }

  if (m_xdg_surface)
  {
    xdg_surface_destroy(m_xdg_surface);
    m_xdg_surface = {};
  }

  if (m_surface)
  {
    wl_surface_destroy(m_surface);
    m_surface = {};
  }

  wl_display_dispatch_pending(m_display);
  wl_display_roundtrip(m_display);
}

std::optional<WindowInfo> WaylandNoGUIPlatform::GetPlatformWindowInfo()
{
  if (m_window_info.type == WindowInfo::Type::Wayland)
    return m_window_info;
  else
    return std::nullopt;
}

void WaylandNoGUIPlatform::SetPlatformWindowTitle(std::string title)
{
  if (m_xdg_toplevel)
    xdg_toplevel_set_title(m_xdg_toplevel, title.c_str());
}

void* WaylandNoGUIPlatform::GetPlatformWindowHandle()
{
  return m_surface;
}

std::optional<u32> WaylandNoGUIPlatform::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
  std::unique_lock lock(m_key_map_mutex);
  for (const auto& it : m_key_map)
  {
    if (StringUtil::Strncasecmp(it.second.c_str(), str.data(), str.length()) == 0)
      return it.first;
  }

  return std::nullopt;
}

std::optional<std::string> WaylandNoGUIPlatform::ConvertHostKeyboardCodeToString(u32 code)
{
  std::unique_lock lock(m_key_map_mutex);
  const auto it = m_key_map.find(static_cast<s32>(code));
  return (it != m_key_map.end()) ? std::optional<std::string>(it->second) : std::nullopt;
}

void WaylandNoGUIPlatform::GlobalRegistryHandler(void* data, wl_registry* registry, uint32_t id, const char* interface,
                                                 uint32_t version)
{
  WaylandNoGUIPlatform* platform = static_cast<WaylandNoGUIPlatform*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0)
  {
    platform->m_compositor =
      static_cast<wl_compositor*>(wl_registry_bind(platform->m_registry, id, &wl_compositor_interface, 1));
  }
  else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0)
  {
    platform->m_xdg_wm_base =
      static_cast<xdg_wm_base*>(wl_registry_bind(platform->m_registry, id, &xdg_wm_base_interface, 1));
  }
  else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
  {
    platform->m_decoration_manager = static_cast<zxdg_decoration_manager_v1*>(
      wl_registry_bind(platform->m_registry, id, &zxdg_decoration_manager_v1_interface, 1));
  }
  else if (std::strcmp(interface, wl_seat_interface.name) == 0)
  {
    static const wl_seat_listener seat_listener = {&WaylandNoGUIPlatform::SeatCapabilities};
    platform->m_wl_seat = static_cast<wl_seat*>(wl_registry_bind(registry, id, &wl_seat_interface, 1));
    wl_seat_add_listener(platform->m_wl_seat, &seat_listener, platform);
  }
}

void WaylandNoGUIPlatform::GlobalRegistryRemover(void* data, wl_registry* registry, uint32_t id) {}

void WaylandNoGUIPlatform::XDGWMBasePing(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial)
{
  xdg_wm_base_pong(xdg_wm_base, serial);
}

void WaylandNoGUIPlatform::XDGSurfaceConfigure(void* data, struct xdg_surface* xdg_surface, uint32_t serial)
{
  xdg_surface_ack_configure(xdg_surface, serial);
}

void WaylandNoGUIPlatform::TopLevelConfigure(void* data, struct xdg_toplevel* xdg_toplevel, int32_t width,
                                             int32_t height, struct wl_array* states)
{
  // If this is zero, it's asking us to set the size.
  if (width == 0 || height == 0)
    return;

  WaylandNoGUIPlatform* platform = static_cast<WaylandNoGUIPlatform*>(data);
  platform->m_window_info.surface_width = width;
  platform->m_window_info.surface_height = height;

  NoGUIHost::ProcessPlatformWindowResize(width, height, platform->m_window_info.surface_scale);
}

void WaylandNoGUIPlatform::TopLevelClose(void* data, struct xdg_toplevel* xdg_toplevel)
{
  Host::RunOnCPUThread([]() { Host::RequestExit(g_settings.save_state_on_exit); });
}

void WaylandNoGUIPlatform::SeatCapabilities(void* data, wl_seat* seat, uint32_t capabilities)
{
  WaylandNoGUIPlatform* platform = static_cast<WaylandNoGUIPlatform*>(data);
  if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD)
  {
    static const wl_keyboard_listener keyboard_listener = {
      &WaylandNoGUIPlatform::KeyboardKeymap, &WaylandNoGUIPlatform::KeyboardEnter, &WaylandNoGUIPlatform::KeyboardLeave,
      &WaylandNoGUIPlatform::KeyboardKey, &WaylandNoGUIPlatform::KeyboardModifiers};
    platform->m_wl_keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(platform->m_wl_keyboard, &keyboard_listener, platform);
  }
  if (capabilities & WL_SEAT_CAPABILITY_POINTER)
  {
    static const wl_pointer_listener pointer_listener = {
      &WaylandNoGUIPlatform::PointerEnter, &WaylandNoGUIPlatform::PointerLeave, &WaylandNoGUIPlatform::PointerMotion,
      &WaylandNoGUIPlatform::PointerButton, &WaylandNoGUIPlatform::PointerAxis};
    platform->m_wl_pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(platform->m_wl_pointer, &pointer_listener, platform);
  }
}

void WaylandNoGUIPlatform::KeyboardKeymap(void* data, wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size)
{
  WaylandNoGUIPlatform* platform = static_cast<WaylandNoGUIPlatform*>(data);
  char* keymap_string = static_cast<char*>(mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0));
  if (platform->m_xkb_keymap)
    xkb_keymap_unref(platform->m_xkb_keymap);
  platform->m_xkb_keymap = xkb_keymap_new_from_string(platform->m_xkb_context, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(keymap_string, size);
  close(fd);

  if (platform->m_xkb_state)
    xkb_state_unref(platform->m_xkb_state);
  platform->m_xkb_state = xkb_state_new(platform->m_xkb_keymap);

  platform->InitializeKeyMap();
}

void WaylandNoGUIPlatform::InitializeKeyMap()
{
  m_key_map.clear();
  Log_VerbosePrintf("Init keymap");

  const xkb_keycode_t min_keycode = xkb_keymap_min_keycode(m_xkb_keymap);
  const xkb_keycode_t max_keycode = xkb_keymap_max_keycode(m_xkb_keymap);
  DebugAssert(max_keycode >= min_keycode);

  for (xkb_keycode_t keycode = min_keycode; keycode <= max_keycode; keycode++)
  {
    const xkb_layout_index_t num_layouts = xkb_keymap_num_layouts_for_key(m_xkb_keymap, keycode);
    if (num_layouts == 0)
      continue;

    // Take the first layout which we find a valid keysym for.
    bool found_keysym = false;
    for (xkb_layout_index_t layout = 0; layout < num_layouts && !found_keysym; layout++)
    {
      const xkb_level_index_t num_levels = xkb_keymap_num_levels_for_key(m_xkb_keymap, keycode, layout);
      if (num_levels == 0)
        continue;

      // Take the first level which we find a valid keysym for.
      for (xkb_level_index_t level = 0; level < num_levels; level++)
      {
        const xkb_keysym_t* keysyms;
        const int num_syms = xkb_keymap_key_get_syms_by_level(m_xkb_keymap, keycode, layout, level, &keysyms);
        if (num_syms == 0)
          continue;

        // Just take the first. Should only be one in most cases anyway.
        const xkb_keysym_t keysym = xkb_keysym_to_upper(keysyms[0]);

        char keysym_name_buf[64];
        if (xkb_keysym_get_name(keysym, keysym_name_buf, sizeof(keysym_name_buf)) <= 0)
          continue;

        m_key_map.emplace(static_cast<s32>(keycode), keysym_name_buf);
        found_keysym = false;
        break;
      }
    }
  }
}

void WaylandNoGUIPlatform::KeyboardEnter(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface,
                                         wl_array* keys)
{
}

void WaylandNoGUIPlatform::KeyboardLeave(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface) {}

void WaylandNoGUIPlatform::KeyboardKey(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key,
                                       uint32_t state)
{
  const xkb_keycode_t keycode = static_cast<xkb_keycode_t>(key + 8);
  const bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
  NoGUIHost::ProcessPlatformKeyEvent(static_cast<s32>(keycode), pressed);
}

void WaylandNoGUIPlatform::KeyboardModifiers(void* data, wl_keyboard* keyboard, uint32_t serial,
                                             uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
                                             uint32_t group)
{
  WaylandNoGUIPlatform* platform = static_cast<WaylandNoGUIPlatform*>(data);
  xkb_state_update_mask(platform->m_xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

void WaylandNoGUIPlatform::PointerEnter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface,
                                        wl_fixed_t surface_x, wl_fixed_t surface_y)
{
}

void WaylandNoGUIPlatform::PointerLeave(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface) {}

void WaylandNoGUIPlatform::PointerMotion(void* data, wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
  const float pos_x = static_cast<float>(wl_fixed_to_double(x));
  const float pos_y = static_cast<float>(wl_fixed_to_double(y));
  NoGUIHost::ProcessPlatformMouseMoveEvent(static_cast<int>(pos_x), static_cast<int>(pos_y));
}

void WaylandNoGUIPlatform::PointerButton(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time,
                                         uint32_t button, uint32_t state)
{
  if (button < BTN_MOUSE || (button - BTN_MOUSE) >= 32)
    return;

  const s32 button_index = (button - BTN_MOUSE);
  const bool button_pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
  NoGUIHost::ProcessPlatformMouseButtonEvent(button_index, button_pressed);
}

void WaylandNoGUIPlatform::PointerAxis(void* data, wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
  const float x = (axis == 1) ? std::clamp(static_cast<float>(wl_fixed_to_double(value)), -1.0f, 1.0f) : 0.0f;
  const float y = (axis == 0) ? std::clamp(static_cast<float>(-wl_fixed_to_double(value)), -1.0f, 1.0f) : 0.0f;
  NoGUIHost::ProcessPlatformMouseWheelEvent(x, y);
}

void WaylandNoGUIPlatform::RunMessageLoop()
{
  while (m_message_loop_running.load(std::memory_order_acquire))
  {
    wl_display_dispatch_pending(m_display);

    {
      std::unique_lock lock(m_callback_queue_mutex);
      while (!m_callback_queue.empty())
      {
        std::function<void()> func = std::move(m_callback_queue.front());
        m_callback_queue.pop_front();
        lock.unlock();
        func();
        lock.lock();
      }
    }

    // TODO: Make this suck less.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void WaylandNoGUIPlatform::ExecuteInMessageLoop(std::function<void()> func)
{
  std::unique_lock lock(m_callback_queue_mutex);
  m_callback_queue.push_back(std::move(func));
}

void WaylandNoGUIPlatform::QuitMessageLoop()
{
  m_message_loop_running.store(false, std::memory_order_release);
}

void WaylandNoGUIPlatform::SetFullscreen(bool enabled)
{
  // how the heck can we do this?
}

bool WaylandNoGUIPlatform::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  return false;
}

bool WaylandNoGUIPlatform::OpenURL(const std::string_view& url)
{
  Log_ErrorPrintf("WaylandNoGUIPlatform::OpenURL() not implemented: %.*s", static_cast<int>(url.size()), url.data());
  return false;
}

bool WaylandNoGUIPlatform::CopyTextToClipboard(const std::string_view& text)
{
  Log_ErrorPrintf("WaylandNoGUIPlatform::CopyTextToClipboard() not implemented: %.*s", static_cast<int>(text.size()), text.data());
  return false;
}

std::unique_ptr<NoGUIPlatform> NoGUIPlatform::CreateWaylandPlatform()
{
  std::unique_ptr<WaylandNoGUIPlatform> ret = std::unique_ptr<WaylandNoGUIPlatform>(new WaylandNoGUIPlatform());
  if (!ret->Initialize())
    return {};

  return ret;
}

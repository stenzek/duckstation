#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "nogui_platform.h"

#include "wayland-xdg-decoration-client-protocol.h"
#include "wayland-xdg-shell-client-protocol.h"
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

class WaylandNoGUIPlatform : public NoGUIPlatform
{
public:
  WaylandNoGUIPlatform();
  ~WaylandNoGUIPlatform();

  bool Initialize();

  void ReportError(const std::string_view& title, const std::string_view& message) override;
  bool ConfirmMessage(const std::string_view& title, const std::string_view& message) override;

  void SetDefaultConfig(SettingsInterface& si) override;

  bool CreatePlatformWindow(std::string title) override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;
  void SetPlatformWindowTitle(std::string title) override;
  void* GetPlatformWindowHandle() override;

  std::optional<u32> ConvertHostKeyboardStringToCode(const std::string_view& str) override;
  std::optional<std::string> ConvertHostKeyboardCodeToString(u32 code) override;

  void RunMessageLoop() override;
  void ExecuteInMessageLoop(std::function<void()> func) override;
  void QuitMessageLoop() override;

  void SetFullscreen(bool enabled) override;

  bool RequestRenderWindowSize(s32 new_window_width, s32 new_window_height) override;

  bool OpenURL(const std::string_view& url) override;
  bool CopyTextToClipboard(const std::string_view& text) override;

private:
  void InitializeKeyMap();

  static void GlobalRegistryHandler(void* data, wl_registry* registry, uint32_t id, const char* interface,
                                    uint32_t version);
  static void GlobalRegistryRemover(void* data, wl_registry* registry, uint32_t id);
  static void XDGWMBasePing(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial);
  static void XDGSurfaceConfigure(void* data, struct xdg_surface* xdg_surface, uint32_t serial);
  static void TopLevelConfigure(void* data, struct xdg_toplevel* xdg_toplevel, int32_t width, int32_t height,
                                struct wl_array* states);
  static void PointerEnter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface, wl_fixed_t surface_x,
                           wl_fixed_t surface_y);
  static void PointerLeave(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface);
  static void PointerMotion(void* data, wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y);
  static void PointerButton(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button,
                            uint32_t state);
  static void PointerAxis(void* data, wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
  static void KeyboardKeymap(void* data, wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size);
  static void KeyboardEnter(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface, wl_array* keys);
  static void KeyboardLeave(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface);
  static void KeyboardKey(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key,
                          uint32_t state);
  static void KeyboardModifiers(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t mods_depressed,
                                uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
  static void SeatCapabilities(void* data, wl_seat* seat, uint32_t capabilities);
  static void TopLevelClose(void* data, struct xdg_toplevel* xdg_toplevel);

  std::atomic_bool m_message_loop_running{false};
  // std::atomic_bool m_fullscreen{false};

  WindowInfo m_window_info = {};

  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  wl_compositor* m_compositor = nullptr;
  xdg_wm_base* m_xdg_wm_base = nullptr;
  wl_surface* m_surface = nullptr;
  wl_region* m_region = nullptr;
  xdg_surface* m_xdg_surface = nullptr;
  xdg_toplevel* m_xdg_toplevel = nullptr;
  zxdg_decoration_manager_v1* m_decoration_manager = nullptr;
  zxdg_toplevel_decoration_v1* m_toplevel_decoration = nullptr;
  wl_seat* m_wl_seat = nullptr;
  wl_keyboard* m_wl_keyboard = nullptr;
  wl_pointer* m_wl_pointer = nullptr;
  xkb_context* m_xkb_context = nullptr;
  xkb_keymap* m_xkb_keymap = nullptr;
  xkb_state* m_xkb_state = nullptr;

  std::unordered_map<s32, std::string> m_key_map;
  std::mutex m_key_map_mutex;

  std::deque<std::function<void()>> m_callback_queue;
  std::mutex m_callback_queue_mutex;
};

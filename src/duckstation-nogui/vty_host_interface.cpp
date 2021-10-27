#include "vty_host_interface.h"
#include "common/log.h"
#include "common/string_util.h"
#include "evdev_key_names.h"
#include "frontend-common/ini_settings_interface.h"
#include "imgui.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <unistd.h>
Log_SetChannel(VTYHostInterface);

#ifdef WITH_DRMKMS
#include "common/drm_display.h"
#endif

VTYHostInterface::VTYHostInterface() = default;

VTYHostInterface::~VTYHostInterface()
{
  CloseEVDevFDs();
}

std::unique_ptr<NoGUIHostInterface> VTYHostInterface::Create()
{
  return std::make_unique<VTYHostInterface>();
}

bool VTYHostInterface::Initialize()
{
  if (!NoGUIHostInterface::Initialize())
    return false;

  OpenEVDevFDs();

  signal(SIGTERM, SIGTERMHandler);
  signal(SIGINT, SIGTERMHandler);
  signal(SIGQUIT, SIGTERMHandler);
  return true;
}

void VTYHostInterface::Shutdown()
{
  CloseEVDevFDs();
  NoGUIHostInterface::Shutdown();
}

bool VTYHostInterface::IsFullscreen() const
{
  return true;
}

bool VTYHostInterface::SetFullscreen(bool enabled)
{
  return enabled;
}

void VTYHostInterface::FixIncompatibleSettings(bool display_osd_messages)
{
  NoGUIHostInterface::FixIncompatibleSettings(display_osd_messages);

  // Some things we definitely don't want.
  g_settings.confim_power_off = false;
}

bool VTYHostInterface::CreatePlatformWindow()
{
  SetImGuiKeyMap();
  return true;
}

void VTYHostInterface::DestroyPlatformWindow()
{
  // nothing to destroy, it's all in the context
}

std::optional<WindowInfo> VTYHostInterface::GetPlatformWindowInfo()
{
  WindowInfo wi;
  wi.type = WindowInfo::Type::Display;
  wi.surface_width = 0;
  wi.surface_height = 0;
  wi.surface_refresh_rate = 0.0f;
  wi.surface_format = WindowInfo::SurfaceFormat::Auto;

  const std::string fullscreen_mode = m_settings_interface->GetStringValue("GPU", "FullscreenMode", "");
  if (!fullscreen_mode.empty())
  {
    if (!ParseFullscreenMode(fullscreen_mode, &wi.surface_width, &wi.surface_height, &wi.surface_refresh_rate))
    {
      Log_ErrorPrintf("Failed to parse fullscreen mode '%s'", fullscreen_mode.c_str());
    }
  }

#ifdef WITH_DRMKMS
  // set to current mode
  if (wi.surface_width == 0)
  {
    if (!DRMDisplay::GetCurrentMode(&wi.surface_width, &wi.surface_height, &wi.surface_refresh_rate))
      Log_ErrorPrintf("Failed to get current mode, will use default.");
  }
#endif

  return wi;
}

void VTYHostInterface::PollAndUpdate()
{
  PollEvDevKeyboards();

  NoGUIHostInterface::PollAndUpdate();
}

void VTYHostInterface::SetMouseMode(bool relative, bool hide_cursor) {}

void VTYHostInterface::OpenEVDevFDs()
{
  for (int i = 0; i < 1000; i++)
  {
    TinyString path;
    path.Format("/dev/input/event%d", i);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      break;

    struct libevdev* obj;
    if (libevdev_new_from_fd(fd, &obj) != 0)
    {
      Log_ErrorPrintf("libevdev_new_from_fd(%s) failed", path.GetCharArray());
      close(fd);
      continue;
    }

    Log_DevPrintf("Input path: %s", path.GetCharArray());
    Log_DevPrintf("Input device name: \"%s\"", libevdev_get_name(obj));
    Log_DevPrintf("Input device ID: bus %#x vendor %#x product %#x", libevdev_get_id_bustype(obj),
                  libevdev_get_id_vendor(obj), libevdev_get_id_product(obj));
    if (!libevdev_has_event_code(obj, EV_KEY, KEY_SPACE))
    {
      Log_DevPrintf("This device does not look like a keyboard");
      libevdev_free(obj);
      close(fd);
      continue;
    }

    const int grab_res = libevdev_grab(obj, LIBEVDEV_GRAB);
    if (grab_res != 0)
      Log_WarningPrintf("Failed to grab '%s' (%s): %d", libevdev_get_name(obj), path.GetCharArray(), grab_res);

    m_evdev_keyboards.push_back({obj, fd});
  }
}

void VTYHostInterface::CloseEVDevFDs()
{
  for (const EvDevKeyboard& kb : m_evdev_keyboards)
  {
    libevdev_grab(kb.obj, LIBEVDEV_UNGRAB);
    libevdev_free(kb.obj);
    close(kb.fd);
  }
  m_evdev_keyboards.clear();
}

void VTYHostInterface::PollEvDevKeyboards()
{
  for (const EvDevKeyboard& kb : m_evdev_keyboards)
  {
    struct input_event ev;
    while (libevdev_next_event(kb.obj, LIBEVDEV_READ_FLAG_NORMAL, &ev) == 0)
    {
      // auto-repeat
      if (ev.value == 2)
        continue;

      const bool pressed = (ev.value == 1);
      const HostKeyCode code = static_cast<HostKeyCode>(ev.code);
      if (static_cast<unsigned>(code) < countof(ImGuiIO::KeysDown))
        ImGui::GetIO().KeysDown[code] = pressed;

      HandleHostKeyEvent(code, 0, pressed);
    }
  }
}

void VTYHostInterface::SetImGuiKeyMap()
{
  ImGuiIO& io = ImGui::GetIO();

  io.KeyMap[ImGuiKey_Tab] = KEY_TAB;
  io.KeyMap[ImGuiKey_LeftArrow] = KEY_LEFT;
  io.KeyMap[ImGuiKey_RightArrow] = KEY_RIGHT;
  io.KeyMap[ImGuiKey_UpArrow] = KEY_UP;
  io.KeyMap[ImGuiKey_DownArrow] = KEY_DOWN;
  io.KeyMap[ImGuiKey_PageUp] = KEY_PAGEUP;
  io.KeyMap[ImGuiKey_PageDown] = KEY_PAGEDOWN;
  io.KeyMap[ImGuiKey_Home] = KEY_HOME;
  io.KeyMap[ImGuiKey_End] = KEY_END;
  io.KeyMap[ImGuiKey_Insert] = KEY_INSERT;
  io.KeyMap[ImGuiKey_Delete] = KEY_DELETE;
  io.KeyMap[ImGuiKey_Backspace] = KEY_BACKSPACE;
  io.KeyMap[ImGuiKey_Space] = KEY_SPACE;
  io.KeyMap[ImGuiKey_Enter] = KEY_ENTER;
  io.KeyMap[ImGuiKey_Escape] = KEY_ESC;
  io.KeyMap[ImGuiKey_KeyPadEnter] = KEY_KPENTER;
  io.KeyMap[ImGuiKey_A] = KEY_A;
  io.KeyMap[ImGuiKey_C] = KEY_C;
  io.KeyMap[ImGuiKey_V] = KEY_V;
  io.KeyMap[ImGuiKey_X] = KEY_X;
  io.KeyMap[ImGuiKey_Y] = KEY_Y;
  io.KeyMap[ImGuiKey_Z] = KEY_Z;
}

std::optional<VTYHostInterface::HostKeyCode> VTYHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  std::optional<int> kc = EvDevKeyNames::GetKeyCodeForName(key_code);
  if (!kc.has_value())
    return std::nullopt;

  return static_cast<HostKeyCode>(kc.value());
}

void VTYHostInterface::SIGTERMHandler(int sig)
{
  Log_InfoPrintf("Recieved SIGTERM");
  static_cast<VTYHostInterface*>(g_host_interface)->m_quit_request = true;
  signal(sig, SIG_DFL);
}

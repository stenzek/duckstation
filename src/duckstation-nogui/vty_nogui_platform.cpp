#include "vty_nogui_platform.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "nogui_host.h"
#include "resource.h"
#include "vty_key_names.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <thread>
#include <unistd.h>
Log_SetChannel(VTYNoGUIPlatform);

#ifdef WITH_DRMKMS
#include "common/drm_display.h"
#endif

VTYNoGUIPlatform::VTYNoGUIPlatform()
{
  m_message_loop_running.store(true, std::memory_order_release);
}

VTYNoGUIPlatform::~VTYNoGUIPlatform()
{
  CloseEVDevFDs();
}

std::unique_ptr<NoGUIPlatform> NoGUIPlatform::CreateVTYPlatform()
{
  std::unique_ptr<VTYNoGUIPlatform> platform(std::make_unique<VTYNoGUIPlatform>());
  if (!platform->Initialize())
    platform.reset();
  return platform;
}

bool VTYNoGUIPlatform::Initialize()
{
  OpenEVDevFDs();
  return true;
}

void VTYNoGUIPlatform::ReportError(const std::string_view& title, const std::string_view& message)
{
  // not implemented
}

bool VTYNoGUIPlatform::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
  // not implemented
  return true;
}

void VTYNoGUIPlatform::SetDefaultConfig(SettingsInterface& si)
{
  // noop
}

bool VTYNoGUIPlatform::CreatePlatformWindow(std::string title)
{
  return true;
}

void VTYNoGUIPlatform::DestroyPlatformWindow()
{
  // noop
}

std::optional<WindowInfo> VTYNoGUIPlatform::GetPlatformWindowInfo()
{
  WindowInfo wi;
  wi.type = WindowInfo::Type::Display;
  wi.surface_width = 0;
  wi.surface_height = 0;
  wi.surface_refresh_rate = 0.0f;
  wi.surface_format = WindowInfo::SurfaceFormat::Auto;

  const std::string fullscreen_mode = Host::GetStringSettingValue("GPU", "FullscreenMode", "");
  if (!fullscreen_mode.empty())
  {
    if (!HostDisplay::ParseFullscreenMode(fullscreen_mode, &wi.surface_width, &wi.surface_height,
                                          &wi.surface_refresh_rate))
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

  // This isn't great, but it's an approximation at least..
  if (wi.surface_width > 0)
    wi.surface_scale = std::max(0.1f, static_cast<float>(wi.surface_width) / 1280.0f);

  return wi;
}

void VTYNoGUIPlatform::SetFullscreen(bool enabled)
{
  // already fullscreen :-)
}

bool VTYNoGUIPlatform::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  return false;
}

bool VTYNoGUIPlatform::OpenURL(const std::string_view& url)
{
  Log_ErrorPrintf("VTYNoGUIPlatform::OpenURL() not implemented: %.*s", static_cast<int>(url.size()), url.data());
  return false;
}

bool VTYNoGUIPlatform::CopyTextToClipboard(const std::string_view& text)
{
  Log_ErrorPrintf("VTYNoGUIPlatform::CopyTextToClipboard() not implemented: %.*s", static_cast<int>(text.size()),
                  text.data());
  return false;
}

void VTYNoGUIPlatform::SetPlatformWindowTitle(std::string title)
{
  Log_InfoPrintf("Window Title: %s", title.c_str());
}

void* VTYNoGUIPlatform::GetPlatformWindowHandle()
{
  return nullptr;
}

void VTYNoGUIPlatform::RunMessageLoop()
{
  while (m_message_loop_running.load(std::memory_order_acquire))
  {
    PollEvDevKeyboards();

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

void VTYNoGUIPlatform::ExecuteInMessageLoop(std::function<void()> func)
{
  std::unique_lock lock(m_callback_queue_mutex);
  m_callback_queue.push_back(std::move(func));
}

void VTYNoGUIPlatform::QuitMessageLoop()
{
  m_message_loop_running.store(false, std::memory_order_release);
}

void VTYNoGUIPlatform::OpenEVDevFDs()
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

void VTYNoGUIPlatform::CloseEVDevFDs()
{
  for (const EvDevKeyboard& kb : m_evdev_keyboards)
  {
    libevdev_grab(kb.obj, LIBEVDEV_UNGRAB);
    libevdev_free(kb.obj);
    close(kb.fd);
  }
  m_evdev_keyboards.clear();
}

void VTYNoGUIPlatform::PollEvDevKeyboards()
{
  for (const EvDevKeyboard& kb : m_evdev_keyboards)
  {
    struct input_event ev;
    while (libevdev_next_event(kb.obj, LIBEVDEV_READ_FLAG_NORMAL, &ev) == 0)
    {
      // auto-repeat
      // TODO: forward char to imgui
      if (ev.value == 2)
        continue;

      const bool pressed = (ev.value == 1);
      NoGUIHost::ProcessPlatformKeyEvent(static_cast<s32>(ev.code), pressed);
    }
  }
}

std::optional<u32> VTYNoGUIPlatform::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
  std::optional<int> converted(VTYKeyNames::GetKeyCodeForName(str));
  return converted.has_value() ? std::optional<u32>(static_cast<u32>(converted.value())) : std::nullopt;
}

std::optional<std::string> VTYNoGUIPlatform::ConvertHostKeyboardCodeToString(u32 code)
{
  const char* keyname = VTYKeyNames::GetKeyName(static_cast<int>(code));
  return keyname ? std::optional<std::string>(std::string(keyname)) : std::nullopt;
}

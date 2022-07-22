#include "win32_raw_input_source.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/host.h"
#include "core/system.h"
#include "input_manager.h"
#include <cmath>
#include <hidusage.h>
#include <malloc.h>

Log_SetChannel(Win32RawInputSource);

static const wchar_t* WINDOW_CLASS_NAME = L"Win32RawInputSource";
static bool s_window_class_registered = false;

static constexpr const u32 ALL_BUTTON_MASKS = RI_MOUSE_BUTTON_1_DOWN | RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_DOWN |
                                              RI_MOUSE_BUTTON_2_UP | RI_MOUSE_BUTTON_3_DOWN | RI_MOUSE_BUTTON_3_UP |
                                              RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_DOWN |
                                              RI_MOUSE_BUTTON_5_UP;

Win32RawInputSource::Win32RawInputSource() = default;

Win32RawInputSource::~Win32RawInputSource() = default;

bool Win32RawInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  if (!RegisterDummyClass())
  {
    Log_ErrorPrintf("(Win32RawInputSource) Failed to register dummy window class");
    return false;
  }

  if (!CreateDummyWindow())
  {
    Log_ErrorPrintf("(Win32RawInputSource) Failed to create dummy window");
    return false;
  }

  if (!OpenDevices())
  {
    Log_ErrorPrintf("(Win32RawInputSource) Failed to open devices");
    return false;
  }

  return true;
}

void Win32RawInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) {}

void Win32RawInputSource::Shutdown()
{
  CloseDevices();
  DestroyDummyWindow();
}

void Win32RawInputSource::PollEvents()
{
  // noop, handled by message pump
}

std::vector<std::pair<std::string, std::string>> Win32RawInputSource::EnumerateDevices()
{
  return {};
}

void Win32RawInputSource::UpdateMotorState(InputBindingKey key, float intensity) {}

void Win32RawInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                                           float small_intensity)
{
}

std::optional<InputBindingKey> Win32RawInputSource::ParseKeyString(const std::string_view& device,
                                                                   const std::string_view& binding)
{
  return std::nullopt;
}

std::string Win32RawInputSource::ConvertKeyToString(InputBindingKey key)
{
  return {};
}

std::vector<InputBindingKey> Win32RawInputSource::EnumerateMotors()
{
  return {};
}

bool Win32RawInputSource::GetGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping)
{
  return {};
}

bool Win32RawInputSource::RegisterDummyClass()
{
  if (s_window_class_registered)
    return true;

  WNDCLASSW wc = {};
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpfnWndProc = DummyWindowProc;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  return (RegisterClassW(&wc) != 0);
}

bool Win32RawInputSource::CreateDummyWindow()
{
  m_dummy_window = CreateWindowExW(0, WINDOW_CLASS_NAME, WINDOW_CLASS_NAME, WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT,
                                   CW_USEDEFAULT, CW_USEDEFAULT, HWND_MESSAGE, NULL, GetModuleHandleW(nullptr), NULL);
  if (!m_dummy_window)
    return false;

  SetWindowLongPtrW(m_dummy_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void Win32RawInputSource::DestroyDummyWindow()
{
  if (!m_dummy_window)
    return;

  DestroyWindow(m_dummy_window);
  m_dummy_window = {};
}

LRESULT CALLBACK Win32RawInputSource::DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg != WM_INPUT)
    return DefWindowProcW(hwnd, msg, wParam, lParam);

  UINT size = 0;
  GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));

  PRAWINPUT data = static_cast<PRAWINPUT>(_alloca(size));
  GetRawInputData((HRAWINPUT)lParam, RID_INPUT, data, &size, sizeof(RAWINPUTHEADER));

  // we shouldn't get any WM_INPUT messages prior to SetWindowLongPtr(), so this'll be fine
  Win32RawInputSource* ris = reinterpret_cast<Win32RawInputSource*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (ris->ProcessRawInputEvent(data))
    return 0;

  // forward through to normal message processing
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool Win32RawInputSource::OpenDevices()
{
  UINT num_devices = 0;
  if (GetRawInputDeviceList(nullptr, &num_devices, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1) ||
      num_devices == 0)
    return false;

  std::vector<RAWINPUTDEVICELIST> devices(num_devices);
  if (GetRawInputDeviceList(devices.data(), &num_devices, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
    return false;
  devices.resize(num_devices);

  for (const RAWINPUTDEVICELIST& rid : devices)
  {
#if 0
    if (rid.dwType == RIM_TYPEKEYBOARD)
      m_num_keyboards++;
#endif
    if (rid.dwType == RIM_TYPEMOUSE)
      m_mice.push_back({rid.hDevice});
  }

  Log_DevPrintf("(Win32RawInputSource) Found %u keyboards and %zu mice", m_num_keyboards, m_mice.size());

  // Grab all keyboard/mouse input.
  if (m_num_keyboards > 0)
  {
    const RAWINPUTDEVICE rrid = {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, 0, m_dummy_window};
    if (!RegisterRawInputDevices(&rrid, 1, sizeof(rrid)))
      return false;
  }
  if (!m_mice.empty())
  {
    const RAWINPUTDEVICE rrid = {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, 0, m_dummy_window};
    if (!RegisterRawInputDevices(&rrid, 1, sizeof(rrid)))
      return false;
  }

  return true;
}

void Win32RawInputSource::CloseDevices()
{
  if (m_num_keyboards > 0)
  {
    const RAWINPUTDEVICE rrid = {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, RIDEV_REMOVE, m_dummy_window};
    RegisterRawInputDevices(&rrid, 1, sizeof(rrid));
    m_num_keyboards = 0;
  }

  if (!m_mice.empty())
  {
    const RAWINPUTDEVICE rrid = {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, RIDEV_REMOVE, m_dummy_window};
    RegisterRawInputDevices(&rrid, 1, sizeof(rrid));
    m_mice.clear();
  }
}

bool Win32RawInputSource::ProcessRawInputEvent(const RAWINPUT* event)
{
  if (event->header.dwType == RIM_TYPEMOUSE)
  {
    const u32 mouse_index = 0;
    for (MouseState& state : m_mice)
    {
      if (state.device != event->header.hDevice)
        continue;

      const RAWMOUSE& rm = event->data.mouse;

      s32 dx = rm.lLastX;
      s32 dy = rm.lLastY;

      // handle absolute positioned devices
      if ((rm.usFlags & MOUSE_MOVE_ABSOLUTE) == MOUSE_MOVE_ABSOLUTE)
      {
        dx -= std::exchange(dx, state.last_x);
        dy -= std::exchange(dy, state.last_y);
      }

      unsigned long button_mask =
        (rm.usButtonFlags & (rm.usButtonFlags ^ std::exchange(state.button_state, rm.usButtonFlags))) &
        ALL_BUTTON_MASKS;

      // when the VM isn't running, allow events to run as normal (so we don't break the UI)
      if (System::GetState() != System::State::Running)
        return false;

      while (button_mask != 0)
      {
        unsigned long bit_index;
        _BitScanForward(&bit_index, button_mask);

        // these are ordered down..up for each button
        const u32 button_number = bit_index >> 1;
        const bool button_pressed = (bit_index & 1u) != 0;
        InputManager::InvokeEvents(InputManager::MakePointerButtonKey(mouse_index, button_number),
                                   static_cast<float>(button_pressed), GenericInputBinding::Unknown);

        button_mask &= ~(1u << bit_index);
      }

      if (dx != 0)
        InputManager::UpdatePointerRelativeDelta(mouse_index, InputPointerAxis::X, static_cast<float>(dx), true);
      if (dy != 0)
        InputManager::UpdatePointerRelativeDelta(mouse_index, InputPointerAxis::Y, static_cast<float>(dy), true);

      return true;
    }
  }

  return false;
}

std::unique_ptr<InputSource> InputSource::CreateWin32RawInputSource()
{
  return std::make_unique<Win32RawInputSource>();
}

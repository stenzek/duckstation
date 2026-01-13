// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "win32_raw_input_source.h"
#include "input_manager.h"
#include "platform_misc.h"

#include "core/gpu_thread.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <hidsdi.h>
#include <hidusage.h>
#include <malloc.h>

LOG_CHANNEL(Win32RawInputSource);

static const wchar_t* WINDOW_CLASS_NAME = L"Win32RawInputSource";
static bool s_window_class_registered = false;

static constexpr const u32 ALL_BUTTON_MASKS = RI_MOUSE_BUTTON_1_DOWN | RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_DOWN |
                                              RI_MOUSE_BUTTON_2_UP | RI_MOUSE_BUTTON_3_DOWN | RI_MOUSE_BUTTON_3_UP |
                                              RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_DOWN |
                                              RI_MOUSE_BUTTON_5_UP;

Win32RawInputSource::Win32RawInputSource() = default;

Win32RawInputSource::~Win32RawInputSource() = default;

bool Win32RawInputSource::Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  if (!RegisterDummyClass())
  {
    ERROR_LOG("Failed to register dummy window class");
    return false;
  }

  if (!CreateDummyWindow())
  {
    ERROR_LOG("Failed to create dummy window");
    return false;
  }

  ReloadDevices();
  return true;
}

void Win32RawInputSource::UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

void Win32RawInputSource::Shutdown()
{
  CloseDevices();
  UnregisterRawInput();
  DestroyDummyWindow();
}

void Win32RawInputSource::PollEvents()
{
  // noop, handled by message pump
}

std::optional<float> Win32RawInputSource::GetCurrentValue(InputBindingKey key)
{
  // not really used
  return std::nullopt;
}

InputManager::DeviceList Win32RawInputSource::EnumerateDevices()
{
  InputManager::DeviceList ret;
  for (u32 pointer_index = 0; pointer_index < static_cast<u32>(m_mice.size()); pointer_index++)
  {
    ret.emplace_back(MakeGenericControllerDeviceKey(InputSourceType::Pointer, pointer_index),
                     InputManager::GetPointerDeviceName(pointer_index), GetMouseDeviceName(pointer_index));
  }

  return ret;
}

void Win32RawInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
}

void Win32RawInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                                           float small_intensity)
{
}

void Win32RawInputSource::UpdateLEDState(InputBindingKey key, float intensity)
{
}

bool Win32RawInputSource::ContainsDevice(std::string_view device) const
{
  return false;
}

std::optional<InputBindingKey> Win32RawInputSource::ParseKeyString(std::string_view device, std::string_view binding)
{
  return std::nullopt;
}

TinyString Win32RawInputSource::ConvertKeyToString(InputBindingKey key)
{
  return {};
}

TinyString Win32RawInputSource::ConvertKeyToIcon(InputBindingKey key, InputManager::BindingIconMappingFunction mapper)
{
  return {};
}

std::unique_ptr<ForceFeedbackDevice> Win32RawInputSource::CreateForceFeedbackDevice(std::string_view device,
                                                                                    Error* error)
{
  Error::SetStringView(error, "Not supported on this input source.");
  return {};
}

InputManager::DeviceEffectList Win32RawInputSource::EnumerateEffects(std::optional<InputBindingInfo::Type> type,
                                                                     std::optional<InputBindingKey> for_device)
{
  return {};
}

u32 Win32RawInputSource::GetPollableDeviceCount() const
{
  return static_cast<u32>(m_mice.size());
}

bool Win32RawInputSource::GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping)
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
  s_window_class_registered = (RegisterClassW(&wc) != 0);
  return s_window_class_registered;
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

std::string Win32RawInputSource::GetMouseDeviceName(u32 index)
{
#if 0
  // Doesn't work for mice :(
  const HANDLE device = m_mice[index].device;
  std::wstring wdevice_name;

  UINT size = 0;
  if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size) == static_cast<UINT>(-1))
    goto error;

  wdevice_name.resize(size);

  UINT written_size = GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, wdevice_name.data(), &size);
  if (written_size == static_cast<UINT>(-1))
    goto error;

  wdevice_name.resize(written_size);
  if (wdevice_name.empty())
    goto error;

  const HANDLE hFile = CreateFileW(wdevice_name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, NULL);
  if (hFile == INVALID_HANDLE_VALUE)
    goto error;

  wchar_t product_string[256];
  if (!HidD_GetProductString(hFile, product_string, sizeof(product_string)))
  {
    CloseHandle(hFile);
    goto error;
  }

  CloseHandle(hFile);

  return StringUtil::WideStringToUTF8String(product_string);

error:
  return "Unknown Device";
#else
  return fmt::format("Raw Input Pointer {}", index);
#endif
}

bool Win32RawInputSource::IsAcceptableRawInputMouse(const RAWINPUTDEVICELIST& rid)
{
  if (rid.dwType == RIM_TYPEMOUSE)
  {
    // Make sure it's a real mouse with buttons.
    // My goal with this was to stop my silly Corsair keyboard from showing up as a mouse... but it reports 32 buttons.
    RID_DEVICE_INFO devinfo = {
      .cbSize = sizeof(devinfo),
      .dwType = RIM_TYPEMOUSE,
      .mouse = {},
    };
    UINT devinfo_size = sizeof(devinfo);
    if (GetRawInputDeviceInfoW(rid.hDevice, RIDI_DEVICEINFO, &devinfo, &devinfo_size) <= 0 ||
        devinfo.mouse.dwNumberOfButtons == 0)
    {
      return false;
    }

    return true;
  }
  else
  {
    return false;
  }
}

bool Win32RawInputSource::ReloadDevices()
{
  std::vector<RAWINPUTDEVICELIST> devices;
  UINT num_devices = 0;
  if (GetRawInputDeviceList(nullptr, &num_devices, sizeof(RAWINPUTDEVICELIST)) != static_cast<UINT>(-1) &&
      num_devices > 0)
  {
    devices.resize(num_devices);
    if (GetRawInputDeviceList(devices.data(), &num_devices, sizeof(RAWINPUTDEVICELIST)) != static_cast<UINT>(-1))
      devices.resize(num_devices);
    else
      devices.clear();
  }

  // close any devices no longer in the list
  bool any_changed = false;
  for (size_t i = 0; i < m_mice.size(); i++)
  {
    MouseState& ms = m_mice[i];
    if (!ms.device)
      continue;

    if (std::ranges::none_of(devices, [&ms](const RAWINPUTDEVICELIST& rid) { return rid.hDevice == ms.device; }))
    {
      DEV_LOG("Detected raw input device {} removal", i);

      ms = {};

      InputManager::OnInputDeviceDisconnected(
        MakeGenericControllerDeviceKey(InputSourceType::Pointer, static_cast<u32>(i)),
        InputManager::GetPointerDeviceName(static_cast<u32>(i)));

      any_changed = true;
    }
  }

  for (const RAWINPUTDEVICELIST& rid : devices)
  {
    // Already tracking?
    if (std::ranges::any_of(m_mice, [&rid](const MouseState& ms) { return (rid.hDevice == ms.device); }))
      continue;

    if (!IsAcceptableRawInputMouse(rid))
      continue;

    // Find a free slot. Might have been closed above.
    auto iter = std::ranges::find_if(m_mice, [](const MouseState& ms) { return !ms.device; });
    if (iter == m_mice.end())
    {
      m_mice.push_back({});
      iter = std::prev(m_mice.end());
    }

    iter->device = rid.hDevice;
    iter->button_state = 0;
    iter->last_x = 0;
    iter->last_y = 0;

    const u32 pointer_index = static_cast<u32>(std::distance(m_mice.begin(), iter));
    InputManager::OnInputDeviceConnected(MakeGenericControllerDeviceKey(InputSourceType::Pointer, pointer_index),
                                         InputManager::GetPointerDeviceName(pointer_index),
                                         GetMouseDeviceName(pointer_index));

    any_changed = true;
  }

  // Drop any trailing closed devices.
  while (!m_mice.empty() && !m_mice.back().device)
    m_mice.pop_back();

  const size_t num_mice = std::ranges::count_if(m_mice, [](const MouseState& ms) { return (ms.device != nullptr); });
  DEV_LOG("Found {} mice", num_mice);

  if (num_mice > 0)
    EnsureRawInputRegistered();
  else
    UnregisterRawInput();

  return any_changed;
}

void Win32RawInputSource::CloseDevices()
{
  if (m_mice.empty())
    return;

  for (size_t i = 0; i < m_mice.size(); i++)
  {
    if (!m_mice[i].device)
      continue;

    InputManager::OnInputDeviceDisconnected(
      MakeGenericControllerDeviceKey(InputSourceType::Pointer, static_cast<u32>(i)),
      InputManager::GetPointerDeviceName(static_cast<u32>(i)));
  }

  m_mice.clear();
}

void Win32RawInputSource::EnsureRawInputRegistered()
{
  if (m_raw_input_registered)
    return;

  const RAWINPUTDEVICE rrid = {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, 0, m_dummy_window};
  if (!RegisterRawInputDevices(&rrid, 1, sizeof(rrid)))
  {
    ERROR_LOG("RegisterRawInputDevices() failed: {}", GetLastError());
    return;
  }

  m_raw_input_registered = true;
}

void Win32RawInputSource::UnregisterRawInput()
{
  if (!m_raw_input_registered)
    return;

  const RAWINPUTDEVICE rrid = {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, RIDEV_REMOVE, m_dummy_window};
  if (!RegisterRawInputDevices(&rrid, 1, sizeof(rrid)))
    ERROR_LOG("RegisterRawInputDevices() to remove failed: {}", GetLastError());

  m_raw_input_registered = false;
}

bool Win32RawInputSource::ProcessRawInputEvent(const RAWINPUT* event)
{
  if (event->header.dwType == RIM_TYPEMOUSE)
  {
    for (u32 pointer_index = 0; pointer_index < static_cast<u32>(m_mice.size()); pointer_index++)
    {
      MouseState& state = m_mice[pointer_index];
      if (state.device != event->header.hDevice)
        continue;

      const RAWMOUSE& rm = event->data.mouse;

      unsigned long button_mask =
        (rm.usButtonFlags & (rm.usButtonFlags ^ std::exchange(state.button_state, rm.usButtonFlags))) &
        ALL_BUTTON_MASKS;

      while (button_mask != 0)
      {
        unsigned long bit_index;
        _BitScanForward(&bit_index, button_mask);

        // these are ordered down..up for each button
        const u32 button_number = bit_index >> 1;
        const bool button_pressed = (bit_index & 1u) == 0;
        InputManager::InvokeEvents(InputManager::MakePointerButtonKey(pointer_index, button_number),
                                   static_cast<float>(button_pressed), GenericInputBinding::Unknown);

        button_mask &= ~(1u << bit_index);
      }

      // handle absolute positioned devices
      if ((rm.usFlags & MOUSE_MOVE_ABSOLUTE) == MOUSE_MOVE_ABSOLUTE)
      {
        // https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse#remarks
        RECT rect;
        if (rm.usFlags & MOUSE_VIRTUAL_DESKTOP)
        {
          rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
          rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
          rect.right = GetSystemMetrics(SM_CXVIRTUALSCREEN);
          rect.bottom = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        }
        else
        {
          rect.left = 0;
          rect.top = 0;
          rect.right = GetSystemMetrics(SM_CXSCREEN);
          rect.bottom = GetSystemMetrics(SM_CYSCREEN);
        }

        int absolute_x = MulDiv(rm.lLastX, rect.right, USHRT_MAX) + rect.left;
        int absolute_y = MulDiv(rm.lLastY, rect.bottom, USHRT_MAX) + rect.top;

        // This is truely awful. But for something that isn't used much, it's the easiest way to get the render rect...
        const WindowInfo& render_wi = GPUThread::GetRenderWindowInfo();
        if (render_wi.type == WindowInfoType::Win32 && GetWindowRect(static_cast<HWND>(render_wi.window_handle), &rect))
        {
          absolute_x -= rect.left;
          absolute_y -= rect.top;
        }

        InputManager::UpdatePointerAbsolutePosition(pointer_index, static_cast<float>(absolute_x),
                                                    static_cast<float>(absolute_y), true);
      }
      else
      {
        // relative is easy
        if (rm.lLastX != 0)
        {
          InputManager::UpdatePointerRelativeDelta(pointer_index, InputPointerAxis::X, static_cast<float>(rm.lLastX),
                                                   true);
        }

        if (rm.lLastY != 0)
        {
          InputManager::UpdatePointerRelativeDelta(pointer_index, InputPointerAxis::Y, static_cast<float>(rm.lLastY),
                                                   true);
        }
      }

      return true;
    }
  }

  return false;
}

std::unique_ptr<InputSource> InputSource::CreateWin32RawInputSource()
{
  return std::make_unique<Win32RawInputSource>();
}

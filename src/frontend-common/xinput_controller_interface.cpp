#include "xinput_controller_interface.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include "core/system.h"
#include <cmath>
Log_SetChannel(XInputControllerInterface);

XInputControllerInterface::XInputControllerInterface() = default;

XInputControllerInterface::~XInputControllerInterface()
{
  if (m_xinput_module)
    FreeLibrary(m_xinput_module);
}

ControllerInterface::Backend XInputControllerInterface::GetBackend() const
{
  return ControllerInterface::Backend::XInput;
}

bool XInputControllerInterface::Initialize(CommonHostInterface* host_interface)
{
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  m_xinput_module = LoadLibraryW(L"xinput1_4");
  if (!m_xinput_module)
  {
    m_xinput_module = LoadLibraryW(L"xinput1_3");
  }
  if (!m_xinput_module)
  {
    m_xinput_module = LoadLibraryW(L"xinput9_1_0");
  }
  if (!m_xinput_module)
  {
    Log_ErrorPrintf("Failed to load XInput module.");
    return false;
  }

  // Try the hidden version of XInputGetState(), which lets us query the guide button.
  m_xinput_get_state =
    reinterpret_cast<decltype(m_xinput_get_state)>(GetProcAddress(m_xinput_module, reinterpret_cast<LPCSTR>(100)));
  if (!m_xinput_get_state)
    reinterpret_cast<decltype(m_xinput_get_state)>(GetProcAddress(m_xinput_module, "XInputGetState"));
  m_xinput_set_state =
    reinterpret_cast<decltype(m_xinput_set_state)>(GetProcAddress(m_xinput_module, "XInputSetState"));
#else
  m_xinput_get_state = XInputGetState;
  m_xinput_set_state = XInputSetState;
#endif
  if (!m_xinput_get_state || !m_xinput_set_state)
  {
    Log_ErrorPrintf("Failed to get XInput function pointers.");
    return false;
  }

  if (!ControllerInterface::Initialize(host_interface))
    return false;

  return true;
}

void XInputControllerInterface::Shutdown()
{
  ControllerInterface::Shutdown();
}

void XInputControllerInterface::PollEvents()
{
  for (u32 i = 0; i < XUSER_MAX_COUNT; i++)
  {
    XINPUT_STATE new_state;
    const DWORD result = m_xinput_get_state(i, &new_state);
    ControllerData& cd = m_controllers[i];
    if (result == ERROR_SUCCESS)
    {
      if (!cd.connected)
      {
        cd.connected = true;
        OnControllerConnected(static_cast<int>(i));
      }

      CheckForStateChanges(i, new_state);
    }
    else
    {
      if (result != ERROR_DEVICE_NOT_CONNECTED)
        Log_WarningPrintf("XInputGetState(%u) failed: 0x%08X / 0x%08X", i, result, GetLastError());

      if (cd.connected)
      {
        cd.connected = false;
        cd.last_state = {};
        OnControllerDisconnected(static_cast<int>(i));
      }
    }
  }
}

void XInputControllerInterface::CheckForStateChanges(u32 index, const XINPUT_STATE& new_state)
{
  ControllerData& cd = m_controllers[index];
  if (new_state.dwPacketNumber == cd.last_state.dwPacketNumber)
    return;

  cd.last_state.dwPacketNumber = new_state.dwPacketNumber;

  XINPUT_GAMEPAD& ogp = cd.last_state.Gamepad;
  const XINPUT_GAMEPAD& ngp = new_state.Gamepad;
  if (ogp.sThumbLX != ngp.sThumbLX)
  {
    HandleAxisEvent(index, Axis::LeftX, ngp.sThumbLX);
    ogp.sThumbLX = ngp.sThumbLX;
  }
  if (ogp.sThumbLY != ngp.sThumbLY)
  {
    HandleAxisEvent(index, Axis::LeftY, -ngp.sThumbLY);
    ogp.sThumbLY = ngp.sThumbLY;
  }
  if (ogp.sThumbRX != ngp.sThumbRX)
  {
    HandleAxisEvent(index, Axis::RightX, ngp.sThumbRX);
    ogp.sThumbRX = ngp.sThumbRX;
  }
  if (ogp.sThumbRY != ngp.sThumbRY)
  {
    HandleAxisEvent(index, Axis::RightY, -ngp.sThumbRY);
    ogp.sThumbRY = ngp.sThumbRY;
  }
  if (ogp.bLeftTrigger != ngp.bLeftTrigger)
  {
    HandleAxisEvent(index, Axis::LeftTrigger, static_cast<s32>(ZeroExtend32(ngp.bLeftTrigger) << 7));
    ogp.bLeftTrigger = ngp.bLeftTrigger;
  }
  if (ogp.bRightTrigger != ngp.bRightTrigger)
  {
    HandleAxisEvent(index, Axis::RightTrigger, static_cast<s32>(ZeroExtend32(ngp.bRightTrigger) << 7));
    ogp.bRightTrigger = ngp.bRightTrigger;
  }

  static constexpr std::array<u16, NUM_BUTTONS> button_masks = {
    {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_BACK,
     0x400 /* XINPUT_GAMEPAD_GUIDE */, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
     XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
     XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT}};

  const u16 old_button_bits = ogp.wButtons;
  const u16 new_button_bits = ngp.wButtons;
  if (old_button_bits != new_button_bits)
  {
    for (u32 button = 0; button < static_cast<u32>(button_masks.size()); button++)
    {
      const u16 button_mask = button_masks[button];
      if ((old_button_bits & button_mask) != (new_button_bits & button_mask))
        HandleButtonEvent(index, button, (new_button_bits & button_mask) != 0);
    }

    ogp.wButtons = ngp.wButtons;
  }
}

void XInputControllerInterface::ClearBindings()
{
  for (ControllerData& cd : m_controllers)
  {
    cd.axis_mapping.fill({});
    cd.button_mapping.fill({});
    cd.axis_button_mapping.fill({});
    cd.button_axis_mapping.fill({});
  }
}

bool XInputControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side,
                                                   AxisCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size() || !m_controllers[controller_index].connected)
    return false;

  if (axis_number < 0 || axis_number >= NUM_AXES)
    return false;

  m_controllers[controller_index].axis_mapping[axis_number][axis_side] = std::move(callback);
  return true;
}

bool XInputControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size() || !m_controllers[controller_index].connected)
    return false;

  if (button_number < 0 || button_number >= NUM_BUTTONS)
    return false;

  m_controllers[controller_index].button_mapping[button_number] = std::move(callback);
  return true;
}

bool XInputControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                           ButtonCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size() || !m_controllers[controller_index].connected)
    return false;

  if (axis_number < 0 || axis_number >= NUM_AXES)
    return false;

  m_controllers[controller_index].axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool XInputControllerInterface::BindControllerHatToButton(int controller_index, int hat_number,
                                                          std::string_view hat_position, ButtonCallback callback)
{
  // Hats don't exist in XInput
  return false;
}

bool XInputControllerInterface::BindControllerButtonToAxis(int controller_index, int button_number,
                                                           AxisCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size() || !m_controllers[controller_index].connected)
    return false;

  if (button_number < 0 || button_number >= NUM_BUTTONS)
    return false;

  m_controllers[controller_index].button_axis_mapping[button_number] = std::move(callback);
  return true;
}

bool XInputControllerInterface::HandleAxisEvent(u32 index, Axis axis, s32 value)
{
  const float f_value = static_cast<float>(value) / (value < 0 ? 32768.0f : 32767.0f);
  Log_DevPrintf("controller %u axis %u %d %f", index, static_cast<u32>(axis), value, f_value);
  DebugAssert(index < XUSER_MAX_COUNT);

  if (DoEventHook(Hook::Type::Axis, index, static_cast<u32>(axis), f_value))
    return true;

  const AxisCallback& cb = m_controllers[index].axis_mapping[static_cast<u32>(axis)][AxisSide::Full];
  if (cb)
  {
    cb(f_value);
    return true;
  }
  else
  {
    const AxisCallback& positive_cb = m_controllers[index].axis_mapping[static_cast<u32>(axis)][AxisSide::Positive];
    const AxisCallback& negative_cb = m_controllers[index].axis_mapping[static_cast<u32>(axis)][AxisSide::Negative];
    if (positive_cb || negative_cb)
    {
      if (positive_cb)
        positive_cb((f_value < 0.0f) ? 0.0f : f_value);
      if (negative_cb)
        negative_cb((f_value >= 0.0f) ? 0.0f : -f_value);

      return true;
    }
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(f_value) >= m_controllers[index].deadzone);
  const bool positive = (f_value >= 0.0f);
  const ButtonCallback& other_button_cb =
    m_controllers[index].axis_button_mapping[static_cast<u32>(axis)][BoolToUInt8(!positive)];
  const ButtonCallback& button_cb =
    m_controllers[index].axis_button_mapping[static_cast<u32>(axis)][BoolToUInt8(positive)];
  if (button_cb)
  {
    button_cb(outside_deadzone);
    if (other_button_cb)
      other_button_cb(false);
    return true;
  }
  else if (other_button_cb)
  {
    other_button_cb(false);
    return true;
  }
  else
  {
    return false;
  }
}

bool XInputControllerInterface::HandleButtonEvent(u32 index, u32 button, bool pressed)
{
  Log_DevPrintf("controller %u button %u %s", index, button, pressed ? "pressed" : "released");
  DebugAssert(index < XUSER_MAX_COUNT);

  static constexpr std::array<FrontendCommon::ControllerNavigationButton, NUM_BUTTONS> nav_button_mapping = {{
    FrontendCommon::ControllerNavigationButton::Activate,      // XINPUT_GAMEPAD_A
    FrontendCommon::ControllerNavigationButton::Cancel,        // XINPUT_GAMEPAD_B
    FrontendCommon::ControllerNavigationButton::Count,         // XINPUT_GAMEPAD_X
    FrontendCommon::ControllerNavigationButton::Count,         // XINPUT_GAMEPAD_Y
    FrontendCommon::ControllerNavigationButton::Count,         // XINPUT_GAMEPAD_BACK
    FrontendCommon::ControllerNavigationButton::Count,         // XINPUT_GAMEPAD_GUIDE
    FrontendCommon::ControllerNavigationButton::Count,         // XINPUT_GAMEPAD_START
    FrontendCommon::ControllerNavigationButton::Count,         // XINPUT_GAMEPAD_LEFT_THUMB
    FrontendCommon::ControllerNavigationButton::Count,         // XINPUT_GAMEPAD_RIGHT_THUMB
    FrontendCommon::ControllerNavigationButton::LeftShoulder,  // XINPUT_GAMEPAD_LEFT_SHOULDER
    FrontendCommon::ControllerNavigationButton::RightShoulder, // XINPUT_GAMEPAD_RIGHT_SHOULDER
    FrontendCommon::ControllerNavigationButton::DPadUp,        // XINPUT_GAMEPAD_DPAD_UP
    FrontendCommon::ControllerNavigationButton::DPadDown,      // XINPUT_GAMEPAD_DPAD_DOWN
    FrontendCommon::ControllerNavigationButton::DPadLeft,      // XINPUT_GAMEPAD_DPAD_LEFT
    FrontendCommon::ControllerNavigationButton::DPadRight,     // XINPUT_GAMEPAD_DPAD_RIGHT
  }};

  if (DoEventHook(Hook::Type::Button, index, button, pressed ? 1.0f : 0.0f))
    return true;

  if (button < nav_button_mapping.size() &&
      nav_button_mapping[button] != FrontendCommon::ControllerNavigationButton::Count)
  {
    m_host_interface->SetControllerNavigationButtonState(nav_button_mapping[button], pressed);
  }

  if (m_host_interface->IsControllerNavigationActive())
  {
    // UI consumed the event
    return true;
  }

  const ButtonCallback& cb = m_controllers[index].button_mapping[button];
  if (cb)
  {
    cb(pressed);
    return true;
  }

  const AxisCallback& axis_cb = m_controllers[index].button_axis_mapping[button];
  if (axis_cb)
  {
    axis_cb(pressed ? 1.0f : -1.0f);
  }
  return true;
}

u32 XInputControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  if (static_cast<u32>(controller_index) >= XUSER_MAX_COUNT || !m_controllers[controller_index].connected)
    return 0;

  return NUM_RUMBLE_MOTORS;
}

void XInputControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths,
                                                            u32 num_motors)
{
  DebugAssert(static_cast<u32>(controller_index) < XUSER_MAX_COUNT);

  XINPUT_VIBRATION vib;
  vib.wLeftMotorSpeed = static_cast<u16>(strengths[0] * 65535.0f);
  vib.wRightMotorSpeed = static_cast<u16>(strengths[1] * 65535.0f);
  m_xinput_set_state(static_cast<u32>(controller_index), &vib);
}

bool XInputControllerInterface::SetControllerDeadzone(int controller_index, float size /* = 0.25f */)
{
  if (static_cast<u32>(controller_index) >= XUSER_MAX_COUNT || !m_controllers[controller_index].connected)
    return false;

  m_controllers[static_cast<u32>(controller_index)].deadzone = std::clamp(std::abs(size), 0.01f, 0.99f);
  Log_InfoPrintf("Controller %d deadzone size set to %f", controller_index,
                 m_controllers[static_cast<u32>(controller_index)].deadzone);
  return true;
}

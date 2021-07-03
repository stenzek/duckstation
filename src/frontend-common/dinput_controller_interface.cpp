#define INITGUID

#include "dinput_controller_interface.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include "core/system.h"
#include <cmath>
#include <limits>
Log_SetChannel(DInputControllerInterface);

using PFNDIRECTINPUT8CREATE = HRESULT(WINAPI*)(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut,
                                               LPUNKNOWN punkOuter);
using PFNGETDFDIJOYSTICK = LPCDIDATAFORMAT(WINAPI*)();

DInputControllerInterface::DInputControllerInterface() = default;

DInputControllerInterface::~DInputControllerInterface()
{
  m_controllers.clear();
  m_dinput.Reset();
  if (m_dinput_module)
    FreeLibrary(m_dinput_module);
}

ControllerInterface::Backend DInputControllerInterface::GetBackend() const
{
  return ControllerInterface::Backend::XInput;
}

bool DInputControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  m_dinput_module = LoadLibraryW(L"dinput8");
  if (!m_dinput_module)
  {
    Log_ErrorPrintf("Failed to load DInput module.");
    return false;
  }

  PFNDIRECTINPUT8CREATE create =
    reinterpret_cast<PFNDIRECTINPUT8CREATE>(GetProcAddress(m_dinput_module, "DirectInput8Create"));
  PFNGETDFDIJOYSTICK get_joystick_data_format =
    reinterpret_cast<PFNGETDFDIJOYSTICK>(GetProcAddress(m_dinput_module, "GetdfDIJoystick"));
  if (!create || !get_joystick_data_format)
  {
    Log_ErrorPrintf("Failed to get DInput function pointers.");
    return false;
  }

  if (!ControllerInterface::Initialize(host_interface))
    return false;

  HRESULT hr = create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                      reinterpret_cast<LPVOID*>(m_dinput.GetAddressOf()), nullptr);
  m_joystick_data_format = get_joystick_data_format();
  if (FAILED(hr) || !m_joystick_data_format)
  {
    Log_ErrorPrintf("DirectInput8Create() failed: %08X", hr);
    return false;
  }

  AddDevices();

  return true;
}

void DInputControllerInterface::Shutdown()
{
  ControllerInterface::Shutdown();
}

static BOOL CALLBACK EnumCallback(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef)
{
  static_cast<std::vector<DIDEVICEINSTANCE>*>(pvRef)->push_back(*lpddi);
  return DIENUM_CONTINUE;
}

void DInputControllerInterface::AddDevices()
{
  std::vector<DIDEVICEINSTANCE> devices;
  m_dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCallback, &devices, DIEDFL_ATTACHEDONLY);

  Log_InfoPrintf("Enumerated %zud evices", devices.size());

  for (DIDEVICEINSTANCE inst : devices)
  {
    ControllerData cd;
    HRESULT hr = m_dinput->CreateDevice(inst.guidInstance, cd.device.GetAddressOf(), nullptr);
    if (FAILED(hr))
    {
      Log_WarningPrintf("Failed to create instance of device [%s, %s]", inst.tszProductName, inst.tszInstanceName);
      continue;
    }

    if (AddDevice(cd, inst.tszProductName))
      m_controllers.push_back(std::move(cd));
  }
}

bool DInputControllerInterface::AddDevice(ControllerData& cd, const char* name)
{
  HRESULT hr = cd.device->SetCooperativeLevel(static_cast<HWND>(m_host_interface->GetTopLevelWindowHandle()),
                                              DISCL_BACKGROUND | DISCL_EXCLUSIVE);
  if (FAILED(hr))
  {
    hr = cd.device->SetCooperativeLevel(static_cast<HWND>(m_host_interface->GetTopLevelWindowHandle()),
                                        DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Failed to set cooperative level for '%s'", name);
      return false;
    }

    Log_WarningPrintf("Failed to set exclusive mode for '%s'", name);
  }

  hr = cd.device->SetDataFormat(m_joystick_data_format);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to set data format for '%s'", name);
    return false;
  }

  hr = cd.device->Acquire();
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to acquire device '%s'", name);
    return false;
  }

  DIDEVCAPS caps = {};
  caps.dwSize = sizeof(caps);
  hr = cd.device->GetCapabilities(&caps);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to get capabilities for '%s'", name);
    return false;
  }

  cd.num_buttons = caps.dwButtons;
  if (cd.num_buttons > NUM_BUTTONS)
  {
    Log_WarningPrintf("Device '%s' has too many buttons (%u), using %u instead.", name, cd.num_buttons, NUM_BUTTONS);
    cd.num_buttons = NUM_BUTTONS;
  }

  static constexpr std::array<u32, NUM_AXISES> axis_offsets = {
    {offsetof(DIJOYSTATE, lX), offsetof(DIJOYSTATE, lY), offsetof(DIJOYSTATE, lZ), offsetof(DIJOYSTATE, lRz),
     offsetof(DIJOYSTATE, lRx), offsetof(DIJOYSTATE, lRy), offsetof(DIJOYSTATE, rglSlider[0]),
     offsetof(DIJOYSTATE, rglSlider[1])}};
  for (u32 i = 0; i < NUM_AXISES; i++)
  {
    // ask for 16 bits of axis range
    DIPROPRANGE range = {};
    range.diph.dwSize = sizeof(range);
    range.diph.dwHeaderSize = sizeof(range.diph);
    range.diph.dwHow = DIPH_BYOFFSET;
    range.diph.dwObj = axis_offsets[i];
    range.lMin = std::numeric_limits<s16>::min();
    range.lMax = std::numeric_limits<s16>::max();
    hr = cd.device->SetProperty(DIPROP_RANGE, &range.diph);

    // did it apply?
    if (SUCCEEDED(cd.device->GetProperty(DIPROP_RANGE, &range.diph)))
    {
      cd.axis_offsets[cd.num_axes] = axis_offsets[i];
      cd.num_axes++;
    }
  }

  cd.has_hat = (caps.dwPOVs > 0);

  hr = cd.device->Poll();
  if (hr == DI_NOEFFECT)
    cd.needs_poll = false;
  else if (hr != DI_OK)
    Log_WarningPrintf("Polling device '%s' failed: %08X", name, hr);

  hr = cd.device->GetDeviceState(sizeof(cd.last_state), &cd.last_state);
  if (hr != DI_OK)
    Log_WarningPrintf("GetDeviceState() for '%s' failed: %08X", name, hr);

  Log_InfoPrintf("%s has %u buttons, %u axes%s", name, cd.num_buttons, cd.num_axes, cd.has_hat ? ", and a hat" : "");

  return (cd.num_buttons > 0 || cd.num_axes > 0 || cd.has_hat);
}

void DInputControllerInterface::PollEvents()
{
  for (u32 i = 0; i < static_cast<u32>(m_controllers.size()); i++)
  {
    ControllerData& cd = m_controllers[i];
    if (!cd.device)
      continue;

    if (cd.needs_poll)
      cd.device->Poll();

    DIJOYSTATE js;
    HRESULT hr = cd.device->GetDeviceState(sizeof(js), &js);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
    {
      hr = cd.device->Acquire();
      if (hr == DI_OK)
        hr = cd.device->GetDeviceState(sizeof(js), &js);

      if (hr != DI_OK)
      {
        cd = {};
        OnControllerDisconnected(static_cast<int>(i));
        continue;
      }
    }
    else if (hr != DI_OK)
    {
      Log_WarningPrintf("GetDeviceState() failed: %08X", hr);
      continue;
    }

    CheckForStateChanges(i, js);
  }
}

std::array<bool, ControllerInterface::NUM_HAT_DIRECTIONS> DInputControllerInterface::GetHatButtons(DWORD hat)
{
  std::array<bool, NUM_HAT_DIRECTIONS> buttons = {};

  const WORD hv = LOWORD(hat);
  if (hv != 0xFFFF)
  {
    if ((hv >= 0 && hv < 9000) || hv >= 31500)
      buttons[HAT_DIRECTION_UP] = true;
    if (hv >= 4500 && hv < 18000)
      buttons[HAT_DIRECTION_RIGHT] = true;
    if (hv >= 13500 && hv < 27000)
      buttons[HAT_DIRECTION_DOWN] = true;
    if (hv >= 22500)
      buttons[HAT_DIRECTION_LEFT] = true;
  }

  return buttons;
}

void DInputControllerInterface::CheckForStateChanges(u32 index, const DIJOYSTATE& new_state)
{
  ControllerData& cd = m_controllers[index];
  DIJOYSTATE& last_state = cd.last_state;

  for (u32 i = 0; i < cd.num_axes; i++)
  {
    LONG new_value;
    LONG old_value;
    std::memcpy(&old_value, reinterpret_cast<const u8*>(&cd.last_state) + cd.axis_offsets[i], sizeof(old_value));
    std::memcpy(&new_value, reinterpret_cast<const u8*>(&new_state) + cd.axis_offsets[i], sizeof(new_value));
    if (old_value != new_value)
    {
      HandleAxisEvent(index, i, new_value);
      std::memcpy(reinterpret_cast<u8*>(&cd.last_state) + cd.axis_offsets[i], &new_value, sizeof(new_value));
    }
  }

  for (u32 i = 0; i < cd.num_buttons; i++)
  {
    if (last_state.rgbButtons[i] != new_state.rgbButtons[i])
    {
      HandleButtonEvent(index, i, new_state.rgbButtons[i] != 0);
      last_state.rgbButtons[i] = new_state.rgbButtons[i];
    }
  }

  if (cd.has_hat)
  {
    if (last_state.rgdwPOV[0] != new_state.rgdwPOV[0])
    {
      Log_InfoPrintf("Hat %u", LOWORD(new_state.rgdwPOV[0]));
      // map hats to the last buttons
      const std::array<bool, NUM_HAT_DIRECTIONS> old_buttons(GetHatButtons(last_state.rgdwPOV[0]));
      const std::array<bool, NUM_HAT_DIRECTIONS> new_buttons(GetHatButtons(new_state.rgdwPOV[0]));
      for (u32 i = 0; i < NUM_HAT_DIRECTIONS; i++)
      {
        if (old_buttons[i] != new_buttons[i])
          HandleButtonEvent(index, cd.num_buttons + i, new_buttons[i]);
      }

      last_state.rgdwPOV[0] = new_state.rgdwPOV[0];
    }
  }
}

void DInputControllerInterface::ClearBindings()
{
  for (ControllerData& cd : m_controllers)
  {
    cd.axis_mapping.fill({});
    cd.button_mapping.fill({});
    cd.axis_button_mapping.fill({});
    cd.button_axis_mapping.fill({});
  }
}

bool DInputControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side,
                                                   AxisCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  if (axis_number < 0 || axis_number >= NUM_AXISES)
    return false;

  m_controllers[controller_index].axis_mapping[axis_number][axis_side] = std::move(callback);
  return true;
}

bool DInputControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  if (button_number < 0 || button_number >= TOTAL_NUM_BUTTONS)
    return false;

  m_controllers[controller_index].button_mapping[button_number] = std::move(callback);
  return true;
}

bool DInputControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                           ButtonCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  if (axis_number < 0 || axis_number >= NUM_AXISES)
    return false;

  m_controllers[controller_index].axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool DInputControllerInterface::BindControllerHatToButton(int controller_index, int hat_number,
                                                          std::string_view hat_position, ButtonCallback callback)
{
  // Hats don't exist in XInput
  return false;
}

bool DInputControllerInterface::BindControllerButtonToAxis(int controller_index, int button_number,
                                                           AxisCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  if (button_number < 0 || button_number >= TOTAL_NUM_BUTTONS)
    return false;

  m_controllers[controller_index].button_axis_mapping[button_number] = std::move(callback);
  return true;
}

bool DInputControllerInterface::HandleAxisEvent(u32 index, u32 axis, s32 value)
{
  const float f_value = static_cast<float>(value) / (value < 0 ? 32768.0f : 32767.0f);
  Log_DevPrintf("controller %u axis %u %d %f", index, static_cast<u32>(axis), value, f_value);
  DebugAssert(index < m_controllers.size());

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

bool DInputControllerInterface::HandleButtonEvent(u32 index, u32 button, bool pressed)
{
  Log_DevPrintf("controller %u button %u %s", index, button, pressed ? "pressed" : "released");
  DebugAssert(index < m_controllers.size());

  if (DoEventHook(Hook::Type::Button, index, button, pressed ? 1.0f : 0.0f))
    return true;

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

u32 DInputControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return 0;

  return 0;
}

void DInputControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths,
                                                            u32 num_motors)
{
  DebugAssert(static_cast<u32>(controller_index) < m_controllers.size());
}

bool DInputControllerInterface::SetControllerDeadzone(int controller_index, float size /* = 0.25f */)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  m_controllers[static_cast<u32>(controller_index)].deadzone = std::clamp(std::abs(size), 0.01f, 0.99f);
  Log_InfoPrintf("Controller %d deadzone size set to %f", controller_index,
                 m_controllers[static_cast<u32>(controller_index)].deadzone);
  return true;
}

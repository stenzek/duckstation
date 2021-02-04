#include "android_controller_interface.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include "core/system.h"
#include <cmath>
Log_SetChannel(AndroidControllerInterface);

AndroidControllerInterface::AndroidControllerInterface() = default;

AndroidControllerInterface::~AndroidControllerInterface() = default;

ControllerInterface::Backend AndroidControllerInterface::GetBackend() const
{
  return ControllerInterface::Backend::Android;
}

bool AndroidControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  if (!ControllerInterface::Initialize(host_interface))
    return false;

  return true;
}

void AndroidControllerInterface::Shutdown()
{
  ControllerInterface::Shutdown();
}

void AndroidControllerInterface::PollEvents() {}

void AndroidControllerInterface::ClearBindings()
{
  for (ControllerData& cd : m_controllers)
  {
    cd.axis_mapping.fill({});
    cd.button_mapping.fill({});
    cd.axis_button_mapping.fill({});
    cd.button_axis_mapping.fill({});
  }
}

bool AndroidControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side,
                                                    AxisCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  if (axis_number < 0 || axis_number >= NUM_AXISES)
    return false;

  m_controllers[controller_index].axis_mapping[axis_number][axis_side] = std::move(callback);
  return true;
}

bool AndroidControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  if (button_number < 0 || button_number >= NUM_BUTTONS)
    return false;

  m_controllers[controller_index].button_mapping[button_number] = std::move(callback);
  return true;
}

bool AndroidControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                            ButtonCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  if (axis_number < 0 || axis_number >= NUM_AXISES)
    return false;

  m_controllers[controller_index].axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool AndroidControllerInterface::BindControllerHatToButton(int controller_index, int hat_number,
                                                           std::string_view hat_position, ButtonCallback callback)
{
  // Hats don't exist in XInput
  return false;
}

bool AndroidControllerInterface::BindControllerButtonToAxis(int controller_index, int button_number,
                                                            AxisCallback callback)
{
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  if (button_number < 0 || button_number >= NUM_BUTTONS)
    return false;

  m_controllers[controller_index].button_axis_mapping[button_number] = std::move(callback);
  return true;
}

bool AndroidControllerInterface::HandleAxisEvent(u32 index, u32 axis, float value)
{
  Log_DevPrintf("controller %u axis %u %f", index, static_cast<u32>(axis), value);
  DebugAssert(index < NUM_CONTROLLERS);

  if (DoEventHook(Hook::Type::Axis, index, static_cast<u32>(axis), value))
    return true;

  const AxisCallback& cb = m_controllers[index].axis_mapping[static_cast<u32>(axis)][AxisSide::Full];
  if (cb)
  {
    cb(value);
    return true;
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(value) >= m_controllers[index].deadzone);
  const bool positive = (value >= 0.0f);
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

bool AndroidControllerInterface::HandleButtonEvent(u32 index, u32 button, bool pressed)
{
  Log_DevPrintf("controller %u button %u %s", index, button, pressed ? "pressed" : "released");
  DebugAssert(index < NUM_CONTROLLERS);

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

u32 AndroidControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  return 0;
}

void AndroidControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths,
                                                             u32 num_motors)
{
}

bool AndroidControllerInterface::SetControllerDeadzone(int controller_index, float size /* = 0.25f */)
{
  if (static_cast<u32>(controller_index) >= NUM_CONTROLLERS)
    return false;

  m_controllers[static_cast<u32>(controller_index)].deadzone = std::clamp(std::abs(size), 0.01f, 0.99f);
  Log_InfoPrintf("Controller %d deadzone size set to %f", controller_index,
                 m_controllers[static_cast<u32>(controller_index)].deadzone);
  return true;
}

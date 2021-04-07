#include "android_controller_interface.h"
#include "android_host_interface.h"
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
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  for (ControllerData& cd : m_controllers)
  {
    cd.axis_mapping.clear();
    cd.button_mapping.clear();
    cd.axis_button_mapping.clear();
    cd.button_axis_mapping.clear();
  }
}

std::optional<int> AndroidControllerInterface::GetControllerIndex(const std::string_view& device)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  for (u32 i = 0; i < static_cast<u32>(m_device_names.size()); i++)
  {
    if (device == m_device_names[i])
      return static_cast<int>(i);
  }

  return std::nullopt;
}

bool AndroidControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side,
                                                    AxisCallback callback)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  m_controllers[controller_index].axis_mapping[axis_number][axis_side] = std::move(callback);
  return true;
}

bool AndroidControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  m_controllers[controller_index].button_mapping[button_number] = std::move(callback);
  return true;
}

bool AndroidControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                            ButtonCallback callback)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  m_controllers[controller_index].axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool AndroidControllerInterface::BindControllerHatToButton(int controller_index, int hat_number,
                                                           std::string_view hat_position, ButtonCallback callback)
{
  return false;
}

bool AndroidControllerInterface::BindControllerButtonToAxis(int controller_index, int button_number,
                                                            AxisCallback callback)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  m_controllers[controller_index].button_axis_mapping[button_number] = std::move(callback);
  return true;
}

void AndroidControllerInterface::SetDeviceNames(std::vector<std::string> device_names)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  m_device_names = std::move(device_names);
  m_controllers.resize(m_device_names.size());
}

void AndroidControllerInterface::SetDeviceRumble(u32 index, bool has_vibrator)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (index >= m_controllers.size())
    return;

  m_controllers[index].has_rumble = has_vibrator;
}

void AndroidControllerInterface::HandleAxisEvent(u32 index, u32 axis, float value)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (index >= m_controllers.size())
    return;

  Log_DevPrintf("controller %u axis %u %f", index, axis, value);
  if (DoEventHook(Hook::Type::Axis, index, axis, value))
    return;

  const ControllerData& cd = m_controllers[index];
  const auto am_iter = cd.axis_mapping.find(axis);
  if (am_iter != cd.axis_mapping.end())
  {
    const AxisCallback& cb = am_iter->second[AxisSide::Full];
    if (cb)
    {
      cb(value);
      return;
    }
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(value) >= cd.deadzone);
  const bool positive = (value >= 0.0f);
  const auto bm_iter = cd.axis_button_mapping.find(axis);
  if (bm_iter != cd.axis_button_mapping.end())
  {
    const ButtonCallback& other_button_cb = bm_iter->second[BoolToUInt8(!positive)];
    const ButtonCallback& button_cb = bm_iter->second[BoolToUInt8(positive)];
    if (button_cb)
    {
      button_cb(outside_deadzone);
      if (other_button_cb)
        other_button_cb(false);
      return;
    }
    else if (other_button_cb)
    {
      other_button_cb(false);
      return;
    }
  }
}

void AndroidControllerInterface::HandleButtonEvent(u32 index, u32 button, bool pressed)
{
  Log_DevPrintf("controller %u button %u %s", index, button, pressed ? "pressed" : "released");

  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (index >= m_controllers.size())
    return;

  if (DoEventHook(Hook::Type::Button, index, button, pressed ? 1.0f : 0.0f))
    return;

  const ControllerData& cd = m_controllers[index];
  const auto button_iter = cd.button_mapping.find(button);
  if (button_iter != cd.button_mapping.end() && button_iter->second)
  {
    button_iter->second(pressed);
    return;
  }

  const auto axis_iter = cd.button_axis_mapping.find(button);
  if (axis_iter != cd.button_axis_mapping.end() && axis_iter->second)
  {
    axis_iter->second(pressed ? 1.0f : -1.0f);
    return;
  }
}

bool AndroidControllerInterface::HasButtonBinding(u32 index, u32 button)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (index >= m_controllers.size())
    return false;

  const ControllerData& cd = m_controllers[index];
  return (cd.button_mapping.find(button) != cd.button_mapping.end() ||
          cd.button_axis_mapping.find(button) != cd.button_axis_mapping.end());
}

u32 AndroidControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  return m_controllers[static_cast<u32>(controller_index)].has_rumble ? NUM_RUMBLE_MOTORS : 0;
}

void AndroidControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths,
                                                             u32 num_motors)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return;

  const float small_motor = strengths[0];
  const float large_motor = strengths[1];
  static_cast<AndroidHostInterface*>(m_host_interface)
    ->SetControllerVibration(static_cast<u32>(controller_index), small_motor, large_motor);
}

bool AndroidControllerInterface::SetControllerDeadzone(int controller_index, float size /* = 0.25f */)
{
  std::unique_lock<std::mutex> lock(m_controllers_mutex);
  if (static_cast<u32>(controller_index) >= m_controllers.size())
    return false;

  m_controllers[static_cast<u32>(controller_index)].deadzone = std::clamp(std::abs(size), 0.01f, 0.99f);
  Log_InfoPrintf("Controller %d deadzone size set to %f", controller_index,
                 m_controllers[static_cast<u32>(controller_index)].deadzone);
  return true;
}

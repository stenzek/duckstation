#include "inputbindingmonitor.h"

ControllerInterface::Hook::CallbackResult
InputButtonBindingMonitor::operator()(const ControllerInterface::Hook& ei) const
{
  if (ei.type == ControllerInterface::Hook::Type::Axis)
  {
    // wait until it's at least half pushed so we don't get confused between axises with small movement
    if (std::abs(ei.value) < 0.5f)
      return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;

    // TODO: this probably should consider the "last value"
    QMetaObject::invokeMethod(m_parent, "bindToControllerAxis", Q_ARG(int, ei.controller_index),
                              Q_ARG(int, ei.button_or_axis_number), Q_ARG(std::optional<bool>, ei.value > 0));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }
  else if (ei.type == ControllerInterface::Hook::Type::Button && ei.value > 0.0f)
  {
    QMetaObject::invokeMethod(m_parent, "bindToControllerButton", Q_ARG(int, ei.controller_index),
                              Q_ARG(int, ei.button_or_axis_number));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }

  return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;
}

ControllerInterface::Hook::CallbackResult InputAxisBindingMonitor::operator()(const ControllerInterface::Hook& ei) const
{
  if (ei.type == ControllerInterface::Hook::Type::Axis)
  {
    // wait until it's at least half pushed so we don't get confused between axises with small movement
    if (std::abs(ei.value) < 0.5f)
      return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;

    QMetaObject::invokeMethod(m_parent, "bindToControllerAxis", Q_ARG(int, ei.controller_index),
                              Q_ARG(int, ei.button_or_axis_number), Q_ARG(std::optional<bool>, std::nullopt));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }
  else if (ei.type == ControllerInterface::Hook::Type::Button && m_axis_type == Controller::AxisType::Half &&
           ei.value > 0.0f)
  {
    QMetaObject::invokeMethod(m_parent, "bindToControllerButton", Q_ARG(int, ei.controller_index),
                              Q_ARG(int, ei.button_or_axis_number));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }

  return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;
}

ControllerInterface::Hook::CallbackResult
InputRumbleBindingMonitor::operator()(const ControllerInterface::Hook& ei) const
{
  if (ei.type == ControllerInterface::Hook::Type::Button && ei.value > 0.0f)
  {
    QMetaObject::invokeMethod(m_parent, "bindToControllerRumble", Q_ARG(int, ei.controller_index));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }

  return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;
}
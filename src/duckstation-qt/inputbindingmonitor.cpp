#include "inputbindingmonitor.h"
#include <cmath>

ControllerInterface::Hook::CallbackResult
InputButtonBindingMonitor::operator()(const ControllerInterface::Hook& ei) const
{
  if (ei.type == ControllerInterface::Hook::Type::Axis)
  {
    // wait until it's at least half pushed so we don't get confused between axises with small movement
    if (std::abs(std::get<float>(ei.value)) < 0.5f)
      return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;

    // TODO: this probably should consider the "last value"
    QMetaObject::invokeMethod(m_parent, "bindToControllerAxis", Q_ARG(int, ei.controller_index),
                              Q_ARG(int, ei.button_or_axis_number), Q_ARG(bool, false),
                              Q_ARG(std::optional<bool>, std::get<float>(ei.value) > 0));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }
  else if (ei.type == ControllerInterface::Hook::Type::Button && std::get<float>(ei.value) > 0.0f)
  {
    QMetaObject::invokeMethod(m_parent, "bindToControllerButton", Q_ARG(int, ei.controller_index),
                              Q_ARG(int, ei.button_or_axis_number));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }
  else if (ei.type == ControllerInterface::Hook::Type::Hat)
  {
    const std::string_view hat_position = std::get<std::string_view>(ei.value);
    if (!hat_position.empty())
    {
      QString str = QString::fromLatin1(hat_position.data(), static_cast<int>(hat_position.size()));
      QMetaObject::invokeMethod(m_parent, "bindToControllerHat", Q_ARG(int, ei.controller_index),
                                Q_ARG(int, ei.button_or_axis_number), Q_ARG(QString, std::move(str)));
      return ControllerInterface::Hook::CallbackResult::StopMonitoring;
    }
  }

  return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;
}

ControllerInterface::Hook::CallbackResult InputAxisBindingMonitor::operator()(const ControllerInterface::Hook& ei) const
{
  if (ei.type == ControllerInterface::Hook::Type::Axis)
  {
    std::optional<bool> half_axis_positive, inverted;
    if (!ProcessAxisInput(ei, half_axis_positive, inverted))
      return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;

    QMetaObject::invokeMethod(m_parent, "bindToControllerAxis", Q_ARG(int, ei.controller_index),
                              Q_ARG(int, ei.button_or_axis_number), Q_ARG(bool, inverted.value_or(false)),
                              Q_ARG(std::optional<bool>, half_axis_positive));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }
  else if (ei.type == ControllerInterface::Hook::Type::Button && m_axis_type == Controller::AxisType::Half &&
           std::get<float>(ei.value) > 0.0f)
  {
    QMetaObject::invokeMethod(m_parent, "bindToControllerButton", Q_ARG(int, ei.controller_index),
                              Q_ARG(int, ei.button_or_axis_number));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }

  return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;
}

bool InputAxisBindingMonitor::ProcessAxisInput(const ControllerInterface::Hook& ei,
                                               std::optional<bool>& half_axis_positive,
                                               std::optional<bool>& inverted) const
{
  const float value = std::get<float>(ei.value);

  if (!ei.track_history) // Keyboard, mouse, game controller
  {
    // wait until it's at least half pushed so we don't get confused between axises with small movement
    if (std::abs(value) < 0.5f)
      return false;

    if (m_axis_type == Controller::AxisType::Half)
      half_axis_positive = (value > 0.0f);

    return true;
  }
  else // Joystick
  {
    auto& history = m_context->m_inputs_history;
    // Reject inputs coming from multiple sources
    if (!history.empty())
    {
      const auto& item = history.front();
      if (ei.controller_index != item.controller_index || ei.button_or_axis_number != item.axis_number)
        return false;
    }
    history.push_back({ei.controller_index, ei.button_or_axis_number, value});
    return AnalyzeInputHistory(half_axis_positive, inverted);
  }
}

bool InputAxisBindingMonitor::AnalyzeInputHistory(std::optional<bool>& half_axis_positive,
                                                  std::optional<bool>& inverted) const
{
  const auto& history = m_context->m_inputs_history;
  const auto [min, max] = std::minmax_element(
    history.begin(), history.end(), [](const auto& left, const auto& right) { return left.value < right.value; });

  // Ignore small input magnitudes
  if (std::abs(max->value - min->value) < 0.5f)
    return false;

  // Used heuristics:
  // * If history contains inputs with both - and + sign (ignoring 0), bind a full axis
  // * If history contains only 0 and inputs of the same sign AND maxes out at 1.0/-1.0, bind a half axis
  // * Use the direction of input changes to determine whether the axis is inverted or not
  if (std::signbit(min->value) != std::signbit(max->value))
  {
    if (min->value != 0.0f && max->value != 0.0f)
    {
      // If max value comes before the min value, invert the half axis
      if (m_axis_type == Controller::AxisType::Half)
      {
        inverted = std::distance(min, max) < 0;
      }

      return true;
    }
  }
  else
  {
    if ((std::abs(min->value) > 0.99f || std::abs(max->value) > 0.99f) &&
        (std::abs(min->value) < 0.01f || std::abs(max->value) < 0.01f))
    {

      if (m_axis_type == Controller::AxisType::Half)
      {
        half_axis_positive = max->value > 0.0f;
      }
      return true;
    }
  }

  return false;
}

ControllerInterface::Hook::CallbackResult
InputRumbleBindingMonitor::operator()(const ControllerInterface::Hook& ei) const
{
  if (ei.type == ControllerInterface::Hook::Type::Button && std::get<float>(ei.value) > 0.0f)
  {
    QMetaObject::invokeMethod(m_parent, "bindToControllerRumble", Q_ARG(int, ei.controller_index));
    return ControllerInterface::Hook::CallbackResult::StopMonitoring;
  }

  return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;
}
#pragma once

#include "frontend-common/controller_interface.h"
#include <QtCore/QObject>
#include <memory>
#include <vector>

// NOTE: Those Monitor classes must be copyable to meet the requirements of std::function, but at the same time we want
// copies to be opaque to the caling code and share context. Therefore, all mutable context of the monitor (if required)
// must be enclosed in a std::shared_ptr. m_parent/m_axis_type don't mutate so they don't need to be stored as such.

class InputButtonBindingMonitor
{
public:
  explicit InputButtonBindingMonitor(QObject* parent) : m_parent(parent) {}

  ControllerInterface::Hook::CallbackResult operator()(const ControllerInterface::Hook& ei) const;

private:
  QObject* m_parent;
};

class InputAxisBindingMonitor
{
public:
  explicit InputAxisBindingMonitor(QObject* parent, Controller::AxisType axis_type)
    : m_parent(parent), m_axis_type(axis_type)
  {
  }

  ControllerInterface::Hook::CallbackResult operator()(const ControllerInterface::Hook& ei) const;

private:
  bool ProcessAxisInput(const ControllerInterface::Hook& ei, std::optional<bool>& half_axis_positive,
                        std::optional<bool>& inverted) const;
  bool AnalyzeInputHistory(std::optional<bool>& half_axis_positive, std::optional<bool>& inverted) const;

  struct Context
  {
    struct History
    {
      int controller_index;
      int axis_number;
      float value;
    };

    std::vector<History> m_inputs_history;
  };

  QObject* m_parent;
  Controller::AxisType m_axis_type;
  std::shared_ptr<Context> m_context = std::make_shared<Context>();
};

class InputRumbleBindingMonitor
{
public:
  explicit InputRumbleBindingMonitor(QObject* parent) : m_parent(parent) {}

  ControllerInterface::Hook::CallbackResult operator()(const ControllerInterface::Hook& ei) const;

private:
  QObject* m_parent;
};
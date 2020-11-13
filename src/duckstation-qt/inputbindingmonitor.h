#pragma once

#include "frontend-common/controller_interface.h"
#include <QtCore/QObject>

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
  QObject* m_parent;
  Controller::AxisType m_axis_type;
};

class InputRumbleBindingMonitor
{
public:
  explicit InputRumbleBindingMonitor(QObject* parent) : m_parent(parent) {}

  ControllerInterface::Hook::CallbackResult operator()(const ControllerInterface::Hook& ei) const;

private:
  QObject* m_parent;
};
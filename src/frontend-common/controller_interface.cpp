#include "controller_interface.h"
#include "common/assert.h"
#include "common/log.h"
#include "core/controller.h"
#include "core/system.h"
#include <cmath>
Log_SetChannel(ControllerInterface);

ControllerInterface::ControllerInterface() = default;

ControllerInterface::~ControllerInterface() = default;

bool ControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  m_host_interface = host_interface;
  return true;
}

void ControllerInterface::Shutdown()
{
  m_host_interface = nullptr;
}

System* ControllerInterface::GetSystem() const
{
  return m_host_interface->GetSystem();
}

Controller* ControllerInterface::GetController(u32 slot) const
{
  System* system = GetSystem();
  return system ? system->GetController(slot) : nullptr;
}

void ControllerInterface::SetHook(Hook::Callback callback)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  Assert(!m_event_intercept_callback);
  m_event_intercept_callback = std::move(callback);
}

void ControllerInterface::ClearHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (m_event_intercept_callback)
    m_event_intercept_callback = {};
}

bool ControllerInterface::DoEventHook(Hook::Type type, int controller_index, int button_or_axis_number, float value)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (!m_event_intercept_callback)
    return false;

  const Hook ei{type, controller_index, button_or_axis_number, value};
  const Hook::CallbackResult action = m_event_intercept_callback(ei);
  if (action == Hook::CallbackResult::StopMonitoring)
    m_event_intercept_callback = {};

  return true;
}

void ControllerInterface::OnControllerConnected(int host_id)
{
  Log_InfoPrintf("Host controller %d connected, updating input map", host_id);
  m_host_interface->UpdateInputMap();
}

void ControllerInterface::OnControllerDisconnected(int host_id)
{
  Log_InfoPrintf("Host controller %d disconnected, updating input map", host_id);
  m_host_interface->UpdateInputMap();
}

void ControllerInterface::ClearBindings() {}

bool ControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisCallback callback)
{
  return false;
}

bool ControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  return false;
}

bool ControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                     ButtonCallback callback)
{
  return false;
}


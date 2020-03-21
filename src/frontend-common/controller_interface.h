#pragma once
#include "common_host_interface.h"
#include "core/types.h"
#include <array>
#include <functional>
#include <map>
#include <mutex>

class HostInterface;
class System;
class Controller;

class ControllerInterface
{
public:
  enum : int
  {
    MAX_NUM_AXISES = 7,
    MAX_NUM_BUTTONS = 15
  };

  using AxisCallback = CommonHostInterface::InputAxisHandler;
  using ButtonCallback = CommonHostInterface::InputButtonHandler;

  ControllerInterface();
  virtual ~ControllerInterface();

  virtual bool Initialize(CommonHostInterface* host_interface);
  virtual void Shutdown();

  // Removes all bindings. Call before setting new bindings.
  virtual void ClearBindings() = 0;

  // Binding to events. If a binding for this axis/button already exists, returns false.
  virtual bool BindControllerAxis(int controller_index, int axis_number, AxisCallback callback) = 0;
  virtual bool BindControllerButton(int controller_index, int button_number, ButtonCallback callback) = 0;
  virtual bool BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                          ButtonCallback callback) = 0;
  
  virtual void PollEvents() = 0;
  virtual void UpdateControllerRumble() = 0;

  // Input monitoring for external access.
  struct Hook
  {
    enum class Type
    {
      Axis,
      Button
    };

    enum class CallbackResult
    {
      StopMonitoring,
      ContinueMonitoring
    };

    using Callback = std::function<CallbackResult(const Hook& ei)>;

    Type type;
    int controller_index;
    int button_or_axis_number;
    float value; // 0/1 for buttons, -1..1 for axises
  };
  void SetHook(Hook::Callback callback);
  void ClearHook();

protected:
  System* GetSystem() const;
  Controller* GetController(u32 slot) const;
  bool DoEventHook(Hook::Type type, int controller_index, int button_or_axis_number, float value);

  void OnControllerConnected(int host_id);
  void OnControllerDisconnected(int host_id);

  CommonHostInterface* m_host_interface = nullptr;

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;
};

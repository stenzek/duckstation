#pragma once
#include "core/types.h"
#include <array>
#include <functional>
#include <map>
#include <mutex>

class HostInterface;
class System;
class Controller;

union SDL_Event;

class SDLControllerInterface
{
public:
  enum : int
  {
    MAX_NUM_AXISES = 7,
    MAX_NUM_BUTTONS = 15
  };

  using AxisCallback = std::function<void(float value)>;
  using ButtonCallback = std::function<void(bool pressed)>;

  SDLControllerInterface();
  ~SDLControllerInterface();

  bool Initialize(HostInterface* host_interface);
  void Shutdown();

  // Removes all bindings. Call before setting new bindings.
  void ClearControllerBindings();

  // Binding to events. If a binding for this axis/button already exists, returns false.
  bool BindControllerAxis(int controller_index, int axis_number, AxisCallback callback);
  bool BindControllerButton(int controller_index, int button_number, ButtonCallback callback);
  bool BindControllerAxisToButton(int controller_index, int axis_number, bool direction, ButtonCallback callback);

  // Default bindings, used by SDL frontend.
  void SetDefaultBindings();

  void PumpSDLEvents();

  bool ProcessSDLEvent(const SDL_Event* event);

  void UpdateControllerRumble();

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

private:
  System* GetSystem() const;
  Controller* GetController(u32 slot) const;
  bool DoEventHook(Hook::Type type, int controller_index, int button_or_axis_number, float value);

  bool OpenGameController(int index);
  bool CloseGameController(int index);
  void CloseGameControllers();
  bool HandleControllerAxisEvent(const SDL_Event* event);
  bool HandleControllerButtonEvent(const SDL_Event* event);

  struct ControllerData
  {
    void* controller;
    void* haptic;
    u32 controller_index;
    float last_rumble_strength;

    std::array<AxisCallback, MAX_NUM_AXISES> axis_mapping;
    std::array<ButtonCallback, MAX_NUM_BUTTONS> button_mapping;
    std::array<std::array<ButtonCallback, 2>, MAX_NUM_AXISES> axis_button_mapping;
  };

  HostInterface* m_host_interface = nullptr;

  std::map<int, ControllerData> m_controllers;

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;

  bool m_initialized = false;
};

extern SDLControllerInterface g_sdl_controller_interface;

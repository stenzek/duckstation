#pragma once
#include "core/types.h"
#include "controller_interface.h"
#include <array>
#include <functional>
#include <vector>
#include <mutex>

union SDL_Event;

class SDLControllerInterface final : public ControllerInterface
{
public:
  SDLControllerInterface();
  ~SDLControllerInterface();

  bool Initialize(CommonHostInterface* host_interface) override;
  void Shutdown() override;

  // Removes all bindings. Call before setting new bindings.
  void ClearBindings() override;

  // Binding to events. If a binding for this axis/button already exists, returns false.
  bool BindControllerAxis(int controller_index, int axis_number, AxisCallback callback) override;
  bool BindControllerButton(int controller_index, int button_number, ButtonCallback callback) override;
  bool BindControllerAxisToButton(int controller_index, int axis_number, bool direction, ButtonCallback callback) override;

  void PollEvents() override;

  bool ProcessSDLEvent(const SDL_Event* event);

  void UpdateControllerRumble() override;

private:
  struct ControllerData
  {
    void* controller;
    void* haptic;
    int joystick_id;
    int player_id;
    float last_rumble_strength;

    std::array<AxisCallback, MAX_NUM_AXISES> axis_mapping;
    std::array<ButtonCallback, MAX_NUM_BUTTONS> button_mapping;
    std::array<std::array<ButtonCallback, 2>, MAX_NUM_AXISES> axis_button_mapping;
  };

  using ControllerDataVector = std::vector<ControllerData>;

  ControllerDataVector::iterator GetControllerDataForController(void* controller);
  ControllerDataVector::iterator GetControllerDataForJoystickId(int id);
  ControllerDataVector::iterator GetControllerDataForPlayerId(int id);
  int GetFreePlayerId() const;

  bool OpenGameController(int index);
  bool CloseGameController(int joystick_index, bool notify);
  bool HandleControllerAxisEvent(const SDL_Event* event);
  bool HandleControllerButtonEvent(const SDL_Event* event);

  ControllerDataVector m_controllers;

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;

  bool m_sdl_subsystem_initialized = false;
};

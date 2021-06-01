#pragma once
#include "common/windows_headers.h"
#include "controller_interface.h"
#include "core/types.h"
#include <Xinput.h>
#include <array>
#include <functional>
#include <mutex>
#include <vector>

class XInputControllerInterface final : public ControllerInterface
{
public:
  XInputControllerInterface();
  ~XInputControllerInterface() override;

  Backend GetBackend() const override;
  bool Initialize(CommonHostInterface* host_interface) override;
  void Shutdown() override;

  // Removes all bindings. Call before setting new bindings.
  void ClearBindings() override;

  // Binding to events. If a binding for this axis/button already exists, returns false.
  bool BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side, AxisCallback callback) override;
  bool BindControllerButton(int controller_index, int button_number, ButtonCallback callback) override;
  bool BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                  ButtonCallback callback) override;
  bool BindControllerHatToButton(int controller_index, int hat_number, std::string_view hat_position,
                                 ButtonCallback callback) override;
  bool BindControllerButtonToAxis(int controller_index, int button_number, AxisCallback callback) override;

  // Changing rumble strength.
  u32 GetControllerRumbleMotorCount(int controller_index) override;
  void SetControllerRumbleStrength(int controller_index, const float* strengths, u32 num_motors) override;

  // Set deadzone that will be applied on axis-to-button mappings
  bool SetControllerDeadzone(int controller_index, float size = 0.25f) override;

  void PollEvents() override;

private:
  enum : u32
  {
    NUM_AXES = 6,
    NUM_BUTTONS = 15,
    NUM_RUMBLE_MOTORS = 2
  };
  enum class Axis : u32
  {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger
  };

  struct ControllerData
  {
    XINPUT_STATE last_state = {};
    bool connected = false;

    float deadzone = 0.25f;

    std::array<std::array<AxisCallback, 3>, NUM_AXES> axis_mapping;
    std::array<ButtonCallback, NUM_BUTTONS> button_mapping;
    std::array<std::array<ButtonCallback, 2>, NUM_AXES> axis_button_mapping;
    std::array<AxisCallback, NUM_BUTTONS> button_axis_mapping;
  };

  using ControllerDataArray = std::array<ControllerData, XUSER_MAX_COUNT>;

  void CheckForStateChanges(u32 index, const XINPUT_STATE& new_state);
  bool HandleAxisEvent(u32 index, Axis axis, s32 value);
  bool HandleButtonEvent(u32 index, u32 button, bool pressed);

  ControllerDataArray m_controllers;

  HMODULE m_xinput_module{};
  DWORD(WINAPI* m_xinput_get_state)(DWORD, XINPUT_STATE*);
  DWORD(WINAPI* m_xinput_set_state)(DWORD, XINPUT_VIBRATION*);
  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;
};

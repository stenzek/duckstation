#pragma once
#include "frontend-common/controller_interface.h"
#include "core/types.h"
#include <array>
#include <functional>
#include <mutex>
#include <vector>

class AndroidControllerInterface final : public ControllerInterface
{
public:
  AndroidControllerInterface();
  ~AndroidControllerInterface() override;

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

  bool HandleAxisEvent(u32 index, u32 axis, float value);
  bool HandleButtonEvent(u32 index, u32 button, bool pressed);

private:
  enum : u32
  {
    NUM_CONTROLLERS = 1,
    NUM_AXISES = 12,
    NUM_BUTTONS = 23
  };

  struct ControllerData
  {
    float deadzone = 0.25f;

    std::array<std::array<AxisCallback, 3>, NUM_AXISES> axis_mapping;
    std::array<ButtonCallback, NUM_BUTTONS> button_mapping;
    std::array<std::array<ButtonCallback, 2>, NUM_AXISES> axis_button_mapping;
    std::array<AxisCallback, NUM_BUTTONS> button_axis_mapping;
  };

  using ControllerDataArray = std::array<ControllerData, NUM_CONTROLLERS>;

  ControllerDataArray m_controllers;

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;
};

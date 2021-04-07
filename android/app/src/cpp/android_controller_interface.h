#pragma once
#include "core/types.h"
#include "frontend-common/controller_interface.h"
#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

class AndroidControllerInterface final : public ControllerInterface
{
public:
  AndroidControllerInterface();
  ~AndroidControllerInterface() override;

  ALWAYS_INLINE u32 GetControllerCount() const { return static_cast<u32>(m_controllers.size()); }

  Backend GetBackend() const override;
  bool Initialize(CommonHostInterface* host_interface) override;
  void Shutdown() override;

  // Removes all bindings. Call before setting new bindings.
  void ClearBindings() override;

  // Binding to events. If a binding for this axis/button already exists, returns false.
  std::optional<int> GetControllerIndex(const std::string_view& device) override;
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

  void SetDeviceNames(std::vector<std::string> device_names);
  void SetDeviceRumble(u32 index, bool has_vibrator);
  void HandleAxisEvent(u32 index, u32 axis, float value);
  void HandleButtonEvent(u32 index, u32 button, bool pressed);
  bool HasButtonBinding(u32 index, u32 button);

private:
  enum : u32
  {
    NUM_RUMBLE_MOTORS = 2
  };

  struct ControllerData
  {
    float deadzone = 0.25f;

    std::map<u32, std::array<AxisCallback, 3>> axis_mapping;
    std::map<u32, ButtonCallback> button_mapping;
    std::map<u32, std::array<ButtonCallback, 2>> axis_button_mapping;
    std::map<u32, AxisCallback> button_axis_mapping;
    bool has_rumble = false;
  };

  std::vector<std::string> m_device_names;
  std::vector<ControllerData> m_controllers;
  std::mutex m_controllers_mutex;

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;
};

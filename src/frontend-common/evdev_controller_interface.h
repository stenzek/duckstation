#pragma once
#include "input_source.h"
#include "core/types.h"
#include <array>
#include <functional>
#include <libevdev/libevdev.h>
#include <mutex>
#include <vector>

#if 0

class EvdevControllerInterface final : public ControllerInterface
{
public:
  EvdevControllerInterface();
  ~EvdevControllerInterface() override;

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
    ControllerData(int fd_, struct libevdev* obj_);
    ControllerData(const ControllerData&) = delete;
    ControllerData(ControllerData&& move);
    ~ControllerData();

    ControllerData& operator=(const ControllerData&) = delete;
    ControllerData& operator=(ControllerData&& move);

    struct libevdev* obj = nullptr;
    int fd = -1;
    int controller_id = 0;
    u32 num_motors = 0;

    float deadzone = 0.25f;

    struct Axis
    {
      u32 id;
      s32 min;
      s32 range;
      std::array<AxisCallback, 3> callback;
      std::array<ButtonCallback, 2> button_callback;
    };

    struct Button
    {
      u32 id;
      ButtonCallback callback;
      AxisCallback axis_callback;
    };

    std::vector<Axis> axes;
    std::vector<Button> buttons;
  };

  ControllerData* GetControllerById(int id);
  bool InitializeController(int index, ControllerData* cd);
  void HandleControllerEvents(ControllerData* cd);
  bool HandleAxisEvent(ControllerData* cd, u32 axis, s32 value);
  bool HandleButtonEvent(ControllerData* cd, u32 button, int button_id, bool pressed);

  std::vector<ControllerData> m_controllers;

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;
};

#endif

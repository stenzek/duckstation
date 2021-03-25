#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include "common/windows_headers.h"
#include "controller_interface.h"
#include "core/types.h"
#include <array>
#include <dinput.h>
#include <functional>
#include <mutex>
#include <vector>
#include <wrl/client.h>

class DInputControllerInterface final : public ControllerInterface
{
public:
  DInputControllerInterface();
  ~DInputControllerInterface() override;

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
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  enum : u32
  {
    NUM_AXISES = 8,
    NUM_BUTTONS = 16,
    NUM_HATS = 1,

    TOTAL_NUM_BUTTONS = NUM_BUTTONS + (NUM_HATS * NUM_HAT_DIRECTIONS),
  };

  struct ControllerData
  {
    ComPtr<IDirectInputDevice8> device;
    DIJOYSTATE last_state = {};
    u32 num_buttons = 0;
    u32 num_axes = 0;

    float deadzone = 0.25f;

    std::array<u32, NUM_AXISES> axis_offsets;

    std::array<std::array<AxisCallback, 3>, NUM_AXISES> axis_mapping;
    std::array<ButtonCallback, TOTAL_NUM_BUTTONS + NUM_HAT_DIRECTIONS> button_mapping;
    std::array<std::array<ButtonCallback, 2>, NUM_AXISES> axis_button_mapping;
    std::array<AxisCallback, TOTAL_NUM_BUTTONS + NUM_HAT_DIRECTIONS> button_axis_mapping;

    bool has_hat = false;
    bool needs_poll = true;
  };

  using ControllerDataArray = std::vector<ControllerData>;

  void AddDevices();
  bool AddDevice(ControllerData& cd, const char* name);

  static std::array<bool, NUM_HAT_DIRECTIONS> GetHatButtons(DWORD hat);

  void CheckForStateChanges(u32 index, const DIJOYSTATE& new_state);

  bool HandleAxisEvent(u32 index, u32 axis, s32 value);
  bool HandleButtonEvent(u32 index, u32 button, bool pressed);

  ControllerDataArray m_controllers;

  HMODULE m_dinput_module{};
  LPCDIDATAFORMAT m_joystick_data_format{};
  ComPtr<IDirectInput8> m_dinput;
  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;
};

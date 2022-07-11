#pragma once
#include "controller.h"
#include <array>
#include <memory>
#include <optional>
#include <string_view>

class AnalogJoystick final : public Controller
{
public:
  enum class Axis : u8
  {
    LeftX,
    LeftY,
    RightX,
    RightY,
    Count
  };

  enum class Button : u8
  {
    Select = 0,
    L3 = 1,
    R3 = 2,
    Start = 3,
    Up = 4,
    Right = 5,
    Down = 6,
    Left = 7,
    L2 = 8,
    R2 = 9,
    L1 = 10,
    R1 = 11,
    Triangle = 12,
    Circle = 13,
    Cross = 14,
    Square = 15,
    Mode = 16,
    Count
  };

  enum class HalfAxis : u8
  {
    LLeft,
    LRight,
    LDown,
    LUp,
    RLeft,
    RRight,
    RDown,
    RUp,
    Count
  };

  static const Controller::ControllerInfo INFO;

  AnalogJoystick(u32 index);
  ~AnalogJoystick() override;

  static std::unique_ptr<AnalogJoystick> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;
  std::optional<u32> GetAnalogInputBytes() const override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void LoadSettings(SettingsInterface& si, const char* section) override;

private:
  enum class TransferState : u8
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    RightAxisX,
    RightAxisY,
    LeftAxisX,
    LeftAxisY
  };

  u16 GetID() const;
  void ToggleAnalogMode();

  float m_analog_deadzone = 0.0f;
  float m_analog_sensitivity = 1.33f;

  // On original hardware, the mode toggle is a switch rather than a button, so we'll enable Analog Mode by default
  bool m_analog_mode = true;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

  std::array<u8, static_cast<u8>(Axis::Count)> m_axis_state{};

  // both directions of axis state, merged to m_axis_state
  std::array<u8, static_cast<u32>(HalfAxis::Count)> m_half_axis_state{};

  TransferState m_transfer_state = TransferState::Idle;
};

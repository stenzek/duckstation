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

  AnalogJoystick(u32 index);
  ~AnalogJoystick() override;

  static std::unique_ptr<AnalogJoystick> Create(u32 index);
  static std::optional<s32> StaticGetAxisCodeByName(std::string_view axis_name);
  static std::optional<s32> StaticGetButtonCodeByName(std::string_view button_name);
  static AxisList StaticGetAxisNames();
  static ButtonList StaticGetButtonNames();
  static u32 StaticGetVibrationMotorCount();
  static SettingList StaticGetSettings();

  ControllerType GetType() const override;
  std::optional<s32> GetAxisCodeByName(std::string_view axis_name) const override;
  std::optional<s32> GetButtonCodeByName(std::string_view button_name) const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetAxisState(s32 axis_code) const override;
  void SetAxisState(s32 axis_code, float value) override;
  bool GetButtonState(s32 button_code) const override;
  void SetButtonState(s32 button_code, bool pressed) override;
  u32 GetButtonStateBits() const override;
  std::optional<u32> GetAnalogInputBytes() const override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void LoadSettings(const char* section) override;

  void SetAxisState(Axis axis, u8 value);
  void SetButtonState(Button button, bool pressed);

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

  u32 m_index;

  float m_axis_scale = 1.00f;

  // On original hardware, the mode toggle is a switch rather than a button, so we'll enable Analog Mode by default
  bool m_analog_mode = true;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

  std::array<u8, static_cast<u8>(Axis::Count)> m_axis_state{};

  TransferState m_transfer_state = TransferState::Idle;
};

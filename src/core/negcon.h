#pragma once
#include "controller.h"
#include <array>
#include <memory>
#include <optional>
#include <string_view>

class NeGcon final : public Controller
{
public:
  enum class Axis : u8
  {
    Steering = 0,
    I = 1,
    II = 2,
    L = 3,
    Count
  };

  enum class Button : u8
  {
    Start = 0,
    Up = 1,
    Right = 2,
    Down = 3,
    Left = 4,
    R = 5,
    B = 6,
    A = 7,
    Count
  };

  NeGcon();
  ~NeGcon() override;

  static std::unique_ptr<NeGcon> Create();
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

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void SetAxisState(Axis axis, u8 value);
  void SetButtonState(Button button, bool pressed);

  u32 GetButtonStateBits() const override;
  std::optional<u32> GetAnalogInputBytes() const override;

  void LoadSettings(const char* section) override;

private:
  enum class TransferState : u8
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    AnalogSteering,
    AnalogI,
    AnalogII,
    AnalogL
  };

  std::array<u8, static_cast<u8>(Axis::Count)> m_axis_state{};

  // buttons are active low; bits 0-2, 8-10, 14-15 are not used and are always high
  u16 m_button_state = UINT16_C(0xFFFF);

  TransferState m_transfer_state = TransferState::Idle;

  float m_steering_deadzone = 0.00f;
};

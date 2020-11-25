#pragma once
#include "controller.h"
#include <array>
#include <memory>
#include <optional>
#include <string_view>

class AnalogController final : public Controller
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
    Analog = 16,
    Count
  };

  static constexpr u8 NUM_MOTORS = 2;

  AnalogController(u32 index);
  ~AnalogController() override;

  static std::unique_ptr<AnalogController> Create(u32 index);
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
  bool DoState(StateWrapper& sw) override;

  void SetAxisState(s32 axis_code, float value) override;
  void SetButtonState(s32 button_code, bool pressed) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void SetAxisState(Axis axis, u8 value);
  void SetButtonState(Button button, bool pressed);

  u32 GetVibrationMotorCount() const override;
  float GetVibrationMotorStrength(u32 motor) override;

  void LoadSettings(const char* section) override;

private:
  using MotorState = std::array<u8, NUM_MOTORS>;

  enum class State : u8
  {
    Idle,
    GetStateIDMSB,
    GetStateButtonsLSB,
    GetStateButtonsMSB,
    GetStateRightAxisX,
    GetStateRightAxisY,
    GetStateLeftAxisX,
    GetStateLeftAxisY,
    ConfigModeIDMSB,
    ConfigModeSetMode,
    SetAnalogModeIDMSB,
    SetAnalogModeVal,
    SetAnalogModeSel,
    GetAnalogModeIDMSB,
    GetAnalogMode1,
    GetAnalogMode2,
    GetAnalogMode3,
    GetAnalogMode4,
    GetAnalogMode5,
    GetAnalogMode6,
    UnlockRumbleIDMSB,
    GetSetRumble1,
    GetSetRumble2,
    GetSetRumble3,
    GetSetRumble4,
    GetSetRumble5,
    GetSetRumble6,
    Command46IDMSB,
    Command461,
    Command462,
    Command463,
    Command464,
    Command465,
    Command466,
    Command47IDMSB,
    Command471,
    Command472,
    Command473,
    Command474,
    Command475,
    Command476,
    Command4CIDMSB,
    Command4CMode,
    Command4C1,
    Command4C2,
    Command4C3,
    Command4C4,
    Command4C5,
    Pad6Bytes,
    Pad5Bytes,
    Pad4Bytes,
    Pad3Bytes,
    Pad2Bytes,
    Pad1Byte,
  };

  u16 GetID() const;
  void SetAnalogMode(bool enabled);
  void SetMotorState(u8 motor, u8 value);
  u8 GetExtraButtonMaskLSB() const;
  void ResetRumbleConfig();
  void SetMotorStateForConfigIndex(int index, u8 value);

  u32 m_index;

  bool m_auto_enable_analog = false;
  bool m_analog_dpad_in_digital_mode = false;
  float m_axis_scale = 1.00f;
  u8 m_rumble_bias = 8;

  bool m_analog_mode = false;
  bool m_analog_locked = false;
  bool m_rumble_unlocked = false;
  bool m_legacy_rumble_unlocked = false;
  bool m_configuration_mode = false;
  u8 m_command_param = 0;

  std::array<u8, static_cast<u8>(Axis::Count)> m_axis_state{};

  enum : u8
  {
    LargeMotor = 0,
    SmallMotor = 1
  };

  std::array<u8, 6> m_rumble_config{};
  int m_rumble_config_large_motor_index = -1;
  int m_rumble_config_small_motor_index = -1;

  bool m_analog_toggle_queued = false;

  // TODO: Set this with command 0x4D and increase response length in digital mode accordingly
  u8 m_digital_mode_additional_bytes = 0;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

  MotorState m_motor_state{};

  State m_state = State::Idle;
};

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

  enum class HalfAxis : u8
  {
    SteeringLeft,
    SteeringRight,
    I,
    II,
    L,
    Count
  };

  static const Controller::ControllerInfo INFO;

  NeGcon(u32 index);
  ~NeGcon() override;

  static std::unique_ptr<NeGcon> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  u32 GetButtonStateBits() const override;
  std::optional<u32> GetAnalogInputBytes() const override;

  void LoadSettings(SettingsInterface& si, const char* section) override;

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

  // steering, merged to m_axis_state
  std::array<u8, 2> m_half_axis_state{};

  // buttons are active low; bits 0-2, 8-10, 14-15 are not used and are always high
  u16 m_button_state = UINT16_C(0xFFFF);

  TransferState m_transfer_state = TransferState::Idle;

  float m_steering_deadzone = 0.00f;

};

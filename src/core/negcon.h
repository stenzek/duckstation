// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

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

  struct AxisModifier
  {
    float deadzone;
    float saturation;
    float linearity;
    float scaling;
    float zero;
    float unit;
  };

  static constexpr float DEFAULT_DEADZONE = 0.00f;
  static constexpr float DEFAULT_SATURATION = 1.00f;
  static constexpr float DEFAULT_LINEARITY = 0.00f;
  static constexpr float DEFAULT_SCALING = 1.00f;
  static constexpr float DEFAULT_STEERING_ZERO = 128.0f;
  static constexpr float DEFAULT_STEERING_UNIT = 128.0f;
  static constexpr float DEFAULT_PEDAL_ZERO = 0.0f;
  static constexpr float DEFAULT_PEDAL_UNIT = 255.0f;
  static constexpr AxisModifier DEFAULT_STEERING_MODIFIER = {
    .deadzone = DEFAULT_DEADZONE,
    .saturation = DEFAULT_SATURATION,
    .linearity = DEFAULT_LINEARITY,
    .scaling = DEFAULT_SCALING,
    .zero = DEFAULT_STEERING_ZERO,
    .unit = DEFAULT_STEERING_UNIT,
  };
  static constexpr AxisModifier DEFAULT_PEDAL_MODIFIER = {
    .deadzone = DEFAULT_DEADZONE,
    .saturation = DEFAULT_SATURATION,
    .linearity = DEFAULT_LINEARITY,
    .scaling = DEFAULT_SCALING,
    .zero = DEFAULT_PEDAL_ZERO,
    .unit = DEFAULT_PEDAL_UNIT,
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
  std::array<float, 2> m_half_axis_state;

  // buttons are active low; bits 0-2, 8-10, 14-15 are not used and are always high
  u16 m_button_state = UINT16_C(0xFFFF);

  TransferState m_transfer_state = TransferState::Idle;

  AxisModifier m_steering_modifier = DEFAULT_STEERING_MODIFIER;
  std::array<AxisModifier, 3> m_half_axis_modifiers = {
    DEFAULT_PEDAL_MODIFIER,
    DEFAULT_PEDAL_MODIFIER,
    DEFAULT_PEDAL_MODIFIER,
  };
};

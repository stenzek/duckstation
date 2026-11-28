// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <memory>

class SettingsInterface;

/// Inputs shared by the NeGcon controllers.
class NeGconInputs
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

  explicit NeGconInputs(u32 half_axis_bind_start);

  float GetBindState(u32 index) const;
  void SetBindState(u32 index, float value);

  u32 GetButtonStateBits() const;
  std::optional<u32> GetAnalogInputBytes() const;

  void LoadSettings(const SettingsInterface& si, const char* section);

  std::array<u8, static_cast<u8>(Axis::Count)> m_axis_state{};

  // steering, merged to m_axis_state
  std::array<float, 2> m_half_axis_state{};

  // buttons are active low; bits 0-2, 8-10, 14-15 are not used and are always high
  u16 m_button_state = UINT16_C(0xFFFF);

  bool m_disable_socd = false;

private:
  u32 m_half_axis_bind_start;
  AxisModifier m_steering_modifier = {};
  std::array<AxisModifier, 3> m_half_axis_modifiers = {};
};

class NeGcon final : public Controller
{
public:
  using Axis = NeGconInputs::Axis;
  using Button = NeGconInputs::Button;
  using HalfAxis = NeGconInputs::HalfAxis;

  static const Controller::ControllerInfo INFO;

  explicit NeGcon(u32 index);
  ~NeGcon() override;

  static std::unique_ptr<NeGcon> Create(u32 index);

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;
  std::optional<u32> GetAnalogInputBytes() const override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

private:
  u8 GetIDByte() const override;
  bool BeginCommand(u8 command) override;

  NeGconInputs m_inputs{static_cast<u32>(Button::Count)};
};

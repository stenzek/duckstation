// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <memory>

class PlayStationMouse final : public Controller
{
public:
  enum class Binding : u8
  {
    Left = 0,
    Right = 1,
    ButtonCount = 2,

    PointerX = 2,
    PointerY = 3,
    BindingCount = 4,
  };

  static const Controller::ControllerInfo INFO;

  explicit PlayStationMouse(u32 index);
  ~PlayStationMouse() override;

  static std::unique_ptr<PlayStationMouse> Create(u32 index);

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

private:
  u8 GetIDByte() const override;
  bool BeginCommand(u8 command) override;

  float m_sensitivity_x = 1.0f;
  float m_sensitivity_y = 1.0f;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  float m_delta_x = 0;
  float m_delta_y = 0;
};
